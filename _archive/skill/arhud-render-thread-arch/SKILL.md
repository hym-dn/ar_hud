---
name: "arhud-render-thread-arch"
description: "AR HUD render-thread architecture (Route B) — move Canvas/Spatial renderers and Storage to the render thread, serialized CommandQueue ring buffer, double-buffered FrameScene. Invoke when implementing the render-thread migration, redesigning CommandQueue as a serialized buffer, passing frame scene data across threads, designing the RenderServer main-thread proxy, or aligning the engine with Godot's RenderingServer threading model."
---

# AR HUD 渲染线程架构（路线 B）设计规范

> 参考 Godot 4.x RenderingServer 多线程模型 + CommandQueueMT。
> 目标：把渲染器（CanvasRenderer/SpatialRenderer）和 Storage 从主线程搬到渲染线程，
> 主线程只发高层语义命令，渲染线程执行所有渲染工作并直接持有 GPU 上下文。

**前置阅读**：根 `AGENTS.md`（分层与依赖铁律）+ `docs/process/dev-workflow.md`（变更分级与 C 类铁律）。

**进度**：B1 ✅ / B2 ✅（2026-08-22）/ B2-R ✅（2026-08-23）/ B3-M0 ✅（2026-08-24~25，全部 4 子项完成）/
B3 ✅（2026-09-03，渲染搬线程 + 每屏 present 线程，见 `arhud-multiscreen-arch` §3.5/§3.6）/
**B3-P ← 下一步**、B4、B5 未开始。落地差异见第五节。

> **2026-08-24 决策**：B3 呈现部分调整为**渲染/上屏分离**——渲染线程渲 FBO 导出
> FrameImage，从不调 SwapBuffers；每屏一个 present 线程（无 GL）平台 API 上屏，
> vblank 等待恢复并行。原"批量 swap"设计废弃，详见 `arhud-multiscreen-arch` §0.3/§3.1/§3.6。

---

## 〇、为什么做路线 B（动机与边界）

### 0.1 当前模型（路线 A，次优）

```
主线程：游戏逻辑 + 剔除/排序/批处理 + 录制 CommandBuffer（重）
渲染线程：回放 CommandBuffer + 提交 GPU（轻，只是回放器）
```

渲染"前半段"压在主线程，同一份信息被序列化两次（高层意图 → GPU 命令 → 字节流回放）。

### 0.2 目标模型（路线 B，对齐 Godot）

```
主线程：游戏逻辑 + 场景变更（轻）——只发高层语义命令
渲染线程：剔除/排序/批处理 + 录 CommandBuffer + 直接执行 + 提交 GPU（重）
```

主线程与渲染线程做**两项不同工作的真并行**（逻辑 ∥ 渲染），命令只序列化一次。

### 0.3 路线 B 解决 / 不解决（务必先认清边界）

| 分类 | 问题 | 路线 B 是否解决 |
|---|---|---|
| **A 类（线程模型一族，路线 B 解决）** | 渲染工作压主线程、非真并行 | ✅ 核心目标 |
| | 双路径渲染（CommandQueue + CommandBuffer）空转唤醒 | ✅ 帧渲染统一为命令 |
| | `CmdBufferUpdate` 深拷贝 | ✅ 同线程录+放 + 改持 payload 区指针（2026-09-27 去逐条堆分配） |
| | CommandQueue 存外部指针脆弱 | ✅ 重构为序列化缓冲区时一并解决 |
| | `MaterialData::version` 跨线程竞态 | ✅ 变更全走命令队列，仅渲染线程触碰 |
| | **SyncFlag 栈分配悬垂（P0）** | ✅ CommandQueue 重构用每线程信号量替代 |
| **B 类（独立问题，路线 B 不解决，需单独修）** | 三套 ID 体系（RID/RDBufferID/BufferID） | ❌ 独立 |
| | 后处理硬编码进 RenderViewport | ❌ 独立（但路线 B 动 RenderViewport，可顺手拆） |
| | ~~`ShaderCreateFromGLSL` 命名泄漏~~ | ✅ 已改（2026-09-27）：`ShaderCreateFromSource` + `ShaderSourceFormat` |
| | `opengl32` PUBLIC 链接 / 命名空间扁平 / README 滞后 / VRAM 估算 / hash 碰撞 | ❌ 独立 |

**结论**：路线 B 不是万能药。B 类问题换不换线程模型都要单独修。本规范聚焦 A 类；B 类在治理清单里跟踪。

---

## 一、分层架构（路线 B 后）

