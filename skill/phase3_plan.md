---
name: "arhud-phase3-plan"
description: "Phase 3-7 development plan for ARHud rendering engine. Invoke when user asks about next development phase, Storage layer implementation, Renderer layer, or RDG implementation timing."
---

# ARHud 渲染引擎 Phase 3-7 开发计划

本 Skill 定义 ARHud 渲染引擎从 Phase 3 到 Phase 7 的完整开发路线，包括资源存储层、渲染器层、
场景集成、Demo 应用以及 RDG/Vulkan 后端的引入时机。

---

## 一、当前项目状态

| Phase | 描述 | 状态 |
|-------|------|:----:|
| Phase 1 | 核心框架 (core/) | ✅ 完成 |
| Phase 2 | 渲染驱动层 (RCD/RDD/RD/CommandBuffer/DrawList) | ✅ 完成 |
| Phase 3 | 资源存储层 (Storage) | ⬜ 待开发 |
| Phase 4 | 渲染器层 (Renderer) | ⬜ 待开发 |
| Phase 5 | 场景与集成 | ⬜ 待开发 |
| Phase 6 | Demo 应用 | ⬜ 待开发 |
| Phase 7 | Vulkan + RDG 完整实现 | ⬜ 待开发 |

### Phase 2 已完成的关键能力

- ✅ 三层 GPU 抽象：RCD → RDD → RD
- ✅ RDD 所有权模型：RD 内部创建/释放 RDD（对齐 Godot）
- ✅ RCD::Initialize 接受窗口参数，内部创建引导 Surface/Context
- ✅ DrawList 公共 API：DrawListBegin/End + 11 个绘制方法
- ✅ CommandBuffer 内部实现：ICommandBuffer 录制/回放（客户端不可见）
- ✅ RID 管理 + 延迟销毁 + 依赖追踪
- ✅ Staging Buffer（CPU→GPU 数据传输）
- ✅ CommandBuffer 池管理
- ✅ 独立设备创建（CreateIndependentDevice）
- ✅ GLStateCache（消除冗余 GL 调用）

---

## 二、RDG 完整实现时机决策

### 2.1 为什么不在当前阶段实现 RDG

| RDG 能力 | 当前是否需要 | 何时需要 |
|----------|:----------:|---------|
| 序列化字节流 | ❌ | Vulkan 后端时（VkCommandBuffer 需要） |
| 帧末统一编译 | ❌ | DrawCall > 500 或有 Compute→Graphics 依赖时 |
| 屏障推导 | ❌ | Vulkan 后端时（OpenGL 驱动自动同步） |
| 多 DrawList 并发 | ❌ | 多线程录制时 |
| 命令重排序 | ❌ | 复杂渲染管线时 |
| 调试录制/回放 | ⚠️ 可选 | 性能剖析/回归测试时 |

**核心判断：** HUD 场景命令量极小（< 100 DrawCall），OpenGL 驱动自动处理同步，
当前 DrawList + ICommandBuffer 录制/回放已经满足需求。RDG 完整实现是内部优化，
公共 API（DrawListBegin/End）不变，可随时替换内部实现。

### 2.2 RDG 引入触发条件

满足以下**任一**条件时启动 RDG 完整实现：

1. **开始实现 Vulkan 后端** — 屏障推导和 VkCommandBuffer 录制是刚需
2. **需要多线程渲染** — 命令队列 + 延迟录制是刚需
3. **需要命令流录制/回放** — 调试/回归测试用途
4. **DrawCall 超过 500** — 屏障合并和重排序有实际收益

### 2.3 RDG 演进路径

```
当前 (Phase 2-6): DrawList + ICommandBuffer 录制/回放
    │
    │  公共 API 不变: DrawListBegin/End/BindPipeline/Draw/...
    │
    ↓
Phase 7: RDG 完整实现
    ├── 序列化字节流（替代 ICommandBuffer 内部列表）
    ├── 帧末统一编译（替代 DrawListEnd 立即执行）
    ├── 屏障推导（Vulkan 刚需）
    ├── 多 DrawList 并发（多线程录制）
    └── 命令重排序（可选优化）
```

---

## 三、Phase 3：资源存储层

### 3.1 整体架构

