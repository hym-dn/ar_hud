/**
 * @file screen_win32.cpp
 * @brief Win32 平台屏幕实现
 *
 * @par 概述
 *   Win32Screen 的实现文件，对应声明见 screen_win32.h。
 *   基于 Win32 多显示器 API 完成屏幕几何信息、DPI、缩放因子、
 *   刷新率与主屏标志的一次性查询与填充。
 *
 * @par 依赖 API
 *   - GetMonitorInfoA：查询显示器几何信息、设备名与主屏标志
 *   - GetDpiForMonitor（shcore.dll，动态加载）：查询显示器 DPI
 *   - EnumDisplaySettingsA：查询当前显示模式，读取刷新率
 *   - GetDeviceCaps / GetDC：DPI 查询失败时的回退途径
 *
 * @par 生命周期
 *   通过静态工厂 Create() 创建，InitializeFromMonitor() 在创建时
 *   完成一次性信息填充，之后所有 getter 均为只读访问。
 *
 * @note 仅在 ARHUD_PLATFORM_WINDOWS 定义时编译。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#include "screen_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "io/logger.h"
#include "os/memory.h"

namespace arhud {

/**
 * @brief 屏幕 ID 自增计数器
 *
 * @par 实现细节
 *   每个 Win32Screen 创建时通过 s_next_screen_id++ 分配唯一 ID，
 *   起始值为 1。为文件内静态变量，在进程生命周期内单调递增。
 *
 * @note 非线程安全，屏幕枚举应在主线程进行。
 */
static uint32 s_next_screen_id = 1;

// ─── 工厂与初始化 ───────────────────────────────────────────────────

/**
 * @brief 创建并初始化 Win32Screen 实例（静态工厂方法）
 *
 * @param[in] hmonitor Win32 显示器句柄（HMONITOR）
 *
 * @return Win32Screen* 创建成功返回实例指针；hmonitor 无效返回 nullptr
 *
 * @par 实现步骤
 *   1. 校验 hmonitor，为空则直接返回 nullptr
 *   2. ARHUD_NEW 分配 Win32Screen 对象
 *   3. 调用 InitializeFromMonitor(hmonitor) 填充屏幕信息
 *   4. 返回实例指针
 *
 * @note 返回指针由调用者（通常为 DisplayServer）负责释放。
 * @see InitializeFromMonitor
 */
Win32Screen* Win32Screen::Create(HMONITOR hmonitor) {
  if (!hmonitor) {
    return nullptr;
  }
  Win32Screen* screen = ARHUD_NEW(Win32Screen)();
  screen->InitializeFromMonitor(hmonitor);
  return screen;
}

/**
 * @brief 从 HMONITOR 初始化屏幕信息
 *
 * @param[in] hmonitor Win32 显示器句柄
 *
 * @par 实现步骤
 *   1. 保存 hmonitor_ 句柄
 *   2. GetMonitorInfoA 填充 MONITORINFOEXA，提取几何信息与主屏标志
 *   3. QueryDpiForMonitor 获取 DPI，并计算 scale = dpi / kBaseDpi
 *   4. QueryRefreshRateForMonitor 获取刷新率
 *   5. 通过全局计数器分配唯一 id
 *
 * @par 几何信息
 *   使用 rcMonitor（显示器完整区域）而非 rcWork（排除任务栏的可用区），
 *   因此 width/height 为显示器原生分辨率，x/y 为虚拟桌面中的位置。
 *
 * @par 失败处理
 *   GetMonitorInfoA 失败时记录告警，几何字段保持默认值，
 *   后续 DPI 与刷新率查询仍会继续尝试执行。
 */
void Win32Screen::InitializeFromMonitor(HMONITOR hmonitor) {
  hmonitor_ = hmonitor;

  MONITORINFOEXA mi;
  mi.cbSize = sizeof(MONITORINFOEXA);
  if (GetMonitorInfoA(hmonitor, &mi)) {
    info_.x = mi.rcMonitor.left;
    info_.y = mi.rcMonitor.top;
    info_.width = mi.rcMonitor.right - mi.rcMonitor.left;
    info_.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info_.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  } else {
    ARHUD_LOG_WARN("Win32Screen: GetMonitorInfoA failed");
  }

  info_.dpi = QueryDpiForMonitor(hmonitor);
  info_.scale = info_.dpi / kBaseDpi;
  info_.refresh_rate = QueryRefreshRateForMonitor(hmonitor);
  info_.id = s_next_screen_id++;
}

// ─── 静态工具方法 ───────────────────────────────────────────────────

/**
 * @brief 查询指定显示器的 DPI（静态工具方法）
 *
 * @param[in] hmonitor Win32 显示器句柄
 *
 * @return float 显示器 DPI（每英寸点数）；所有途径均失败时返回 kBaseDpi
 *
 * @par 实现步骤
 *   1. 动态加载 shcore.dll
 *   2. GetProcAddress 取 GetDpiForMonitor 函数指针
 *   3. 调用 GetDpiForMonitor(hmonitor, 0（MDT_EFFECTIVE_DPI），&dpi_x, &dpi_y)
 *   4. 成功且 dpi_y > 0 则返回 dpi_y
 *   5. 失败则回退到 GetDeviceCaps(GetDC(nullptr), LOGPIXELSY)
 *   6. 仍失败返回 kBaseDpi
 *
 * @par DPI 类型
 *   参数 0 对应 MDT_EFFECTIVE_DPI，即考虑用户缩放设置后实际生效的 DPI。
 *
 * @par 资源管理
 *   shcore.dll 的各条分支均通过 FreeLibrary 释放，避免句柄泄漏；
 *   HDC 通过 ReleaseDC 释放。
 *
 * @note 采用动态加载以兼容不提供 shcore.dll 的旧版 Windows。
 */