```
app/                        ← 应用层
  └─ demo：只调 ARHudEngine + RenderServer（主线程 API），不直接碰 storage/renderer
      │  高层语义命令（Push / PushAndSync）
      ▼
servers/rendering/          ← 渲染服务层（本次大改）
  ├─ RenderServer           ← 【新增】主线程 API 代理：把调用序列化为命令
  ├─ CommandQueue           ← 【重构】SPSC 序列化环形缓冲区（替代 Entry 列表）
  ├─ ItemStorage            ← 【B2-R】服务端 item 状态（保留模式；替代 FrameScene）
  ├─ ARHudEngine            ← 【B3-M0 起持有共享渲染栈；B3 起持有渲染线程】多屏引擎（PIMPL）
  ├─ RenderViewport           ← 【B3-M0 瘦身】轻量视口（surface/后处理/呈现调度），对齐 Godot Window
  ├─ renderer/              ← CanvasRenderer/SpatialRenderer【改为渲染线程执行】
  ├─ storage/               ← 资源存储【改为渲染线程独占访问】
  ├─ RenderingDevice        ← RD（保留；CommandBuffer 改渲染线程录+立即执行）
  └─ RCD/RDD 接口           ← 不变
      ▼
drivers/gl/                 ← GL 驱动（不变，仍在渲染线程执行）
platform/                   ← 平台抽象（不变）
core/                       ← 基础库（Semaphore 视需要加 WaitTimeout）
```

**依赖方向不变**（治理铁律）：app → servers → drivers/platform → core，严格单向。

**线程归属（新增规则）**：

| 组件 | 主线程 | 渲染线程 |
|---|---|---|
| ARHudEngine / RenderServer | ✅ 调用入口 | ❌ |
| CommandQueue（push 端） | ✅ 写 | ❌ |
| ItemStorage（B2-R 服务端 item 状态） | ❌（B2 过渡期主线程读前经 PushAndSync 屏障） | ✅ 独占 |
| CommandQueue（flush 端） | ❌ | ✅ 读+执行 |
| renderer / storage / RD / GL | ❌ | ✅ 独占 |

---

## 二、核心组件设计

### 2.1 CommandQueue：SPSC 序列化环形缓冲区（重构）

替代当前 `LocalVector<Entry>`（函数指针 + 外部数据指针）。参考 Godot `CommandQueueMT` 的定长环形字节缓冲区。

**设计要点**：
- 单生产者（主线程写）单消费者（渲染线程读），读写指针原子化，无锁快路径
- 定长 1MB，无运行时分配（车规）
- 命令 = 定长头 + 负载（变长数据内联在负载后）
- 同步用**每线程信号量**（替代栈 SyncFlag，修复 P0 悬垂）

```cpp
// command_queue.h（重构后）
class CommandQueue
{
public:
    static constexpr uint32_t kCapacity = 1u << 20;   // 1MB 环形缓冲区
    static constexpr uint32_t kMaxSyncThreads = 4;     // 支持同步等待的线程数

    /** @brief 命令头（8 字节，定长） */
    struct CommandHeader
    {
        uint8_t  type;      ///< CommandType
        int8_t   sync_idx;  ///< >=0 同步（执行后 post sync_sems_[sync_idx]）；-1 异步
        uint16_t size;      ///< 含头总字节数（遍历用）
        uint32_t reserved;  ///< 对齐填充
    };

    // ── 主线程 ──
    /** @brief 异步推送命令（序列化进缓冲区） */
    template <typename T> void Push(const T &p_cmd);

    /** @brief 同步推送：等待渲染线程执行完毕，结果写回 p_cmd 内的 result 字段 */
    template <typename T> void PushAndSync(T &p_cmd);

    // ── 渲染线程 ──
    /** @brief 执行缓冲区中所有待处理命令 */
    void Flush();

    void SetNotifySemaphore(Semaphore *p_sem);
    void SignalShutdown();
    bool IsShuttingDown() const;

private:
    alignas(ARHUD_CACHE_LINE_SIZE) std::atomic<uint32_t> write_pos_{0};
    alignas(ARHUD_CACHE_LINE_SIZE) std::atomic<uint32_t> read_pos_{0};
    uint8_t buffer_[kCapacity];
    Semaphore *notify_semaphore_ = nullptr;
    Semaphore sync_sems_[kMaxSyncThreads];      ///< 每线程一个，替代栈 SyncFlag
    Thread::ID render_thread_id_ = Thread::kUnassignedId;
    std::atomic<bool> shutting_down_{false};

    /** @brief 渲染线程：按 type 分发执行单条命令 */
    void Dispatch(const CommandHeader *p_header);
};
```

**写路径（Push）**：
1. 计算 `size = sizeof(T)`，检查缓冲区剩余空间
2. 空间不足 → 背压：主线程自旋/等待渲染线程消费（`read_pos_` 前进）后再写；禁止静默丢弃
3. `memcpy` 序列化 header + payload 到 `write_pos_`
4. 原子 release 更新 `write_pos_`
5. `notify_semaphore_->Post()` 唤醒渲染线程

**读路径（Flush，仅渲染线程）**：
1. acquire 读 `write_pos_`，从 `read_pos_` 遍历到 `write_pos_`
2. 逐条 `Dispatch(header)`：switch(type) → 调用对应 storage/renderer 方法
3. 若 `sync_idx >= 0`：`sync_sems_[sync_idx].Post()`（唤醒等待的主线程）
4. 原子 release 更新 `read_pos_`

