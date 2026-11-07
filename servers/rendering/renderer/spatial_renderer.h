

#ifndef ARHUD_SERVERS_RENDERING_RENDERER_SPATIAL_RENDERER_H_
#define ARHUD_SERVERS_RENDERING_RENDERER_SPATIAL_RENDERER_H_

#include "io/logger.h"
#include "math/camera_matrix.h"
#include "os/memory.h"
#include "rendering_device.h"
#include "storage/material_storage.h"
#include "storage/viewport_scene_state.h"  // DirectionalLight（per-viewport 灯光状态，ADR-0007 迁至 storage/viewport_scene_state.h）
#include "string/ustring.h"
#include "template/hash_map.h"
#include "template/local_vector.h"

namespace arhud {
class ShaderStorage;
class MaterialStorage;
class MeshStorage;
class TextureStorage;

struct SpatialItem {
  RID material;
  RID mesh;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  uint8 render_priority = 0;
  bool disable_cull = false;
};

struct RenderListItem {
  RID material_rid;
  RID mesh_rid;
  float transform[16];
  uint8 priority;
  bool disable_cull;
  RID next_material_rid;
};

class SpatialRenderer {
  ARHUD_DISABLE_COPY_MOVE(SpatialRenderer);

 public:
  static constexpr uint32 kMaxSpatialItems = 256;
  static constexpr uint32 kModelUBODataSize = 64;
  static constexpr uint32 kMaterialUBODataSize = 4;
  static constexpr uint32 kDefaultUBOAlignment = 256;
  SpatialRenderer() = default;
  ~SpatialRenderer();

  Error Initialize(RenderingDevice* p_rd, ShaderStorage* p_shader_storage,
                   MaterialStorage* p_material_storage,
                   MeshStorage* p_mesh_storage,
                   TextureStorage* p_texture_storage);

  void Finalize();

  void SpatialItemPush(const SpatialItem& p_item);

  void SpatialItemsClear();

  void RenderSpatialItems(const CameraMatrix& p_camera,
                          const DirectionalLight* p_light,
                          uint32 p_screen_width, uint32 p_screen_height,
                          bool p_clear_color = true, bool p_clear_depth = true,
                          const float p_clear_color_value[4] = nullptr,
                          RDFramebufferID p_framebuffer = RDFramebufferID());

  bool IsInitialized() const {
    return initialized_;
  }

  RID GetShaderRID() const {
    return shader_rid_;
  }

 private:
  void BuildRenderList();

  RDUniformSetID CreateMaterialUniformSet(RID p_material);

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
  RDPipelineID pipeline_no_cull_id_;
  RDPipelineID depth_pipeline_id_;
  RDPipelineID depth_pipeline_no_cull_id_;
  bool use_depth_prepass_ = false;

  RDBufferID camera_ubo_;
  RDUniformSetID camera_set_;

  RDBufferID model_ubo_;
  RDUniformSetID model_set_;

  RDBufferID light_ubo_;
  RDUniformSetID light_set_;

  RDBufferID material_ubo_;

  uint32 ubo_alignment_ = kDefaultUBOAlignment;
  uint32 model_ubo_stride_ = kDefaultUBOAlignment;
  uint32 material_ubo_stride_ = kDefaultUBOAlignment;
  RID default_texture_rid_;

  LocalVector<SpatialItem> spatial_items_;
  LocalVector<RenderListItem> render_list_;

  HashMap<MaterialUniformSetCacheKey, RDUniformSetID>
      material_uniform_set_cache_;
};

}  // namespace arhud

#endif  // ARHUD_SERVERS_RENDERING_RENDERER_SPATIAL_RENDERER_H_
