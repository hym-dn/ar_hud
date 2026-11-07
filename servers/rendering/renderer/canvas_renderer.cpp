

#include "canvas_renderer.h"

#include "draw_list_scope.h"
#include "io/logger.h"
#include "os/memory.h"
#include "shaders/hud_canvas.h"
#include "shaders/hud_text.h"
#include "storage/font_storage.h"
#include "storage/material_storage.h"
#include "storage/mesh_storage.h"
#include "storage/shader_storage.h"
#include "storage/texture_storage.h"

namespace arhud {

CanvasRenderer::~CanvasRenderer() {
  if (initialized_) {
    Finalize();
  }
}

Error CanvasRenderer::Initialize(RenderingDevice* p_rd,
                                 ShaderStorage* p_shader_storage,
                                 MaterialStorage* p_material_storage,
                                 MeshStorage* p_mesh_storage,
                                 TextureStorage* p_texture_storage,
                                 FontStorage* p_font_storage) {
  if (initialized_) {
    ARHUD_LOG_ERROR("kAlreadyExists",
                    "CanvasRenderer::Initialize() already initialized");
    return Error::kAlreadyExists;
  }

  if (!p_rd || !p_rd->IsInitialized() || !p_shader_storage ||
      !p_material_storage || !p_mesh_storage || !p_texture_storage ||
      !p_font_storage) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::Initialize() invalid parameters");
    return Error::kFailed;
  }

  rd_ = p_rd;
  shader_storage_ = p_shader_storage;
  material_storage_ = p_material_storage;
  mesh_storage_ = p_mesh_storage;
  texture_storage_ = p_texture_storage;
  font_storage_ = p_font_storage;

  LocalVector<VertexAttribute> attribs;
  attribs.Resize(3);
  attribs[0].binding = 0;
  attribs[0].location = 0;
  attribs[0].offset = 0;
  attribs[0].format = DataFormat::kR32G32Sfloat;
  attribs[0].stride = kBatchVertexStride * sizeof(float);
  attribs[0].frequency = VertexFrequency::kVertex;

  attribs[1].binding = 0;
  attribs[1].location = 1;
  attribs[1].offset = 8;
  attribs[1].format = DataFormat::kR32G32Sfloat;
  attribs[1].stride = kBatchVertexStride * sizeof(float);
  attribs[1].frequency = VertexFrequency::kVertex;

  attribs[2].binding = 0;
  attribs[2].location = 2;
  attribs[2].offset = 16;
  attribs[2].format = DataFormat::kR32G32B32A32Sfloat;
  attribs[2].stride = kBatchVertexStride * sizeof(float);
  attribs[2].frequency = VertexFrequency::kVertex;

  vertex_format_id_ = rd_->VertexFormatCreate(
      VectorView<VertexAttribute>(attribs.Data(), attribs.Size()));
  if (!vertex_format_id_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::Initialize() VertexFormatCreate failed");
    return Error::kFailed;
  }

  LocalVector<ShaderStageSource> stages;
  stages.Resize(2);
  stages[0].stage = ShaderStage::kVertex;
  stages[0].source = kHudCanvasVert;
  stages[1].stage = ShaderStage::kFragment;
  stages[1].source = kHudCanvasFrag;

  LocalVector<ShaderUniform> uniforms;
  uniforms.Resize(2);

  uniforms[0].type = UniformType::kUniformBuffer;
  uniforms[0].writable = false;
  uniforms[0].binding = 0;
  uniforms[0].stages = BitField<ShaderStage>(ShaderStage::kVertex);
  uniforms[0].length = 0;
  uniforms[0].size = 64;
  uniforms[0].name = "ProjectionBlock";

  uniforms[1].type = UniformType::kSamplerWithTexture;
  uniforms[1].writable = false;
  uniforms[1].binding = 0;
  uniforms[1].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  uniforms[1].length = 0;
  uniforms[1].size = 0;
  uniforms[1].name = "u_texture";

  shader_rid_ = shader_storage_->ShaderCreateFromSource("hud_canvas", stages,
                                                        uniforms, 0);

  if (!shader_rid_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::Initialize() ShaderCreateFromSource failed");
    return Error::kFailed;
  }

  shader_rd_id_ = shader_storage_->ShaderGetRDId(shader_rid_);
  if (!shader_rd_id_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::Initialize() shader compilation failed");
    return Error::kFailed;
  }

  PipelineRasterizationState raster;
  raster.cull_mode = PolygonCullMode::kDisabled;
  raster.front_face = PolygonFrontFace::kCounterClockwise;