**同步路径（PushAndSync）**：
1. 分配 `sync_idx`（按调用线程 ID 映射到固定槽，线程级复用，非每命令分配）
2. 序列化命令（`sync_idx >= 0`）入队，Post 信号量
3. `sync_sems_[sync_idx].Wait()` 阻塞
4. 渲染线程执行后 Post 该信号量，主线程醒来读 `p_cmd` 内 result 字段

**为什么这修复了 SyncFlag P0**：信号量是 `sync_sems_` 数组成员，生命周期 = CommandQueue 本身，不存在"命令在队列里但 flag 已析构"的悬垂。结果通过 payload 内字段写回，不需要额外堆分配。

### 2.2 FrameScene：双缓冲帧场景数据（新增）

> **已废弃（B2-R 替代，B3-M0 删除）**：item/相机/灯光已存服务端 ItemStorage（保留
> 模式，增量更新），帧提交仅信号——FrameScene 死路径在 B3-M0 一并删除。本节保留作
> 历史设计记录；相机/灯光在多屏下进一步 per-screen 化（见 `arhud-multiscreen-arch` §3.3），
> 其后按 **ADR-0007** 自 `ItemStorage` 迁入 `RenderViewport::ViewportSceneState`。

每帧的场景数据（相机/灯光/变长 item 列表）体积大且嵌套，**不塞进命令缓冲区**，用双缓冲槽承载。命令只携带帧号，渲染线程按帧号读对应槽。

```cpp
// frame_scene.h（新增）
struct FrameScene
{
    Camera2D camera_2d;
    Camera3D camera_3d;
    DirectionalLight light;
    bool has_light = false;
    LocalVector<SpatialItem> spatial_items;      ///< 3D 项
    LocalVector<HUDCanvasLayer> canvas_layers;   ///< 2D 图层（内含 canvas_items/text_items）

    /** @brief 每帧开始清空（保留 capacity，零分配） */
    void Clear()
    {
        spatial_items.Clear();
        canvas_layers.Clear();
        has_light = false;
    }
};
```

**双缓冲与同步**（复用现有 frame slot 机制，`frame_count_ = 2`）：
- 主线程写 `frame_scenes_[frame_number % 2]`
- 渲染线程读 `frame_scenes_[(frame_number - 1) % 2]`
- 主线程写槽前，确认渲染线程已消费该槽的上上帧数据（帧计数原子量 / 每槽 consumed 信号量）
- `LocalVector` 用 PagedAllocator，`Clear()` 保留 capacity → 稳态零分配

### 2.3 RenderServer：主线程 API 代理（新增）

app 可见的唯一渲染 API。把高层调用序列化为命令。**主线程通过它操作渲染线程上的 storage/renderer。**

```cpp
// render_server.h（新增）
class RenderServer
{
public:
    Error Initialize(CommandQueue *p_queue, FrameScene *p_scenes, uint32_t p_frame_count);
    void Finalize();

    // ── 资源创建（同步，返回 RID）──
    RID TextureCreateFromData(const char *p_name, const uint8_t *p_data, uint32_t p_size,
                              uint32_t p_w, uint32_t p_h, DataFormat p_format);
    RID ShaderCreateFromSource(const char *p_name, const LocalVector<ShaderStageSource> &p_stages,
                               const LocalVector<ShaderUniform> &p_uniforms, uint32_t p_push_size);
    RID MaterialCreate(const char *p_name, RID p_shader, MaterialDataType p_type);
    RID MeshCreateQuad(const char *p_name, float p_w, float p_h);
    RID MeshCreateBox(const char *p_name, float p_w, float p_h, float p_d);
    RID MeshCreateSphere(const char *p_name, float p_radius, uint32_t p_rings, uint32_t p_sectors);
    RID FontLoadFromFreeType(const char *p_name, const char *p_path, float p_size,
                             const uint32_t *p_charcodes, uint32_t p_count, float p_spread);

    // ── 资源变更（异步）──
    void MaterialSetUniform(RID p_material, uint32_t p_index, const void *p_data, uint32_t p_size);
    void MaterialSetTexture(RID p_material, uint32_t p_index, RID p_texture);

    // ── 资源释放（异步）──
    void TextureFree(RID p_rid);
    void ShaderFree(RID p_rid);
    void MaterialFree(RID p_rid);
    void MeshFree(RID p_rid);
    void FontFree(RID p_rid);

    // ── 帧 ──
    /** @brief 开始帧：返回可写的 FrameScene 槽（主线程填充） */
    FrameScene *BeginScene();
    /** @brief 提交帧场景 + 请求呈现（异步） */
    void SubmitScene();
    void Present();

private:
    CommandQueue *queue_ = nullptr;
    FrameScene *scenes_ = nullptr;      ///< 双缓冲数组
    uint32_t frame_count_ = 2;
    uint64_t frame_number_ = 0;
};
```

**实现要点**：每个方法把参数打包成对应命令结构体，`Push`（异步）或 `PushAndSync`（同步返回 RID）。**RenderServer 不持有任何 GPU 资源、不直接调 storage**——它只是序列化器。

