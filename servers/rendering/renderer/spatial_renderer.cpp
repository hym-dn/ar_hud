

#include "spatial_renderer.h"

#include "draw_list_scope.h"
#include "io/logger.h"
#include "os/memory.h"
#include "shaders/hud_spatial.h"
#include "storage/material_storage.h"
#include "storage/mesh_storage.h"
#include "storage/shader_storage.h"
#include "storage/texture_storage.h"

namespace arhud {

SpatialRenderer::~SpatialRenderer() {
  if (initialized_) {
    Finalize();
  }
}

Error SpatialRenderer::Initialize(RenderingDevice* p_rd,
                                  ShaderStorage* p_shader_storage,
                                  MaterialStorage* p_material_storage,
                                  MeshStorage* p_mesh_storage,
                                  TextureStorage* p_texture_storage) {
  if (initialized_) {
    ARHUD_LOG_ERROR("kAlreadyExists",
                    "SpatialRenderer::Initialize() already initialized");
    return Error::kAlreadyExists;
  }

  if (!p_rd || !p_rd->IsInitialized() || !p_shader_storage ||
      !p_material_storage || !p_mesh_storage || !p_texture_storage) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() invalid parameters");
    return Error::kFailed;
  }

  rd_ = p_rd;
  shader_storage_ = p_shader_storage;
  material_storage_ = p_material_storage;
  mesh_storage_ = p_mesh_storage;
  texture_storage_ = p_texture_storage;

  uint64 alignment =
      rd_->GetDeviceDriver()->LimitGet(Limit::kMinUniformBufferOffsetAlignment);
  ubo_alignment_ =
      (alignment > 0) ? static_cast<uint32>(alignment) : kDefaultUBOAlignment;
  model_ubo_stride_ =
      ((kModelUBODataSize + ubo_alignment_ - 1) / ubo_alignment_) *
      ubo_alignment_;
  material_ubo_stride_ =
      ((kMaterialUBODataSize + ubo_alignment_ - 1) / ubo_alignment_) *
      ubo_alignment_;

  LocalVector<VertexAttribute> attribs;
  attribs.Resize(3);
  attribs[0].binding = 0;
  attribs[0].location = 0;
  attribs[0].offset = 0;
  attribs[0].format = DataFormat::kR32G32B32Sfloat;
  attribs[0].stride = MeshStorage::kVertex3DStride;
  attribs[0].frequency = VertexFrequency::kVertex;

  attribs[1].binding = 0;
  attribs[1].location = 1;
  attribs[1].offset = 12;
  attribs[1].format = DataFormat::kR32G32B32Sfloat;
  attribs[1].stride = MeshStorage::kVertex3DStride;
  attribs[1].frequency = VertexFrequency::kVertex;

  attribs[2].binding = 0;
  attribs[2].location = 2;
  attribs[2].offset = 24;
  attribs[2].format = DataFormat::kR32G32Sfloat;
  attribs[2].stride = MeshStorage::kVertex3DStride;
  attribs[2].frequency = VertexFrequency::kVertex;

  vertex_format_id_ = rd_->VertexFormatCreate(
      VectorView<VertexAttribute>(attribs.Data(), attribs.Size()));
  if (!vertex_format_id_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() VertexFormatCreate failed");
    return Error::kFailed;
  }

  LocalVector<ShaderStageSource> stages;
  stages.Resize(2);
  stages[0].stage = ShaderStage::kVertex;
  stages[0].source = kHudSpatialVert;
  stages[1].stage = ShaderStage::kFragment;
  stages[1].source = kHudSpatialFrag;

  LocalVector<ShaderUniform> uniforms;
  uniforms.Resize(5);

  uniforms[0].type = UniformType::kUniformBuffer;
  uniforms[0].writable = false;
  uniforms[0].binding = 0;
  uniforms[0].stages = BitField<ShaderStage>(ShaderStage::kVertex);
  uniforms[0].length = 0;
  uniforms[0].size = 128;
  uniforms[0].name = nullptr;

  uniforms[1].type = UniformType::kSamplerWithTexture;
  uniforms[1].writable = false;
  uniforms[1].binding = 0;
  uniforms[1].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  uniforms[1].length = 0;
  uniforms[1].size = 0;
  uniforms[1].name = "u_texture";

