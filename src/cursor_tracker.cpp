#include "cursor_tracker.h"
#include "accessibility_helper.h"
#include <algorithm>
#include <algorithm> // For std::sort
#include <cctype>
#include <imm.h>
#include <locale>
#include <oleacc.h>
#include <psapi.h>
#include <utils.h>

namespace weasel {

CursorTracker::CursorTracker()
    : enabled_(true), update_threshold_(5), cache_timeout_ms_(50),
      last_target_window_(nullptr), call_count_(0), cache_hit_count_(0),
      last_valid_position_{0, 0}, consecutive_invalid_count_(0) {

  DEBUG << "CursorTracker initialized";
}

CursorTracker::~CursorTracker() {
  if (call_count_ > 0) {
    float cache_hit_rate = (float)cache_hit_count_ / call_count_ * 100.0f;
    DEBUG << "CursorTracker stats - Calls: " << call_count_
          << ", Cache hits: " << cache_hit_count_
          << ", Hit rate: " << cache_hit_rate << "%";
  }
}

CursorPosition CursorTracker::GetCursorPosition(HWND targetWindow) {
  call_count_++;

  if (!enabled_ || !targetWindow) {
    return {};
  }

  // 检查缓存是否有效
  auto now = std::chrono::steady_clock::now();
  if (cached_position_.valid && cached_position_.targetWindow == targetWindow &&
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - cached_position_.timestamp)
              .count() < cache_timeout_ms_) {
    cache_hit_count_++;
    return cached_position_;
  }

  ApplicationType appType = DetectApplicationType(targetWindow);
  SetCacheTimeoutForApp(appType);

  // 1. 收集所有可能的候选位置
  std::vector<PositionCandidate> candidates;
  if (auto pt = TryGetGUIThreadInfo(targetWindow))
    candidates.push_back({*pt, CursorDetectionMethod::GUI_THREAD_INFO, 0});
  if (auto pt = TryGetIMEComposition(targetWindow))
    candidates.push_back({*pt, CursorDetectionMethod::IME_COMPOSITION, 0});
  if (auto pt = TryGetAccessibility(targetWindow))
    candidates.push_back({*pt, CursorDetectionMethod::ACCESSIBILITY, 0});
  if (auto pt = TryGetCaretPos(targetWindow))
    candidates.push_back({*pt, CursorDetectionMethod::CARET_POS, 0});
  if (auto pt = TryGetMousePosition())
    candidates.push_back({*pt, CursorDetectionMethod::MOUSE_FALLBACK, 0});

  if (candidates.empty()) {
    DEBUG << "No cursor candidates found.";
    return cached_position_.valid ? cached_position_ : CursorPosition();
  }

  // 2. 找到最佳候选位置
  PositionCandidate best_candidate =
      FindBestCandidate(candidates, targetWindow, appType);

  // 3. 创建并返回最终结果
  CursorPosition result(best_candidate.point, best_candidate.method,
                        targetWindow);
  AdjustPositionForWindow(result.point, targetWindow);

  if (ShouldUpdatePosition(result)) {
    UpdateCache(result);
    last_valid_position_ = result.point; // 更新上次有效位置
    DEBUG << "Position updated to (" << result.point.x << ", " << result.point.y
          << ") using method " << (int)result.method;
  } else {
    // 如果位置未更新，返回缓存的位置以保持稳定
    return cached_position_;
  }

  return result;
}

PositionCandidate
CursorTracker::FindBestCandidate(std::vector<PositionCandidate> &candidates,
                                 HWND hwnd, ApplicationType appType) {
  for (auto &candidate : candidates) {
    ScoreCandidate(candidate, hwnd, appType);
  }

  // 按分数降序排序
  std::sort(candidates.begin(), candidates.end(),
            [](const auto &a, const auto &b) { return a.score > b.score; });

  DEBUG << "Found " << candidates.size()
        << " candidates. Best score: " << candidates[0].score;
  return candidates[0];
}