### 2.4 RenderViewport：渲染线程主循环重构

> **B3 落地形态（多屏）**：渲染线程归 ARHudEngine，主循环在 Engine 级——Flush 命令 →
> 遍历 due screens（渲染到 per-screen FBO + 导出 FrameImage）→ 通知 present 线程 →
> post frame_done 帧节拍。不再属于单个 RenderViewport。**上屏不在渲染线程**——每屏一个
> present 线程（无 GL）平台 API 上屏（2026-08-24 决策，替代原"批量 swap"）。详见
> `arhud-multiscreen-arch` §3.5/§3.6。本节为单屏原始设计。

渲染线程从"帧回放器"变为"纯命令 flush 循环"。

```cpp
// render_viewport.cpp 渲染线程（重构后）
void RenderViewport::RenderThreadFunc()
{
    context_driver_->MakeCurrent(context_id_, surface_id_);
    rd_.MakeRenderThread();
    // ... VSync 重设 ...
    render_thread_ready_.Post();

    while (render_thread_running_.load())
    {
        main_to_render_.Wait();                 // 等主线程发命令
        if (!render_thread_running_.load()) { break; }

        HandleResizeIfNeeded();
        command_queue_.Flush();                 // 执行所有命令（含 SubmitScene → 跑 renderer）
    }

    command_queue_.Flush();
    context_driver_->ReleaseCurrent();
}
```

**关键变化**：`ExecuteFrame`（回放预录 CommandBuffer）被移除。渲染改由 `kSubmitScene` 命令的 handler 触发：

```cpp
// 渲染线程：SubmitScene 命令 handler
void RenderViewport::OnSubmitScene(uint64_t p_frame_number)
{
    FrameScene &scene = frame_scenes_[p_frame_number % frame_count_];
    // 直接在本线程跑渲染器（持有 GL 上下文，直接发 GPU 命令）
    if (!scene.spatial_items.IsEmpty())
    {
        compositor_.GetSpatialRenderer()->RenderSpatialItems(
            &rd_, scene.camera_3d, scene.has_light ? &scene.light : nullptr,
            scene.spatial_items.Data(), scene.spatial_items.Size(), width_, height_, true);
    }
    for (uint32_t i = 0; i < scene.canvas_layers.Size(); ++i)
    {
        compositor_.GetCanvasRenderer()->RenderCanvasItems(
            &rd_, scene.camera_2d, scene.canvas_layers[i], width_, height_, false);
    }
}
```

### 2.5 Storage / Renderer：渲染线程独占（线程安全简化）

路线 B 后，storage 和 renderer **只在渲染线程被访问**（主线程一律经 RenderServer 命令）。带来两个简化：

1. **`MaterialData::version` 竞态消除**：version 只被渲染线程读写，无跨线程。
2. **RIDOwner 可降级**：`RIDOwner<p_thread_safe>` 的模板参数可从 `<true>`（带锁）降为 `<false>`（无锁），消除热路径锁竞争。**但需先把"主线程经命令访问"落实到位**——迁移过渡期保持 `<true>`，稳定后再降。

**唯一例外**：资源创建要把 RID 返回主线程（主线程后续在 item 里引用该 RID）。用 `PushAndSync` 同步返回（见 3.2 的权衡）。

### 2.6 RD 的 CommandBuffer 去向（保留，但同线程录+放）

- **GL 路径**：渲染线程调 `RD::DrawListBegin/Draw/End` 录进 CommandBuffer，**立即在本线程执行**（不再跨线程）。
- **Vulkan 路径（Phase 9）**：RD 的 CommandBuffer 直接映射 `VkCommandBuffer`，录制在渲染线程（Vulkan 要求命令池属主线程录制）。完全对齐。
- **收益**：`CmdBufferUpdate` 深拷贝**已优化**（2026-09-27）——命令改持 CommandBuffer 内建 payload 区指针，消除"每条命令一次堆分配 + 逐条析构"。（未采用字面"引用 Staging Buffer"：命令数据源常为调用方短命局部，且把每帧 UBO 更新灌进有容量上限的 staging 有耗尽风险。）

> 说明：RD CommandBuffer 机制保留，但跨线程的 frame slot 轮转机制**已简化**（2026-09-27）——录制/回放同线程后，帧命令缓冲退化为**单实例复用**，`pool_state`/`frame_used` 与多槽池随之删除；`frames_[]` 延迟销毁槽与 Fence 仍按 `frame_count_` 保留。

---

## 三、命令词汇表与序列化格式

### 3.1 命令类型

```cpp
enum class CommandType : uint8_t
{
    kNone = 0,
    // 资源创建（同步 PushAndSync，payload 内含 RID result 写回）
    kTextureCreateFromData, kShaderCreateFromSource, kMaterialCreate,
    kMeshCreateQuad, kMeshCreateBox, kMeshCreateSphere, kFontLoadFromFreeType,
    // 资源变更（异步 Push）
    kMaterialSetUniform, kMaterialSetTexture, kTextureUpdate,
    // 资源释放（异步 Push，走 RD 延迟销毁语义）
    kTextureFree, kShaderFree, kMaterialFree, kMeshFree, kFontFree,
    // 帧（异步 Push）
    kSubmitScene, kPresent,
    // 控制（异步）
    kResize, kShutdown,
    kMax
};
```