  uniforms[2].type = UniformType::kUniformBuffer;
  uniforms[2].writable = false;
  uniforms[2].binding = 1;
  uniforms[2].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  uniforms[2].length = 0;
  uniforms[2].size = 4;
  uniforms[2].name = nullptr;

  uniforms[3].type = UniformType::kUniformBuffer;
  uniforms[3].writable = false;
  uniforms[3].binding = 2;
  uniforms[3].stages = BitField<ShaderStage>(ShaderStage::kVertex);
  uniforms[3].length = 0;
  uniforms[3].size = 64;
  uniforms[3].name = nullptr;

  uniforms[4].type = UniformType::kUniformBuffer;
  uniforms[4].writable = false;
  uniforms[4].binding = 3;
  uniforms[4].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  uniforms[4].length = 0;
  uniforms[4].size = 48;
  uniforms[4].name = nullptr;

  shader_rid_ = shader_storage_->ShaderCreateFromSource("hud_spatial", stages,
                                                        uniforms, 0);

  if (!shader_rid_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialRenderer::Initialize() ShaderCreateFromSource failed");
    return Error::kFailed;
  }

  shader_rd_id_ = shader_storage_->ShaderGetRDId(shader_rid_);
  if (!shader_rd_id_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() shader compilation failed");
    return Error::kFailed;
  }

  PipelineRasterizationState raster_cull;
  raster_cull.cull_mode = PolygonCullMode::kBack;
  raster_cull.front_face = PolygonFrontFace::kCounterClockwise;

  PipelineRasterizationState raster_no_cull;
  raster_no_cull.cull_mode = PolygonCullMode::kDisabled;
  raster_no_cull.front_face = PolygonFrontFace::kCounterClockwise;

  PipelineMultisampleState multisample;
  multisample.sample_count = TextureSamples::k1;

  PipelineDepthStencilState depth_stencil;
  depth_stencil.enable_depth_test = true;
  depth_stencil.enable_depth_write = true;
  depth_stencil.depth_compare_operator = CompareOperator::kLess;

  PipelineColorBlendState blend = PipelineColorBlendState::CreateBlend(1);

  pipeline_id_ = rd_->RenderPipelineCreate(
      shader_rd_id_, vertex_format_id_, RenderPrimitive::kTriangles,
      raster_cull, multisample, depth_stencil, blend,
      BitField<PipelineDynamicStateFlags>(), rd_->GetDefaultRenderPass());

  if (!pipeline_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialRenderer::Initialize() RenderPipelineCreate (cull) failed");
    return Error::kFailed;
  }

  pipeline_no_cull_id_ = rd_->RenderPipelineCreate(
      shader_rd_id_, vertex_format_id_, RenderPrimitive::kTriangles,
      raster_no_cull, multisample, depth_stencil, blend,
      BitField<PipelineDynamicStateFlags>(), rd_->GetDefaultRenderPass());

  if (!pipeline_no_cull_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialRenderer::Initialize() RenderPipelineCreate (no-cull) failed");
    return Error::kFailed;
  }

  RID depth_shader_rid =
      shader_storage_->ShaderGetVariant(shader_rid_, ShaderVariant::kDepthPass);
  RDShaderID depth_shader_rd_id =
      shader_storage_->ShaderGetRDId(depth_shader_rid);