Storage 层负责 GPU 资源的生命周期管理，每种资源类型有独立的 Storage 类。
Storage 通过 RD 的 RID 管理接口创建资源，不直接调用 RDD。

```
┌──────────────────────────────────────────────────────────┐
│                    ARHudCompositor                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │
│  │TextureStorage│  │ShaderStorage │  │MaterialStorage│   │
│  │  RID→Texture │  │  RID→Shader  │  │ RID→Material │   │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘   │
│         │                 │                  │           │
│  ┌──────┴───────┐  ┌──────┴───────┐                     │
│  │  MeshStorage │  │  FontStorage │                     │
│  │  RID→Mesh   │  │  RID→Font    │                     │
│  └──────┬───────┘  └──────┬───────┘                     │
│         │                 │                              │
│         └────────┬────────┘                              │
│                  ↓                                       │
│         ┌──────────────┐                                 │
│         │RenderingDevice│  ← RID 管理 + RDD 调用         │
│         └──────────────┘                                 │
└──────────────────────────────────────────────────────────┘
```

### 3.2 依赖关系与实施顺序

```
ShaderStorage ──→ MaterialStorage ──→ TextureStorage
     │                                    │
     │                                    ↓
     └──→ MeshStorage ──────────→ FontStorage
```

| 顺序 | 组件 | 依赖 | 核心功能 | 预估 |
|:----:|------|------|---------|:----:|
| 3.1 | **ShaderStorage** | RDD::ShaderCreate/Compile | GLSL 加载/编译/链接，Uniform 位置缓存 | 1 周 |
| 3.2 | **TextureStorage** | RDD::TextureCreate/Update | 2D 纹理创建/更新/释放，格式转换，Mipmap | 1 周 |
| 3.3 | **MaterialStorage** | ShaderStorage + TextureStorage | 材质定义（Shader + Uniform 参数），材质排序 | 0.5 周 |
| 3.4 | **MeshStorage** | RDD::BufferCreate | 2D 网格（Quad/Line/Rect/Polygon），VBO/IBO | 0.5 周 |
| 3.5 | **FontStorage** | TextureStorage + FreeType | SDF 字体纹理生成，字形缓存 | 1 周 |

### 3.3 关键设计决策

#### 3.3.1 Storage 与 RD 的关系

**决策：Storage 通过 RD 创建资源（利用 RD 的 RID 管理和依赖追踪）**

```cpp
// Storage 持有 RD 指针，通过 RD 的公共 API 创建资源
class TextureStorage {
    RenderingDevice *rd_ = nullptr;

    RID TextureCreate(const TextureCreateInfo &info) {
        // 调用 RD 的 RID 管理接口
        RDBufferID rd_id = rd_->TextureCreate(info);
        // Storage 内部簿记
        Texture *tex = texture_owner_.Create(rd_id);
        return tex->rid;
    }
};
```

**理由：**
- RD 的 RID 管理提供统一的生命周期管理
- RD 的依赖追踪（dependency_map）可以追踪资源间依赖
- RD 的延迟销毁（FreePending）在帧边界安全释放资源
- Storage 不需要直接接触 RDD，保持层次隔离

#### 3.3.2 Storage 接口与实现分离

```
servers/redering/storage/
├── shader_storage.h          ← 接口定义
├── texture_storage.h         ← 接口定义
├── material_storage.h        ← 接口定义
├── mesh_storage.h            ← 接口定义
└── font_storage.h            ← 接口定义

drivers/gl/storage/
├── shader_storage_gl.h/.cpp  ← GL 实现
├── texture_storage_gl.h/.cpp ← GL 实现
├── material_storage_gl.h/.cpp← GL 实现
├── mesh_storage_gl.h/.cpp    ← GL 实现
└── font_storage_gl.h/.cpp    ← GL 实现
```

**理由：** 与 Godot 的 Storage 模式对齐，接口在 `servers/`，实现在 `drivers/`。
未来 Vulkan 后端只需新增 `drivers/vulkan/storage/` 实现。

#### 3.3.3 Shader 变体系统

**决策：Phase 3 只实现基础编译链接，变体系统留到 Phase 4**

Phase 3 ShaderStorage 只需要：
- 加载 GLSL 源码
- 编译 + 链接 → GLuint program
- 反射 Uniform 名称/类型/位置
- 缓存 Uniform 位置

