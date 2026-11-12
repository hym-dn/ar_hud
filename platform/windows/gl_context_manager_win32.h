/**
 * @file gl_context_manager_win32.h
 * @brief Win32 平台 OpenGL 上下文管理实现声明
 *
 * @par 概述
 *   GLContextManagerWin32 是 IGLContextManager 接口的 Windows 平台实现，
 *   使用原生 WGL API 管理 OpenGL 渲染上下文和渲染表面。
 *   采用统一 Context 模型，所有 Context 地位平等，无主从区分。
 *
 * @par 统一 Context 模型
 *   @code
 *   contexts_[0..N] — 所有 Context 地位平等，不共享资源
 *   surfaces_[0..M] — 渲染表面（HDC + HWND），可绑定到任意 Context
 *
 *   模式 A（当前引擎）：1 Context + N Surface，MakeCurrent 逐屏切换
 *                      drawable（渲染线程独占 context，Swap 由
 *                      present 线程承担 —— 见 SwapBuffers 跨线程语义）
 *   模式 B（接口能力）：N Context + N Surface，每线程独立
 *   @endcode
 *
 * @par Context 创建流程
 *   首次 CreateContext() 使用两步法：
 *   1. 创建临时 2.1 上下文 → 加载 WGL 扩展 → 创建正式 3.3 Core Profile
 *   后续 CreateContext() 直接使用 wglCreateContextAttribsARB
 *
 * @par 像素格式索引共享（模式 A 的 UB 防护，2026-08-30）
 *   绑定同一 HGLRC 的所有 HDC 必须像素格式**索引一致**（WGL 契约）。
 *   本实现的 ConfigurePixelFormat 在进程内缓存首个窗口选定的索引
 *   （shared_pixel_format_），后续窗口 SetPixelFormat 沿用 ——
 *   消除混合显卡/多适配器下各自 ChoosePixelFormat 的索引漂移。
 *   详见私有方法 ConfigurePixelFormat 的 Doxygen。
 *
 * @par VSync 实现
 *   - VSyncMode::kDisabled：wglSwapIntervalEXT(0)
 *   - VSyncMode::kEnabled：wglSwapIntervalEXT(1)
 *   - VSyncMode::kAdaptive：wglSwapIntervalEXT(-1)
 *     （需 WGL_EXT_swap_control_tear 扩展支持，缺省回退 kEnabled）
 *
 * @author yameng.he
 * @version 4.2
 * @date 2026-08-30
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#ifndef ARHUD_PLATFORM_WINDOWS_GL_CONTEXT_MANAGER_WIN32_H_
#define ARHUD_PLATFORM_WINDOWS_GL_CONTEXT_MANAGER_WIN32_H_

#include "typedefs.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "gl_context_manager.h"
#include "template/local_vector.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace arhud {
/**
 * @brief Win32 平台 OpenGL 上下文管理实现
 *
 * 使用原生 WGL API 实现统一 Context 模型。
 * 所有 Context 地位平等，不共享资源。
 *
 * @par 实现要点（与接口契约的对应）
 *   - 像素格式：进程内索引共享（ConfigurePixelFormat），保证
 *     单 Context 多 Drawable 的绑定合法性
 *   - SwapBuffers：直接调 gdi32 ::SwapBuffers(HDC) —— 不触碰
 *     GL 上下文，天然可被 present 线程（零 GL）调用
 *   - VSync：按 surface 记录模式，绑定时经 wglSwapIntervalEXT
 *     重放（对抗 per-context/per-drawable 方言差异）
 *
 * @see IGLContextManager  基类接口（含跨线程语义与方言陷阱说明）
 */
class GLContextManagerWin32 : public IGLContextManager {
 public:
  GLContextManagerWin32();
  virtual ~GLContextManagerWin32() override;

  /**
   * @brief 初始化 Win32 GL 管理器
   *
   * 加载 opengl32.dll 并获取基础 WGL 函数指针。
   *
   * @return Error::kOK 初始化成功
   * @return Error::kFailed 已初始化或加载驱动失败
   */
  virtual Error Initialize() override;

