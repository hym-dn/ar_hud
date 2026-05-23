/**
 * @file screen_win32.h
 * @brief Win32 平台屏幕实现声明
 *
 * @par 概述
 *   使用 Win32 多显示器 API 实现屏幕信息查询和枚举。
 *   继承 IScreen 接口，提供 Windows 平台特定屏幕功能。
 *
 * @par 架构设计
 *   Win32Screen 通过以下 Win32 API 实现屏幕管理：
 *   - EnumDisplayMonitors：枚举系统中所有显示器
 *   - GetMonitorInfoExA：获取显示器几何信息和名称
 *   - GetDpiForMonitor（shcore.dll）：获取显示器 DPI
 *   - EnumDisplaySettingsA：获取显示器刷新率
 *
 * @par 显示器识别
 *   每个显示器由 HMONITOR 句柄标识，在 Win32 中唯一。
 *   通过 MonitorFromWindow 或 MonitorFromPoint 根据窗口位置或坐标找到对应显示器。
 *
 * @par DPI 获取
 *   DPI 通过动态加载 shcore.dll 的 GetDpiForMonitor 获取：
 *   - MDT_EFFECTIVE_DPI：实际使用的 DPI（考虑用户缩放设置）
 *   - MDT_ANGULAR_DPI：角 DPI（用于高密度显示）
 *   - MDT_RAW_DPI：物理 DPI（显示器固有）
 *   本实现使用 MDT_EFFECTIVE_DPI。
 *
 *   如果 shcore.dll 不可用（旧版 Windows），回退到从窗口获取 DPI。
 *
 * @par 刷新率获取
 *   使用 EnumDisplaySettingsA 获取当前显示模式中的刷新率。
 *   如果获取失败，默认使用 60.0f Hz。
 *
 * @par 屏幕方向
 *   屏幕方向通过 DEVMODE dmDisplayOrientation 字段获取：
 *   - DMDO_DEFAULT：默认（横向）
 *   - DMDO_90：旋转 90 度（纵向）
 *   - DMDO_180：旋转 180 度（倒置横向）
 *   - DMDO_270：旋转 270 度（纵向）
 *
 * @par 坐标系
 *   - 屏幕坐标：原点为虚拟显示器左上角 (0, 0)
 *   - 多显示器：可能存在负坐标（主显示器左侧或上方）
 *   - 可用区域：排除任务栏后的区域（通过 SystemParametersInfo 获取）
 *
 * @note 仅在 ARHUD_PLATFORM_WINDOWS 定义时可用
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "screen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace arhud
{

    /**
     * @class Win32Screen
     * @brief Win32 平台屏幕实现类
     *
     * @par 继承层次
     *   IScreen（抽象接口）
     *     └── Win32Screen（具体实现）
     *
     * @par 生命周期
     *   1. Create() 静态工厂方法：传入 HMONITOR，创建并初始化 Win32Screen
     *   2. 使用：查询屏幕属性
     *   3. 析构：释放资源（主要资源由 DisplayServer 统一管理）
     *
     * @par 屏幕信息
     *   ScreenInfo 包含以下静态信息：
     *   - id：显示器 ID（在 DisplayServer 中唯一）
     *   - x, y：显示器在工作区中的位置
     *   - width, height：显示器分辨率
     *   - dpi：显示器 DPI
     *   - scale：DPI 缩放因子（dpi / 96.0f）
     *   - refresh_rate：刷新率
     *   - is_primary：是否为主显示器
     *   - orientation：屏幕方向
     *
     * @par 线程安全
     *   非线程安全。屏幕信息在创建时确定，之后只读访问。
     *   如需动态响应屏幕变化，应在 DisplayServer 层处理。
     *
     * @par 内存管理
     *   使用 ARHUD_DISABLE_COPY_MOVE 禁止拷贝和移动。
     *   实例由 DisplayServer 的 LocalVector 管理。
     */
    class Win32Screen : public IScreen
    {
    public:
        /**
         * @brief 创建 Win32Screen 实例（静态工厂方法）
         *
         * @param[in] hmonitor Win32 显示器句柄（HMONITOR）
         *
         * @return Win32Screen* 新创建的屏幕对象，失败返回 nullptr
         *
         * @par 创建步骤
         *   1. 使用 new 创建 Win32Screen 实例
         *   2. 调用 InitializeFromMonitor(hmonitor) 初始化
         *   3. 如果初始化失败，删除实例并返回 nullptr
         *
         * @par 初始化内容
         *   - 从 GetMonitorInfoExA 获取显示器名称和几何信息
         *   - 从 GetDpiForMonitor 获取 DPI
         *   - 从 EnumDisplaySettingsA 获取刷新率
         *   - 确定主显示器标志
         *
         * @note 返回的指针应由调用者负责管理（通常由 DisplayServer 持有）
         */
        static Win32Screen *Create(HMONITOR hmonitor);

        /**
         * @brief 默认析构函数
         *
         * @par 行为
         *   释放 Win32Screen 实例。
         *   hmonitor_ 句柄不需要显式释放（由系统管理）。
         *
         * @note 析构时确保实例已从 DisplayServer 移除
         */
        virtual ~Win32Screen() override = default;

        ARHUD_DISABLE_COPY_MOVE(Win32Screen);

        /**
         * @brief 获取屏幕唯一标识符
         *
         * @return ScreenID 屏幕 ID（在 DisplayServer 中唯一）
         *
         * @par ID 分配
         *   ID 在 Win32Screen 创建时由 DisplayServer 分配，
         *   并通过 InitializeFromMonitor 中设置到 info_.id。
         *
         * @par 用途
         *   - 区分不同屏幕
         *   - 作为屏幕相关 API 的句柄
         *   - 窗口关联到屏幕时使用
         */
        virtual ScreenID GetId() const override;

        /**
         * @brief 获取屏幕完整信息结构
         *
         * @return const ScreenInfo& 屏幕信息结构的常量引用
         *
         * @par 信息内容
         *   返回的 ScreenInfo 包含：
         *   - 基本信息：id, x, y, width, height
         *   - DPI 信息：dpi, scale
         *   - 显示参数：refresh_rate
         *   - 屏幕属性：is_primary, orientation
         *
         * @par 用途
         *   用于批量获取屏幕所有属性。
         *   单独获取某个属性时，使用专用 getter 方法更高效。
         *
         * @par 示例
         *   @code
         *   const ScreenInfo& info = screen->GetInfo();
         *   printf("Screen %u: %dx%d @ %.1fHz\n",
         *          info.id, info.width, info.height, info.refresh_rate);
         *   @endcode
         */
        virtual const ScreenInfo &GetInfo() const override;

        /**
         * @brief 获取屏幕宽度（像素）
         *
         * @return int32_t 屏幕宽度
         *
         * @par 分辨率
         *   返回屏幕的原生分辨率（横向像素数）。
         *   不包含任何缩放或 DPI 影响。
         *
         * @par 多显示器
         *   每个显示器可以有不同的分辨率。
         */
        virtual int32_t GetWidth() const override;

        /**
         * @brief 获取屏幕高度（像素）
         *
         * @return int32_t 屏幕高度
         *
         * @par 分辨率
         *   返回屏幕的原生分辨率（纵向像素数）。
         *   不包含任何缩放或 DPI 影响。
         *
         * @par 多显示器
         *   每个显示器可以有不同的分辨率。
         */
        virtual int32_t GetHeight() const override;

        /**
         * @brief 获取屏幕 DPI
         *
         * @return float 屏幕 DPI（每英寸点数）
         *
         * @par DPI 来源
         *   从 GetDpiForMonitor(hmonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) 获取。
         *   通常返回垂直和水平相同的 DPI 值。
         *
         * @par 典型值
         *   - 标准 DPI：96（100% 缩放）
         *   - 高 DPI：144（150% 缩放）
         *   - 更高 DPI：192（200% 缩放）
         *
         * @par 用途
         *   - 计算 scale = dpi / 96.0f
         *   - UI 元素根据 DPI 缩放
         *   - 渲染参数计算
         */
        virtual float GetDpi() const override;

        /**
         * @brief 获取屏幕刷新率
         *
         * @return float 刷新率（Hz）
         *
         * @par 刷新率获取
         *   从 EnumDisplaySettingsA 获取当前显示模式的刷新率。
         *   如果获取失败，默认返回 60.0f Hz。
         *
         * @par 典型值
         *   - 60 Hz：标准刷新率
         *   - 120 Hz：高刷新率（游戏显示器）
         *   - 144 Hz：电竞显示器
         *   - 165 Hz、240 Hz 等
         *
         * @par 用途
         *   - VSync 同步目标
         *   - 帧率上限参考
         */
        virtual float GetRefreshRate() const override;

        /**
         * @brief 获取屏幕 DPI 缩放因子
         *
         * @return float DPI 缩放因子（1.0 = 100%, 1.25 = 125%, 1.5 = 150%）
         *
         * @par 计算方式
         *   scale = dpi / 96.0f
         *   其中 96.0f 是标准 DPI（100% 缩放基准）。
         *
         * @par 用途
         *   - UI 元素缩放：element_size * scale
         *   - 渲染分辨率调整：高 DPI 下可选择渲染到更高分辨率
         *   - 保持物理大小一致
         *
         * @par 示例
         *   @code
         *   // 按钮在不同 DPI 下保持物理大小一致
         *   float scale = screen->GetScale();
         *   int32_t button_width = static_cast<int32_t>(100 * scale);  // 100 CSS 像素
         *   @endcode
         */
        virtual float GetScale() const override;

        /**
         * @brief 查询是否为主显示器
         *
         * @return true 这是主显示器（桌面扩展模式下图标和任务栏所在的屏幕）
         * @return false 这是辅助显示器
         *
         * @par 主显示器
         *   主显示器是桌面扩展模式下的默认显示器。
         *   系统中只有一个主显示器。
         *   登录对话框、系统对话框通常显示在主显示器上。
         *
         * @par 检测方式
         *   从 MONITORINFOEX 结构中的 dwFlags 标志检测：
         *   - MONITORINFOF_PRIMARY：主显示器标志
         *
         * @par AR HUD 用途
         *   AR HUD 主窗口通常在主显示器上打开。
         *   但根据使用场景，也可能在辅助显示器上全屏运行。
         */
        virtual bool IsPrimary() const override;

        /**
         * @brief 获取原生显示器句柄
         *
         * @return void* 指向 HMONITOR 的指针
         *
         * @par 使用场景
         *   - 获取更多显示器信息（使用 GetMonitorInfo）
         *   - 创建与特定显示器关联的窗口
         *   - 调用 EnumDisplayDevices 枚举显示设备
         *
         * @par 示例
         *   @code
         *   HMONITOR hmonitor = *(static_cast<HMONITOR*>(screen->GetNativeHandle()));
         *   MONITORINFO mi = { sizeof(MONITORINFO) };
         *   GetMonitorInfo(hmonitor, &mi);
         *   // ... 使用 mi 结构信息
         *   @endcode
         */
        virtual void *GetNativeHandle() const override;

    private:
        /**
         * @brief 私有构造函数
         *
         * @par 访问控制
         *   构造函数为 private，只能通过 Create() 工厂方法创建实例。
         *   保证始终通过正确的方式初始化。
         */
        Win32Screen() = default;

        /**
         * @brief 从 HMONITOR 初始化屏幕信息
         *
         * @param[in] hmonitor Win32 显示器句柄
         *
         * @par 初始化步骤
         *   1. 保存 hmonitor_ 句柄
         *   2. 调用 GetMonitorInfoExA 获取基本几何信息
         *   3. 调用 QueryDpiForMonitor 获取 DPI
         *   4. 调用 QueryRefreshRateForMonitor 获取刷新率
         *   5. 设置 is_primary 标志
         *   6. 计算 scale = dpi / 96.0f
         *
         * @par 几何信息
         *   - rcMonitor：显示器在工作区的矩形区域
         * - rcWork：排除任务栏等的可用区域
         *   本实现使用 rcMonitor。
         *
         * @par 屏幕方向
         *   从 DEVMODE dmDisplayOrientation 获取。
         *   注意：方向信息需要单独调用 EnumDisplaySettings 获取。
         */
        void InitializeFromMonitor(HMONITOR hmonitor);

        /**
         * @brief 查询指定显示器的 DPI
         *
         * @param[in] hmonitor Win32 显示器句柄
         *
         * @return float 显示器 DPI
         *
         * @par 获取方式
         *   动态加载 shcore.dll，调用 GetDpiForMonitor：
         *   @code
         *   typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(
         *       HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
         *   // ...
         *   UINT dpiX, dpiY;
         *   hr = g_pfnGetDpiForMonitor(hmonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
         *   @endcode
         *
         * @par DPI 类型
         *   - MDT_EFFECTIVE_DPI：实际生效的 DPI（考虑用户缩放）
         *   - MDT_ANGULAR_DPI：角 DPI（用于精确角度计算）
         *   - MDT_RAW_DPI：物理 DPI（不考虑软件缩放）
         *
         * @par 回退机制
         *   如果 shcore.dll 不可用或 GetDpiForMonitor 失败：
         *   - 尝试使用 GetDeviceCaps(GetDC(nullptr), LOGPIXELS) 获取系统 DPI
         *   - 如果仍失败，默认返回 96.0f
         */
        static float QueryDpiForMonitor(HMONITOR hmonitor);

        /**
         * @brief 查询指定显示器的刷新率
         *
         * @param[in] hmonitor Win32 显示器句柄
         *
         * @return float 刷新率（Hz）
         *
         * @par 获取方式
         *   使用 EnumDisplaySettingsA 获取当前显示模式：
         *   @code
         *   DEVMODE dm = { sizeof(DEVMODE) };
         *   EnumDisplaySettings(szDeviceName, ENUM_CURRENT_SETTINGS, &dm);
         *   refresh_rate = dm.dmDisplayFrequency;
         *   @endcode
         *
         * @par 设备名称
         *   设备名称从 MONITORINFOEX 的 szDevice 字段获取。
         *
         * @par 失败处理
         *   如果获取失败，默认返回 60.0f Hz。
         */
        static float QueryRefreshRateForMonitor(HMONITOR hmonitor);

        /** @brief 屏幕完整信息（由 InitializeFromMonitor 填充） */
        ScreenInfo info_;

        /** @brief Win32 显示器句柄 */
        HMONITOR hmonitor_ = nullptr;
    };

}

#endif