### 3.2 序列化格式

定长头 + 定长负载 + 变长数据（内联）。

```
[CommandHeader][FixedPayload][VariableData...]
```

示例（带变长像素的纹理创建，同步）：

```cpp
struct TextureCreateFromDataCmd
{
    CommandHeader header;        // type=kTextureCreateFromData, sync_idx>=0
    // 定长参数
    uint32_t width, height;
    DataFormat format;
    uint32_t data_size;          // 变长像素字节数
    RID result;                  // 渲染线程写回，主线程 sync 等待后读
    // 变长像素数据紧跟其后（data_size 字节）
    // 名称字符串也内联（以 '\0' 结尾）或省略用 RID 调试名
};
```

**同步 vs 异步的选择**：
- **创建类 → 同步（PushAndSync）**：主线程需要立刻拿到 RID 引用进 item。
- **变更/释放/帧类 → 异步（Push）**：不需要返回值，主线程发完就走，**这是主线程不被渲染拖慢的关键**。

> 权衡：创建类同步会短暂阻塞主线程。HUD 资源创建集中在初始化期，运行期动态创建少，可接受。**后续优化项**：RID 预分配（主线程先占 RID 位，渲染线程后填资源），彻底消除创建阻塞。列入 backlog，不在路线 B 主体。

### 3.3 Dispatch 表（渲染线程）

```cpp
void CommandQueue::Dispatch(const CommandHeader *p_header)
{
    switch (static_cast<CommandType>(p_header->type))
    {
    case CommandType::kTextureCreateFromData: ExecTextureCreate(p_header); break;
    case CommandType::kMaterialCreate:        ExecMaterialCreate(p_header); break;
    // ... 每个命令一个 Exec 函数，解析 payload 调 storage/renderer ...
    case CommandType::kSubmitScene:           owner_->OnSubmitScene(...); break;
    case CommandType::kPresent:               owner_->OnPresent(); break;
    default: ARHUD_LOG_ERROR("kFailed", "unknown command type %u", p_header->type); break;
    }
}
```

Dispatch 需要访问 storage/renderer/compositor——通过 `CommandQueue` 持有的 `RenderViewport*`（或专门的 `CommandExecutor`）回调。**建议**：抽一个 `CommandExecutor`（渲染线程侧，持有 compositor/storage/renderer 引用），CommandQueue 只做传输，Executor 做执行，职责分离。

> **现状（ADR-0009，2026-10-02，M3 完成）**：分派机制由**集中 `switch`** 迁移为**命令自派发（POD thunk）**——命令头携带函数指针 thunk（`CommandHeader.exec`，头 8→16B），`Flush` 直接调用；命令结构体自带 `Execute(CommandExecutor&, var_data, type)`。**全部 33 结构体 / 41 命令已迁移，中央 switch 与 `ICommandExecutor`、`HasExecute` 均已删除**（方案 b）。6 个共用布局结构体各含局部 `switch(p_type)`。详见 `docs/adr/0009-command-self-dispatch.md`。

### 3.4 相机命令载荷：传「意图参数」而非「计算结果矩阵」（ADR-0006，草案）

> **状态：草案**（方案先行产物，待 ADR-0006 评审通过后实现）。

**问题**：`CameraMatrix` 是**惰性**的——`SetPerspective/SetOrthographic/LookAt` 只置
`matrices_dirty_ = true`；`Get*Matrix()` 直接返回成员、**不触发 `UpdateMatrices()`**。
"矩阵化传输"因此双重失效：

1. `RenderServer::Camera3DSet` 读 `GetProjectionMatrix()` 拿到**未重算的默认矩阵**；
2. `ExecCamera3DSet`（现 `Camera3DSetCmd::Execute`，ADR-0009）新建的 `CameraMatrix` 处于 dirty 态，被 `SpatialRenderer` 的
   `p_camera.UpdateMatrices()` **按默认参数重算并覆盖**。

⇒ 3D 相机端到端恒为默认（fov 45 / eye 原点 / target -Z）。证据链见
`docs/devlog/2026-09-27_camera_pipeline_per_viewport.md` §4。

**方案（ADR-0006）**：`Camera3DSetCmd` 改为携带**相机意图参数**，渲染侧重建：

```cpp
struct Camera3DSetCmd
{
    static constexpr RenderCommandType kType = RenderCommandType::kCamera3DSet;
    uint32 viewport_index;
    uint8  projection_type;   // kPerspective / kOrthographic
    uint8  keep_aspect;       // kKeepHeight / kKeepWidth
    uint8  ortho_mode;        // kSize / kManualRect
    uint8  reserved;
    float  fov_y, aspect, near_z, far_z;
    float  ortho_size;
    float  ortho_left, ortho_right, ortho_bottom, ortho_top;
    float  position[3], target[3], up[3];
};
```