Phase 4 再引入 Godot 风格的 Version/Variant 系统：
- Version：内置定义 + 用户代码
- Variant：`#ifdef` 切换行为（深度预pass vs 颜色pass）
- Specialization：`#ifdef` 切换性能特征

#### 3.3.4 FreeType 集成时机

**决策：Phase 3 先用预生成 SDF 纹理图集，FontStorage 完整实现延后**

Phase 3 FontStorage 只需要：
- 从预生成 SDF 图集加载字体
- 查询字形 UV 矩形
- 生成字形 Mesh

FreeType 运行时 SDF 生成留到 Phase 4，因为：
- FreeType 是外部依赖（Conan），需要先配置构建系统
- SDF 生成算法需要调参，不阻塞其他 Storage 开发
- 预生成图集足以验证渲染管线

### 3.4 ShaderStorage 详细设计

```cpp
// servers/redering/storage/shader_storage.h

struct ShaderInfo {
    RID rid;
    RDD::ShaderID driver_id;
    LocalVector<ShaderUniform> uniforms;
    bool is_valid = false;
};

struct ShaderUniform {
    enum class Type {
        SAMPLER,
        SAMPLER_WITH_TEXTURE,
        UNIFORM_BUFFER,
        STORAGE_BUFFER,
    };
    Type type;
    uint32_t binding;
    UString name;
};

class ShaderStorage {
public:
    Error Initialize(RenderingDevice *p_rd);
    void Finalize();

    RID ShaderCreateFromSource(const char *p_vertex_source,
                               const char *p_fragment_source);
    void ShaderFree(RID p_shader);

    const ShaderInfo *ShaderGetInfo(RID p_shader) const;
    RDD::ShaderID ShaderGetDriverID(RID p_shader) const;
    const LocalVector<ShaderUniform> &ShaderGetUniforms(RID p_shader) const;

private:
    RenderingDevice *rd_ = nullptr;
    RIDOwner<ShaderInfo, true> shader_owner_;
};
```

### 3.5 TextureStorage 详细设计

```cpp
// servers/redering/storage/texture_storage.h

struct TextureInfo {
    RID rid;
    RDD::TextureID driver_id;
    uint32_t width = 0;
    uint32_t height = 0;
    RDD::DataFormat format = RDD::DataFormat::DATA_FORMAT_R8G8B8A8_UNORM;
    uint32_t mip_levels = 1;
    bool is_valid = false;
};

class TextureStorage {
public:
    Error Initialize(RenderingDevice *p_rd);
    void Finalize();

    RID TextureCreate(uint32_t p_width, uint32_t p_height,
                      RDD::DataFormat p_format,
                      uint32_t p_mip_levels = 1);
    RID TextureCreateFromData(const uint8_t *p_data, uint32_t p_data_size,
                              uint32_t p_width, uint32_t p_height,
                              RDD::DataFormat p_format);
    void TextureUpdate(RID p_texture, const uint8_t *p_data,
                       uint32_t p_data_size,
                       uint32_t p_x = 0, uint32_t p_y = 0,
                       uint32_t p_width = 0, uint32_t p_height = 0);
    void TextureGenerateMipmaps(RID p_texture);
    void TextureFree(RID p_texture);

    const TextureInfo *TextureGetInfo(RID p_texture) const;
    RDD::TextureID TextureGetDriverID(RID p_texture) const;

private:
    RenderingDevice *rd_ = nullptr;
    RIDOwner<TextureInfo, true> texture_owner_;
};
```

### 3.6 MaterialStorage 详细设计

```cpp
// servers/redering/storage/material_storage.h

struct MaterialInfo {
    RID rid;
    RID shader_rid;
    LocalVector<RID> texture_rids;
    LocalVector<uint8_t> uniform_data;
    bool is_valid = false;
};

class MaterialStorage {
public:
    Error Initialize(RenderingDevice *p_rd,
                     ShaderStorage *p_shader_storage,
                     TextureStorage *p_texture_storage);
    void Finalize();

    RID MaterialCreate(RID p_shader);
    void MaterialSetUniform(RID p_material, uint32_t p_index,
                            const void *p_data, uint32_t p_size);
    void MaterialSetTexture(RID p_material, uint32_t p_slot, RID p_texture);
    void MaterialFree(RID p_material);

    const MaterialInfo *MaterialGetInfo(RID p_material) const;

private:
    RenderingDevice *rd_ = nullptr;
    ShaderStorage *shader_storage_ = nullptr;
    TextureStorage *texture_storage_ = nullptr;
    RIDOwner<MaterialInfo, true> material_owner_;
};
```