  PipelineMultisampleState multisample;
  multisample.sample_count = TextureSamples::k1;

  PipelineDepthStencilState depth_stencil;
  depth_stencil.enable_depth_test = false;
  depth_stencil.enable_depth_write = false;

  PipelineColorBlendState blend = PipelineColorBlendState::CreateBlend(1);

  pipeline_id_ = rd_->RenderPipelineCreate(
      shader_rd_id_, vertex_format_id_, RenderPrimitive::kTriangles, raster,
      multisample, depth_stencil, blend, BitField<PipelineDynamicStateFlags>(),
      rd_->GetDefaultRenderPass());

  if (!pipeline_id_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::Initialize() RenderPipelineCreate failed");
    return Error::kFailed;
  }

  projection_ubo_ = rd_->BufferCreate(
      64, BitField<BufferUsageBits>(BufferUsageBits::kUniform),
      MemoryAllocationType::kCpu);

  if (!projection_ubo_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed", "CanvasRenderer::Initialize() projection UBO create failed");
    return Error::kFailed;
  }

  {
    BoundUniform bound;
    bound.type = UniformType::kUniformBuffer;
    bound.binding = 0;
    bound.ids.PushBack(rd_->BufferGetDriverId(projection_ubo_));

    projection_set_ = rd_->UniformSetCreate(VectorView<BoundUniform>(&bound, 1),
                                            shader_rd_id_, 0);

    if (!projection_set_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "CanvasRenderer::Initialize() projection UniformSet create failed");
      return Error::kFailed;
    }
  }

  batch_vbo_ = rd_->BufferCreate(
      kBatchVboInitialSize,
      BitField<BufferUsageBits>(BufferUsageBits::kVertex) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kGpu);

  if (!batch_vbo_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::Initialize() batch VBO create failed");
    return Error::kFailed;
  }
  batch_vbo_size_ = kBatchVboInitialSize;

  uint8 white_pixel[4] = {255, 255, 255, 255};
  default_texture_rid_ = texture_storage_->TextureCreateFromData(
      "default_white", white_pixel, sizeof(white_pixel), 1, 1,
      DataFormat::kR8G8B8A8Unorm);
  if (!default_texture_rid_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::Initialize() default texture create failed");
    return Error::kFailed;
  }

  initialized_ = true;
  ARHUD_LOG_INFO("CanvasRenderer::Initialize() success");

  Error text_err = InitializeTextPipeline();
  if (text_err != Error::kOK) {
    ARHUD_LOG_WARN(
        "CanvasRenderer::Initialize() text pipeline init failed (non-fatal)");
  }

  return Error::kOK;
}

