/**
 * @file render_globals.h
 * @brief 渲染层全局可达点（ADR-0010，对齐 Godot RenderingServerGlobals）
 *
 * 以 `static inline` 指针**平铺**暴露渲染栈的关键对象（5 个资源 Storage、
 * 2 个 Renderer、ItemStorage、视口解析器、帧提交信号），使命令实现体
 * 无需"上下文参数"即可直达存储——对齐 Godot 的
 * `RenderingServerGlobals`（`#define RSG RenderingServerGlobals`）。
 *
 * @par 与 Godot 的对应
 *   | Godot                          | 本工程                          |
 *   |--------------------------------|---------------------------------|
 *   | `RSG::texture_storage` 等       | `RenderGlobals::texture_storage`|
 *   | `RSG::rasterizer`（Compositor） | `RenderGlobals::spatial_...`    |
 *
 * @par 装配与生命周期（唯一写入者：ARHudEngine）
 *   - `ARHudEngine::Initialize`：共享栈（Compositor/RD）建成后**填入**；
 *   - `ARHudEngine::Finalize`／析构：栈释放**清空**（调用 `Reset()`）。
 *   - 只含指针，**零动态分配**（不动 ADR-0001）。
 *
 * @par 线程
 *   装配/清空发生在**渲染线程启动之前 / 停止之后**（引擎级时序）；
 *   读取发生在渲染线程（命令执行）与主线程（旁路）。指针本身在栈存活
 *   期间为常量，不构成数据竞争。
 *
 * @note 本工程为**单实例**引擎：多个 ARHudEngine 同时初始化会相互覆盖；
 *       该约束由引擎层保证（见 ADR-0010 §4.2）。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-10-02
 * @ingroup rendering
 */

#ifndef ARHUD_SERVERS_RENDERING_RENDER_GLOBALS_H_
#define ARHUD_SERVERS_RENDERING_RENDER_GLOBALS_H_

#include <atomic>

#include "typedefs.h"

namespace arhud {

// 前置声明：本头仅持有指针，不引入各存储/渲染器的定义。
class ShaderStorage;
class TextureStorage;
class MaterialStorage;
class MeshStorage;
class FontStorage;
class CanvasRenderer;
class SpatialRenderer;
class ItemStorage;
class IViewportProvider;

/**
 * @brief 渲染层全局可达点（ADR-0010，RSG 式平铺）
 *
 * 由 ARHudEngine 装配/清空；命令实现体经此直达存储，替代"执行器上下文"。
 */
struct RenderGlobals {
  // ── 资源 Storage（GPU-backed，归 ARHudCompositor）──
  static inline ShaderStorage* shader_storage = nullptr;      ///< 着色器存储
  static inline TextureStorage* texture_storage = nullptr;    ///< 纹理存储
  static inline MaterialStorage* material_storage = nullptr;  ///< 材质存储
  static inline MeshStorage* mesh_storage = nullptr;          ///< 网格存储
  static inline FontStorage* font_storage = nullptr;          ///< 字体存储

  // ── Renderer（归 ARHudCompositor）──
  static inline CanvasRenderer* canvas_renderer = nullptr;    ///< 2D 批渲染器
  static inline SpatialRenderer* spatial_renderer = nullptr;  ///< 3D 渲染器

  // ── 状态 Storage（CPU-only，归 ARHudEngine）──
  static inline ItemStorage* item_storage = nullptr;  ///< 保留模式 item 账本

  // ── 服务与信号 ──
  static inline IViewportProvider* viewport_provider =
      nullptr;  ///< 视口解析（ADR-0007）
  static inline std::atomic<bool>* submit_signal =
      nullptr;  ///< kSubmitScene 信号

  /**
   * @brief 清空全部可达点（引擎栈释放时调用）
   *
   * @note 供 ARHudEngine 的 Finalize／析构调用；测试亦可借此重置。
   */
  static void Reset() {
    shader_storage = nullptr;
    texture_storage = nullptr;
    material_storage = nullptr;
    mesh_storage = nullptr;
    font_storage = nullptr;
    canvas_renderer = nullptr;
    spatial_renderer = nullptr;
    item_storage = nullptr;
    viewport_provider = nullptr;
    submit_signal = nullptr;
  }
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDER_GLOBALS_H_