### 3.7 MeshStorage 详细设计

```cpp
// servers/redering/storage/mesh_storage.h

struct MeshInfo {
    RID rid;
    RDD::BufferID vertex_buffer_id;
    RDD::BufferID index_buffer_id;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t vertex_stride = 0;
    RDD::DataFormat index_format = RDD::DataFormat::DATA_FORMAT_R16_UINT;
    bool is_valid = false;
};

class MeshStorage {
public:
    Error Initialize(RenderingDevice *p_rd);
    void Finalize();

    RID MeshCreate(const void *p_vertices, uint32_t p_vertex_count,
                   uint32_t p_vertex_stride,
                   const void *p_indices, uint32_t p_index_count,
                   RDD::DataFormat p_index_format);
    void MeshFree(RID p_mesh);

    // 预创建常用 2D 图元
    RID CreateQuad(float p_width, float p_height);
    RID CreateRect(float p_x, float p_y, float p_width, float p_height);
    RID CreateLine(float p_x0, float p_y0, float p_x1, float p_y1,
                   float p_width);

    const MeshInfo *MeshGetInfo(RID p_mesh) const;
    RDD::BufferID MeshGetVertexBufferID(RID p_mesh) const;
    RDD::BufferID MeshGetIndexBufferID(RID p_mesh) const;

private:
    RenderingDevice *rd_ = nullptr;
    RIDOwner<MeshInfo, true> mesh_owner_;
};
```

### 3.8 FontStorage 详细设计（简化版）

```cpp
// servers/redering/storage/font_storage.h

struct GlyphInfo {
    uint32_t charcode = 0;
    float uv_x = 0, uv_y = 0;
    float uv_width = 0, uv_height = 0;
    float advance = 0;
    float offset_x = 0, offset_y = 0;
    float width = 0, height = 0;
};

struct FontInfo {
    RID rid;
    RID texture_rid;
    float height = 0;
    float ascent = 0;
    float descent = 0;
    HashMap<uint32_t, GlyphInfo> glyphs;
    bool is_valid = false;
};

class FontStorage {
public:
    Error Initialize(RenderingDevice *p_rd, TextureStorage *p_texture_storage);
    void Finalize();

    // Phase 3: 从预生成 SDF 图集加载
    RID FontLoadFromSdfAtlas(const char *p_atlas_path,
                             const char *p_metrics_path,
                             uint32_t p_atlas_width, uint32_t p_atlas_height);
    void FontFree(RID p_font);

    const FontInfo *FontGetInfo(RID p_font) const;
    const GlyphInfo *FontGetGlyph(RID p_font, uint32_t p_charcode) const;

private:
    RenderingDevice *rd_ = nullptr;
    TextureStorage *texture_storage_ = nullptr;
    RIDOwner<FontInfo, true> font_owner_;
};
```

### 3.9 Phase 3 验收标准

每个 Storage 组件完成后，必须通过以下验收：

| 组件 | 验收 Demo |
|------|----------|
| ShaderStorage | 编译一个 vertex+fragment shader，打印 Uniform 列表 |
| TextureStorage | 加载一张 PNG 纹理，通过 DrawList 绘制到窗口 |
| MaterialStorage | 创建材质（Shader + 纹理），绘制带纹理的 Quad |
| MeshStorage | 创建 Quad/Line 图元，通过 DrawList 绘制 |
| FontStorage | 加载 SDF 图集，渲染 "Hello ARHud" 文字 |

---

## 四、Phase 4：渲染器层