  /**
   * @brief 关闭 Win32 GL 管理器并释放所有资源
   *
   * 清理所有 OpenGL 上下文、渲染表面和动态加载的函数指针。
   */
  virtual void Shutdown() override;

  /**
   * @brief 创建 GL 渲染上下文
   *
   * 首次调用使用两步法（临时上下文 → 加载扩展 → 正式 3.3 Core Profile）。
   * 后续调用直接使用 wglCreateContextAttribsARB。
   * 所有 Context 不共享资源。
   *
   * @param[in] p_surface 已创建的渲染表面 ID（提供 HDC 与像素格式）
   *
   * @return 新创建的 Context ID
   * @retval kInvalidContextId 创建失败
   *
   * @pre Initialize() 已成功调用
   * @pre p_surface 有效且已配置像素格式
   */
  virtual ContextID CreateContext(SurfaceID p_surface) override;

  /**
   * @brief 销毁 GL 渲染上下文
   *
   * @param[in] p_context_id Context ID
   *
   * @pre p_context_id 有效
   */
  virtual void DestroyContext(ContextID p_context_id) override;

  /**
   * @brief 查询上下文数量
   * @return 当前存活的上下文数量
   */
  virtual uint32 GetContextCount() const override;

  /**
   * @brief 创建渲染表面
   *
   * @param[in] p_screen_id 关联的屏幕 ID
   * @param[in] p_native_window 原生窗口句柄 (HWND)
   * @param[in] p_width 窗口客户区宽度（像素）
   * @param[in] p_height 窗口客户区高度（像素）
   *
   * @return 新创建的 Surface ID
   * @retval kInvalidSurfaceId 创建失败
   */
  virtual SurfaceID CreateSurface(IScreen::ScreenID p_screen_id,
                                  void* p_native_window, uint32 p_width,
                                  uint32 p_height) override;

  /**
   * @brief 销毁渲染表面
   *
   * @param[in] p_surface Surface ID
   */
  virtual void DestroySurface(SurfaceID p_surface) override;

  /**
   * @brief 将指定上下文绑定到指定表面的调用线程
   *
   * @param[in] p_context_id Context ID
   * @param[in] p_surface Surface ID
   *
   * @return Error::kOK 绑定成功
   * @return Error::kInvalidParameter Context ID 或 Surface ID 无效
   * @return Error::kFailed Surface 被其他 Context 占有或平台绑定失败
   *
   * @par drawable 切换
   *   同一 Context 依次绑定不同 Surface 即切换 drawable ——
   *   已是 current 时重复绑定幂等返回 kOK（切 drawable 的
   *   逐屏循环里同屏重绑零开销）。
   */
  virtual Error MakeCurrent(ContextID p_context_id,
                            SurfaceID p_surface) override;

  /**
   * @brief 释放当前线程的 GL 上下文
   *
   * 清除 Context 记录的 bound_surface 后执行
   * wglMakeCurrent(hdc, nullptr) —— context 可被其他线程绑定。
   */
  virtual void ReleaseCurrent() override;

  /**
   * @brief 交换指定渲染表面的前后缓冲区
   *
   * @param[in] p_surface Surface ID
   *
   * @par 实现语义
   *   直接调 gdi32 ::SwapBuffers(surf->hdc) —— 纯 GDI 调用，
   *   **不需要调用线程持有 current context**（缓冲交换是
   *   surface 属性而非 context 属性），present 线程可独立调用。
   *   Surface 无效时静默返回。
   */
  virtual void SwapBuffers(SurfaceID p_surface) override;

  /**
   * @brief 设置指定渲染表面的垂直同步模式
   *
   * @param[in] p_surface Surface ID
   * @param[in] p_mode VSync 模式
   */
  virtual void SetVsyncMode(SurfaceID p_surface, VSyncMode p_mode) override;

  /**
   * @brief 获取指定渲染表面的垂直同步模式
   *
   * @param[in] p_surface Surface ID
   * @return 当前的 VSync 模式
   */
  virtual VSyncMode GetVsyncMode(SurfaceID p_surface) const override;

