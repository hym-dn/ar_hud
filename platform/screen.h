/**
 * @file screen.h
 * @brief 平台无关屏幕抽象接口
 *
 * 提供多显示器支持，包括屏幕几何信息、DPI、刷新率和缩放比。
 * 参考 Godot 4.6 DisplayServer 屏幕管理设计，为 AR HUD 多屏场景定制。
 *
 * @par 架构定位：
 *   IScreen 是平台抽象层的最底层接口之一，
 *   每个物理显示器对应一个 IScreen 实例。
 *   由 IDisplayServer 创建和管理，应用层通过 IDisplayServer 获取。
 *
 * @par 设计要点：
 *   - 每个物理显示器对应一个 IScreen 实例
 *   - ScreenInfo 存储静态属性（位置、尺寸、DPI、刷新率）
 *   - 平台实现通过 GetNativeHandle() 暴露底层句柄（如 HMONITOR）
 *   - ScreenID 全局唯一，由 DisplayServer 分配
 *
 * @par 多屏 AR HUD 场景：
 *   AR HUD 常需要多显示器支持，例如：
 *   - 仪表盘显示器（主屏，1280x720 @ 60Hz）
 *   - 抬头显示器（副屏，1920x1080 @ 120Hz）
 *   - 乘客娱乐屏（副屏，1920x1080 @ 60Hz）
 *
 * @par 使用示例：
 *   @code{.cpp}
 *   auto* ds = IDisplayServer::GetSingleton();
 *   uint32_t screen_count = ds->GetScreenCount();
 *   for (uint32_t i = 0; i < screen_count; ++i) {
 *       auto* screen = ds->GetScreen(i);
 *       printf("Screen %u: %ux%u @ %.1fHz, DPI=%.0f\n",
 *              screen->GetId(),
 *              screen->GetWidth(),
 *              screen->GetHeight(),
 *              screen->GetRefreshRate(),
 *              screen->GetDpi());
 *   }
 *
 *   // 创建窗口到副屏
 *   IScreen::ScreenID hud_screen = ds->GetPrimaryScreen(); //
 * 或者遍历找特定屏幕 WindowDesc desc; desc.screen_id = hud_screen; desc.width =
 * 1920; desc.height = 1080;
 *   @endcode
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"

namespace arhud
{
    /**
     * @brief 屏幕方向枚举
     *
     * 描述屏幕的自然方向（物理放置方式）。
     * 用于辅助应用正确布局内容（如手机平板横竖屏切换）。
     *
     * @note 大多数桌面显示器固定为横向（Landscape），
     *       移动设备和部分嵌入式屏幕支持旋转。
     * @see ScreenInfo::orientation
     */
    enum class ScreenOrientation
    {
        /**
         * @brief 横屏（默认）
         *
         * 屏幕宽度大于高度，宽度沿水平方向。
         * 标准桌面显示器和电视的默认方向。
         */
        kLandscape,

        /**
         * @brief 竖屏
         *
         * 屏幕高度大于宽度，高度沿水平方向。
         * 智能手机、某些嵌入式显示器、平板电脑可旋转时会出现。
         */
        kPortrait,

        /**
         * @brief 反向横屏
         *
         * 横向但上下颠倒（旋转 180 度）。
         * 某些安装方向特殊的嵌入式显示器。
         */
        kLandscapeReversed,

        /**
         * @brief 反向竖屏
         *
         * 竖向但左右颠倒（旋转 180 度）。
         */
        kPortraitReversed
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // ScreenInfo
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 屏幕静态信息
     *
     * 存储屏幕的静态属性，在屏幕连接后确定，不频繁变化。
     * 用于窗口创建时的屏幕选择和渲染参数计算。
     *
     * @par DPI 与缩放：
     *   - dpi = 物理 DPI（由系统或显示器 EDID 报告）
     *   - scale = dpi / 96.0f（相对于标准 96 DPI 的缩放比）
     *   - AR HUD 渲染前需要用 scale 缩放 UI 元素
     *
     * @par 坐标系统：
     *   屏幕坐标系以主屏左上角为原点 (0, 0)。
     *   多屏情况下，副屏坐标可能为负数（位于主屏左侧或上方）。
     *
     * @see IScreen
     */
    struct ScreenInfo
    {
        /**
         * @brief 屏幕唯一标识符
         *
         * 由 DisplayServer 分配，全局唯一。
         */
        uint32_t id = 0;

        /**
         * @brief 屏幕左上角 X 坐标（像素）
         *
         * 相对于虚拟桌面的 X 坐标。
         * 主屏通常为 0，副屏可能为负值（左侧）或大于主屏宽度（右侧）。
         */
        int32_t x = 0;

        /**
         * @brief 屏幕左上角 Y 坐标（像素）
         *
         * 相对于虚拟桌面的 Y 坐标。
         * 主屏通常为 0，副屏可能为负值（上方）。
         */
        int32_t y = 0;

        /**
         * @brief 屏幕宽度（像素）
         *
         * 屏幕的水平分辨率。
         * 通常与显示模式（mode）中的分辨率一致。
         */
        int32_t width = 0;

        /**
         * @brief 屏幕高度（像素）
         *
         * 屏幕的垂直分辨率。
         */
        int32_t height = 0;

        /**
         * @brief 屏幕 DPI（每英寸点数）
         *
         * 物理屏幕的真实 DPI，由系统或显示器 EDID 报告。
         * 标准桌面显示器通常为 96 DPI（1:1 缩放）。
         * 高 DPI 显示器（Apple Retina、Windows HiDPI）可能为 144、192、240 等。
         *
         * @par AR HUD 重要性：
         *   AR HUD 需要精确 DPI 信息来计算：
         *   - 抬头显示器中符号的角大小（arc minutes）
         *   - 远近场切换时的 UI 缩放
         */
        float dpi = 96.0f;

        /**
         * @brief 屏幕刷新率（Hz）
         *
         * 显示器每秒刷新的次数。
         * 常见值：60Hz（标准）、120Hz（高刷游戏屏）、144Hz（电竞屏）。
         * AR HUD 场景可能需要 60Hz 以上以减少动态模糊。
         */
        float refresh_rate = 60.0f;

        /**
         * @brief DPI 缩放因子
         *
         * 相对于标准 96 DPI 的缩放比。
         * 计算公式：scale = dpi / 96.0f
         *
         * @par 使用示例：
         *   @code{.cpp}
         *   float scale = screen->GetScale();
         *   // UI 元素需要按 scale 缩放以保持物理尺寸一致
         *   float ui_element_size_px = ui_element_size_inch * screen->GetDpi();
         *   @endcode
         */
        float scale = 1.0f;

        /**
         * @brief 是否为主屏
         *
         * true 表示此屏幕是系统主屏（通常包含任务栏）。
         * 窗口默认在主屏创建，除非明确指定其他屏幕。
         *
         * @note 只有一个屏幕的主屏标志为 true
         */
        bool is_primary = false;

        /**
         * @brief 屏幕方向
         *
         * 屏幕的自然放置方向。
         * @see ScreenOrientation
         */
        ScreenOrientation orientation = ScreenOrientation::kLandscape;
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // IScreen
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief 平台无关屏幕抽象接口
     *
     * 定义屏幕属性查询的统一接口。
     * 平台特定实现（Win32Screen / X11Screen 等）继承此接口。
     *
     * @par 关键职责：
     *   - 屏幕几何信息查询（位置、尺寸）
     *   - DPI 和缩放因子查询
     *   - 刷新率查询
     *   - 原生句柄暴露（供 DisplayServer 底层操作）
     *
     * @par 线程安全：
     *   非线程安全。应在主线程查询屏幕信息。
     *
     * @par 与 DisplayServer 的关系：
     *   IScreen 实例由 IDisplayServer 创建和管理。
     *   应用层不直接创建 IScreen，而是通过 IDisplayServer 获取。
     *
     * @par 实现注意事项：
     *   - Win32: 使用 EnumDisplayMonitors + GetMonitorInfoEx
     *   - X11:   使用 RandR 扩展（未来支持）
     *   - Android: 使用 Display 类（未来支持）
     *
     * @see IDisplayServer  屏幕管理器
     * @see ScreenInfo      屏幕静态信息
     */
    class IScreen
    {
    public:
        /** @brief 屏幕 ID 类型 */
        using ScreenID = uint32_t;

        /** @brief 无效屏幕 ID 常量 */
        static constexpr ScreenID kInvalidScreenId = UINT32_MAX;

        /** @brief 虚析构，确保子类正确清理 */
        virtual ~IScreen() = default;

        /**
         * @brief 获取屏幕 ID
         *
         * @return 屏幕唯一标识符
         */
        virtual ScreenID GetId() const = 0;

        /**
         * @brief 获取屏幕完整信息
         *
         * 返回包含屏幕所有静态属性的结构体引用。
         *
         * @return ScreenInfo 常引用
         *
         * @par 使用示例：
         *   @code{.cpp}
         *   const auto& info = screen->GetInfo();
         *   printf("Screen %u: %dx%d at (%d,%d)\n",
         *          info.id, info.width, info.height, info.x, info.y);
         *   @endcode
         */
        virtual const ScreenInfo &GetInfo() const = 0;

        /**
         * @brief 获取屏幕宽度
         *
         * @return 屏幕宽度（像素）
         *
         * @see GetInfo().width
         */
        virtual int32_t GetWidth() const = 0;

        /**
         * @brief 获取屏幕高度
         *
         * @return 屏幕高度（像素）
         *
         * @see GetInfo().height
         */
        virtual int32_t GetHeight() const = 0;

        /**
         * @brief 获取屏幕 DPI
         *
         * @return 每英寸点数
         *
         * @par AR HUD 应用：
         *   DPI 用于计算 HUD 符号的物理视角大小。
         *   不同驾驶位置（远场/近场）需要不同缩放。
         *
         * @see GetInfo().dpi
         */
        virtual float GetDpi() const = 0;

        /**
         * @brief 获取屏幕刷新率
         *
         * @return 刷新率（Hz）
         *
         * @see GetInfo().refresh_rate
         */
        virtual float GetRefreshRate() const = 0;

        /**
         * @brief 获取 DPI 缩放因子
         *
         * @return 相对于 96 DPI 的缩放比
         *
         * @par 使用场景：
         *   Windows 允许用户设置缩放（125%、150%等），
         *   此值反映系统设置的缩放级别。
         *
         * @see GetInfo().scale
         */
        virtual float GetScale() const = 0;

        /**
         * @brief 查询是否为主屏
         *
         * @return true 表示主屏
         *
         * @see GetInfo().is_primary
         */
        virtual bool IsPrimary() const = 0;

        /**
         * @brief 获取原生屏幕句柄
         *
         * 返回平台特定的原生屏幕句柄。
         * 用于平台底层操作或与图形 API 交互。
         *
         * @return 原生句柄指针
         * @retval Win32: HMONITOR
         * @retval X11:   RROutput（未来支持）
         *
         * @note 大多数应用不需要调用此方法
         */
        virtual void *GetNativeHandle() const = 0;
    };

} // namespace arhud