### 4.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                     ARHudCompositor                       │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │                  HUDRenderer                         │ │
│  │  ┌───────────┐ ┌───────────┐ ┌───────────┐         │ │
│  │  │ Layer 0   │ │ Layer 1   │ │ Layer N   │  ...     │ │
│  │  │ Background│ │ ADAS      │ │ Warning   │         │ │
│  │  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘         │ │
│  │        │              │              │               │ │
│  │  ┌─────┴──────────────┴──────────────┴─────┐        │ │
│  │  │          CanvasRenderer                  │        │ │
│  │  │  ┌────────────────────────────────────┐  │        │ │
│  │  │  │  批处理 (同材质合并绘制)            │  │        │ │
│  │  │  │  顶点缓冲池                        │  │        │ │
│  │  │  │  裁剪与变换                        │  │        │ │
│  │  │  └────────────────────────────────────┘  │        │ │
│  │  └──────────────────────────────────────────┘        │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  Post-Process: 畸变校正 + Gamma 校正                 │ │
│  └─────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

### 4.2 实施顺序

| 顺序 | 组件 | 依赖 | 核心功能 | 预估 |
|:----:|------|------|---------|:----:|
| 4.1 | **CanvasRenderer** | Phase 3 全部 Storage | 2D 批处理渲染，裁剪，变换 | 1.5 周 |
| 4.2 | **HUDRenderer** | CanvasRenderer | HUD 图层管理，Z-Order 排序 | 1 周 |
| 4.3 | **Compositor** | HUDRenderer | 帧流程编排，后处理，Present | 1 周 |
| 4.4 | **Shader 变体系统** | ShaderStorage | Version/Variant/Specialization | 1 周 |

### 4.3 CanvasRenderer 核心设计

```cpp
// servers/redering/canvas_renderer.h

struct CanvasItem {
    RID material;
    RID mesh;
    float transform[16];
    float clip_rect[4];
    uint32_t z_order;
};

class CanvasRenderer {
public:
    Error Initialize(RenderingDevice *p_rd,
                     ShaderStorage *p_shader_storage,
                     TextureStorage *p_texture_storage,
                     MaterialStorage *p_material_storage,
                     MeshStorage *p_mesh_storage);
    void Finalize();

    // 批处理渲染
    void RenderCanvasItems(const CanvasItem *p_items, uint32_t p_count);

private:
    RenderingDevice *rd_ = nullptr;
    ShaderStorage *shader_storage_ = nullptr;
    TextureStorage *texture_storage_ = nullptr;
    MaterialStorage *material_storage_ = nullptr;
    MeshStorage *mesh_storage_ = nullptr;

    // 批处理
    struct Batch {
        RID material;
        LocalVector<CanvasItem> items;
    };
    LocalVector<Batch> batches_;

    void BuildBatches(const CanvasItem *p_items, uint32_t p_count);
    void RenderBatch(const Batch &p_batch);
};
```

### 4.4 HUDRenderer 核心设计

```cpp
// servers/redering/hud_renderer.h

struct HUDLayer {
    enum class Type {
        BACKGROUND,
        ADAS,
        NAVIGATION,
        DASHBOARD,
        WARNING,
    };
    Type type;
    int32_t z_order = 0;
    LocalVector<CanvasItem> items;
};

class HUDRenderer {
public:
    Error Initialize(CanvasRenderer *p_canvas_renderer);
    void Finalize();

    void Render(const LocalVector<HUDLayer> &p_layers);

private:
    CanvasRenderer *canvas_renderer_ = nullptr;
};
```

### 4.5 Compositor 核心设计

```cpp
// servers/redering/compositor.h

class ARHudCompositor {
public:
    Error Initialize(RenderingDevice *p_rd);
    void Finalize();

    void BeginFrame(double frame_step);
    void Render(ARHudSceneTree *scene);
    void EndFrame(bool present);

private:
    RenderingDevice *rd_ = nullptr;

    // Storage 子系统
    ShaderStorage *shader_storage_ = nullptr;
    TextureStorage *texture_storage_ = nullptr;
    MaterialStorage *material_storage_ = nullptr;
    MeshStorage *mesh_storage_ = nullptr;
    FontStorage *font_storage_ = nullptr;

    // 渲染器
    CanvasRenderer *canvas_renderer_ = nullptr;
    HUDRenderer *hud_renderer_ = nullptr;

    // 后处理
    RID distortion_pipeline_;
    RID distortion_uniform_set_;

    // 帧状态
    uint64_t frame_number_ = 0;
    double delta_time_ = 0.0;
};
```

### 4.6 Phase 4 验收标准