  /**
   * @brief 获取指定渲染表面的设备上下文句柄
   *
   * @param[in] p_surface Surface ID
   * @return 设备上下文句柄 (HDC)，无效时返回 nullptr
   */
  virtual void* GetDeviceContext(SurfaceID p_surface) const override;

  /**
   * @brief 获取指定 OpenGL 上下文的渲染上下文句柄
   *
   * @param[in] p_context_id Context ID
   * @return 渲染上下文句柄 (HGLRC)，无效时返回 nullptr
   */
  virtual void* GetRenderContext(ContextID p_context_id) const override;

  /**
   * @brief 获取 OpenGL 过程函数的地址。
   *
   * 返回平台相关的 GL 扩展函数指针获取回调（如 wglGetProcAddress），
   * 用于 GL 加载器（如 GLAD）在运行时解析 GL 扩展入口点。
   *
   * @return 指向过程地址获取函数的指针，类型为 void*(*)()；
   *         若平台不支持或未初始化则返回 nullptr。
   */
  virtual void* GetGLProcAddress() const override;

  /**
   * @brief 组合式 OpenGL 函数加载器，按优先级依次从多个 GL
   * 扩展库中查找指定函数。
   *
   * 依次尝试通过 GLAD
   * 内置加载器获取函数指针，若未找到则回退至系统原生加载方式，
   * 确保在多驱动环境下仍能正确解析 GL 函数地址。
   *
   * @param[in] name 待加载的 OpenGL 函数名称，以空字符结尾的 C 字符串。
   * @return 成功时返回函数指针，未找到时返回 nullptr。
   */
  static void* CombinedGLLoader(const char* name);

  /**
   * @brief 获取已加载的 OpenGL 模块句柄。
   * @return 已加载的 OpenGL 模块句柄，若未加载则返回 nullptr。
   */
  void* GetOpenGLModule() const;

  /**
   * @brief 检查管理器是否已初始化
   * @return true 已初始化，false 未初始化
   */
  virtual bool IsInitialized() const override;

  /**
   * @brief 跨 API GPU 资源共享实现（Mode X 零拷贝上屏）
   *
   * @return WGL_NV_DX_interop2 实现（进程内无状态单例，调用方不拥有）；
   *         未初始化时返回 nullptr
   *
   * @note 返回非空不代表扩展可用；实际能力由
   *       IGPUInterop::OpenDevice 的成败决定。
   */
  virtual IGPUInterop* GetGPUInterop() const override;

 private:
  /**
   * @brief 渲染表面簿记（Win32）
   *
   * 每项对应一次 CreateSurface：持有窗口的 DC（已配置像素格式）
   * 与归属关系。字段语义见 IGLContextManager 文件头
   * "Context 与 Surface 的关系"。
   */
  struct GLSurface {
    HWND hwnd = nullptr;  ///< 关联窗口（桌面 DC 场景为 nullptr）
    HDC hdc = nullptr;    ///< 窗口设备上下文（像素格式已配置）
    VSyncMode vsync_mode =
        VSyncMode::kEnabled;  ///< 记录的 VSync 模式（绑定时重放）
    IScreen::ScreenID screen_id =
        IScreen::kInvalidScreenId;  ///< 关联屏幕（仅簿记）
    ContextID owner_context_id =
        kInvalidContextId;  ///< 首次绑定本表面的 Context（单一占有）
    bool is_valid = false;  ///< 槽位有效标记（销毁后 false，槽不回收）
  };

  /**
   * @brief GL 上下文簿记（Win32）
   *
   * 每项对应一次 CreateContext：持有 HGLRC 与当前绑定表面。
   */
  struct GLContext {
    HGLRC hrc = nullptr;  ///< 原生 GL 渲染上下文
    SurfaceID bound_surface =
        kInvalidSurfaceId;  ///< 当前绑定表面（ReleaseCurrent 时清除）
    bool is_valid = false;  ///< 槽位有效标记
  };