  if (depth_shader_rd_id.IsValid()) {
    PipelineColorBlendState blend_depth =
        PipelineColorBlendState::CreateDisabled(1);

    blend_depth.attachments[0].write_r = false;
    blend_depth.attachments[0].write_g = false;
    blend_depth.attachments[0].write_b = false;
    blend_depth.attachments[0].write_a = false;

    PipelineDepthStencilState depth_stencil_depth;
    depth_stencil_depth.enable_depth_test = true;
    depth_stencil_depth.enable_depth_write = true;
    depth_stencil_depth.depth_compare_operator = CompareOperator::kLessOrEqual;

    depth_pipeline_id_ = rd_->RenderPipelineCreate(
        depth_shader_rd_id, vertex_format_id_, RenderPrimitive::kTriangles,
        raster_cull, multisample, depth_stencil_depth, blend_depth,
        BitField<PipelineDynamicStateFlags>(), rd_->GetDefaultRenderPass());

    if (!depth_pipeline_id_.IsValid()) {
      ARHUD_LOG_WARN(
          "SpatialRenderer::Initialize() depth pre-pass pipeline (cull) "
          "creation failed, depth pre-pass disabled");
    }

    depth_pipeline_no_cull_id_ = rd_->RenderPipelineCreate(
        depth_shader_rd_id, vertex_format_id_, RenderPrimitive::kTriangles,
        raster_no_cull, multisample, depth_stencil_depth, blend_depth,
        BitField<PipelineDynamicStateFlags>(), rd_->GetDefaultRenderPass());

    if (!depth_pipeline_no_cull_id_.IsValid()) {
      ARHUD_LOG_WARN(
          "SpatialRenderer::Initialize() depth pre-pass pipeline (no-cull) "
          "creation failed, depth pre-pass disabled");
    }

    use_depth_prepass_ =
        depth_pipeline_id_.IsValid() && depth_pipeline_no_cull_id_.IsValid();
  } else {
    ARHUD_LOG_WARN(
        "SpatialRenderer::Initialize() depth variant shader not available, "
        "depth pre-pass disabled");
    use_depth_prepass_ = false;
  }

  camera_ubo_ = rd_->BufferCreate(
      128, BitField<BufferUsageBits>(BufferUsageBits::kUniform),
      MemoryAllocationType::kCpu);

  if (!camera_ubo_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() camera UBO create failed");
    return Error::kFailed;
  }

  {
    BoundUniform bound;
    bound.type = UniformType::kUniformBuffer;
    bound.binding = 0;
    bound.ids.PushBack(rd_->BufferGetDriverId(camera_ubo_));

    camera_set_ = rd_->UniformSetCreate(VectorView<BoundUniform>(&bound, 1),
                                        shader_rd_id_, 0);

    if (!camera_set_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "SpatialRenderer::Initialize() camera UniformSet create failed");
      return Error::kFailed;
    }
  }

  model_ubo_ =
      rd_->BufferCreate(kMaxSpatialItems * model_ubo_stride_,
                        BitField<BufferUsageBits>(BufferUsageBits::kUniform),
                        MemoryAllocationType::kCpu);

  if (!model_ubo_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() model UBO create failed");
    return Error::kFailed;
  }

  {
    BoundUniform bound;
    bound.type = UniformType::kUniformBufferDynamic;
    bound.binding = 2;
    bound.ids.PushBack(rd_->BufferGetDriverId(model_ubo_));
    bound.range_size = 64;

    model_set_ = rd_->UniformSetCreate(VectorView<BoundUniform>(&bound, 1),
                                       shader_rd_id_, 2);

    if (!model_set_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "SpatialRenderer::Initialize() model UniformSet create failed");
      return Error::kFailed;
    }
  }

  light_ubo_ = rd_->BufferCreate(
      48, BitField<BufferUsageBits>(BufferUsageBits::kUniform),
      MemoryAllocationType::kCpu);

  if (!light_ubo_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() light UBO create failed");
    return Error::kFailed;
  }

  {
    BoundUniform bound;
    bound.type = UniformType::kUniformBuffer;
    bound.binding = 3;
    bound.ids.PushBack(rd_->BufferGetDriverId(light_ubo_));

    light_set_ = rd_->UniformSetCreate(VectorView<BoundUniform>(&bound, 1),
                                       shader_rd_id_, 3);

    if (!light_set_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "SpatialRenderer::Initialize() light UniformSet create failed");
      return Error::kFailed;
    }
  }

  material_ubo_ =
      rd_->BufferCreate(kMaxSpatialItems * material_ubo_stride_,
                        BitField<BufferUsageBits>(BufferUsageBits::kUniform),
                        MemoryAllocationType::kCpu);

  if (!material_ubo_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "SpatialRenderer::Initialize() material UBO create failed");
    return Error::kFailed;
  }

  uint8 white_pixel[4] = {255, 255, 255, 255};
  default_texture_rid_ = texture_storage_->TextureCreateFromData(
      "default_white", white_pixel, sizeof(white_pixel), 1, 1,
      DataFormat::kR8G8B8A8Unorm);
  if (!default_texture_rid_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialRenderer::Initialize() default texture create failed");
    return Error::kFailed;
  }

  initialized_ = true;
  ARHUD_LOG_INFO("SpatialRenderer::Initialize() success");
  return Error::kOK;
}

