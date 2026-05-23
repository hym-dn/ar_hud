/**
 * @file command_buffer_gl.cpp
 * @brief OpenGL 命令缓冲实现（源文件）
 *
 * 本文件实现 GLCommandBuffer 类的所有方法，包括：
 * - **生命周期管理**：构造/析构、Begin/End/Execute/Reset
 * - **命令录制**：所有 ICommandBuffer 接口方法的 override 实现
 * - **命令回放**：ReplayCommand() 的 switch-case 分发逻辑
 * - **内部原语**：Emplace<TCmd>() 模板方法
 *
 * @par 录制-回放架构概览
 *   @code
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                 录制线程（独占 buffer）                     │
 *   │  cmd->Draw(...) → 状态守卫 → Emplace<CmdDraw>(kDraw)      │
 *   │                        ↓                                   │
 *   │              PagedAllocator::Alloc() → RecordedCommand*    │
 *   │              placement new CmdDraw()                       │
 *   │              commands_.PushBack(ptr)                       │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │              GL 线程回放（独占 buffer）                    │
 *   │  cmd->Execute() → 遍历 commands_ → ReplayCommand(cmd)      │
 *   │                        ↓                                   │
 *   │              switch(cmd.type) → driver_->CommandXxx()      │
 *   │                        ↓                                   │
 *   │                   OpenGL API 调用                          │
 *   └─────────────────────────────────────────────────────────────┘
 *   @endcode
 *
 * @par 关键实现细节
 *
 * **1. 多缓冲录制模式**
 *   所有公共录制方法（Draw、BindPipeline 等）遵循统一模式：
 *   ```
 *   if (state_ != kRecording) return;     // 状态守卫（atomic load）
 *   auto &cmd = Emplace<TCmd>(type);       // 分配 + 构造
 *   // ... 填充 cmd 字段 ...
 *   ```
 *   多缓冲模式下每个 CommandBuffer 由单一线程独占，
 *   通过 atomic state_ 保证跨线程状态可见性。
 *
 * **2. PagedAllocator 内存复用**
 *   Begin()/Reset() 调用 PagedAllocator::Free() 释放命令对象，
 *   但 PagedAllocator 本身不释放页内存。后续 Emplace() 时从已释放的
 *   槽位重新分配，避免每帧 malloc/free 开销。
 *
 * **3. 无锁 Execute 设计**
 *   Execute() 不获取 Mutex，基于多缓冲模式：
 *   - 每个 CommandBuffer 由单一线程独占使用
 *   - End() 已被调用，录制线程不再写入 commands_
 *   - 渲染队列保证同一时间只有一个消费者调用 Execute()
 *   - atomic state_ 保证状态跨线程可见
 *
 * **4. Union 安全构造**
 *   Emplace<TCmd>() 使用 placement new 在 union 内存上构造具体类型，
 *   避免直接赋值导致的未定义行为（union 成员未被正确初始化）。
 *
 * @see command_buffer_gl.h 类定义与接口文档
 * @see rendering_device_driver_gl.cpp driver_->CommandXxx() 实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-09
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering_opengl
 */

#include "command_buffer_gl.h"
#include "rendering_device_driver_gl.h"

#include "io/logger.h"

namespace arhud
{

	// ═══════════════════════════════════════════════════════════════════════
	// 构造 / 析构
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief 构造函数实现
	 *
	 * 初始化成员变量，设置关联的设备驱动指针。
	 * 初始状态为 kRecording，可立即开始录制。
	 *
	 * @param[in] p_driver OpenGL 设备驱动指针（非空，生命周期 > 本对象）
	 *
	 * @par 成员初始化列表
	 *   - driver_(p_driver)：存储驱动指针用于后续回放
	 *   - state_(kRecording)：默认枚举初始值
	 *   - command_allocator_：默认构造（预分配第一页内存）
	 *   - commands_：默认构造（空 LocalVector）
	 */
	GLCommandBuffer::GLCommandBuffer(GLRenderingDeviceDriver *p_driver)
		: driver_(p_driver)
	{
	}