  /**
   * @brief 配置设备上下文的像素格式（进程内索引共享）
   *
   * @param p_hdc 设备上下文句柄
   * @return Error::kOK 配置成功
   * @return Error::kFailed 配置失败
   *
   * @par 索引共享机制（单 Context 多 Drawable 的 UB 防护）
   *   WGL 契约：wglMakeCurrent(hdc, hrc) 要求 hdc 的像素格式与
   *   hrc 创建时的格式**索引一致**（描述符等价不算）。若每个窗口
   *   各自 ChoosePixelFormat，单 GPU 上几乎总能得到同一索引
   *   （侥幸），但混合显卡/多适配器系统上不同适配器的格式表
   *   不同，相同请求会返回不同索引 —— 共享上下文绑过去即 UB。
   *
   *   因此：首个窗口 ChoosePixelFormat 选定后缓存索引
   *   （shared_pixel_format_），后续窗口直接 SetPixelFormat
   *   沿用该索引，从根上保证一致性（Qt/SDL 同款模式）。
   *
   * @par 回退路径
   *   SetPixelFormat(缓存索引) 失败（典型：窗口落在另一适配器
   *   上、该索引不存在）→ 该窗口独立 ChoosePixelFormat 并打
   *   WARN —— 其格式与共享上下文可能不一致，绑定属
   *   best-effort（混合适配器拓扑本就超出单 context 模型）。
   *
   * @par 像素格式属性
   *   - 颜色深度：32位 RGBA
   *   - 深度缓冲：24位
   *   - 模板缓冲：8位
   *   - 双缓冲：启用
   *   - 累积缓冲：禁用
   *
   * @par 注意事项
   *   - 每个设备上下文只能配置一次（已有格式则直接返回 kOK）
   *   - 必须在创建 OpenGL 上下文之前调用
   *   - 仅在主线程（Surface 创建期）调用，缓存无并发访问
   */
  Error ConfigurePixelFormat(HDC p_hdc);

  /**
   * @brief 加载 OpenGL 驱动并获取基础 WGL 函数指针
   *
   * 动态加载 opengl32.dll 并获取基础 WGL 函数指针。
   *
   * @return Error::kOK 加载成功
   * @return Error::kFailed 加载失败
   *
   * @par 获取的函数指针
   *   - wglCreateContext：创建 OpenGL 上下文
   *   - wglMakeCurrent：绑定上下文到设备上下文
   *   - wglDeleteContext：删除 OpenGL 上下文
   *   - wglGetProcAddress：获取 OpenGL 扩展函数指针
   */
  Error LoadOpenGLDriver();

  /**
   * @brief 使用临时上下文创建 OpenGL 3.3 Core Profile 上下文
   *
   * 采用两步法创建 OpenGL 3.3 Core Profile 上下文：
   * 1. 创建临时 OpenGL 2.1 上下文
   * 2. 使用 wglCreateContextAttribsARB 创建正式的 3.3 Core Profile 上下文
   *
   * @param p_hdc 设备上下文句柄
   * @return HGLRC 创建的 OpenGL 3.3 Core Profile 上下文句柄
   * @return nullptr 创建失败
   *
   * @par 创建流程
   *   1. 创建临时 OpenGL 2.1 上下文
   *   2. 绑定临时上下文以获取扩展函数
   *   3. 获取 wglCreateContextAttribsARB 函数指针
   *   4. 创建 OpenGL 3.3 Core Profile 上下文
   *   5. 删除临时上下文
   *   6. 绑定正式上下文
   *   7. 加载 WGL 扩展函数
   *
   * @par 上下文属性
   *   - OpenGL 版本：3.3 Core Profile
   *   - 前向兼容：启用
   *   - 不共享资源：nullptr
   */
  HGLRC CreateCoreProfileContext(HDC p_hdc);

  /**
   * @brief 通过临时上下文加载 WGL 扩展
   *
   * WGL 扩展（如 wglCreateContextAttribsARB）需要先有一个活跃的 GL 上下文
   * 才能通过 wglGetProcAddress 获取。本方法创建临时上下文，加载扩展后清理。
   *
   * @param[in] p_hdc 设备上下文
   * @return true 扩展加载成功，false 失败
   *
   * @note 仅在首次 CreateContext 时调用一次
   */
  bool LoadWGLExtensions(HDC p_hdc);