void SpatialRenderer::Finalize() {
  if (!initialized_) {
    return;
  }

  for (auto it = material_uniform_set_cache_.Begin();
       it != material_uniform_set_cache_.End(); ++it) {
    auto pair = *it;
    if (pair.value.IsValid()) {
      rd_->UniformSetFree(pair.value);
    }
  }
  material_uniform_set_cache_.Clear();

  if (default_texture_rid_.IsValid()) {
    texture_storage_->TextureFree(default_texture_rid_);
    default_texture_rid_ = RID();
  }

  if (material_ubo_.IsValid()) {
    rd_->BufferFree(material_ubo_);
    material_ubo_ = RDBufferID();
  }

  if (light_set_.IsValid()) {
    rd_->UniformSetFree(light_set_);
    light_set_ = RDUniformSetID();
  }

  if (light_ubo_.IsValid()) {
    rd_->BufferFree(light_ubo_);
    light_ubo_ = RDBufferID();
  }

  if (model_set_.IsValid()) {
    rd_->UniformSetFree(model_set_);
    model_set_ = RDUniformSetID();
  }

  if (model_ubo_.IsValid()) {
    rd_->BufferFree(model_ubo_);
    model_ubo_ = RDBufferID();
  }

  if (camera_set_.IsValid()) {
    rd_->UniformSetFree(camera_set_);
    camera_set_ = RDUniformSetID();
  }

  if (camera_ubo_.IsValid()) {
    rd_->BufferFree(camera_ubo_);
    camera_ubo_ = RDBufferID();
  }

  if (pipeline_no_cull_id_.IsValid()) {
    rd_->PipelineFree(pipeline_no_cull_id_);
    pipeline_no_cull_id_ = RDPipelineID();
  }

  if (pipeline_id_.IsValid()) {
    rd_->PipelineFree(pipeline_id_);
    pipeline_id_ = RDPipelineID();
  }

  if (depth_pipeline_no_cull_id_.IsValid()) {
    rd_->PipelineFree(depth_pipeline_no_cull_id_);
    depth_pipeline_no_cull_id_ = RDPipelineID();
  }

  if (depth_pipeline_id_.IsValid()) {
    rd_->PipelineFree(depth_pipeline_id_);
    depth_pipeline_id_ = RDPipelineID();
  }

  if (shader_rid_.IsValid()) {
    shader_storage_->ShaderFree(shader_rid_);
    shader_rid_ = RID();
    shader_rd_id_ = RDShaderID();
  }

  if (vertex_format_id_.IsValid()) {
    rd_->VertexFormatFree(vertex_format_id_);
    vertex_format_id_ = RDVertexFormatID();
  }

  spatial_items_.Clear();
  render_list_.Clear();

  rd_ = nullptr;
  shader_storage_ = nullptr;
  material_storage_ = nullptr;
  mesh_storage_ = nullptr;
  texture_storage_ = nullptr;

  initialized_ = false;
  ARHUD_LOG_INFO("SpatialRenderer::Finalize() done");
}

void SpatialRenderer::SpatialItemPush(const SpatialItem& p_item) {
  spatial_items_.PushBack(p_item);
}

void SpatialRenderer::SpatialItemsClear() {
  spatial_items_.Clear();
  render_list_.Clear();
}