| 组件 | 验收 Demo |
|------|----------|
| CanvasRenderer | 绘制 100 个带纹理的 Quad，验证批处理合并 |
| HUDRenderer | 渲染 5 个 HUD 图层（Background→Warning），验证 Z-Order |
| Compositor | 完整帧循环：BeginFrame→Render→EndFrame→Present |
| Shader 变体 | 同一 Shader 编译 2 个 Variant（颜色 pass + 深度 pass） |

---

## 五、Phase 5：场景与集成

### 5.1 实施顺序

| 顺序 | 组件 | 核心功能 | 预估 |
|:----:|------|---------|:----:|
| 5.1 | **Scene Tree** | 节点基类，2D 节点，HUD 元素节点 | 1 周 |
| 5.2 | **Engine 主循环** | 初始化→帧循环→清理 | 0.5 周 |
| 5.3 | **数据绑定** | HUD 数据→Uniform Buffer 更新 | 0.5 周 |

### 5.2 Scene Tree 设计

```cpp
// scene/node.h

class Node {
public:
    virtual ~Node() = default;

    void AddChild(Node *p_child);
    void RemoveChild(Node *p_child);

    virtual void Update(double p_delta) = 0;
    virtual void Render(ARHudCompositor *p_compositor) = 0;

protected:
    Node *parent_ = nullptr;
    LocalVector<Node *> children_;
};

// scene/hud_node.h

class HUDNode : public Node {
public:
    void SetPosition(float p_x, float p_y);
    void SetSize(float p_width, float p_height);
    void SetZOrder(int32_t p_z_order);
    void SetVisible(bool p_visible);

    void Update(double p_delta) override;
    void Render(ARHudCompositor *p_compositor) override;

protected:
    float x_ = 0, y_ = 0;
    float width_ = 0, height_ = 0;
    int32_t z_order_ = 0;
    bool visible_ = true;
    RID material_;
    RID mesh_;
};

// scene/hud_text_node.h

class HUDTextNode : public HUDNode {
public:
    void SetText(const UString &p_text);
    void SetFont(RID p_font);
    void SetFontSize(float p_size);
    void SetColor(float p_r, float p_g, float p_b, float p_a);

    void Update(double p_delta) override;
    void Render(ARHudCompositor *p_compositor) override;

private:
    UString text_;
    RID font_;
    float font_size_ = 16.0f;
    float color_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// scene/hud_icon_node.h

class HUDIconNode : public HUDNode {
public:
    void SetTexture(RID p_texture);
    void SetOpacity(float p_opacity);

private:
    RID texture_;
    float opacity_ = 1.0f;
};
```

### 5.3 Engine 主循环

```cpp
// app/engine.h

class ARHudEngine {
public:
    Error Initialize(const Config &config);
    void Run();
    void Finalize();

private:
    void MainLoop();
    void UpdateScene(double p_delta);
    void RenderFrame();

    IRenderingContextDriver *context_driver_ = nullptr;
    RenderingDevice *rendering_device_ = nullptr;
    ARHudCompositor *compositor_ = nullptr;
    ARHudSceneTree *scene_tree_ = nullptr;
    IWindow *main_window_ = nullptr;

    struct Config {
        uint32_t window_width = 1920;
        uint32_t window_height = 720;
        bool vsync = true;
    } config_;
};
```

### 5.4 Phase 5 验收标准

| 组件 | 验收 Demo |
|------|----------|
| Scene Tree | 创建节点树，Update 传播，Render 传播 |
| Engine 主循环 | 窗口创建→初始化→帧循环→清理，无内存泄漏 |
| 数据绑定 | 车速数据变化→Uniform Buffer 更新→仪表盘数值更新 |

---

## 六、Phase 6：Demo 应用

### 6.1 实施顺序

| 顺序 | Demo | 核心功能 | 预估 |
|:----:|------|---------|:----:|
| 6.1 | **仪表盘 Demo** | 速度/转速/油量表盘，指针动画 | 1 周 |
| 6.2 | **导航箭头 Demo** | 导航方向箭头，旋转动画 | 0.5 周 |
| 6.3 | **ADAS 车道线 Demo** | 车道线渲染，障碍物标注 | 0.5 周 |
| 6.4 | **综合 Demo** | 三个 Demo 合并，数据驱动 | 1 周 |

### 6.2 仪表盘 Demo 场景树

