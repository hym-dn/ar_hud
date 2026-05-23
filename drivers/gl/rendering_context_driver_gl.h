/**
 * @file rendering_context_driver_gl.h
 * @brief OpenGL 渲染上下文驱动实现
 *
 * GLRenderingContextDriver 是 IRenderingContextDriver 的 OpenGL 实现，
 * 通过 IGLManager 管理 GL 上下文，使用 GL 字符串查询枚举 GPU 设备。
 *
 * @par 架构定位
 *   @code
 *   IRenderingContextDriver (接口)
 *       └── GLRenderingContextDriver (OpenGL 实现)
 *               ├── 持有 IGLManager* 引用（不拥有所有权）
 *               ├── 管理 SurfaceInfo[] 数组
 *               ├── 持有 Device[] 数组（GL 设备信息）
 *               └── 创建 GLDeviceDriver 实例（未来实现）
 *   @endcode
 *
 * @par 与 Vulkan RCD 的对比
 *   | 维度         | OpenGL RCD                    | Vulkan RCD                    |
 *   |-------------|-------------------------------|-------------------------------|
 *   | API 实例化   | 无需（GL 无 Instance 概念）     | VkInstance 创建               |
 *   | 设备枚举     | glGetString 查询当前设备       | vkEnumeratePhysicalDevices    |
 *   | 设备数量     | 始终 1（当前上下文所在设备）     | 可多个                        |
 *   | Surface     | IGLManager 屏幕上下文映射      | VkSurfaceKHR                  |
 *   | 驱动创建     | GLDeviceDriver（直接 GL 调用）  | VkDevice + 队列选择            |
 *
 * @par 厂商识别策略
 *   OpenGL 通过 glGetString(GL_VENDOR) 返回字符串匹配厂商 ID：
 *   - "NVIDIA Corporation" → Vendor::kNVIDIA (0x10DE)
 *   - "Intel"              → Vendor::kIntel (0x8086)
 *   - "AMD" / "ATI"        → Vendor::kAMD (0x1002)
 *   - "ARM"                → Vendor::kARM (0x13B5)
 *   - "Qualcomm"           → Vendor::kQualcomm (0x5143)
 *
 * @par 设备类型推断
 *   - NVIDIA → kDiscreteGpu（NVIDIA 桌面/移动 GPU 均为独立显卡）
 *   - Intel  → kIntegratedGpu（Intel UHD/Iris 为集成显卡）
 *   - AMD    → 需要检查名称区分集成/独立
 *   - ARM/Qualcomm → kIntegratedGpu（移动端 SoC GPU）
 *
 * @par 设计参考
 *   - Godot 4.6 RenderingContextDriverVulkan（设备枚举模式）
 *   - Godot 4.6 GLManagerNative_Windows（GL 上下文管理）
 *   - arch_skill.md §1.1 三层 GPU API 抽象
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-08
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "rendering_context_driver.h"

namespace arhud
{

    /**
     * @brief OpenGL 渲染上下文驱动
     *
     * 通过 IGLManager 管理 GL 上下文，使用 GL 字符串查询枚举 GPU 设备。
     * Surface 操作直接映射到 IGLManager 的屏幕上下文操作。
     *
     * @par 关键设计
     *   - 不拥有 IGLManager（由外部注入，通常是 Engine 或 DisplayServer）
     *   - Surface 数组使用 LocalVector，索引即为 SurfaceID
     *   - 设备信息在 Initialize() 时一次性查询并缓存
     *   - 驱动变通方案在设备枚举时根据厂商设置
     *
     * @par 线程安全
     *   - Initialize / Shutdown / SurfaceCreate / SurfaceDestroy 应在主线程调用
     *   - SurfaceSetSize / SurfaceSetVsyncMode 可在任意线程调用
     *   - ContextCreate / ContextDestroy 应在主线程调用
     *
     * @see IRenderingContextDriver  基类接口
     */
    class GLRenderingContextDriver : public IRenderingContextDriver
    {
    public:
        /**
         * @brief 构造函数
         *
         * @param[in] p_gl_manager GL 管理器指针（不拥有所有权）
         *
         * @pre p_gl_manager 非 nullptr
         * @pre p_gl_manager->Initialize() 已成功调用
         */
        explicit GLRenderingContextDriver(IGLManager *p_gl_manager);

        /**
         * @brief 析构函数
         *
         * 自动调用 Shutdown() 释放资源。
         * 不销毁 IGLManager（由外部管理）。
         */
        ~GLRenderingContextDriver() override;

        // 禁止拷贝和移动
        GLRenderingContextDriver(const GLRenderingContextDriver &) = delete;
        GLRenderingContextDriver &operator=(const GLRenderingContextDriver &) = delete;
        GLRenderingContextDriver(GLRenderingContextDriver &&) = delete;
        GLRenderingContextDriver &operator=(GLRenderingContextDriver &&) = delete;

        // ─── IRenderingContextDriver 接口实现 ─────────────────────────

        Error Initialize(IScreen::ScreenID p_screen_id,
                         void *p_native_window,
                         uint32_t p_width, uint32_t p_height) override;
        void Shutdown() override;

        uint32_t GetDeviceCount() const override;
        const Device &GetDevice(uint32_t p_device_index) const override;
        bool DeviceSupportsPresent(uint32_t p_device_index, SurfaceID p_surface) const override;

        IRenderingDeviceDriver *CreateDeviceDriver() override;
        void DriverFree(IRenderingDeviceDriver *p_driver) override;

        SurfaceID SurfaceCreate(IScreen::ScreenID p_screen_id,
                                void *p_native_window,
                                uint32_t p_width, uint32_t p_height) override;
        void SurfaceDestroy(SurfaceID p_surface) override;
        void SurfaceSetSize(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) override;
        void SurfaceSetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) override;
        VSyncMode SurfaceGetVsyncMode(SurfaceID p_surface) const override;
        uint32_t SurfaceGetWidth(SurfaceID p_surface) const override;
        uint32_t SurfaceGetHeight(SurfaceID p_surface) const override;
        void SurfaceSetNeedsResize(SurfaceID p_surface, bool p_needs_resize) override;
        bool SurfaceGetNeedsResize(SurfaceID p_surface) const override;
        IGLManager::SurfaceID SurfaceGetAPISurface(SurfaceID p_surface) const override;

        ContextID ContextCreate(SurfaceID p_surface) override;
        void ContextDestroy(ContextID p_context_id) override;
        uint32_t GetContextCount() const override;

        bool IsInitialized() const override;

    private:
        // ─── 内部方法 ────────────────────────────────────────────────

        /**
         * @brief 查询 GPU 设备信息
         *
         * 通过 GL 字符串查询填充 Device 结构。
         * 必须在 GL 上下文已绑定的线程上调用。
         */
        void QueryDeviceInfo();

        /**
         * @brief 从 GL 厂商字符串推断 PCI 厂商 ID
         *
         * @param[in] p_vendor_string glGetString(GL_VENDOR) 返回的字符串
         * @return PCI 厂商 ID
         */
        static uint32_t DetermineVendorId(const char *p_vendor_string);

        /**
         * @brief 从厂商 ID 推断设备类型
         *
         * @param[in] p_vendor_id PCI 厂商 ID
         * @return 推断的设备类型
         */
        static DeviceType DetermineDeviceType(uint32_t p_vendor_id);

        /**
         * @brief 检查驱动变通方案
         *
         * 根据厂商和驱动版本设置 Workarounds。
         *
         * @param[in] p_vendor_id PCI 厂商 ID
         * @param[in] p_renderer_string glGetString(GL_RENDERER) 返回的字符串
         * @param[in] p_version_string glGetString(GL_VERSION) 返回的字符串
         * @param[out] p_workarounds 填充的变通方案
         */
        static void CheckWorkarounds(uint32_t p_vendor_id,
                                     const char *p_renderer_string,
                                     const char *p_version_string,
                                     Workarounds &p_workarounds);

        /**
         * @brief 获取 Surface 信息指针
         *
         * @param[in] p_surface Surface ID
         * @return SurfaceInfo 指针
         * @retval nullptr Surface 不存在
         */
        SurfaceInfo *GetSurfaceInfo(SurfaceID p_surface);
        const SurfaceInfo *GetSurfaceInfo(SurfaceID p_surface) const;

        // ─── 成员变量 ────────────────────────────────────────────────

        /** @brief GL 管理器指针（不拥有所有权） */
        IGLManager *gl_manager_ = nullptr;

        /** @brief 是否已初始化 */
        bool initialized_ = false;

        /** @brief GPU 设备信息数组（OpenGL 通常只有 1 个设备） */
        LocalVector<Device> devices_;

        /** @brief 渲染表面信息数组（索引 = SurfaceID） */
        LocalVector<SurfaceInfo> surfaces_;

        /** @brief RCD 创建的 GL 上下文 ID 列表（用于 Shutdown 时清理） */
        LocalVector<IGLManager::ContextID> created_contexts_;

        /** @brief 缓存的 GL 渲染器字符串（由 QueryDeviceInfo 填充） */
        char renderer_string_[256] = {};

        /** @brief 缓存的 GL 厂商字符串（由 QueryDeviceInfo 填充） */
        char vendor_string_[256] = {};

        /** @brief 缓存的 GL 版本字符串（由 QueryDeviceInfo 填充） */
        char version_string_[256] = {};
    };

} // namespace arhud