void SpatialRenderer::RenderSpatialItems(const CameraMatrix& p_camera,
                                         const DirectionalLight* p_light,
                                         uint32 p_screen_width,
                                         uint32 p_screen_height,
                                         bool p_clear_color, bool p_clear_depth,
                                         const float p_clear_color_value[4],
                                         RDFramebufferID p_framebuffer) {
  if (spatial_items_.IsEmpty()) {
    return;
  }

  p_camera.UpdateMatrices();

  rd_->BufferUpdate(camera_ubo_, 0, sizeof(float) * 16,
                    p_camera.GetProjectionMatrix());
  rd_->BufferUpdate(camera_ubo_, sizeof(float) * 16, sizeof(float) * 16,
                    p_camera.GetViewMatrix());

  if (p_light != nullptr) {
    float light_data[12];
    light_data[0] = p_light->direction[0];
    light_data[1] = p_light->direction[1];
    light_data[2] = p_light->direction[2];
    light_data[3] = 0.0f;
    light_data[4] = p_light->color[0];
    light_data[5] = p_light->color[1];
    light_data[6] = p_light->color[2];
    light_data[7] = p_light->energy;
    light_data[8] = p_light->ambient;
    light_data[9] = 0.0f;
    light_data[10] = 0.0f;
    light_data[11] = 0.0f;
    rd_->BufferUpdate(light_ubo_, 0, sizeof(light_data), light_data);
  } else {
    float no_light[12] = {0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f};
    rd_->BufferUpdate(light_ubo_, 0, sizeof(no_light), no_light);
  }

  BuildRenderList();

  for (uint32 i = 0; i < render_list_.Size(); ++i) {
    const RenderListItem& item = render_list_[i];
    rd_->BufferUpdate(model_ubo_, i * model_ubo_stride_, 64, item.transform);
  }

  for (uint32 i = 0; i < render_list_.Size(); ++i) {
    const RenderListItem& item = render_list_[i];
    const Material* mat = material_storage_->MaterialGetInfo(item.material_rid);
    if (mat && mat->data && mat->data->uniform_data.Size() >= sizeof(float)) {
      float opacity =
          *reinterpret_cast<const float*>(mat->data->uniform_data.Data());
      rd_->BufferUpdate(material_ubo_, i * material_ubo_stride_, sizeof(float),
                        &opacity);
    } else {
      float default_opacity = 1.0f;
      rd_->BufferUpdate(material_ubo_, i * material_ubo_stride_, sizeof(float),
                        &default_opacity);
    }
  }

  float default_clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float* clear =
      p_clear_color_value ? p_clear_color_value : default_clear;

  if (use_depth_prepass_) {
    DrawListScope depth_scope(
        rd_,
        p_framebuffer.IsValid() ? p_framebuffer : rd_->GetDefaultFramebuffer(),
        false, p_clear_depth, false, nullptr);
    DrawListID depth_dl = depth_scope.Get();

    rd_->DrawListSetViewport(depth_dl, 0, 0, p_screen_width, p_screen_height);

    RDPipelineID current_depth_pipeline;

    for (uint32 i = 0; i < render_list_.Size(); ++i) {
      const RenderListItem& item = render_list_[i];

      RDPipelineID target_depth_pipeline =
          item.disable_cull ? depth_pipeline_no_cull_id_ : depth_pipeline_id_;

      if (target_depth_pipeline.GetId() != current_depth_pipeline.GetId()) {
        rd_->DrawListBindRenderPipeline(depth_dl, target_depth_pipeline);
        rd_->DrawListBindUniformSet(depth_dl, camera_set_, 0);
        current_depth_pipeline = target_depth_pipeline;
      }

      rd_->DrawListBindUniformSetWithDynamicOffset(depth_dl, model_set_, 2,
                                                   i * model_ubo_stride_);

      const MeshInfo* mesh_info = mesh_storage_->MeshGetInfo(item.mesh_rid);
      if (!mesh_info || mesh_info->surfaces.IsEmpty()) {
        continue;
      }

      for (uint32 s = 0; s < mesh_info->surfaces.Size(); ++s) {
        const SurfaceInfo* surface = mesh_info->surfaces[s];
        if (!surface) {
          continue;
        }

        uint64 offset = 0;
        rd_->DrawListBindVertexBuffers(depth_dl, &surface->vertex_buffer_id,
                                       &offset, 1);

        if (surface->index_buffer_id.IsValid() && surface->index_count > 0) {
          rd_->DrawListBindIndexBuffer(depth_dl, surface->index_buffer_id,
                                       surface->index_format, 0);
          rd_->DrawListDrawIndexed(depth_dl, surface->index_count);
        } else {
          rd_->DrawListDraw(depth_dl, surface->vertex_count);
        }
      }
    }
  }

  DrawListScope scope(
      rd_,
      p_framebuffer.IsValid() ? p_framebuffer : rd_->GetDefaultFramebuffer(),
      p_clear_color, (use_depth_prepass_ ? false : p_clear_depth), false,
      clear);
  DrawListID dl = scope.Get();

  rd_->DrawListSetViewport(dl, 0, 0, p_screen_width, p_screen_height);

  RDPipelineID current_pipeline;

  for (uint32 i = 0; i < render_list_.Size(); ++i) {
    const RenderListItem& item = render_list_[i];

    RDPipelineID target_pipeline =
        item.disable_cull ? pipeline_no_cull_id_ : pipeline_id_;

    if (target_pipeline.GetId() != current_pipeline.GetId()) {
      rd_->DrawListBindRenderPipeline(dl, target_pipeline);
      rd_->DrawListBindUniformSet(dl, camera_set_, 0);
      rd_->DrawListBindUniformSet(dl, light_set_, 3);
      current_pipeline = target_pipeline;
    }

    RDUniformSetID mat_set = CreateMaterialUniformSet(item.material_rid);
    if (!mat_set.IsValid()) {
      continue;
    }
    rd_->DrawListBindUniformSetWithDynamicOffset(dl, mat_set, 1,
                                                 i * material_ubo_stride_);

    rd_->DrawListBindUniformSetWithDynamicOffset(dl, model_set_, 2,
                                                 i * model_ubo_stride_);

    const MeshInfo* mesh_info = mesh_storage_->MeshGetInfo(item.mesh_rid);
    if (!mesh_info || mesh_info->surfaces.IsEmpty()) {
      continue;
    }

    for (uint32 s = 0; s < mesh_info->surfaces.Size(); ++s) {
      const SurfaceInfo* surface = mesh_info->surfaces[s];
      if (!surface) {
        continue;
      }

      uint64 offset = 0;
      rd_->DrawListBindVertexBuffers(dl, &surface->vertex_buffer_id, &offset,
                                     1);

      if (surface->index_buffer_id.IsValid() && surface->index_count > 0) {
        rd_->DrawListBindIndexBuffer(dl, surface->index_buffer_id,
                                     surface->index_format, 0);
        rd_->DrawListDrawIndexed(dl, surface->index_count);
      } else {
        rd_->DrawListDraw(dl, surface->vertex_count);
      }
    }
  }
}
void SpatialRenderer::BuildRenderList() {
  render_list_.Clear();

  for (uint32 i = 0; i < spatial_items_.Size(); ++i) {
    const SpatialItem& item = spatial_items_[i];

    const Material* mat = material_storage_->MaterialGetInfo(item.material);
    if (!mat || !mat->is_valid) {
      continue;
    }

    if (mat->data_type != MaterialDataType::kSpatial) {
      ARHUD_LOG_WARN(
          "SpatialRenderer::BuildRenderList() material is not spatial type, "
          "skipping");
      continue;
    }

    uint8 priority = 0;
    RID next_pass_rid;
    if (mat->data) {
      auto* spatial_data = static_cast<SpatialMaterialData*>(mat->data);
      priority = spatial_data->priority;
      next_pass_rid = spatial_data->next_pass;
    }

    RenderListItem rl_item;
    rl_item.material_rid = item.material;
    rl_item.mesh_rid = item.mesh;
    memcpy(rl_item.transform, item.transform, sizeof(float) * 16);
    rl_item.priority = priority;
    rl_item.disable_cull = item.disable_cull;
    rl_item.next_material_rid = next_pass_rid;
    render_list_.PushBack(rl_item);

    RID next_rid = next_pass_rid;
    while (next_rid.IsValid()) {
      const Material* next_mat = material_storage_->MaterialGetInfo(next_rid);
      if (!next_mat || !next_mat->is_valid || !next_mat->data) {
        break;
      }

      if (next_mat->data_type != MaterialDataType::kSpatial) {
        break;
      }

      auto* next_spatial = static_cast<SpatialMaterialData*>(next_mat->data);

      RenderListItem pass_item;
      pass_item.material_rid = next_rid;
      pass_item.mesh_rid = item.mesh;
      memcpy(pass_item.transform, item.transform, sizeof(float) * 16);
      pass_item.priority = next_spatial->priority;
      pass_item.disable_cull = item.disable_cull;
      pass_item.next_material_rid = next_spatial->next_pass;
      render_list_.PushBack(pass_item);

      next_rid = next_spatial->next_pass;
    }
  }

  if (render_list_.Size() > 1) {
    struct SortHelper {
      static int Compare(const void* p_a, const void* p_b) {
        const RenderListItem* a = static_cast<const RenderListItem*>(p_a);
        const RenderListItem* b = static_cast<const RenderListItem*>(p_b);
        if (a->priority != b->priority) {
          return (a->priority > b->priority) - (a->priority < b->priority);
        }
        uintptr_t ra = a->material_rid.GetId();
        uintptr_t rb = b->material_rid.GetId();
        return (ra > rb) - (ra < rb);
      }
    };

    qsort(render_list_.Data(), render_list_.Size(), sizeof(RenderListItem),
          SortHelper::Compare);
  }
}

