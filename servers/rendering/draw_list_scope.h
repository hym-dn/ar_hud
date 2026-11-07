#ifndef ARHUD_SERVERS_RENDERING_DRAW_LIST_SCOPE_H_
#define ARHUD_SERVERS_RENDERING_DRAW_LIST_SCOPE_H_

#include "rendering_device.h"

namespace arhud {

/**
 * @brief DrawList RAII 辅助类
 *
 * 确保 DrawListEnd() 在作用域结束时自动调用，防止遗漏。
 * 对齐 Godot 的 RAII 模式，简化帧录制代码。
 *
 * @par 典型使用
 *   @code
 *   {
 *       DrawListScope scope(rd);
 *       if (!scope.IsActive()) return;
 *       rd->DrawListSetViewport(scope.Get(), 0, 0, w, h);
 *       rd->DrawListBindRenderPipeline(scope.Get(), pipeline);
 *       rd->DrawListDrawIndexed(scope.Get(), index_count);
 *   } // 自动调用 DrawListEnd()
 *   @endcode
 */
class DrawListScope {
  ARHUD_DISABLE_COPY_MOVE(DrawListScope);

 public:
  /**
   * @brief 构造并开始绘制列表（渲染到屏幕）
   *
   * @param[in] p_rd RenderingDevice 指针
   */
  explicit DrawListScope(RenderingDevice* p_rd)
      : rd_(p_rd),
        id_(p_rd != nullptr ? p_rd->DrawListBegin() : kInvalidDrawListId) {}

  /**
   * @brief 构造并开始绘制列表（渲染到指定 Framebuffer）
   *
   * @param[in] p_rd           RenderingDevice 指针
   * @param[in] p_framebuffer  目标 Framebuffer
   * @param[in] p_clear_colors 是否清除颜色附件
   * @param[in] p_clear_depth  是否清除深度附件
   * @param[in] p_clear_stencil 是否清除模板附件
   * @param[in] p_clear_color  清除颜色值
   * @param[in] p_clear_depth_val 清除深度值
   * @param[in] p_clear_stencil_val 清除模板值
   */
  DrawListScope(RenderingDevice* p_rd, RDFramebufferID p_framebuffer,
                bool p_clear_colors = false, bool p_clear_depth = false,
                bool p_clear_stencil = false,
                const float p_clear_color[4] = nullptr,
                float p_clear_depth_val = 1.0f, uint32 p_clear_stencil_val = 0)
      : rd_(p_rd),
        id_(p_rd != nullptr ? p_rd->DrawListBegin(
                                  p_framebuffer, p_clear_colors, p_clear_depth,
                                  p_clear_stencil, p_clear_color,
                                  p_clear_depth_val, p_clear_stencil_val)
                            : kInvalidDrawListId) {}

  /**
   * @brief 析构并结束绘制列表
   */
  ~DrawListScope() {
    if (IsActive()) {
      rd_->DrawListEnd();
    }
  }

  /**
   * @brief 获取 DrawListID
   * @return DrawListID
   */
  DrawListID Get() const {
    return id_;
  }

  /**
   * @brief 检查 DrawList 是否活跃
   * @return true 活跃
   */
  bool IsActive() const {
    return id_ != kInvalidDrawListId;
  }

 private:
  RenderingDevice* rd_;
  DrawListID id_;
};

/**
 * @brief ComputeList RAII 辅助类
 *
 * 确保 ComputeListEnd() 在作用域结束时自动调用。
 */
class ComputeListScope {
  ARHUD_DISABLE_COPY_MOVE(ComputeListScope);

 public:
  explicit ComputeListScope(RenderingDevice* p_rd)
      : rd_(p_rd),
        id_(p_rd != nullptr ? p_rd->ComputeListBegin()
                            : kInvalidComputeListId) {}

  ~ComputeListScope() {
    if (IsActive()) {
      rd_->ComputeListEnd();
    }
  }

  ComputeListID Get() const {
    return id_;
  }
  bool IsActive() const {
    return id_ != kInvalidComputeListId;
  }

 private:
  RenderingDevice* rd_;
  ComputeListID id_;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_DRAW_LIST_SCOPE_H_