	/**
	 * @brief 析构函数实现
	 *
	 * 清理所有剩余的已录制命令对象。
	 * 通过调用 Reset() 实现统一的清理逻辑，
	 * 避免析构和 Reset 的代码重复。
	 *
	 * @warning 析构时不应有其他线程正在使用本对象。
	 *          如果有线程正在 Execute() 中，行为未定义。
	 *
	 * @note 析构前应已停止所有对 CommandBuffer 的引用。
	 */
	GLCommandBuffer::~GLCommandBuffer()
	{
		Reset();
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 生命周期管理
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief Begin() 实现 — 开始新一轮命令录制
	 *
	 * 重置命令缓冲区到干净状态。释放上一轮所有命令对象，
	 * 清空命令列表，进入 Recording 状态。
	 *
	 * @par 多缓冲模式下的状态约束
	 *   缓冲池保证 Begin() 被调用时，buffer 不处于 kExecuting 状态：
	 *   - **kSubmitted**：正常路径，上一帧 Execute() 已完成
	 *   - **kReady**：End() 已调用但尚未 Execute()，允许重新录制
	 *   - **kRecording**：容错，自动 Reset 并重新开始
	 *   - **kExecuting**：编程错误，缓冲池不应分配正在执行的 buffer
	 *
	 * @par 时间复杂度
	 *   O(n)，其中 n 为上一轮录制的命令数量。
	 */
	void GLCommandBuffer::Begin()
	{
		CommandBufferState current_state = state_.load(std::memory_order_acquire);

		switch (current_state)
		{
		case CommandBufferState::kReady:
		case CommandBufferState::kSubmitted:
			break;
		case CommandBufferState::kRecording:
			break;
		case CommandBufferState::kExecuting:
			ARHUD_ASSERT(false, "Begin() called while buffer is executing. "
								"Buffer pool should not allocate executing buffers.");
			return;
		default:
			ARHUD_LOG_ERROR("ERR_INVALID_STATE", "Begin(): unknown state %u",
							static_cast<uint32_t>(current_state));
			return;
		}

		for (uint32_t i = 0; i < commands_.Size(); ++i)
		{
			command_allocator_.Free(commands_[i]);
		}

		commands_.Clear();
		state_.store(CommandBufferState::kRecording, std::memory_order_release);
		resource_tracker_.BeginFrame();
	}

	/**
	 * @brief End() 实现 — 结束命令录制
	 *
	 * 将状态从 Recording 切换为 Ready，表示所有命令已录制完毕，
	 * 可以提交给渲染线程执行。
	 *
	 * @par 算法步骤
	 *   1. 检查 state_ == kRecording（atomic load）
	 *      - 如果是：切换到 kReady（atomic store release）
	 *      - 如果不是：静默忽略（可能是重复 End 或非法状态）
	 *
	 * @note 不检查 commands_ 是否为空。允许空的 CommandBuffer 被 Execute
	 *       （Execute 会跳过空列表，直接设为 Submitted）。
	 */
	void GLCommandBuffer::End()
	{
		if (state_.load(std::memory_order_acquire) == CommandBufferState::kRecording)
		{
			state_.store(CommandBufferState::kReady, std::memory_order_release);
		}
	}

	/**
	 * @brief Execute() 实现 — 回放所有已录制命令到 GL API
	 *
	 * 核心回放方法。按录制顺序逐条将命令转换为 GL API 调用。
	 * 这是整个 CommandBuffer 系统的关键路径。
	 *
	 * @par 算法步骤
	 *   1. **前置条件检查**：断言 state_ == kReady
	 *      多缓冲模式下，提交队列保证 Execute 只在 End() 之后调用
	 *   2. **状态转换**：state_ = kExecuting（标记正在回放）
	 *   3. **线性遍历**：for i in [0, commands_.Size()):
	 *      - ReplayCommand(*commands_[i]) — 单条命令分发
	 *   4. **完成标记**：state_ = kSubmitted（标记回放完成）
	 *
	 * @par 性能特征
	 *   - **时间复杂度**：O(n)，n = 命令数量
	 *   - **每次 ReplayCommand 开销**：
	 *     switch-case 分支预测 + 函数调用 + GL API 调用
	 *   - **无锁设计**：多缓冲下 GL 线程独占 buffer
	 *   - **缓存友好**：commands_ 是连续指针数组，顺序访问
	 *
	 * @par 错误处理策略
	 *   - 不捕获异常（项目禁止异常）
	 *   - GL 错误由 driver_->CommandXxx() 内部处理（如 Logger 记录）
	 *   - 单条命令失败不影响后续命令执行（switch-case 继续下一条）
	 *
	 * @warning 调用者必须确保当前线程拥有有效的 GL 上下文（MakeCurrent）。
	 */
	void GLCommandBuffer::Execute()
	{
		CommandBufferState current_state = state_.load(std::memory_order_acquire);

		ARHUD_ASSERT(current_state == CommandBufferState::kReady,
					 "Execute() called on buffer not in kReady state. "
					 "Submission queue should only enqueue after End().");

		if (current_state != CommandBufferState::kReady)
		{
			return;
		}

		state_.store(CommandBufferState::kExecuting, std::memory_order_release);

		for (uint32_t i = 0; i < commands_.Size(); ++i)
		{
			ReplayCommand(*commands_[i]);
		}

		state_.store(CommandBufferState::kSubmitted, std::memory_order_release);
	}

	/**
	 * @brief Reset() 实现 — 重置命令缓冲区
	 *
	 * 释放所有已录制命令对象，回到干净的 Recording 状态。
	 * 用于跨帧复用同一个 CommandBuffer 对象。
	 *
	 * @par 典型调用时机
	 *   ```
	 *   Frame N:  Begin() → 录制 → End() → Execute() → Reset()
	 *   Frame N+1: Begin() → 录制 → End() → Execute() → Reset()
	 *   ...
	 *   ```
	 *
	 * @par 与 Begin() 的关系
	 *   内部逻辑与 Begin() 相似，但调用 EndFrame() 而非 BeginFrame()。
	 *   两者都执行：
	 *   1. 遍历 Free 所有命令对象
	 *   2. Clear 命令列表
	 *   3. 设 Recording 状态
	 *   4. 重置追踪器：resource_tracker_.EndFrame()
	 *
	 * @par 多缓冲模式下的状态约束
	 *   缓冲池保证 Reset() 被调用时，buffer 不处于 kExecuting 状态。
	 *   如果遇到 kExecuting，说明缓冲池管理有编程错误。
	 */
	void GLCommandBuffer::Reset()
	{
		CommandBufferState current_state = state_.load(std::memory_order_acquire);

		ARHUD_ASSERT(current_state != CommandBufferState::kExecuting,
					 "Reset() called while buffer is executing. "
					 "Buffer pool should not reset executing buffers.");

		if (current_state == CommandBufferState::kExecuting)
		{
			return;
		}

		for (uint32_t i = 0; i < commands_.Size(); ++i)
		{
			command_allocator_.Free(commands_[i]);
		}

		commands_.Clear();
		state_.store(CommandBufferState::kRecording, std::memory_order_release);
		resource_tracker_.EndFrame();
	}

	/**
	 * @brief GetState() 实现 — 获取当前生命周期状态
	 *
	 * 直接返回 state_ 成员变量。
	 *
	 * @return CommandBufferState 当前状态值
	 *
	 * @note state_ 为 atomic 类型，读取保证可见性。
	 *       多缓冲模式下每个 CommandBuffer 由单一线程独占，
	 *       读取结果与实际状态一致。
	 */
	CommandBufferState GLCommandBuffer::GetState() const
	{
		return state_.load(std::memory_order_acquire);
	}

	/**
	 * @brief GetCommandCount() 实现 — 获取已录制命令数量
	 *
	 * 返回当前命令列表中的条目数。
	 *
	 * @return uint32_t 命令数量（0 ~ UINT32_MAX）
	 *
	 * @note 返回值在录制过程中动态增长（每次 Emplace 后 +1）。
	 *       如需稳定快照，应在 End() 之后、Execute() 之前读取。
	 *       用于性能分析、调试断言或日志记录。
	 */
	uint32_t GLCommandBuffer::GetCommandCount() const
	{
		return static_cast<uint32_t>(commands_.Size());
	}

	// ═══════════════════════════════════════════════════════════════════════
	// Render Pass 命令录制
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief BeginRenderPass() 实现 — 录制开始渲染 Pass 命令
	 *
	 * 将 BeginRenderPass 参数拷贝到 CmdBeginRenderPass 结构体中。
	 * clear_values 数组需要逐一拷贝到 FixedVector 中。
	 *
	 * @par 录制流程
	 *   1. 检查 state_ == kRecording（否则静默返回）
	 *   2. Emplace<CmdBeginRenderPass>(kBeginRenderPass)
	 *      → 分配 RecordedCommand + placement new CmdBeginRenderPass
	 *   3. 拷贝 render_pass, framebuffer, rect_x/y/w/h
	 *   4. 清空 cmd.clear_values FixedVector
	 *   5. 遍历 p_clear_values，逐一 PushBack 到 cmd.clear_values
	 *
	 * @note clear_values 使用 FixedVector（栈内嵌数组）存储，
	 *       最大支持 kMaxClearValues=4 个附件清除值，
	 *       覆盖 HUD 场景的典型需求（Color0 + DepthStencil）。
	 */
	void GLCommandBuffer::BeginRenderPass(
		RenderPassID p_render_pass, FramebufferID p_framebuffer,
		VectorView<RenderPassClearValue> p_clear_values, int32_t p_rect_x,
		int32_t p_rect_y, uint32_t p_rect_w, uint32_t p_rect_h)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdBeginRenderPass>(CommandType::kBeginRenderPass);
		cmd.render_pass = p_render_pass;
		cmd.framebuffer = p_framebuffer;
		cmd.rect_x = p_rect_x;
		cmd.rect_y = p_rect_y;
		cmd.rect_w = p_rect_w;
		cmd.rect_h = p_rect_h;

		cmd.clear_values.Clear();
		for (uint32_t i = 0; i < p_clear_values.Size(); ++i)
		{
			cmd.clear_values.PushBack(p_clear_values[i]);
		}

		// 资源追踪：注册并记录 RenderPass 和 Framebuffer 的使用
		resource_tracker_.Register(p_render_pass.GetId(), ResourceType::kRenderPass,
								   "BeginRenderPass:RP");
		resource_tracker_.Register(p_framebuffer.GetId(), ResourceType::kFramebuffer,
								   "BeginRenderPass:FB");
		resource_tracker_.RecordUsage(p_framebuffer.GetId(),
									  ResourceUsage::kAttachmentColorReadWrite,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	/**
	 * @brief EndRenderPass() 实现 — 录制结束渲染 Pass 命令
	 *
	 * 录制一条无额外参数的 kEndRenderPass 类型命令。
	 * 仅记录类型标签，回放时调用 driver_->CommandEndRenderPass()。
	 *
	 * @note 使用 Emplace<RecordedCommand::Data> 而非具体结构体类型，
	 *       因为 EndRenderPass 不携带额外数据（仅 type 字段有意义）。
	 *       Data 的默认构造不做任何事（union 成员不初始化）。
	 */
	void GLCommandBuffer::EndRenderPass()
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		Emplace<RecordedCommand::Data>(CommandType::kEndRenderPass);
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 资源绑定命令录制
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief BindRenderPipeline() 实现 — 录制绑定管线命令
	 *
	 * 将管线 ID 存入 CmdBindPipeline 结构体。
	 * 回放时触发 glUseProgram + PSO 状态应用。
	 */
	void GLCommandBuffer::BindRenderPipeline(PipelineID p_pipeline)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdBindPipeline>(CommandType::kBindPipeline);
		cmd.pipeline = p_pipeline;

		// 资源追踪：注册并记录 Pipeline 的使用
		resource_tracker_.Register(p_pipeline.GetId(), ResourceType::kPipeline,
								   "BindPipeline");
		resource_tracker_.RecordUsage(p_pipeline.GetId(),
									  ResourceUsage::kTextureSample,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	/**
	 * @brief BindUniformSet() 实现 — 录制绑定 Uniform Set 命令
	 *
	 * 将 Uniform Set ID 和 set_index 存入 CmdBindUniformSet 结构体。
	 * 回放时绑定 UBO 和采样器到对应的 GL 绑定点。
	 */
	void GLCommandBuffer::BindUniformSet(UniformSetID p_uniform_set,
										 uint32_t p_set_index)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdBindUniformSet>(CommandType::kBindUniformSet);
		cmd.uniform_set = p_uniform_set;
		cmd.set_index = p_set_index;

		// 资源追踪：注册并记录 UniformSet 的使用
		resource_tracker_.Register(p_uniform_set.GetId(), ResourceType::kUniformSet,
								   "BindUniformSet");
		resource_tracker_.RecordUsage(p_uniform_set.GetId(),
									  ResourceUsage::kTextureSample,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	/**
	 * @brief BindVertexBuffers() 实现 — 录制绑定顶点缓冲区命令
	 *
	 * 拷贝顶点缓冲区 ID 数组和偏移量数组到 CmdBindVertexBuffers 结构体。
	 *
	 * @par 边界保护
	 *   如果 p_count > kMaxBindings(4)，只拷贝前 kMaxBindings 个元素。
	 *   HUD 场景通常只需 1~2 个顶点缓冲区（位置 + UV），
	 *   4 个的上限留有余量。
	 *
	 * @note buffers[] 和 offsets[] 是 C 风格数组（裸指针），
	 *       由调用者保证在 Emplace 完成前有效。
	 *       拷贝到 Cmd 结构体后就与原始数组无关了。
	 */
	void GLCommandBuffer::BindVertexBuffers(const BufferID *p_buffers,
											const uint64_t *p_offsets,
											uint32_t p_count)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdBindVertexBuffers>(CommandType::kBindVertexBuffers);
		uint32_t copy_count = (p_count > CmdBindVertexBuffers::kMaxBindings)
								  ? CmdBindVertexBuffers::kMaxBindings
								  : p_count;
		cmd.count = copy_count;
		for (uint32_t i = 0; i < copy_count; ++i)
		{
			cmd.buffers[i] = p_buffers[i];
			cmd.offsets[i] = p_offsets[i];

			// 资源追踪：注册并记录每个顶点缓冲区的使用
			resource_tracker_.Register(p_buffers[i].GetId(), ResourceType::kBuffer,
									   "BindVB");
			resource_tracker_.RecordUsage(p_buffers[i].GetId(),
										  ResourceUsage::kCopyFrom,
										  static_cast<uint32_t>(commands_.Size() - 1));
		}
	}

	/**
	 * @brief BindIndexBuffer() 实现 — 录制绑定索引缓冲区命令
	 *
	 * 将索引缓冲区 ID、格式和偏移存入 CmdBindIndexBuffer 结构体。
	 * 回放时调用 glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...)。
	 */
	void GLCommandBuffer::BindIndexBuffer(BufferID p_buffer,
										  IndexBufferFormat p_format,
										  uint64_t p_offset)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdBindIndexBuffer>(CommandType::kBindIndexBuffer);
		cmd.buffer = p_buffer;
		cmd.format = p_format;
		cmd.offset = p_offset;

		// 资源追踪：注册并记录索引缓冲区的使用
		resource_tracker_.Register(p_buffer.GetId(), ResourceType::kBuffer, "BindIB");
		resource_tracker_.RecordUsage(p_buffer.GetId(), ResourceUsage::kCopyFrom,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 绘制命令录制
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief Draw() 实现 — 录制非索引绘制命令
	 *
	 * 将绘制参数存入 CmdDraw 结构体。
	 * 回放时调用 glDrawArraysInstancedBaseInstance()。
	 *
	 * @par 参数映射
	 *   | CmdDraw 字段 | GL 参数 |
	 *   |-------------|---------|
	 *   | vertex_count | count |
	 *   | instance_count | primcount |
	 *   | base_vertex | first（对 DrawArrays 无效但保留兼容性） |
	 *   | first_instance | baseinstance |
	 */
	void GLCommandBuffer::Draw(uint32_t p_vertex_count, uint32_t p_instance_count,
							   uint32_t p_base_vertex, uint32_t p_first_instance)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdDraw>(CommandType::kDraw);
		cmd.vertex_count = p_vertex_count;
		cmd.instance_count = p_instance_count;
		cmd.base_vertex = p_base_vertex;
		cmd.first_instance = p_first_instance;
	}

	/**
	 * @brief DrawIndexed() 实现 — 录制索引绘制命令
	 *
	 * 将绘制参数存入 CmdDrawIndexed 结构体。
	 * 回放时调用 glDrawElementsInstancedBaseVertexBaseInstance()。
	 *
	 * @par 参数映射
	 *   | CmdDrawIndexed 字段 | GL 参数 |
	 *   |---------------------|---------|
	 *   | index_count | count |
	 *   | instance_count | primcount |
	 *   | first_index | indices（字节偏移由格式决定） |
	 *   | vertex_offset | basevertex |
	 *   | first_instance | baseinstance |
	 */
	void GLCommandBuffer::DrawIndexed(uint32_t p_index_count,
									  uint32_t p_instance_count,
									  uint32_t p_first_index,
									  int32_t p_vertex_offset,
									  uint32_t p_first_instance)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdDrawIndexed>(CommandType::kDrawIndexed);
		cmd.index_count = p_index_count;
		cmd.instance_count = p_instance_count;
		cmd.first_index = p_first_index;
		cmd.vertex_offset = p_vertex_offset;
		cmd.first_instance = p_first_instance;
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 动态状态设置命令录制
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief SetViewport() 实现 — 录制视口设置命令
	 *
	 * 将视口参数存入 CmdSetViewport 结构体。
	 * 回放时调用 glViewport(x, y, width, height)。
	 */
	void GLCommandBuffer::SetViewport(int32_t p_x, int32_t p_y, uint32_t p_width,
									  uint32_t p_height)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdSetViewport>(CommandType::kSetViewport);
		cmd.x = p_x;
		cmd.y = p_y;
		cmd.width = p_width;
		cmd.height = p_height;
	}

	/**
	 * @brief SetScissor() 实现 — 录制裁剪矩形设置命令
	 *
	 * 将裁剪矩形参数存入 CmdSetScissor 结构体。
	 * 回放时调用 glScissor(x, y, width, height)。
	 */
	void GLCommandBuffer::SetScissor(
		int32_t p_x, int32_t p_y, uint32_t p_width, uint32_t p_height)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdSetScissor>(CommandType::kSetScissor);
		cmd.x = p_x;
		cmd.y = p_y;
		cmd.width = p_width;
		cmd.height = p_height;
	}

	/**
	 * @brief SetBlendConstants() 实现 — 录制混合常量颜色命令
	 *
	 * 将 RGBA 颜色值存入 CmdSetBlendConstants 结构体。
	 * 回放时调用 glBlendColor(r, g, b, a)。
	 */
	void GLCommandBuffer::SetBlendConstants(float p_r, float p_g, float p_b,
											float p_a)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdSetBlendConstants>(CommandType::kSetBlendConstants);
		cmd.r = p_r;
		cmd.g = p_g;
		cmd.b = p_b;
		cmd.a = p_a;
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 数据传输命令录制
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief ClearBuffer() 实现 — 录制缓冲区清除命令
	 *
	 * 将缓冲区 ID 和清除范围存入 CmdClearBuffer 结构体。
	 * 回放时调用 glClearBufferData/glClearBufferSubData。
	 *
	 * @warning 此操作可能导致 GPU-CPU 同步等待（GPU Stall）。
	 */
	void GLCommandBuffer::ClearBuffer(BufferID p_buffer, uint64_t p_offset,
									  uint64_t p_size)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdClearBuffer>(CommandType::kClearBuffer);
		cmd.buffer = p_buffer;
		cmd.offset = p_offset;
		cmd.size = p_size;

		// 资源追踪：注册并记录缓冲区清除操作
		resource_tracker_.Register(p_buffer.GetId(), ResourceType::kBuffer,
								   "ClearBuffer");
		resource_tracker_.RecordUsage(p_buffer.GetId(), ResourceUsage::kCopyTo,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	/**
	 * @brief CopyBuffer() 实现 — 录制缓冲区拷贝命令
	 *
	 * 将源/目标缓冲区 ID 和拷贝区域列表存入 CmdCopyBuffer 结构体。
	 * regions 数组逐一拷贝到 FixedVector 中。
	 * 回放时对每个区域调用 glCopyBufferSubData。
	 */
	void GLCommandBuffer::CopyBuffer(BufferID p_src_buffer, BufferID p_dst_buffer,
									 VectorView<BufferCopyRegion> p_regions)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdCopyBuffer>(CommandType::kCopyBuffer);
		cmd.src_buffer = p_src_buffer;
		cmd.dst_buffer = p_dst_buffer;

		cmd.regions.Clear();
		for (uint32_t i = 0; i < p_regions.Size(); ++i)
		{
			cmd.regions.PushBack(p_regions[i]);
		}

		// 资源追踪：注册并记录源/目标缓冲区的使用
		resource_tracker_.Register(p_src_buffer.GetId(), ResourceType::kBuffer,
								   "CopyBuffer:Src");
		resource_tracker_.Register(p_dst_buffer.GetId(), ResourceType::kBuffer,
								   "CopyBuffer:Dst");
		resource_tracker_.RecordUsage(p_src_buffer.GetId(), ResourceUsage::kCopyFrom,
									  static_cast<uint32_t>(commands_.Size() - 1));
		resource_tracker_.RecordUsage(p_dst_buffer.GetId(), ResourceUsage::kCopyTo,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	/**
	 * @brief ClearColorTexture() 实现 — 录制颜色纹理清除命令
	 *
	 * 将纹理 ID、清除颜色和子资源范围存入 CmdClearColorTexture 结构体。
	 * 回放时调用 glClearTexSubImage。
	 */
	void GLCommandBuffer::ClearColorTexture(
		TextureID p_texture, float p_color_r, float p_color_g, float p_color_b,
		float p_color_a, const TextureSubresourceRange &p_subresources)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdClearColorTexture>(CommandType::kClearColorTexture);
		cmd.texture = p_texture;
		cmd.color_r = p_color_r;
		cmd.color_g = p_color_g;
		cmd.color_b = p_color_b;
		cmd.color_a = p_color_a;
		cmd.subresources = p_subresources;

		// 资源追踪：注册并记录颜色纹理清除操作
		resource_tracker_.Register(p_texture.GetId(), ResourceType::kTexture,
								   "ClearColorTex");
		resource_tracker_.RecordUsage(p_texture.GetId(), ResourceUsage::kCopyTo,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	/**
	 * @brief ClearDepthStencilTexture() 实现 — 录制深度/模板纹理清除命令
	 *
	 * 将纹理 ID、深度值、模板值和子资源范围存入 CmdClearDepthStencilTexture
	 * 结构体。 回放时调用 glClearTexSubImage（depth_stencil 格式）。
	 */
	void GLCommandBuffer::ClearDepthStencilTexture(
		TextureID p_texture, float p_depth, uint32_t p_stencil,
		const TextureSubresourceRange &p_subresources)
	{
		if (state_.load(std::memory_order_acquire) != CommandBufferState::kRecording)
		{
			return;
		}

		auto &cmd = Emplace<CmdClearDepthStencilTexture>(
			CommandType::kClearDepthStencilTexture);
		cmd.texture = p_texture;
		cmd.depth = p_depth;
		cmd.stencil = p_stencil;
		cmd.subresources = p_subresources;

		// 资源追踪：注册并记录深度/模板纹理清除操作
		resource_tracker_.Register(p_texture.GetId(), ResourceType::kTexture,
								   "ClearDSTex");
		resource_tracker_.RecordUsage(p_texture.GetId(), ResourceUsage::kCopyTo,
									  static_cast<uint32_t>(commands_.Size() - 1));
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 私有辅助方法
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief Emplace<TCmd>() 模板方法实现 — 核心录制原语
	 *
	 * 所有公共录制方法的最终落点。负责：
	 *   1. 从 PagedAllocator 分配一个 RecordedCommand 对象
	 *   2. 设置命令类型标签
	 *   3. 在 union 内存上 placement new 构造具体的 TCmd 类型
	 *   4. 将对象指针追加到 commands_ 有序数组
	 *   5. 返回 TCmd 的可写引用供调用者填充字段
	 *
	 * @tparam TCmd 具体的命令数据结构类型
	 * @param[in] p_type 命令类型枚举值
	 * @return TCmd& 新分配命令数据的引用
	 *
	 * @par 内存布局示意
	 *   @code
	 *   PagedAllocator 分配的内存:
	 *   ┌──────────────────────────────────────────┐
	 *   │ RecordedCommand {                         │
	 *   │   type: CommandType (1B + 7B padding)     │
	 *   │   data: union {                           │
	 *   │     [TCmd 类型的数据 — placement new]     │
	 *   │   }                                       │
	 *   │ }                                         │
	 *   └──────────────────────────────────────────┘
	 *   @endcode
	 *
	 * @par 为什么用 placement new
	 *   RecordedCommand::Data 是联合体，其成员共享原始内存。
	 *   直接赋值 `cmd.data = TCmd{...}` 会调用 TCmd 的拷贝赋值运算符，
	 *   但 union 内存上没有有效的 TCmd 对象（未构造），导致 UB。
	 *   placement new `new (&cmd->data) TCmd()` 正确地在已知地址构造对象。
	 *
	 * @par reinterpret_cast 安全性
	 *   返回 `reinterpret_cast<TCmd&>(cmd->data)` 是安全的，因为：
	 *   - cmd->data 就是刚刚通过 placement new 构造 TCmd 的地址
	 *   - TCmd 是 Data 联合体的成员之一
	 *   - 别名规则允许在同一块内存上以不同成员类型访问
	 *
	 * @pre state_ == kRecording
	 */
	template <typename TCmd>
	TCmd &GLCommandBuffer::Emplace(CommandType p_type)
	{
		RecordedCommand *cmd = command_allocator_.Alloc();
		cmd->type = p_type;
		new (&cmd->data) TCmd();

		commands_.PushBack(cmd);

		return reinterpret_cast<TCmd &>(cmd->data);
	}

	/**
	 * @brief ReplayCommand() 实现 — 单条命令回放分发
	 *
	 * Execute() 的核心分发逻辑。根据 RecordedCommand::type 字段，
	 * 通过 switch-case 将命令分派到对应的 driver_->CommandXxx() 方法。
	 *
	 * @param[in] p_cmd 已录制命令的常量引用
	 *
	 * @par 分发表（完整映射）
	 *   | case | 提取的数据 | driver_-> 调用 | GL 等效 |
	 *   |------|-----------|---------------|---------|
	 *   | kBeginRenderPass | begin_render_pass | CommandBeginRenderPass(rp, fb,
	 * clears, x, y, w, h) | glBindFramebuffer + glClear | | kEndRenderPass | (无) |
	 * CommandEndRenderPass() | （无操作） | | kBindPipeline |
	 * bind_pipeline.pipeline | CommandBindRenderPipeline(id) | glUseProgram + PSO |
	 *   | kBindUniformSet | bind_uniform_set | CommandBindUniformSet(set, idx) |
	 * glBindBufferBase | | kBindVertexBuffers | bind_vertex_buffers |
	 * CommandBindVertexBuffers(bufs, offs, n) | glBindVertexBuffer × n | |
	 * kBindIndexBuffer | bind_index_buffer | CommandBindIndexBuffer(buf, fmt, off)
	 * | glBindBuffer(EAB) | | kDraw | draw | CommandDraw(vc, ic, bv, fi) |
	 * glDrawArrays... | | kDrawIndexed | draw_indexed | CommandDrawIndexed(ic, ic,
	 * fi, vo, fi) | glDrawElements... | | kSetViewport | set_viewport |
	 * CommandSetViewport(x, y, w, h) | glViewport | | kSetScissor | set_scissor |
	 * CommandSetScissor(x, y, w, h) | glScissor | | kSetBlendConstants |
	 * set_blend_constants | CommandSetBlendConstants(r,g,b,a) | glBlendColor | |
	 * kClearBuffer | clear_buffer | CommandClearBuffer(buf, off, sz) |
	 * glClearBufferData | | kCopyBuffer | copy_buffer | CommandCopyBuffer(src, dst,
	 * regs) | glCopyBufferSubData × N | | kClearColorTexture | clear_color_texture
	 * | CommandClearColorTexture(tex,r,g,b,a,sr) | glClearTexSubImage | |
	 * kClearDepthStencilTexture | clear_depth_stencil_texture |
	 * CommandClearDepthStencilTexture(tex,d,s,sr) | glClearTexSubImage |
	 *
	 * @par 数据提取模式
	 *   对于带参数的命令，使用局部 const 引用提取数据：
	 *   ```cpp
	 *   case CommandType::kDraw:
	 *   {
	 *       const auto &c = p_cmd.data.draw;  // 类型安全提取
	 *       driver_->CommandDraw(c.vertex_count, c.instance_count,
	 *                            c.base_vertex, c.first_instance);
	 *       break;
	 *   }
	 *   ```
	 *   对于无参数命令（如 kEndRenderPass），直接调用 driver_ 方法。
	 *
	 * @par default 分支
	 *   静默忽略未知命令类型。这是防御性编程策略：
	 *   - 避免因数据损坏导致崩溃
	 *   - 允许未来扩展命令类型而不破坏旧版本回放
	 *   - 生产环境可通过 Logger 记录警告
	 *
	 * @warning 此方法不加锁！必须在 GL 上下文线程由 Execute() 串行调用。
	 *       driver_->CommandXxx() 内部会修改 GL 状态机，
	 *       并行调用会导致 GL 状态竞争。
	 */
	void GLCommandBuffer::ReplayCommand(const RecordedCommand &p_cmd)
	{
		switch (p_cmd.type)
		{
		case CommandType::kBeginRenderPass:
		{
			const auto &c = p_cmd.data.begin_render_pass;
			driver_->CommandBeginRenderPass(c.render_pass, c.framebuffer,
											c.clear_values, c.rect_x, c.rect_y,
											c.rect_w, c.rect_h);
			break;
		}

		case CommandType::kEndRenderPass:
			driver_->CommandEndRenderPass();
			break;

		case CommandType::kBindPipeline:
			driver_->CommandBindRenderPipeline(p_cmd.data.bind_pipeline.pipeline);
			break;

		case CommandType::kBindUniformSet:
		{
			const auto &c = p_cmd.data.bind_uniform_set;
			driver_->CommandBindUniformSet(c.uniform_set, c.set_index);
			break;
		}

		case CommandType::kBindVertexBuffers:
		{
			const auto &c = p_cmd.data.bind_vertex_buffers;
			driver_->CommandBindVertexBuffers(c.buffers, c.offsets, c.count);
			break;
		}

		case CommandType::kBindIndexBuffer:
		{
			const auto &c = p_cmd.data.bind_index_buffer;
			driver_->CommandBindIndexBuffer(c.buffer, c.format, c.offset);
			break;
		}

		case CommandType::kDraw:
		{
			const auto &c = p_cmd.data.draw;
			driver_->CommandDraw(c.vertex_count, c.instance_count, c.base_vertex,
								 c.first_instance);
			break;
		}

		case CommandType::kDrawIndexed:
		{
			const auto &c = p_cmd.data.draw_indexed;
			driver_->CommandDrawIndexed(c.index_count, c.instance_count, c.first_index,
										c.vertex_offset, c.first_instance);
			break;
		}

		case CommandType::kSetViewport:
		{
			const auto &c = p_cmd.data.set_viewport;
			driver_->CommandSetViewport(c.x, c.y, c.width, c.height);
			break;
		}

		case CommandType::kSetScissor:
		{
			const auto &c = p_cmd.data.set_scissor;
			driver_->CommandSetScissor(c.x, c.y, c.width, c.height);
			break;
		}

		case CommandType::kSetBlendConstants:
		{
			const auto &c = p_cmd.data.set_blend_constants;
			driver_->CommandSetBlendConstants(c.r, c.g, c.b, c.a);
			break;
		}

		case CommandType::kClearBuffer:
		{
			const auto &c = p_cmd.data.clear_buffer;
			driver_->CommandClearBuffer(c.buffer, c.offset, c.size);
			break;
		}

		case CommandType::kCopyBuffer:
		{
			const auto &c = p_cmd.data.copy_buffer;
			driver_->CommandCopyBuffer(c.src_buffer, c.dst_buffer, c.regions);
			break;
		}

		case CommandType::kClearColorTexture:
		{
			const auto &c = p_cmd.data.clear_color_texture;
			driver_->CommandClearColorTexture(c.texture, c.color_r, c.color_g,
											  c.color_b, c.color_a, c.subresources);
			break;
		}

		case CommandType::kClearDepthStencilTexture:
		{
			const auto &c = p_cmd.data.clear_depth_stencil_texture;
			driver_->CommandClearDepthStencilTexture(c.texture, c.depth, c.stencil,
													 c.subresources);
			break;
		}

		default:
			break;
		}
	}

	// ═══════════════════════════════════════════════════════════════════════
	// 资源追踪查询接口
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @brief GetResourceTracker() 实现 — 返回资源追踪器指针
	 *
	 * 返回内部 resource_tracker_ 成员的常量指针。
	 * 用于外部查询当前帧的资源使用情况、调试信息或性能统计。
	 *
	 * @return const ResourceTracker* 始终返回 &resource_tracker_（非 nullptr）
	 *
	 * @par 典型使用场景
	 *   @code
	 *   // 调试时输出资源使用状态
	 *   const ResourceTracker *tracker = cmd_buf->GetResourceTracker();
	 *   if (tracker) {
	 *       tracker->DumpState();
	 *   }
	 *
	 *   // 性能监控：统计活跃资源数
	 *   uint32_t active_resources = tracker->GetActiveCount();
	 *   @endcode
	 *
	 * @note 返回的指针在 GLCommandBuffer 生命周期内始终有效。
	 *       返回的是常量指针，外部无法修改追踪器状态。
	 */
	const ResourceTracker *GLCommandBuffer::GetResourceTracker() const
	{
		return &resource_tracker_;
	}

} // namespace arhud