RDUniformSetID SpatialRenderer::CreateMaterialUniformSet(RID p_material) {
  if (!p_material.IsValid()) {
    ARHUD_LOG_WARN(
        "SpatialRenderer::CreateMaterialUniformSet() invalid material RID");
    return RDUniformSetID();
  }

  const Material* mat = material_storage_->MaterialGetInfo(p_material);
  if (!mat || !mat->data) {
    ARHUD_LOG_WARN(
        "SpatialRenderer::CreateMaterialUniformSet() material data not found");
    return RDUniformSetID();
  }

  MaterialData* mat_data = mat->data;

  MaterialUniformSetCacheKey key;
  key.material_rid = p_material;
  key.version = mat_data->version;

  RDUniformSetID* cached = material_uniform_set_cache_.GetPtr(key);
  if (cached != nullptr && cached->IsValid()) {
    return *cached;
  }

  LocalVector<MaterialUniformSetCacheKey> old_keys;
  for (auto it = material_uniform_set_cache_.Begin();
       it != material_uniform_set_cache_.End(); ++it) {
    auto pair = *it;
    if (pair.key.material_rid == p_material) {
      if (pair.value.IsValid()) {
        rd_->UniformSetFree(pair.value);
      }
      old_keys.PushBack(pair.key);
    }
  }
  for (uint32 i = 0; i < old_keys.Size(); ++i) {
    material_uniform_set_cache_.Erase(old_keys[i]);
  }

  LocalVector<BoundUniform> bound_uniforms;
  bound_uniforms.Resize(2);

  bound_uniforms[0].type = UniformType::kSamplerWithTexture;
  bound_uniforms[0].binding = 0;
  bound_uniforms[0].immutable_sampler = false;

  if (mat_data->texture_count > 0 && mat_data->texture_rids.Size() > 0 &&
      mat_data->texture_rids[0].IsValid()) {
    RDTextureID tex_rd_id =
        texture_storage_->TextureGetRDId(mat_data->texture_rids[0]);
    if (tex_rd_id.IsValid()) {
      TextureID tex_driver_id = rd_->TextureGetDriverId(tex_rd_id);
      bound_uniforms[0].ids.PushBack(BufferID(tex_driver_id.GetId()));
    }
  }

  if (bound_uniforms[0].ids.IsEmpty() && default_texture_rid_.IsValid()) {
    RDTextureID tex_rd_id =
        texture_storage_->TextureGetRDId(default_texture_rid_);
    if (tex_rd_id.IsValid()) {
      TextureID tex_driver_id = rd_->TextureGetDriverId(tex_rd_id);
      bound_uniforms[0].ids.PushBack(BufferID(tex_driver_id.GetId()));
    }
  }

  bound_uniforms[1].type = UniformType::kUniformBufferDynamic;
  bound_uniforms[1].binding = 1;
  bound_uniforms[1].ids.PushBack(rd_->BufferGetDriverId(material_ubo_));
  bound_uniforms[1].range_size = 4;

  RDUniformSetID new_set = rd_->UniformSetCreate(
      VectorView<BoundUniform>(bound_uniforms.Data(), bound_uniforms.Size()),
      shader_rd_id_, 1);

  if (!new_set.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "SpatialRenderer::CreateMaterialUniformSet() UniformSetCreate failed");
    return RDUniformSetID();
  }

  material_uniform_set_cache_.Insert(key, new_set);

  return new_set;
}

}  // namespace arhud