- 发送侧 `RenderServer` 用 getter 抽参数；执行侧 `SetPerspective/SetOrthographic/LookAt` 重建
  → 惰性重算**自然正确**（与惰性设计协同，而非对抗）。
- **不传** `use_manual_view_`（`SetViewTransform` 全仓零调用方）：新增 `IsUsingManualView()` 后
  由 RenderServer **WARN 拒绝**该模式，登记 backlog。
- `Camera2DSetCmd`（`Transform2D` 平铺，无惰性）/ `DirectionalLightSetCmd`（纯 float）**不受影响**。

**分阶段**（每阶段可编译可测）：① `CameraMatrix` 补 2 个 getter；② 命令换参数结构体 +
Executor 重建 + 单测；③ demo 视觉冒烟 + 归档。

> 已否决：矩阵传输 + `SetMatrices` 清 dirty（传"结果"、需绕过惰性、易漏同步）；仅读前
> `UpdateMatrices()`（只修一半）；整体 memcpy `CameraMatrix`（暴露内部布局、ABI 脆弱）；
> 从矩阵反解参数（不可逆）。

---

## 四、帧时序（Frame Timeline）

```
主线程                                    渲染线程（独占 GL 上下文）
  │                                         │
  ├─ BeginScene()                           │
  │   ├─ 等待帧槽可写（渲染落后 < 2 帧）     │
  │   └─ 返回 FrameScene[N%2]               │
  │                                         │
  ├─ 游戏逻辑：填充场景                      │
  │   （相机/灯光/items 写入 FrameScene 槽） │
  │                                         │
  ├─ 资源命令（如有，Push/PushAndSync）      │
  │                                         │
  ├─ SubmitScene() → Push kSubmitScene(N)   │
  ├─ Present()     → Push kPresent          │
  ├─ Post(notify_semaphore_) ──────────────►├─ Wait 返回
  │                                         ├─ Flush：
  │  ◄── 主线程立即继续下一帧逻辑 ──┐        │   ├─ 资源命令 → storage
  │  （不等渲染，真并行）            │        │   ├─ kSubmitScene → 读 FrameScene 槽
  │                                并行      │   │     → 跑 renderer → 直接发 GPU 命令
  │                                │        │   ├─ kPresent → SwapBuffers
  │                                │        │   └─ sync_sems_.Post（唤醒同步命令调用方）
  │                                │        │
  ├─ BeginScene() 帧 N+1 ◄─────────┘        └─ loop 回 Wait
```

**关键**：主线程 `SubmitScene + Present` 后**立即返回**，不等渲染完成。主线程跑帧 N+1 逻辑时，渲染线程在跑帧 N 渲染——真并行。

---

## 五、迁移计划（每阶段可编译、可测）

| 阶段 | 内容 | 验收 | 状态 |
|---|---|---|---|
| **B1：CommandQueue 序列化缓冲区** | 重构 Entry 列表 → SPSC 环形缓冲区；每线程 sync semaphore 替代栈 SyncFlag（**顺带修 P0**）；命令词汇表 + Dispatch | NullDriver 下命令往返单测通过；现有 demo 行为不变 | ✅ |
| **B2：RenderServer + FrameScene** | 新增 RenderServer（资源方法序列化）；新增 FrameScene 双缓冲；app 资源创建改走 RenderServer | demo 资源创建经命令，RID 正确返回 | ✅ 2026-08-22 |
| **B2-R：保留模式重构** | 新增 ItemStorage（服务端 item 状态）；RenderServer 保留模式 API（CanvasItemCreate/Set*/Free）；RenderViewport::RenderFromStorage；4 demo 迁移；item 命令 PushAndSync（B2 过渡） | demo item 创建一次 + 增量更新，不再每帧重建 layer | ✅ 2026-08-23 |
| **B3-M0：多屏共享栈上提 + 离屏渲染/导出管线** | 共享栈 RenderViewport → ARHudEngine；RenderViewport 瘦身为轻量视口；GL context 改离屏单 context（上屏窗口无 GL 像素格式）；渲染统一 scene→FBO→导出 FrameImage + 平台 PresentImage 接口；ItemStorage 全局化 + visibility_mask/cull_mask + per-screen 相机；删 FrameScene 死路径（详见 `arhud-multiscreen-arch`） | 4 demo 单屏视觉不变；双窗口 demo（cull mask 区分内容、共享 RID） | ← 下一步 |
| **B3：渲染搬线程 + present 线程** | 渲染线程归 ARHudEngine（Engine 级多屏循环：Flush → 遍历 due screens 渲 FBO → 导出 → 通知 present 线程 → frame_done 节拍）；PresentThread[N] 无 GL 平台 API 上屏（vblank 并行）；新增 kSubmitScene handler（全局信号；kPresent 移除）；移除 ExecuteFrame 跨线程回放；item 命令切异步 Push；demo → SubmitScene() | demo 渲染结果与路线 A 一致；SubmitScene 立即返回；双窗口 demo 双屏并行上屏 | 未开始 |
| **B3-P：呈现调度** | RenderSchedule{decimation, phase, clock_screen}（渲染侧；时钟屏恒渲染恒上屏；HUD 抽稀）+ ItemStorage dirty bit（非时钟屏静态跳过）+ present 时序日志 + QNX pixmap 零拷贝 | 60+30 双屏 demo：HUD present 计数 = 仪表一半；静态 HUD 渲染计数不增 | 未开始 |
| **B4：Storage 线程安全收敛** | 确认主线程零直接接触 storage；过渡期保持 RIDOwner\<true\>，稳定后评估降 \<false\> | 静态检查主线程无 storage 直接调用 | 未开始 |
| **B5：清理与优化** | ~~简化 RD 跨线程 frame slot 机制~~ ✅（2026-09-27，命令缓冲退化为单实例）；~~CmdBufferUpdate 深拷贝优化~~ ✅（2026-09-27）；拆后处理出 RenderViewport | 治理 grep 过；帧率不退化 | 进行中 |

