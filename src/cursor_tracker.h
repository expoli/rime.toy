#pragma once
#ifndef CURSOR_TRACKER_H
#define CURSOR_TRACKER_H

#include <chrono>
#include <memory>
#include <optional>
#include <utils.h>
#include <vector>
#include <windows.h>

// 前向声明
namespace weasel {
class AccessibilityHelper;
}

namespace weasel {

enum class CursorDetectionMethod {
  GUI_THREAD_INFO, // GetGUIThreadInfo - 最通用的方法
  IME_COMPOSITION, // ImmGetCompositionWindow - IME 专用
  ACCESSIBILITY,   // 无障碍接口 - 兼容性好
  CARET_POS,       // GetCaretPos - 同线程专用
  MOUSE_FALLBACK   // 鼠标位置回退 - 最后手段
};

enum class ApplicationType {
  UNKNOWN,
  TERMINAL,      // 终端应用 (Windows Terminal, CMD, PowerShell)
  BROWSER,       // 浏览器应用 (Chrome, Edge, Firefox)
  ELECTRON,      // Electron 应用 (VSCode, Discord, Slack, Teams)
  FILE_MANAGER,  // 文件管理器 (Explorer)
  OFFICE,        // Office 应用 (Word, Excel, PowerPoint)
  STANDARD_WIN32 // 标准 Win32 应用
};

struct CursorPosition {
  POINT point;                                     // 光标位置
  bool valid;                                      // 位置是否有效
  CursorDetectionMethod method;                    // 检测方法
  HWND targetWindow;                               // 目标窗口
  std::chrono::steady_clock::time_point timestamp; // 时间戳

  CursorPosition()
      : point{0, 0}, valid(false),
        method(CursorDetectionMethod::MOUSE_FALLBACK), targetWindow(nullptr),
        timestamp(std::chrono::steady_clock::now()) {}

  CursorPosition(const POINT &pt, CursorDetectionMethod m, HWND hwnd)
      : point(pt), valid(true), method(m), targetWindow(hwnd),
        timestamp(std::chrono::steady_clock::now()) {}
};

// 用于启发式评分的候选位置结构
struct PositionCandidate {
  POINT point;
  CursorDetectionMethod method;
  int score;
};

class CursorTracker {
public:
  CursorTracker();
  ~CursorTracker();

  // 主要接口
  CursorPosition GetCursorPosition(HWND targetWindow);

  // 配置接口
  bool IsEnabled() const { return enabled_; }
  void SetEnabled(bool enabled) { enabled_ = enabled; }
  void SetUpdateThreshold(int pixels) { update_threshold_ = pixels; }
  void SetCacheTimeout(int milliseconds) { cache_timeout_ms_ = milliseconds; }
  void SetCacheTimeoutForApp(ApplicationType appType);

  // 调试接口
  const CursorPosition &GetLastPosition() const { return cached_position_; }
  void InvalidateCache() { cached_position_.valid = false; }

private:
  // 各种检测方法实现 (返回 optional 以收集所有证据)
  std::optional<POINT> TryGetGUIThreadInfo(HWND hwnd);
  std::optional<POINT> TryGetIMEComposition(HWND hwnd);
  std::optional<POINT> TryGetAccessibility(HWND hwnd);
  std::optional<POINT> TryGetCaretPos(HWND hwnd);
  std::optional<POINT> TryGetMousePosition();

  // 启发式引擎核心方法
  PositionCandidate
  FindBestCandidate(std::vector<PositionCandidate> &candidates, HWND hwnd,
                    ApplicationType appType);
  int ScoreCandidate(PositionCandidate &candidate, HWND hwnd,
                     ApplicationType appType);

  // 辅助方法
  bool IsPositionValid(const POINT &pt, HWND hwnd);
  bool ShouldUpdatePosition(const CursorPosition &newPos);
  void UpdateCache(const CursorPosition &pos);

  // 位置调整方法
  void AdjustPositionForWindow(POINT &pt, HWND hwnd);
  bool IsPointInWindow(const POINT &pt, HWND hwnd);

  // 应用类型检测
  ApplicationType DetectApplicationType(HWND hwnd);

private:
  // 配置参数
  bool enabled_;         // 是否启用光标跟踪
  int update_threshold_; // 位置变化阈值(像素)
  int cache_timeout_ms_; // 缓存超时时间(毫秒)

  // 状态数据
  CursorPosition cached_position_; // 缓存的光标位置
  HWND last_target_window_;        // 上次的目标窗口

  // 位置稳定性相关
  POINT last_valid_position_;     // 上次有效位置
  int consecutive_invalid_count_; // 连续无效位置计数

  // 无障碍接口助手
  std::unique_ptr<AccessibilityHelper> accessibility_helper_;

  // 性能统计
  mutable int call_count_;      // 调用次数
  mutable int cache_hit_count_; // 缓存命中次数
};

} // namespace weasel

#endif // CURSOR_TRACKER_H