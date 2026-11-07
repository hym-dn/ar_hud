# 开发日志：item_storage.h 补"为什么不在 Compositor 里"定位注释

- 日期：2026-09-19
- 范围：servers/rendering/storage/item_storage.h（纯注释，零行为变化）
- 依据：变更分级 A 类；用户 Stage A/B 带读中的架构追问——"ItemStorage 为何
  单独持有而不放进 ARHudCompositor 保持一致"

## 1. 变更总览

| 项 | 落点 |
|---|---|
| 定位表下新增 `@par 为什么不放进 ARHudCompositor` 注释块（四条理由 + Godot 同构 + 代价声明） | item_storage.h 文件头 |

## 2. 内容要点（问答固化）

1. **装配依赖分野**：Compositor::Initialize(&rd) 七步链全员依赖 RD；ItemStorage
   纯 CPU 状态零依赖、无 Initialize——合并即制造假依赖
2. **可测性**：单测裸构造（25 用例）依赖"零依赖"这一事实；进 Compositor 则退化集成测试
3. **消费方跨越渲染子系统**：RenderServer/CommandExecutor/RenderScreen 三方读写，
   账本不属于"被 RenderScreen 调用的渲染子系统"
4. **dirty 门控是调度概念**：服务 Engine 级渲染调度，发生在渲染子系统调用之前
   Godot 同构：RendererCompositor 持 renderer 侧 storages，场景状态在 RS 层。
   代价 = EngineStackRefs 多一根指针。

## 3. 验收结果

- 全量构建零 error / 零 warning（纯注释）

## 4. 遗留

- [x] 无。