**依赖**：B1 先行（NullDriver 单测兜底，重构 CommandQueue 才敢动上层）。B3 是核心，动 RenderViewport 前后必须有 demo 对比验证。

### B2 实际落地与设计的差异（2026-08-22）

设计为本节上文，实现按以下差异落地（`servers/rendering/`）：

| 设计 | 实际实现 | 原因 |
|---|---|---|
| `CommandType` 枚举 | `RenderCommandType`（[render_commands.h](../../servers/rendering/render_commands.h)） | 避免与工程已有 CommandType 枚举冲突 |
| Dispatch 内嵌 CommandQueue | 独立 `CommandExecutor`（实现 `ICommandExecutor` 接口，CommandQueue::SetExecutor 绑定） | 职责分离，便于单测 mock（按 3.3 建议） |
| `FrameScene` 结构体 | `FrameSceneData` + `FrameSceneBuffer`（**已删除**：`frame_scene.h/.cpp` 于 B3-M0 移除，2026-09-27 删孤儿文件本体） | 数据与双缓冲管理分离；`BeginWrite/Submit/TryAcquireRead` API |
| `Push(const T&)` 单参 | `Push(cmd, type, CommandDataSpan*, count)` | 变长数据（像素/名称/码点）经 span 内联，不限于单结构体 |
| RenderServer 直调禁止 | `CanBypassQueue()` 短路：无消费者/调用者即渲染线程/已关闭 → 直调 Storage | 初始化期渲染线程未启动，主线程持 GL 上下文 |
| MaterialSetUniform/SetTexture 异步 | B2 保持主线程直调 | 渲染仍在主线程（路线 A），命令化反而引入竞态；B3 切换 |
| 命令含 kShaderCreateFromSource/kTextureUpdate | 未实现（枚举未含） | demo 尚未用到；按需补 |
| kSubmitScene/kPresent/kResize/kShutdown | 枚举已预留，无执行器 | B3 范围 |

**B2 验证**：4 个 demo 全走 RenderServer；单测 6/6 绿（CommandQueue 往返/短路/shutdown）；phase4_canvas 运行验证 RID 写回正确。

**注意事项**：CommandQueue 内联 1MB 环形缓冲区，**必须堆分配**（栈实例溢出 Windows 默认 1MB 线程栈）。

### B2-R 实际落地（2026-08-23）：保留模式

**背景**：B2 用立即模式（每帧 BeginScene 重建 FrameScene 提交）。B2-R 改为保留模式，对齐 Godot RenderingServer：item 状态存服务端 ItemStorage，增量更新，帧提交仅信号。

**核心组件**：

| 文件 | 职责 |
|---|---|
| `storage/item_storage.h/.cpp` | 渲染线程 per-item 状态存储（Canvas/Spatial/Text ItemData + camera/light 全局状态），按 RID 线性表 |
| `render_commands.h` | item CRUD + camera/light set 命令（Spatial/Text Set* 复用 Canvas 同布局命令，靠 type 区分） |
| `render_server.h/.cpp` | 保留模式 API（CanvasItemCreate/Set*/Free 等）；item 命令全部 `PushAndSync` |
| `render_viewport.h/.cpp` | `RenderFromStorage()`：从 ItemStorage 收集 item + 按 layer 分组，复用立即模式 `Render()` |
| `command_executor.h/.cpp` | item 命令分发到 ItemStorage 对应方法 |

**B2 过渡决策（item 命令同步化）**：
渲染仍在主线程（`RenderFromStorage` 主线程读 ItemStorage），而 ItemStorage 写在渲染线程（命令执行）。若用异步 `Push`，主线程读取时刻命令可能尚未执行（当帧丢 item）或正在执行（数据竞态）。故 B2 item 命令全部走 `PushAndSync`：读取前渲染线程已执行完毕并回到 Wait，信号量提供跨线程内存屏障。B3 渲染搬线程后读写同线程，切回异步 `Push`（真并行关键）。