int CursorTracker::ScoreCandidate(PositionCandidate &candidate, HWND hwnd,
                                  ApplicationType appType) {
  int score = 0;

  // 1. 基础分 (基于方法)
  switch (candidate.method) {
  case CursorDetectionMethod::IME_COMPOSITION:
    score = 120;
    break;
  case CursorDetectionMethod::ACCESSIBILITY:
    score = 100;
    break;
  case CursorDetectionMethod::GUI_THREAD_INFO:
    score = 80;
    break;
  case CursorDetectionMethod::CARET_POS:
    score = 60;
    break;
  case CursorDetectionMethod::MOUSE_FALLBACK:
    score = 20;
    break;
  }

  // 2. 启发式规则 (加分/扣分)
  // 惩罚: 接近原点 (通常是无效数据)
  if (candidate.point.x < 20 && candidate.point.y < 20) {
    score -= 1000;
  }

  // 奖励: 在窗口内部
  if (IsPointInWindow(candidate.point, hwnd)) {
    score += 50;
  }

  // 奖励/惩罚: 与上次有效位置的距离
  if (last_valid_position_.x != 0 || last_valid_position_.y != 0) {
    int dx = abs(candidate.point.x - last_valid_position_.x);
    int dy = abs(candidate.point.y - last_valid_position_.y);
    if (dx < 20 && dy < 20) {
      score += 30; // 稳定，少量奖励
    } else if (dx > 500 || dy > 400) {
      score -= 100; // 位置跳跃，严厉惩罚
    }
  }

  // 奖励: 在同一个监视器
  HMONITOR hMonitor = MonitorFromPoint(candidate.point, MONITOR_DEFAULTTONULL);
  HMONITOR hWindowMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (hMonitor && hMonitor == hWindowMonitor) {
    score += 20;
  }

  candidate.score = score;
  return score;
}

std::optional<POINT> CursorTracker::TryGetGUIThreadInfo(HWND hwnd) {
  if (!hwnd)
    return std::nullopt;
  DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
  if (!threadId)
    return std::nullopt;

  GUITHREADINFO gti = {sizeof(GUITHREADINFO)};
  if (!GetGUIThreadInfo(threadId, &gti))
    return std::nullopt;

  if (gti.flags & GUI_CARETBLINKING && gti.hwndCaret) {
    POINT pt = {gti.rcCaret.left, gti.rcCaret.bottom + 2};
    if (ClientToScreen(gti.hwndCaret, &pt))
      return pt;
  }

  if (gti.hwndFocus) {
    RECT rect;
    if (GetWindowRect(gti.hwndFocus, &rect)) {
      return POINT{rect.left + 10, rect.top + 25};
    }
  }
  return std::nullopt;
}

std::optional<POINT> CursorTracker::TryGetIMEComposition(HWND hwnd) {
  if (!hwnd)
    return std::nullopt;
  HIMC hIMC = ImmGetContext(hwnd);
  if (!hIMC)
    return std::nullopt;

  POINT pt;
  COMPOSITIONFORM cf = {};
  if (ImmGetCompositionWindow(hIMC, &cf)) {
    pt = {cf.ptCurrentPos.x, cf.ptCurrentPos.y + 20};
    if (ClientToScreen(hwnd, &pt)) {
      ImmReleaseContext(hwnd, hIMC);
      return pt;
    }
  }

  ImmReleaseContext(hwnd, hIMC);
  return std::nullopt;
}

std::optional<POINT> CursorTracker::TryGetCaretPos(HWND hwnd) {
  if (GetCurrentThreadId() != GetWindowThreadProcessId(hwnd, nullptr)) {
    return std::nullopt;
  }
  POINT pt;
  if (GetCaretPos(&pt)) {
    pt.y += 20;
    if (ClientToScreen(hwnd, &pt))
      return pt;
  }
  return std::nullopt;
}

std::optional<POINT> CursorTracker::TryGetAccessibility(HWND hwnd) {
  if (!accessibility_helper_) {
    accessibility_helper_ = std::make_unique<AccessibilityHelper>();
  }
  POINT pt;
  if (accessibility_helper_->GetCaretPosition(hwnd, pt)) {
    return pt;
  }
  return std::nullopt;
}

std::optional<POINT> CursorTracker::TryGetMousePosition() {
  POINT pt;
  if (GetCursorPos(&pt))
    return pt;
  return std::nullopt;
}

// --- Helper methods (mostly unchanged) ---