void CanvasRenderer::Finalize() {
  if (!initialized_) {
    return;
  }

  FinalizeTextPipeline();

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

  if (batch_vbo_.IsValid()) {
    rd_->BufferFree(batch_vbo_);
    batch_vbo_ = RDBufferID();
    batch_vbo_size_ = 0;
  }

  if (projection_set_.IsValid()) {
    rd_->UniformSetFree(projection_set_);
    projection_set_ = RDUniformSetID();
  }

  if (projection_ubo_.IsValid()) {
    rd_->BufferFree(projection_ubo_);
    projection_ubo_ = RDBufferID();
  }

  if (pipeline_id_.IsValid()) {
    rd_->PipelineFree(pipeline_id_);
    pipeline_id_ = RDPipelineID();
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

  canvas_items_.Clear();
  batches_.Clear();

  rd_ = nullptr;
  shader_storage_ = nullptr;
  material_storage_ = nullptr;
  mesh_storage_ = nullptr;
  texture_storage_ = nullptr;
  font_storage_ = nullptr;

  initialized_ = false;
  ARHUD_LOG_INFO("CanvasRenderer::Finalize() done");
}

void CanvasRenderer::CanvasItemPush(const CanvasItem& p_item) {
  canvas_items_.PushBack(p_item);
}

void CanvasRenderer::CanvasItemsClear() {
  canvas_items_.Clear();
  batches_.Clear();
}

void CanvasRenderer::TextItemPush(const TextItem& p_item) {
  text_items_.PushBack(p_item);
}

void CanvasRenderer::TextItemsClear() {
  text_items_.Clear();
}

void CanvasRenderer::RenderCanvasItems(const float* p_projection,
                                       uint32 p_screen_width,
                                       uint32 p_screen_height,
                                       bool p_clear_color,
                                       const float p_clear_color_value[4],
                                       RDFramebufferID p_framebuffer) {
  if (canvas_items_.IsEmpty()) {
    return;
  }

  SortCanvasItems();
  BuildBatches();

  rd_->BufferUpdate(projection_ubo_, 0, sizeof(float) * 16, p_projection);

  uint32 total_data_size = 0;
  for (uint32 b = 0; b < batches_.Size(); ++b) {
    total_data_size +=
        static_cast<uint32>(batches_[b].vertices.Size() * sizeof(float));
  }

  if (total_data_size == 0) {
    return;
  }

  EnsureBatchVBOSize(total_data_size);

  uint32 vbo_offset = 0;
  for (uint32 b = 0; b < batches_.Size(); ++b) {
    const CanvasBatch& batch = batches_[b];
    if (batch.vertices.IsEmpty()) {
      continue;
    }

    uint32 data_size =
        static_cast<uint32>(batch.vertices.Size() * sizeof(float));
    rd_->BufferUpdate(batch_vbo_, vbo_offset, data_size, batch.vertices.Data());
    vbo_offset += data_size;
  }

  float default_clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float* clear =
      p_clear_color_value ? p_clear_color_value : default_clear;
  DrawListScope scope(
      rd_,
      p_framebuffer.IsValid() ? p_framebuffer : rd_->GetDefaultFramebuffer(),
      p_clear_color, false, false, clear);
  DrawListID dl = scope.Get();

  rd_->DrawListSetViewport(dl, 0, 0, p_screen_width, p_screen_height);
  rd_->DrawListBindRenderPipeline(dl, pipeline_id_);
  rd_->DrawListBindUniformSet(dl, projection_set_, 0);

  vbo_offset = 0;
  for (uint32 b = 0; b < batches_.Size(); ++b) {
    const CanvasBatch& batch = batches_[b];
    if (batch.vertex_count == 0) {
      if (!batch.vertices.IsEmpty()) {
        vbo_offset +=
            static_cast<uint32>(batch.vertices.Size() * sizeof(float));
      }
      continue;
    }

    rd_->DrawListBindUniformSet(dl, batch.material_uniform_set, 1);

    RDBufferID vertex_buffers[] = {batch_vbo_};
    uint64 offsets[] = {vbo_offset};
    rd_->DrawListBindVertexBuffers(dl, vertex_buffers, offsets, 1);

    rd_->DrawListDraw(dl, batch.vertex_count);

    vbo_offset += static_cast<uint32>(batch.vertices.Size() * sizeof(float));
  }
}

void CanvasRenderer::SortCanvasItems() {
  if (canvas_items_.Size() <= 1) {
    return;
  }

  struct SortHelper {
    static int Compare(const void* p_a, const void* p_b) {
      int32 za = static_cast<const CanvasItem*>(p_a)->z_order;
      int32 zb = static_cast<const CanvasItem*>(p_b)->z_order;
      return (za > zb) - (za < zb);
    }
  };

  qsort(canvas_items_.Data(), canvas_items_.Size(), sizeof(CanvasItem),
        SortHelper::Compare);
}

void CanvasRenderer::BuildBatches() {
  batches_.Clear();

  if (canvas_items_.IsEmpty()) {
    return;
  }

  RID current_material;
  uint32 current_batch = UINT32_MAX;

  for (uint32 i = 0; i < canvas_items_.Size(); ++i) {
    const CanvasItem& item = canvas_items_[i];

    if (item.material != current_material || current_batch == UINT32_MAX) {
      CanvasBatch batch;
      batch.material = item.material;
      batch.material_uniform_set = CreateMaterialUniformSet(item.material);

      if (!batch.material_uniform_set.IsValid()) {
        continue;
      }

      current_batch = batches_.Size();
      batches_.PushBack(batch);
      current_material = item.material;
    }

    CanvasBatch& batch = batches_[current_batch];
    batch.item_indices.PushBack(i);

    if (item.nine_patch_enabled) {
      AppendNinePatchVertices(item, batch.vertices);
    } else {
      AppendItemVertices(item, batch.vertices);
    }

    batch.vertex_count =
        static_cast<uint32>(batch.vertices.Size()) / kBatchVertexStride;
  }
}

RDUniformSetID CanvasRenderer::CreateMaterialUniformSet(RID p_material) {
  if (!p_material.IsValid()) {
    ARHUD_LOG_WARN(
        "CanvasRenderer::CreateMaterialUniformSet() invalid material RID");
    return RDUniformSetID();
  }

  const Material* mat = material_storage_->MaterialGetInfo(p_material);
  if (!mat || !mat->data) {
    ARHUD_LOG_WARN(
        "CanvasRenderer::CreateMaterialUniformSet() material data not found");
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
  bound_uniforms.Resize(1);

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

  RDUniformSetID new_set = rd_->UniformSetCreate(
      VectorView<BoundUniform>(bound_uniforms.Data(), bound_uniforms.Size()),
      shader_rd_id_, 1);

  if (!new_set.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::CreateMaterialUniformSet() UniformSetCreate failed");
    return RDUniformSetID();
  }

  material_uniform_set_cache_.Insert(key, new_set);

  return new_set;
}

void CanvasRenderer::AppendItemVertices(const CanvasItem& p_item,
                                        LocalVector<float>& p_vertices) {
  const MeshInfo* mesh_info = mesh_storage_->MeshGetInfo(p_item.mesh);
  if (!mesh_info || mesh_info->surfaces.IsEmpty()) {
    return;
  }

  const SurfaceInfo* surface = mesh_info->surfaces[0];
  if (!surface || surface->cpu_vertex_data.IsEmpty()) {
    return;
  }

  const float* src_vertices =
      reinterpret_cast<const float*>(surface->cpu_vertex_data.Data());
  uint32 src_vertex_count = surface->vertex_count;
  uint32 src_stride_float = surface->vertex_stride / sizeof(float);

  auto transform_pos = [&](float x, float y) -> void {
    const float* m = p_item.transform;
    float rx = m[0] * x + m[4] * y + m[12];
    float ry = m[1] * x + m[5] * y + m[13];
    p_vertices.PushBack(rx);
    p_vertices.PushBack(ry);
  };

  if (surface->index_count > 0 && !surface->cpu_index_data.IsEmpty()) {
    uint32 tri_count = surface->index_count / 3;
    const uint8* idx_data = surface->cpu_index_data.Data();

    for (uint32 t = 0; t < tri_count; ++t) {
      uint32 i0, i1, i2;
      if (surface->index_format == IndexBufferFormat::kUint16) {
        const uint16* idx16 = reinterpret_cast<const uint16*>(idx_data);
        i0 = idx16[t * 3 + 0];
        i1 = idx16[t * 3 + 1];
        i2 = idx16[t * 3 + 2];
      } else {
        const uint32* idx32 = reinterpret_cast<const uint32*>(idx_data);
        i0 = idx32[t * 3 + 0];
        i1 = idx32[t * 3 + 1];
        i2 = idx32[t * 3 + 2];
      }

      uint32 indices[3] = {i0, i1, i2};
      for (uint32 vi = 0; vi < 3; ++vi) {
        uint32 idx = indices[vi];
        const float* v = src_vertices + idx * src_stride_float;
        transform_pos(v[0], v[1]);
        p_vertices.PushBack(v[2]);
        p_vertices.PushBack(v[3]);
        p_vertices.PushBack(1.0f);
        p_vertices.PushBack(1.0f);
        p_vertices.PushBack(1.0f);
        p_vertices.PushBack(p_item.opacity);
      }
    }
  } else {
    for (uint32 i = 0; i < src_vertex_count; ++i) {
      const float* v = src_vertices + i * src_stride_float;
      transform_pos(v[0], v[1]);
      p_vertices.PushBack(v[2]);
      p_vertices.PushBack(v[3]);
      p_vertices.PushBack(1.0f);
      p_vertices.PushBack(1.0f);
      p_vertices.PushBack(1.0f);
      p_vertices.PushBack(p_item.opacity);
    }
  }
}

void CanvasRenderer::AppendNinePatchVertices(const CanvasItem& p_item,
                                             LocalVector<float>& p_vertices) {
  const MeshInfo* mesh_info = mesh_storage_->MeshGetInfo(p_item.mesh);
  if (!mesh_info || mesh_info->surfaces.IsEmpty()) {
    return;
  }

  const SurfaceInfo* surface = mesh_info->surfaces[0];
  if (!surface) {
    return;
  }

  uint32 tex_width = 1;
  uint32 tex_height = 1;
  const Material* mat = material_storage_->MaterialGetInfo(p_item.material);
  if (mat && mat->data && mat->data->texture_count > 0 &&
      mat->data->texture_rids.Size() > 0 &&
      mat->data->texture_rids[0].IsValid()) {
    const TextureInfo* tex_info =
        texture_storage_->TextureGetInfo(mat->data->texture_rids[0]);
    if (tex_info) {
      tex_width = tex_info->width > 0 ? tex_info->width : 1;
      tex_height = tex_info->height > 0 ? tex_info->height : 1;
    }
  }

  float quad_w = surface->aabb.size.x;
  float quad_h = surface->aabb.size.y;
  float origin_x = surface->aabb.position.x;
  float origin_y = surface->aabb.position.y;

  float ml = p_item.nine_patch_margin[0];
  float mt = p_item.nine_patch_margin[1];
  float mr = p_item.nine_patch_margin[2];
  float mb = p_item.nine_patch_margin[3];

  if (ml + mr > quad_w) {
    float scale = quad_w / (ml + mr);
    ml *= scale;
    mr *= scale;
  }
  if (mt + mb > quad_h) {
    float scale = quad_h / (mt + mb);
    mt *= scale;
    mb *= scale;
  }

  float x0 = origin_x;
  float x1 = origin_x + ml;
  float x2 = origin_x + quad_w - mr;
  float x3 = origin_x + quad_w;

  float y0 = origin_y;
  float y1 = origin_y + mt;
  float y2 = origin_y + quad_h - mb;
  float y3 = origin_y + quad_h;

  float u0 = 0.0f;
  float u1 = ml / static_cast<float>(tex_width);
  float u2 = 1.0f - mr / static_cast<float>(tex_width);
  float u3 = 1.0f;

  float v0 = 0.0f;
  float v1 = mt / static_cast<float>(tex_height);
  float v2 = 1.0f - mb / static_cast<float>(tex_height);
  float v3 = 1.0f;

  float px[4] = {x0, x1, x2, x3};
  float py[4] = {y0, y1, y2, y3};
  float pu[4] = {u0, u1, u2, u3};
  float pv[4] = {v0, v1, v2, v3};

  auto transform_pos = [&](float x, float y) -> void {
    const float* m = p_item.transform;
    float rx = m[0] * x + m[4] * y + m[12];
    float ry = m[1] * x + m[5] * y + m[13];
    p_vertices.PushBack(rx);
    p_vertices.PushBack(ry);
  };

  for (uint32 row = 0; row < 3; ++row) {
    for (uint32 col = 0; col < 3; ++col) {
      float qx0 = px[col];
      float qx1 = px[col + 1];
      float qy0 = py[row];
      float qy1 = py[row + 1];
      float qu0 = pu[col];
      float qu1 = pu[col + 1];
      float qv0 = pv[row];
      float qv1 = pv[row + 1];

      struct QuadVertex {
        float x, y, u, v;
      };

      QuadVertex quad_verts[6] = {
          {qx0, qy0, qu0, qv0}, {qx1, qy0, qu1, qv0}, {qx1, qy1, qu1, qv1},

          {qx0, qy0, qu0, qv0}, {qx1, qy1, qu1, qv1}, {qx0, qy1, qu0, qv1},
      };

      for (uint32 vi = 0; vi < 6; ++vi) {
        transform_pos(quad_verts[vi].x, quad_verts[vi].y);
        p_vertices.PushBack(quad_verts[vi].u);
        p_vertices.PushBack(quad_verts[vi].v);
        p_vertices.PushBack(1.0f);
        p_vertices.PushBack(1.0f);
        p_vertices.PushBack(1.0f);
        p_vertices.PushBack(p_item.opacity);
      }
    }
  }
}

void CanvasRenderer::EnsureBatchVBOSize(uint32 p_required_size) {
  if (p_required_size <= batch_vbo_size_) {
    return;
  }

  uint32 new_size = p_required_size * 2;

  if (batch_vbo_.IsValid()) {
    rd_->BufferFree(batch_vbo_);
  }

  batch_vbo_ = rd_->BufferCreate(
      new_size,
      BitField<BufferUsageBits>(BufferUsageBits::kVertex) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kGpu);

  if (!batch_vbo_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::EnsureBatchVBOSize() VBO reallocation failed");
    batch_vbo_size_ = 0;
    return;
  }

  batch_vbo_size_ = new_size;
}

Error CanvasRenderer::InitializeTextPipeline() {
  LocalVector<ShaderStageSource> text_stages;
  text_stages.Resize(2);
  text_stages[0].stage = ShaderStage::kVertex;
  text_stages[0].source = kHudTextVert;
  text_stages[1].stage = ShaderStage::kFragment;
  text_stages[1].source = kHudTextFrag;

  LocalVector<ShaderUniform> text_uniforms;
  text_uniforms.Resize(3);

  text_uniforms[0].type = UniformType::kUniformBuffer;
  text_uniforms[0].writable = false;
  text_uniforms[0].binding = 0;
  text_uniforms[0].stages = BitField<ShaderStage>(ShaderStage::kVertex);
  text_uniforms[0].length = 0;
  text_uniforms[0].size = 64;
  text_uniforms[0].name = "ProjectionBlock";

  text_uniforms[1].type = UniformType::kSamplerWithTexture;
  text_uniforms[1].writable = false;
  text_uniforms[1].binding = 0;
  text_uniforms[1].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  text_uniforms[1].length = 0;
  text_uniforms[1].size = 0;
  text_uniforms[1].name = "u_font_sdf";

  text_uniforms[2].type = UniformType::kUniformBuffer;
  text_uniforms[2].writable = false;
  text_uniforms[2].binding = 1;
  text_uniforms[2].stages = BitField<ShaderStage>(ShaderStage::kFragment);
  text_uniforms[2].length = 0;
  text_uniforms[2].size = 16;
  text_uniforms[2].name = "SmoothingBlock";

  text_shader_rid_ = shader_storage_->ShaderCreateFromSource(
      "hud_text", text_stages, text_uniforms, 0);

  if (!text_shader_rid_.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::InitializeTextPipeline() "
                    "ShaderCreateFromSource failed");
    return Error::kFailed;
  }

  text_shader_rd_id_ = shader_storage_->ShaderGetRDId(text_shader_rid_);
  if (!text_shader_rd_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::InitializeTextPipeline() shader compilation failed");
    return Error::kFailed;
  }

  PipelineRasterizationState raster;
  raster.cull_mode = PolygonCullMode::kDisabled;
  raster.front_face = PolygonFrontFace::kCounterClockwise;

  PipelineMultisampleState multisample;
  multisample.sample_count = TextureSamples::k1;

  PipelineDepthStencilState depth_stencil;
  depth_stencil.enable_depth_test = false;
  depth_stencil.enable_depth_write = false;

  PipelineColorBlendState blend = PipelineColorBlendState::CreateBlend(1);

  text_pipeline_id_ = rd_->RenderPipelineCreate(
      text_shader_rd_id_, vertex_format_id_, RenderPrimitive::kTriangles,
      raster, multisample, depth_stencil, blend,
      BitField<PipelineDynamicStateFlags>(), rd_->GetDefaultRenderPass());

  if (!text_pipeline_id_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::InitializeTextPipeline() RenderPipelineCreate failed");
    return Error::kFailed;
  }

  text_smoothing_ubo_ = rd_->BufferCreate(
      16, BitField<BufferUsageBits>(BufferUsageBits::kUniform),
      MemoryAllocationType::kCpu);

  if (!text_smoothing_ubo_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::InitializeTextPipeline() smoothing UBO create failed");
    return Error::kFailed;
  }

  float smoothing_data[4] = {kDefaultSmoothing, 0.0f, 0.0f, 0.0f};
  rd_->BufferUpdate(text_smoothing_ubo_, 0, sizeof(smoothing_data),
                    smoothing_data);

  {
    BoundUniform bound;
    bound.type = UniformType::kUniformBuffer;
    bound.binding = 1;
    bound.ids.PushBack(rd_->BufferGetDriverId(text_smoothing_ubo_));

    text_smoothing_set_ = rd_->UniformSetCreate(
        VectorView<BoundUniform>(&bound, 1), text_shader_rd_id_, 2);

    if (!text_smoothing_set_.IsValid()) {
      ARHUD_LOG_ERROR("kFailed",
                      "CanvasRenderer::InitializeTextPipeline() smoothing "
                      "UniformSet create failed");
      return Error::kFailed;
    }
  }

  text_vbo_ = rd_->BufferCreate(
      kTextVboInitialSize,
      BitField<BufferUsageBits>(BufferUsageBits::kVertex) |
          BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
      MemoryAllocationType::kGpu);

  if (!text_vbo_.IsValid()) {
    ARHUD_LOG_ERROR(
        "kFailed",
        "CanvasRenderer::InitializeTextPipeline() text VBO create failed");
    return Error::kFailed;
  }
  text_vbo_size_ = kTextVboInitialSize;

  ARHUD_LOG_INFO("CanvasRenderer::InitializeTextPipeline() success");
  return Error::kOK;
}

void CanvasRenderer::FinalizeTextPipeline() {
  if (text_vbo_.IsValid()) {
    rd_->BufferFree(text_vbo_);
    text_vbo_ = RDBufferID();
    text_vbo_size_ = 0;
  }

  if (text_smoothing_set_.IsValid()) {
    rd_->UniformSetFree(text_smoothing_set_);
    text_smoothing_set_ = RDUniformSetID();
  }

  if (text_smoothing_ubo_.IsValid()) {
    rd_->BufferFree(text_smoothing_ubo_);
    text_smoothing_ubo_ = RDBufferID();
  }

  if (cached_font_texture_set_.IsValid()) {
    rd_->UniformSetFree(cached_font_texture_set_);
    cached_font_texture_set_ = RDUniformSetID();
  }
  cached_font_rid_ = RID();

  if (text_pipeline_id_.IsValid()) {
    rd_->PipelineFree(text_pipeline_id_);
    text_pipeline_id_ = RDPipelineID();
  }

  if (text_shader_rid_.IsValid()) {
    shader_storage_->ShaderFree(text_shader_rid_);
    text_shader_rid_ = RID();
    text_shader_rd_id_ = RDShaderID();
  }
}

RDUniformSetID CanvasRenderer::CreateFontTextureUniformSet(RID p_font) {
  if (!p_font.IsValid() || !font_storage_) {
    return RDUniformSetID();
  }

  RID texture_rid = font_storage_->FontGetTextureRID(p_font);
  if (!texture_rid.IsValid()) {
    ARHUD_LOG_WARN(
        "CanvasRenderer::CreateFontTextureUniformSet() font texture RID "
        "invalid");
    return RDUniformSetID();
  }

  RDTextureID tex_rd_id = texture_storage_->TextureGetRDId(texture_rid);
  if (!tex_rd_id.IsValid()) {
    ARHUD_LOG_WARN(
        "CanvasRenderer::CreateFontTextureUniformSet() font texture RD ID "
        "invalid");
    return RDUniformSetID();
  }

  TextureID tex_driver_id = rd_->TextureGetDriverId(tex_rd_id);

  LocalVector<BoundUniform> bound_uniforms;
  bound_uniforms.Resize(1);

  bound_uniforms[0].type = UniformType::kSamplerWithTexture;
  bound_uniforms[0].binding = 0;
  bound_uniforms[0].immutable_sampler = false;

  bound_uniforms[0].ids.PushBack(BufferID(tex_driver_id.GetId()));

  RDUniformSetID uniform_set = rd_->UniformSetCreate(
      VectorView<BoundUniform>(bound_uniforms.Data(), bound_uniforms.Size()),
      text_shader_rd_id_, 1);

  if (!uniform_set.IsValid()) {
    ARHUD_LOG_ERROR("kFailed",
                    "CanvasRenderer::CreateFontTextureUniformSet() "
                    "UniformSetCreate failed");
  }

  return uniform_set;
}

void CanvasRenderer::SortTextItems() {
  if (text_items_.Size() <= 1) {
    return;
  }

  struct SortHelper {
    static int Compare(const void* p_a, const void* p_b) {
      int32 za = static_cast<const TextItem*>(p_a)->z_order;
      int32 zb = static_cast<const TextItem*>(p_b)->z_order;
      return (za > zb) - (za < zb);
    }
  };

  qsort(text_items_.Data(), text_items_.Size(), sizeof(TextItem),
        SortHelper::Compare);
}

void CanvasRenderer::RenderTextItems(const float* p_projection,
                                     uint32 p_screen_width,
                                     uint32 p_screen_height,
                                     RDFramebufferID p_framebuffer) {
  if (!initialized_ || !text_pipeline_id_.IsValid() || text_items_.IsEmpty()) {
    return;
  }

  SortTextItems();

  struct TextDrawInfo {
    uint32 vertex_count = 0;
    uint32 vbo_offset = 0;
    RDUniformSetID font_uniform_set;
  };

  LocalVector<TextDrawInfo> draw_infos;
  draw_infos.Resize(text_items_.Size());

  LocalVector<float> all_vertices;
  uint32 total_data_size = 0;

  for (uint32 ti = 0; ti < text_items_.Size(); ++ti) {
    const TextItem& item = text_items_[ti];

    if (!item.text || item.length == 0 || !item.font.IsValid()) {
      continue;
    }

    const FontInfo* font_info = font_storage_->FontGetInfo(item.font);
    if (!font_info || !font_info->is_valid) {
      continue;
    }

    uint32 start_idx = static_cast<uint32>(all_vertices.Size());
    float cursor_x = item.x;
    float cursor_y = item.y;

    for (uint32 i = 0; i < item.length; ++i) {
      uint32 charcode = item.text[i];
      const GlyphInfo* glyph = font_storage_->FontGetGlyph(item.font, charcode);
      if (!glyph) {
        continue;
      }

      float x0 = cursor_x + glyph->offset_x;
      float y0 = cursor_y - glyph->offset_y;
      float x1 = x0 + glyph->width;
      float y1 = y0 + glyph->height;

      float u0 = glyph->uv_x;
      float v0 = glyph->uv_y;
      float u1 = glyph->uv_x + glyph->uv_w;
      float v1 = glyph->uv_y + glyph->uv_h;

      float quad_verts[6][8] = {
          {x0, y0, u0, v0, item.color[0], item.color[1], item.color[2],
           item.color[3]},
          {x1, y0, u1, v0, item.color[0], item.color[1], item.color[2],
           item.color[3]},
          {x1, y1, u1, v1, item.color[0], item.color[1], item.color[2],
           item.color[3]},

          {x0, y0, u0, v0, item.color[0], item.color[1], item.color[2],
           item.color[3]},
          {x1, y1, u1, v1, item.color[0], item.color[1], item.color[2],
           item.color[3]},
          {x0, y1, u0, v1, item.color[0], item.color[1], item.color[2],
           item.color[3]},
      };

      for (uint32 vi = 0; vi < 6; ++vi) {
        for (uint32 fi = 0; fi < 8; ++fi) {
          all_vertices.PushBack(quad_verts[vi][fi]);
        }
      }

      cursor_x += glyph->advance;
    }

    uint32 vert_count = (static_cast<uint32>(all_vertices.Size()) - start_idx) /
                        kBatchVertexStride;
    uint32 data_size = vert_count * kBatchVertexStride * sizeof(float);

    draw_infos[ti].vertex_count = vert_count;
    draw_infos[ti].vbo_offset = total_data_size;
    total_data_size += data_size;

    if (item.font == cached_font_rid_ && cached_font_texture_set_.IsValid()) {
      draw_infos[ti].font_uniform_set = cached_font_texture_set_;
    } else {
      if (cached_font_texture_set_.IsValid()) {
        rd_->UniformSetFree(cached_font_texture_set_);
        cached_font_texture_set_ = RDUniformSetID();
      }

      RDUniformSetID font_uniform_set = CreateFontTextureUniformSet(item.font);
      if (!font_uniform_set.IsValid()) {
        ARHUD_LOG_ERROR("kFailed",
                        "CanvasRenderer::RenderTextItems() font UniformSet "
                        "creation failed");
        draw_infos[ti].vertex_count = 0;
        continue;
      }

      cached_font_texture_set_ = font_uniform_set;
      cached_font_rid_ = item.font;
      draw_infos[ti].font_uniform_set = font_uniform_set;
    }
  }

  if (all_vertices.IsEmpty()) {
    return;
  }

  if (total_data_size > text_vbo_size_) {
    uint32 new_size = total_data_size * 2;

    if (text_vbo_.IsValid()) {
      rd_->BufferFree(text_vbo_);
    }

    text_vbo_ = rd_->BufferCreate(
        new_size,
        BitField<BufferUsageBits>(BufferUsageBits::kVertex) |
            BitField<BufferUsageBits>(BufferUsageBits::kTransferTo),
        MemoryAllocationType::kGpu);

    if (!text_vbo_.IsValid()) {
      ARHUD_LOG_ERROR(
          "kFailed",
          "CanvasRenderer::RenderTextItems() text VBO allocation failed");
      text_vbo_size_ = 0;
      return;
    }
    text_vbo_size_ = new_size;
  }

  rd_->BufferUpdate(text_vbo_, 0, total_data_size, all_vertices.Data());

  rd_->BufferUpdate(projection_ubo_, 0, sizeof(float) * 16, p_projection);

  DrawListScope scope(
      rd_,
      p_framebuffer.IsValid() ? p_framebuffer : rd_->GetDefaultFramebuffer(),
      false, false, nullptr);
  DrawListID dl = scope.Get();

  if (!scope.IsActive()) {
    ARHUD_LOG_ERROR("kFailed", "RenderTextItems: DrawListBegin failed!");
    return;
  }

  rd_->DrawListSetViewport(dl, 0, 0, p_screen_width, p_screen_height);
  rd_->DrawListBindRenderPipeline(dl, text_pipeline_id_);
  rd_->DrawListBindUniformSet(dl, projection_set_, 0);
  rd_->DrawListBindUniformSet(dl, text_smoothing_set_, 2);

  for (uint32 ti = 0; ti < text_items_.Size(); ++ti) {
    const TextDrawInfo& info = draw_infos[ti];
    if (info.vertex_count == 0 || !info.font_uniform_set.IsValid()) {
      continue;
    }

    rd_->DrawListBindUniformSet(dl, info.font_uniform_set, 1);

    RDBufferID vertex_buffers[] = {text_vbo_};
    uint64 offsets[] = {info.vbo_offset};
    rd_->DrawListBindVertexBuffers(dl, vertex_buffers, offsets, 1);
    rd_->DrawListDraw(dl, info.vertex_count);
  }
}

}  // namespace arhud
