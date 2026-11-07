

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_CANVAS_RENDERER_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_CANVAS_RENDERER_H_

#include "io/logger.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "storage/font_storage.h"
#include "storage/material_storage.h"
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

namespace arhud {

class ShaderStorage;
class MaterialStorage;
class MeshStorage;
class TextureStorage;

struct CanvasItem {
  RID material;
  RID mesh;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float clip_rect[4] = {0, 0, 0, 0};
  int32 z_order = 0;
  float opacity = 1.0f;

  float nine_patch_margin[4] = {0, 0, 0, 0};
  bool nine_patch_enabled = false;
};

struct TextItem {
  RID font;
  const uint32* text = nullptr;
  uint32 length = 0;
  float x = 0.0f;
  float y = 0.0f;
  float color[4] = {1, 1, 1, 1};
  int32 z_order = 0;
};

struct CanvasBatch {
  RID material;
  RDUniformSetID material_uniform_set;
  LocalVector<uint32> item_indices;
  LocalVector<float> vertices;
  uint32 vertex_count = 0;
};

/**
 * @brief HUD Canvas 图层（Z 序分组）
 *
 * 一个图层是同一 z_order 层次的 CanvasItem/TextItem 集合，
 * 图层间按 layer 值升序渲染。主线程构建后随 FrameScene 拷贝进帧槽。
 *
 * @note TextItem::text 指向调用方内存；跨线程传递时由
 *       FrameScene 负责码点深拷贝与指针重定向（见 frame_scene.h）。
 */
struct HUDCanvasLayer {
  int32 layer = 0;                       ///< 图层序（小者先渲染）
  LocalVector<CanvasItem> canvas_items;  ///< 2D 绘制项
  LocalVector<TextItem> text_items;      ///< 文本绘制项
};

class CanvasRenderer {
  ARHUD_DISABLE_COPY_MOVE(CanvasRenderer);

 public:
  CanvasRenderer() = default;
  ~CanvasRenderer();

  Error Initialize(RenderingDevice* p_rd, ShaderStorage* p_shader_storage,
                   MaterialStorage* p_material_storage,
                   MeshStorage* p_mesh_storage,
                   TextureStorage* p_texture_storage,
                   FontStorage* p_font_storage);

  void Finalize();

  void CanvasItemPush(const CanvasItem& p_item);

  void CanvasItemsClear();

  void TextItemPush(const TextItem& p_item);

  void TextItemsClear();

  void RenderCanvasItems(const float* p_projection, uint32 p_screen_width,
                         uint32 p_screen_height, bool p_clear_color = false,
                         const float p_clear_color_value[4] = nullptr,
                         RDFramebufferID p_framebuffer = RDFramebufferID());

  void RenderTextItems(const float* p_projection, uint32 p_screen_width,
                       uint32 p_screen_height,
                       RDFramebufferID p_framebuffer = RDFramebufferID());

  bool IsInitialized() const {
    return initialized_;
  }

  RID GetShaderRID() const {
    return shader_rid_;
  }

 private:
  void SortCanvasItems();

  void SortTextItems();

  void BuildBatches();

  RDUniformSetID CreateMaterialUniformSet(RID p_material);

  void AppendItemVertices(const CanvasItem& p_item,
                          LocalVector<float>& p_vertices);

  void AppendNinePatchVertices(const CanvasItem& p_item,
                               LocalVector<float>& p_vertices);

  void EnsureBatchVBOSize(uint32 p_required_size);

  Error InitializeTextPipeline();

  void FinalizeTextPipeline();

  RDUniformSetID CreateFontTextureUniformSet(RID p_font);

 private:
  bool initialized_ = false;

  RenderingDevice* rd_ = nullptr;
  ShaderStorage* shader_storage_ = nullptr;
  MaterialStorage* material_storage_ = nullptr;
  MeshStorage* mesh_storage_ = nullptr;
  TextureStorage* texture_storage_ = nullptr;

  RID shader_rid_;
  RDShaderID shader_rd_id_;
  RDVertexFormatID vertex_format_id_;
  RDPipelineID pipeline_id_;

  RDBufferID projection_ubo_;
  RDUniformSetID projection_set_;

  RDBufferID batch_vbo_;
  uint32 batch_vbo_size_ = 0;
  static constexpr uint32 kBatchVboInitialSize = 64 * 1024;
  static constexpr uint32 kBatchVertexStride = 8;
  RID default_texture_rid_;

  LocalVector<CanvasItem> canvas_items_;
  LocalVector<TextItem> text_items_;
  LocalVector<CanvasBatch> batches_;

  HashMap<MaterialUniformSetCacheKey, RDUniformSetID>
      material_uniform_set_cache_;

  FontStorage* font_storage_ = nullptr;
  RID text_shader_rid_;
  RDShaderID text_shader_rd_id_;
  RDPipelineID text_pipeline_id_;

  RDBufferID text_smoothing_ubo_;
  RDUniformSetID text_smoothing_set_;

  RDBufferID text_vbo_;
  uint32 text_vbo_size_ = 0;
  static constexpr uint32 kTextVboInitialSize = 16 * 1024;

  RDUniformSetID cached_font_texture_set_;
  RID cached_font_rid_;
  static constexpr float kDefaultSmoothing = 0.8f;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_CANVAS_RENDERER_H_