**修复执行器 2 处 bug**（迁移验证时发现）：
1. `ExecMaterialCreate` 空 shader RID 时未按 `data_type` 取渲染器内置着色器 -> 补 `GetCanvasRenderer/SpatialRenderer::GetShaderRID()` 解析（与 RenderServer 文档契约一致）
2. `ExecuteCommand` 分发错误：`kSpatialItemSetMaterial/SetMesh`、`kTextItemSetFont/SetZOrder/SetLayer/SetVisible` 误调 Canvas 版执行器（写到 CanvasItem 而非对应 item）-> 补 6 个 Exec 方法，分发到 ItemStorage 对应方法

**B2-R 验证**：4 demo 全部保留模式渲染正常（材质/纹理/网格 RID 全 valid，无 invalid material 警告）；item 创建一次 + 渲染循环只调 `RenderFromStorage()`（不再每帧 PushBack）；含动态 item 创建/释放；6000/600 帧干净退出。

---

## 六、测试策略（headless 优先）

依赖 **NullDriver**（`NullRenderingContextDriver` + `NullRenderingDeviceDriver`，全部返回假 ID、不做 GPU 调用）——让 CommandQueue/RenderServer/RenderViewport 逻辑无需窗口、无需 GL 即可单测。

| 测试 | 内容 | 状态 |
|---|---|---|
| CommandQueue 序列化 | Push/PushAndSync 写读指针回绕、背压、Dispatch 正确性 | 部分（6 用例，回绕/背压未覆盖） |
| 命令往返 | 主线程 PushAndSync 创建 → 渲染线程 flush → 验证 storage 状态 + RID 写回 | 部分（函数指针路径，序列化命令待 NullDriver） |
| FrameScene 双缓冲 | 连续 SubmitScene N 帧，验证主写/渲染读无竞态、无撕裂 | 未开始（B3 接线后） |
| 帧流水 | 多帧连续提交，验证帧槽同步（主线程不覆写未消费槽） | 未开始 |
| Shutdown | 命令在途时 SignalShutdown，验证无悬垂、无死锁（P0 回归） | ✅ |
| 真机冒烟 | 5 个 demo 启动渲染正确、3s 内可关 | ✅（phase4_canvas 已验，其余构建通过） |

---

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| FrameScene 双缓冲竞态（主写/渲染读同槽） | 帧槽同步（帧计数 + consumed 标志），复用现有 frame slot 机制；单测覆盖 |
| 命令缓冲区溢出（变长命令/突发） | 定长 1MB + 背压（满则主线程等渲染消费）；item 大数据放 FrameScene 槽而非命令，命令保持小（<256B） |
| 资源创建同步阻塞主线程 | 第一版可接受（创建集中在 init）；RID 预分配列 backlog |
| 大改引入回归 | 每阶段可编译可测；NullDriver headless 单测先行；demo 前后对比 |
| 过渡期主线程仍直接碰 storage | 治理 grep 检查 + 静态断言；B4 阶段强制收敛 |

---

## 八、验收标准（路线 B 完成定义）

1. `grep -rn "GetStorage\|GetRenderer" app/` 零命中（app 不直接碰 storage/renderer，全走 RenderServer）✅（B2 达成）
2. 渲染线程执行 renderer/storage 的全部 GPU 调用（主线程零 GL 调用）
3. 5 个 demo 渲染结果与路线 A 一致，帧率不退化
4. NullDriver headless 单测全绿（含 P0 Shutdown 回归）
5. 主线程 `SubmitScene` 后立即返回（日志可证主线程不阻塞于渲染）
6. 多屏（B3-M0/B3/B3-P）：双窗口 demo 共享 RID 正常渲染、双 present 线程并行上屏；
   HUD 屏 decimation=2 时 present 计数 = 时钟屏一半；静态帧非时钟屏渲染计数不增

---

## 九、与其他工作的关系

- **多屏架构（`arhud-multiscreen-arch`）**：本规范单屏线程模型在多屏下的扩展——B3-M0
  把共享栈从 RenderViewport 上提到 ARHudEngine（对齐 Godot Window/Viewport）并建离屏
  渲染/导出管线；B3 的渲染线程循环改为 Engine 级遍历多屏 + 渲染/上屏分离（present
  线程平台 API 上屏，2026-08-24 决策）+ frame_done 节拍。两份规范配合阅读。
- **治理清单**：B 类问题在 `skill/arhud-improvement` + 根 `AGENTS.md` 中跟踪，与本规范并行推进，不冲突。
- **Facade API（HudTexture/HudMaterial 等 RAII 句柄）**：叠加在 RenderServer 之上，是 app 易用性层。路线 B 稳定后再做，此时 RenderServer 已天然是 app 唯一入口，Facade 只是它的薄包装。
- **Phase 6 Scene Tree**：retained 场景树节点变化 → 直接发增量命令（ItemSet*）喂给
  RenderServer（B2-R 保留模式天然衔接，渲染器零改动）；节点可见性映射 item
  visibility_mask（多屏 cull mask 消费，见 `arhud-multiscreen-arch` §3.3）。