```
ARHudSceneTree
├── Background (Layer 0, z=-100)
│   └── BackgroundGradient (全屏渐变纹理)
├── Dashboard (Layer 3, z=0)
│   ├── Speedometer (速度表盘)
│   │   ├── DialBackground (圆形背景)
│   │   ├── DialTicks (刻度线)
│   │   ├── SpeedNeedle (速度指针)
│   │   └── SpeedText (数字显示)
│   ├── Tachometer (转速表盘)
│   │   ├── DialBackground
│   │   ├── DialTicks
│   │   ├── RPMNeedle
│   │   └── RPMText
│   └── FuelGauge (油量表)
│       ├── FuelBackground
│       └── FuelBar
└── Warning (Layer 4, z=100)
    └── LowFuelWarning (低油量警告)
```

---

## 七、Phase 7：Vulkan + RDG 完整实现

### 7.1 触发条件回顾

满足以下**任一**条件时启动 Phase 7：
1. 开始实现 Vulkan 后端
2. 需要多线程渲染
3. 需要命令流录制/回放用于调试
4. DrawCall 超过 500

### 7.2 实施顺序

| 顺序 | 组件 | 核心功能 | 预估 |
|:----:|------|---------|:----:|
| 7.1 | **RDG 序列化字节流** | 替换 ICommandBuffer 内部列表为字节流 | 1 周 |
| 7.2 | **RDG 帧末统一编译** | DrawListEnd 不立即执行，帧末统一编译 | 1 周 |
| 7.3 | **RDG 屏障推导** | 资源使用追踪 + 自动屏障插入 | 1 周 |
| 7.4 | **Vulkan RCD** | VkInstance + VkPhysicalDevice + VkDevice | 1 周 |
| 7.5 | **Vulkan RDD** | VkCommandBuffer + VkPipeline + 资源管理 | 2 周 |
| 7.6 | **Vulkan CommandBuffer** | VkCommandBuffer 录制/提交 | 1 周 |
| 7.7 | **多线程命令队列** | 主线程录制 + 渲染线程播放 | 1 周 |

### 7.3 RDG 完整实现时的 API 兼容性

**公共 API 不变：**

```cpp
// Phase 2-6: DrawList + ICommandBuffer
DrawListID dl = device->DrawListBegin();
device->DrawListBindRenderPipeline(dl, pipeline);
device->DrawListDraw(dl, vertex_count, 1);
device->DrawListEnd();

// Phase 7: DrawList + RDG 字节流（API 完全相同！）
DrawListID dl = device->DrawListBegin();
device->DrawListBindRenderPipeline(dl, pipeline);
device->DrawListDraw(dl, vertex_count, 1);
device->DrawListEnd();
```

**内部实现替换：**

```
Phase 2-6:
  DrawListBegin → CommandBufferAcquire → ICommandBuffer::Begin
  DrawListBindPipeline → ICommandBuffer::BindPipeline
  DrawListEnd → ICommandBuffer::End → Execute → CommandBufferRelease

Phase 7:
  DrawListBegin → 标记 DrawList 活跃，分配指令流空间
  DrawListBindPipeline → 序列化到 instruction_stream
  DrawListEnd → 标记 DrawList 结束（不执行！）
  Compositor::EndFrame → RDG 编译 + 屏障推导 + 统一执行
```

---

## 八、总时间线

| Phase | 描述 | 预估 | 累计 |
|-------|------|:----:|:----:|
| Phase 3 | 资源存储层 | 4 周 | 4 周 |
| Phase 4 | 渲染器层 | 4.5 周 | 8.5 周 |
| Phase 5 | 场景与集成 | 2 周 | 10.5 周 |
| Phase 6 | Demo 应用 | 3 周 | 13.5 周 |
| Phase 7 | Vulkan + RDG | 8 周 | 21.5 周 |

**里程碑：**
- **M1 (4 周)**: Storage 层完成，可以创建和使用 GPU 资源
- **M2 (8.5 周)**: Renderer 层完成，可以渲染 HUD 图层
- **M3 (10.5 周)**: 场景集成完成，Engine 主循环可运行
- **M4 (13.5 周)**: Demo 应用完成，仪表盘/导航/ADAS 可演示
- **M5 (21.5 周)**: Vulkan + RDG 完成，双后端支持