  /**
   * @brief 加载 WGL 扩展函数
   *
   * 获取 VSync 相关的 WGL 扩展函数指针，并检测自适应 VSync 支持。
   * 必须在 OpenGL 上下文绑定后调用。
   *
   * @par 加载的扩展函数
   *   - wglSwapIntervalEXT：设置垂直同步间隔
   *   - wglGetSwapIntervalEXT：获取当前垂直同步间隔
   *
   * @par 检测的扩展
   *   - WGL_EXT_swap_control：VSync 控制支持
   *   - WGL_EXT_swap_control_tear：自适应 VSync 支持
   */
  void LoadExtensions();

  /**
   * @brief 获取渲染表面指针
   *
   * @param p_surface 表面 ID
   * @return GLSurface* 表面指针
   * @return nullptr 表面 ID 无效
   */
  GLSurface* GetSurface(SurfaceID p_surface);

  /**
   * @brief 获取渲染表面常量指针
   *
   * @param p_surface 表面 ID
   * @return const GLSurface* 表面常量指针
   * @return nullptr 表面 ID 无效
   */
  const GLSurface* GetSurface(SurfaceID p_surface) const;

  /**
   * @brief 获取 OpenGL 上下文指针
   *
   * @param p_context_id 上下文 ID
   * @return GLContext* 上下文指针
   * @return nullptr 上下文 ID 无效
   */
  GLContext* GetContext(ContextID p_context_id);

  /**
   * @brief 获取 OpenGL 上下文常量指针
   *
   * @param p_context_id 上下文 ID
   * @return const GLContext* 上下文常量指针
   * @return nullptr 上下文 ID 无效
   */
  const GLContext* GetContext(ContextID p_context_id) const;

 private:
  LocalVector<GLSurface> surfaces_;
  LocalVector<GLContext> contexts_;

  bool is_initialized_ = false;
  bool extensions_loaded_ = false;
  HMODULE opengl_module_ = nullptr;

  /**
   * @brief 共享像素格式索引（首个窗口选定后缓存）
   *
   * 0 = 尚未选定（像素格式索引从 1 开始，0 可作哨兵）。
   * 后续窗口 SetPixelFormat 直接沿用，保证单 Context 多
   * Drawable 模式下所有 HDC 索引一致（WGL 绑定契约）。
   * Shutdown 时重置（新会话可能面对不同驱动/适配器）。
   */
  int shared_pixel_format_ = 0;

  using WglCreateContextFunc = HGLRC(WINAPI*)(HDC);
  using WglMakeCurrentFunc = BOOL(WINAPI*)(HDC, HGLRC);
  using WglDeleteContextFunc = BOOL(WINAPI*)(HGLRC);
  using WglGetProcAddressFunc = PROC(WINAPI*)(LPCSTR);
  using WglCreateContextAttribsARBFunc = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
  using WglSwapIntervalEXTFunc = BOOL(WINAPI*)(int);
  using WglGetSwapIntervalEXTFunc = int(WINAPI*)();

  WglCreateContextFunc wgl_create_context_ = nullptr;
  WglMakeCurrentFunc wgl_make_current_ = nullptr;
  WglDeleteContextFunc wgl_delete_context_ = nullptr;
  WglGetProcAddressFunc wgl_get_proc_address_ = nullptr;
  WglCreateContextAttribsARBFunc wgl_create_context_attribs_arb_ = nullptr;
  WglSwapIntervalEXTFunc wgl_swap_interval_ext_ = nullptr;
  WglGetSwapIntervalEXTFunc wgl_get_swap_interval_ext_ = nullptr;

  bool swap_control_tear_supported_ = false;
};

}  // namespace arhud

#endif  // ARHUD_PLATFORM_WINDOWS

#endif  // ARHUD_PLATFORM_WINDOWS_GL_CONTEXT_MANAGER_WIN32_H_