float Win32Screen::QueryDpiForMonitor(HMONITOR hmonitor) {
  HMODULE shcore = LoadLibraryA("shcore.dll");
  if (shcore) {
    using GetDpiForMonitorFunc = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    auto func = reinterpret_cast<GetDpiForMonitorFunc>(
        GetProcAddress(shcore, "GetDpiForMonitor"));
    if (func) {
      UINT dpi_x = 0;
      UINT dpi_y = 0;
      HRESULT hr = func(hmonitor, 0, &dpi_x, &dpi_y);
      FreeLibrary(shcore);
      if (SUCCEEDED(hr) && dpi_y > 0) {
        return static_cast<float>(dpi_y);
      }
    } else {
      FreeLibrary(shcore);
    }
  }

  HDC hdc = GetDC(nullptr);
  if (hdc) {
    float dpi = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSY));
    ReleaseDC(nullptr, hdc);
    return dpi;
  }

  return kBaseDpi;
}

/**
 * @brief 查询指定显示器的刷新率（静态工具方法）
 *
 * @param[in] hmonitor Win32 显示器句柄
 *
 * @return float 刷新率（Hz）；获取失败返回默认值 60.0f
 *
 * @par 实现步骤
 *   1. GetMonitorInfoA 获取 MONITORINFOEXA，取出 szDevice 设备名
 *   2. EnumDisplaySettingsA(szDevice, ENUM_CURRENT_SETTINGS) 读取当前 DEVMODE
 *   3. 若 DEVMODE 标记 DM_DISPLAYFREQUENCY，返回 dmDisplayFrequency
 *   4. 任一步骤失败返回默认 60.0f
 *
 * @note dmDisplayFrequency 字段单位为 Hz。
 */
float Win32Screen::QueryRefreshRateForMonitor(HMONITOR hmonitor) {
  MONITORINFOEXA mi;
  mi.cbSize = sizeof(MONITORINFOEXA);
  if (!GetMonitorInfoA(hmonitor, &mi)) {
    return 60.0f;
  }

  DEVMODEA dm;
  ZeroMemory(&dm, sizeof(dm));
  dm.dmSize = sizeof(DEVMODEA);

  if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
    if (dm.dmFields & DM_DISPLAYFREQUENCY) {
      return static_cast<float>(dm.dmDisplayFrequency);
    }
  }

  return 60.0f;
}

// ─── 属性访问 ───────────────────────────────────────────────────────

/**
 * @brief 获取屏幕唯一标识符
 *
 * @return ScreenID 屏幕 ID，来源于 info_.id
 *
 * @par 实现细节
 *   ID 在 InitializeFromMonitor 中由全局计数器 s_next_screen_id 分配。
 */
IScreen::ScreenID Win32Screen::GetId() const {
  return info_.id;
}

/**
 * @brief 获取屏幕完整信息结构
 *
 * @return const ScreenInfo& 内部 info_ 的常量引用
 *
 * @par 实现细节
 *   直接返回成员引用，无拷贝开销；调用方不可修改。
 */
const ScreenInfo& Win32Screen::GetInfo() const {
  return info_;
}

/**
 * @brief 获取屏幕宽度（像素）
 *
 * @return int32 宽度，来源于 info_.width
 *
 * @par 实现细节
 *   由 GetMonitorInfoA 的 rcMonitor 计算得出，为显示器原生横向像素数。
 */
int32 Win32Screen::GetWidth() const {
  return info_.width;
}

/**
 * @brief 获取屏幕高度（像素）
 *
 * @return int32 高度，来源于 info_.height
 *
 * @par 实现细节
 *   由 GetMonitorInfoA 的 rcMonitor 计算得出，为显示器原生纵向像素数。
 */
int32 Win32Screen::GetHeight() const {
  return info_.height;
}

/**
 * @brief 获取屏幕 DPI
 *
 * @return float DPI，来源于 info_.dpi
 *
 * @par 实现细节
 *   在 InitializeFromMonitor 中通过 QueryDpiForMonitor 获取。
 */
float Win32Screen::GetDpi() const {
  return info_.dpi;
}

/**
 * @brief 获取屏幕刷新率
 *
 * @return float 刷新率（Hz），来源于 info_.refresh_rate
 *
 * @par 实现细节
 *   在 InitializeFromMonitor 中通过 QueryRefreshRateForMonitor 获取，
 *   获取失败时为默认值 60.0f。
 */
float Win32Screen::GetRefreshRate() const {
  return info_.refresh_rate;
}

/**
 * @brief 获取屏幕 DPI 缩放因子
 *
 * @return float 缩放因子，来源于 info_.scale
 *
 * @par 实现细节
 *   在 InitializeFromMonitor 中计算：scale = dpi / kBaseDpi，
 *   1.0 表示 100% 缩放，1.5 表示 150% 缩放。
 */
float Win32Screen::GetScale() const {
  return info_.scale;
}

/**
 * @brief 查询是否为主显示器
 *
 * @return true 主显示器
 * @return false 辅助显示器
 *
 * @par 实现细节
 *   来源于 info_.is_primary，由 GetMonitorInfoA 返回的 dwFlags 中
 *   MONITORINFOF_PRIMARY 标志判定。
 */
bool Win32Screen::IsPrimary() const {
  return info_.is_primary;
}

/**
 * @brief 获取原生显示器句柄
 *
 * @return void* 指向 Win32 HMONITOR 句柄
 *
 * @par 实现细节
 *   返回创建时保存的 hmonitor_，供上层调用其他 Win32 显示器 API 使用。
 *
 * @note 句柄由系统管理，调用方无需释放。
 */
void* Win32Screen::GetNativeHandle() const {
  return hmonitor_;
}

}  // namespace arhud

#endif