bool CursorTracker::IsPositionValid(const POINT &pt, HWND hwnd) {
  // 1. 拒绝接近原点的垃圾值
  if (pt.x <= 10 && pt.y <= 10) {
    return false;
  }

  // 2. 确保点在某个有效的显示器上
  if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == NULL) {
    return false;
  }

  // 3. 检查是否在虚拟屏幕的合理范围内
  int virtualScreenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int virtualScreenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  RECT virtualScreenRect = {virtualScreenX, virtualScreenY,
                            virtualScreenX + virtualScreenWidth,
                            virtualScreenY + virtualScreenHeight};

  // 允许一定的边界外区域
  InflateRect(&virtualScreenRect, 100, 100);

  return PtInRect(&virtualScreenRect, pt);
}

bool CursorTracker::ShouldUpdatePosition(const CursorPosition &newPos) {
  if (!cached_position_.valid)
    return true;
  if (newPos.targetWindow != cached_position_.targetWindow)
    return true;

  int dx = abs(newPos.point.x - cached_position_.point.x);
  int dy = abs(newPos.point.y - cached_position_.point.y);
  return (dx >= update_threshold_ || dy >= update_threshold_);
}

void CursorTracker::UpdateCache(const CursorPosition &pos) {
  cached_position_ = pos;
  last_target_window_ = pos.targetWindow;
}

void CursorTracker::AdjustPositionForWindow(POINT &pt, HWND hwnd) {
  if (!hwnd)
    return;

  HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(MONITORINFO)};
  if (GetMonitorInfo(hMonitor, &mi)) {
    RECT &workArea = mi.rcWork;
    const int candidateWidth = 300;
    const int candidateHeight = 150;

    if (pt.x + candidateWidth > workArea.right)
      pt.x = workArea.right - candidateWidth;
    if (pt.x < workArea.left)
      pt.x = workArea.left;
    if (pt.y + candidateHeight > workArea.bottom)
      pt.y = pt.y - candidateHeight - 25;
    if (pt.y < workArea.top)
      pt.y = workArea.top;
  }
}

bool CursorTracker::IsPointInWindow(const POINT &pt, HWND hwnd) {
  if (!hwnd)
    return false;
  RECT rect;
  if (GetWindowRect(hwnd, &rect)) {
    return PtInRect(&rect, pt);
  }
  return false;
}

ApplicationType CursorTracker::DetectApplicationType(HWND hwnd) {
  if (!hwnd)
    return ApplicationType::UNKNOWN;

  wchar_t className[256] = {0};
  wchar_t processName[256] = {0};
  GetClassName(hwnd, className, 255);

  DWORD processId;
  GetWindowThreadProcessId(hwnd, &processId);
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, processId);
  if (hProcess) {
    GetModuleBaseName(hProcess, NULL, processName, 255);
    CloseHandle(hProcess);
  }

  std::wstring classStr(className);
  std::wstring procStr(processName);
  std::transform(classStr.begin(), classStr.end(), classStr.begin(),
                 ::towlower);
  std::transform(procStr.begin(), procStr.end(), procStr.begin(), ::towlower);

  if (procStr.find(L"code.exe") != std::wstring::npos ||
      procStr.find(L"electron") != std::wstring::npos ||
      procStr.find(L"discord") != std::wstring::npos ||
      procStr.find(L"slack") != std::wstring::npos ||
      classStr.find(L"chrome_widgetwin") != std::wstring::npos) {
    return ApplicationType::ELECTRON;
  }
  if (procStr.find(L"chrome") != std::wstring::npos ||
      procStr.find(L"msedge") != std::wstring::npos ||
      procStr.find(L"firefox") != std::wstring::npos) {
    return ApplicationType::BROWSER;
  }
  if (procStr.find(L"windowsterminal") != std::wstring::npos ||
      procStr.find(L"conhost") != std::wstring::npos) {
    return ApplicationType::TERMINAL;
  }
  if (procStr.find(L"explorer") != std::wstring::npos) {
    return ApplicationType::FILE_MANAGER;
  }
  if (procStr.find(L"winword") != std::wstring::npos ||
      procStr.find(L"excel") != std::wstring::npos) {
    return ApplicationType::OFFICE;
  }

  return ApplicationType::STANDARD_WIN32;
}

void CursorTracker::SetCacheTimeoutForApp(ApplicationType appType) {
  switch (appType) {
  case ApplicationType::ELECTRON:
    cache_timeout_ms_ = 500;
    break;
  case ApplicationType::BROWSER:
    cache_timeout_ms_ = 100;
    break;
  case ApplicationType::TERMINAL:
    cache_timeout_ms_ = 80;
    break;
  default:
    cache_timeout_ms_ = 50;
    break;
  }
}

} // namespace weasel
