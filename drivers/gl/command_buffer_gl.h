/**
 * @file command_buffer_gl.h
 * @brief OpenGL 命令缓冲实现（头文件）
 *
 * 本文件定义 GLCommandBuffer 类，它是 ICommandBuffer 接口的 OpenGL 后端实现。
 * 核心职责是将录制阶段收集的渲染命令在回放阶段转换为 GL API 调用。
 *
 * @par 架构定位
 *   @code
 *   ┌──────────────────────┐     录制        ┌────────────────────────┐
 *   │ 用户代码              │ ──────────→    │ GLCommandBuffer         │
 *   │ (Main Thread)         │  ICommandBuffer │                        │
 *   │                      │  接口方法       │ commands_ (指针数组)     │
 *   │ cmd->Draw(...)       │                │ command_allocator_      │
 *   │ cmd->BindPipeline()  │                │ state_ (atomic)         │
 *   └──────────────────────┘                └──────────┬─────────────┘
 *                                                      │ Execute()
 *                                                      ▼
 *   ┌──────────────────────┐     回放        ┌────────────────────────┐
 *   │ OpenGL Driver        │ ←─────────────  │ ReplayCommand()        │
 *   │                      │  driver_->Xxx() │ switch(type) → GL API  │
 *   │ glDrawArrays()       │                │ glUseProgram()         │
 *   │ glBindFramebuffer()  │                │ glClear()              │
 *   └──────────────────────┘                └────────────────────────┘
 *   @endcode
 *
 * @par 内存管理策略
 *   - **命令对象分配**：使用 PagedAllocator<RecordedCommand> 分配，
 *     提供 O(1) 分配/释放，避免 malloc/free 开销和内存碎片
 *   - **命令顺序记录**：LocalVector<RecordedCommand*> 指针数组，
 *     避免 RecordedCommand（含 union）不可复制的问题
 *   - **生命周期**：Reset()/Begin() 时遍历指针数组逐一 Free，
 *     析构函数中同样清理所有剩余命令
 *   - **PagedAllocator 复用**：allocator 本身不释放，仅重置内部页链表
 *
 * @par 线程安全模型
 *   | 操作 | 线程 | 同步机制 | 说明 |
 *   |------|------|----------|------|
 *   | Begin / End / 所有 Bind/Draw 方法 | 独占线程 | atomic state_ | 多缓冲下同一线程独占 |
 *   | GetState / GetCommandCount | 任意线程 | atomic state_ | 原子读 |
 *   | Execute | GL 上下文线程 | atomic state_ | 必须独占调用 |
 *   | Reset | 独占线程 | atomic state_ | 多缓冲下同一线程独占 |
 *
 * @par 与立即模式的关系
 *   GLCommandBuffer **不替代** GLRenderingDeviceDriver 的立即模式 CommandXxx()。
 *   它是更高层的录制-回放抽象：
 *   ```
 *   用户代码 → ICommandBuffer::Draw()  → [录制 CmdDraw]
 *   Execute  → GLCommandBuffer::ReplayCommand()
 *            → driver_->CommandDraw()  → [立即模式] → glDrawArrays()
 *   ```
 *
 * @see command_buffer.h          接口定义与命令类型
 * @see rendering_device_driver_gl.h 设备驱动实现
 * @see IRenderingDeviceDriver     创建/释放入口接口
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-09
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering_opengl
 */

#pragma once

#include "command_buffer.h"
#include "template/paged_allocator.h"

#include <atomic>

namespace arhud
{
	// 前向声明，避免头文件循环依赖
	class GLRenderingDeviceDriver;

	// ═══════════════════════════════════════════════════════════════════════
	// GLCommandBuffer
	// ═══════════════════════════════════════════════════════════════════════

	/**
	 * @class GLCommandBuffer
	 * @brief OpenGL 命令缓冲实现类
	 *
	 * 实现 ICommandBuffer 接口的 OpenGL 后端。提供：
	 * - **线程安全的命令录制**：多缓冲模式下每个 CommandBuffer 由单一线程独占，通过 atomic state_ 同步
	 * - **高效的命令存储**：PagedAllocator + 指针数组的两级存储结构
	 * - **线性的命令回放**：按录制顺序逐条调用 driver_->CommandXxx()
	 * - **跨帧复用**：Begin → 录制 → End → Execute → Reset 循环
	 *
	 * @par 内部数据结构
	 *   @code
	 *   ┌─────────────────────────────────────────────────────┐
	 *   │ GLCommandBuffer                                     │
	 *   ├─────────────────────────────────────────────────────┤
	 *   │ driver_*: GLRenderingDeviceDriver*                  │
	 *   │ state_: atomic<CommandBufferState>                  │
	 *   │                                                     │
	 *   │ command_allocator_: PagedAllocator<RecordedCommand>  │
	 *   │   ┌─────┬─────┬─────┬─────┐  (页链表)               │
	 *   │   │Page0│Page1│Page2│ ... │  每页 N 个 RecordedCmd │
	 *   │   └─────┴─────┴─────┴─────┘                         │
	 *   │                                                     │
	 *   │ commands_: LocalVector<RecordedCommand*>            │
	 *   │   ┌────┬────┬────┬────┬────┐                       │
	 *   │   │ptr0│ptr1│ptr2│ptr3│... │  指向 allocator 分配的对象│
	 *   │   └────┴────┴────┴────┴────┘                       │
	 *   └─────────────────────────────────────────────────────┘
	 *   @endcode
	 *
	 * @par 性能特征
	 *   | 操作 | 时间复杂度 | 说明 |
	 *   |------|-----------|------|
	 *   | Emplace<TCmd>() | O(1) 均 | PagedAllocator 分配 + PushBack |
	 *   | Begin() / Reset() | O(n) | n = 上一轮命令数，逐一 Free |
	 *   | Execute() | O(n) | n = 当前命令数，线性回放 |
	 *   | End() / GetState() | O(1) | 仅状态赋值/读取 |
	 *
	 * @par 典型使用模式（双缓冲）
	 *   @code
	 *   // === 初始化（一次）===
	 *   ICommandBuffer *cmd_buf[2];
	 *   cmd_buf[0] = driver->CommandBufferCreate();
	 *   cmd_buf[1] = driver->CommandBufferCreate();
	 *   int current = 0;
	 *
	 *   // === 每帧主线程 ===
	 *   ICommandBuffer *cmd = cmd_buf[current];
	 *   cmd->Begin();
	 *   // ... 录制本帧渲染命令 ...
	 *   cmd->End();
	 *   render_queue->Submit(cmd);
	 *   current = 1 - current;  // 切换到另一个 buffer
	 *
	 *   // === 渲染线程 ===
	 *   while (ICommandBuffer *cmd = render_queue->Dequeue())
	 *   {
	 *       cmd->Execute();  // 回放到 GL
	 *       cmd->Reset();    // 准备下一帧复用
	 *   }
	 *   @endcode
	 */
	class GLCommandBuffer : public ICommandBuffer
	{
	public:
		/**
		 * @brief 构造函数
		 *
		 * 初始化命令缓冲区，关联到指定的 OpenGL 设备驱动。
		 * 初始状态为 kRecording，可立即开始录制命令。
		 *
		 * @param[in] p_driver 关联的 OpenGL 渲染设备驱动指针。
		 *                     不能为 nullptr，且生命周期必须长于本对象。
		 *                     回放时通过此指针调用 driver_->CommandXxx() 方法。
		 *
		 * @post state_ == kRecording
		 * @post commands_ 为空
		 * @post command_allocator_ 已初始化
		 *
		 * @note PagedAllocator 在构造时预分配第一页内存，
		 *       后续按需扩展（每页默认 256 个 RecordedCommand 对象）
		 */
		explicit GLCommandBuffer(GLRenderingDeviceDriver *p_driver);

		/**
		 * @brief 析构函数
		 *
		 * 释放所有已录制的命令对象并清理内部状态。
		 *
		 * @warning 析构前必须确保没有正在进行的 Execute() 调用。
		 *          如果有其他线程持有本对象的引用，析构后访问将导致 UB。
		 *
		 * @par 清理流程
		 *   1. 遍历 commands_ 数组
		 *   2. 对每个 RecordedCommand* 调用显式析构（处理 union 成员）
		 *   3. 调用 command_allocator_.Free() 释放内存
		 *   4. 清空 commands_ 数组
		 */
		~GLCommandBuffer() override;

		/// 禁止拷贝构造和拷贝赋值（含 PagedAllocator 和 atomic）
		ARHUD_DISABLE_COPY_MOVE(GLCommandBuffer);

		// ═══════════════════════════════════════════════════════════════════
		// 生命周期管理
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 开始命令录制（override）
		 *
		 * 重置命令缓冲区到初始 Recording 状态。
		 * 释放上一轮录制的所有命令对象，清空命令列表。
		 *
		 * @par 实现细节
		 *   1. 检查 state_（atomic load）
		 *   2. 如果 kExecuting，断言失败（缓冲池不应分配正在执行的 buffer）
		 *   3. 遍历 commands_[i]，对每个指针：
		 *      a. 显式调用 data 成员的析构（根据 type 字段判断实际类型）
		 *      b. command_allocator_.Free(ptr) 释放对象内存
		 *   4. commands_.Clear() 清空指针数组
		 *   5. state_ = kRecording（atomic store）
		 *
		 * @note 可在任何非 Executing 状态下调用。
		 *       如果当前有未执行的 Ready 状态命令，它们将被丢弃。
		 *
		 * @thread_safety 多缓冲下由独占线程调用，atomic state_ 保证可见性
		 *
		 * @see ICommandBuffer::Begin() 接口文档
		 * @see Reset() 功能相同的方法
		 */
		void Begin() override;

		/**
		 * @brief 结束命令录制（override）
		 *
		 * 将状态从 kRecording 切换为 kReady。
		 * 此后录制方法的调用将被忽略（静默返回）。
		 *
		 * @pre state_ == kRecording（否则直接返回，不做任何操作）
		 * @post state_ == kReady
		 *
		 * @thread_safety 多缓冲下由独占线程调用，atomic state_ 保证可见性
		 *
		 * @see ICommandBuffer::End() 接口文档
		 */
		void End() override;

		/**
		 * @brief 回放所有已录制命令到 GL（override）
		 *
		 * 按录制顺序遍历 commands_ 数组，对每条命令调用 ReplayCommand()，
		 * 将其转换为对应的 driver_->CommandXxx() 调用。
		 *
		 * @par 实现细节
		 *   1. 检查 state_ == kReady，否则直接返回
		 *   2. 设置 state_ = kExecuting（不加锁，假设 GL 线程独占）
		 *   3. for i in [0, commands_.Size()):
		 *      ReplayCommand(*commands_[i])
		 *   4. 设置 state_ = kSubmitted
		 *
		 * @pre End() 已成功调用（state_ == kReady）
		 * @pre 当前线程拥有有效的 GL 上下文
		 *
		 * @warning 多缓冲模式下由渲染线程独占调用，atomic state_ 保证状态可见性。
		 *          调用者必须确保当前线程拥有有效的 GL 上下文。
		 *
		 * @thread_safety 非线程安全（必须在 GL 上下文线程独占调用）
		 *
		 * @see ICommandBuffer::Execute() 接口文档
		 * @see ReplayCommand() 单条命令回放实现
		 */
		void Execute() override;

		/**
		 * @brief 重置命令缓冲区（override）
		 *
		 * 释放所有已录制命令对象，清空命令列表，
		 * 回到初始 kRecording 状态。
		 *
		 * @par 与 Begin() 的关系
		 *   内部实现完全相同。语义区别：
		 *   - Begin() 强调"开始新一轮录制"（帧首调用）
		 *   - Reset() 强调"强制清理"（错误恢复或 Execute 后调用）
		 *
		 * @pre state_ != kExecuting
		 * @post state_ == kRecording, commands_ 为空
		 *
		 * @thread_safety 多缓冲下由独占线程调用，atomic state_ 保证可见性
		 *
		 * @see ICommandBuffer::Reset() 接口文档
		 */
		void Reset() override;

		/**
		 * @brief 获取当前状态（override）
		 *
		 * 返回命令缓冲区的生命周期状态，用于外部决策。
		 *
		 * @return CommandBufferState 当前状态值
		 *
		 * @note 由于 Execute() 不加锁修改 state_，
		 *       在多线程场景下读取可能短暂不一致。
		 *       如需严格一致，需外部协调 Execute 和此调用的执行顺序。
		 *
		 * @thread_safety 线程安全（atomic load）
		 *
		 * @see ICommandBuffer::GetState() 接口文档
		 * @see CommandBufferState 状态枚举及转换图
		 */
		CommandBufferState GetState() const override;

		/**
		 * @brief 获取已录制命令数量（override）
		 *
		 * 返回当前命令列表中的命令条数。
		 * 用于性能监控、调试和断言检查。
		 *
		 * @return uint32_t 命令数量（0 表示空缓冲区）
		 *
		 * @note 返回值在录制过程中动态变化。
		 *       如需稳定快照，应在 End() 之后读取。
		 *
		 * @thread_safety 线程安全（commands_.Size() 为 const 方法）
		 *
		 * @see ICommandBuffer::GetCommandCount() 接口文档
		 */
		uint32_t GetCommandCount() const override;

		// ═══════════════════════════════════════════════════════════════════
		// Render Pass 命令
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 开始渲染 Pass（override）
		 *
		 * 录制 BeginRenderPass 命令。将参数拷贝到 CmdBeginRenderPass 结构体，
		 * 通过 Emplace<CmdBeginRenderPass>() 追加到命令列表。
		 *
		 * @param[in] p_render_pass   渲染 Pass ID
		 * @param[in] p_framebuffer   帧缓冲 ID
		 * @param[in] p_clear_values  清除值数组视图
		 * @param[in] p_rect_x       渲染区域 X
		 * @param[in] p_rect_y       渲染区域 Y
		 * @param[in] p_rect_w       渲染区域宽度
		 * @param[in] p_rect_h       渲染区域高度
		 *
		 * @pre state_ == kRecording
		 *
		 * @par 内部行为
		 *   1. 检查 state_ == kRecording（atomic load）
		 *   2. auto &cmd = Emplace<CmdBeginRenderPass>(kBeginRenderPass)
		 *   3. cmd.render_pass = p_render_pass
		 *   4. cmd.framebuffer = p_framebuffer
		 *   5. 遍历 p_clear_values，PushBack 到 cmd.clear_values
		 *   6. 设置 rect_x/y/w/h
		 *
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void BeginRenderPass(
			RenderPassID p_render_pass,
			FramebufferID p_framebuffer,
			VectorView<RenderPassClearValue> p_clear_values,
			int32_t p_rect_x, int32_t p_rect_y,
			uint32_t p_rect_w, uint32_t p_rect_h) override;

		/**
		 * @brief 结束渲染 Pass（override）
		 *
		 * 录制一条 kEndRenderPass 类型命令（无额外参数）。
		 *
		 * @pre state_ == kRecording
		 *
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void EndRenderPass() override;

		// ═══════════════════════════════════════════════════════════════════
		// 资源绑定命令
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 绑定渲染管线（override）
		 *
		 * 录制 CmdBindPipeline 命令。
		 *
		 * @param[in] p_pipeline 管线 ID
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void BindRenderPipeline(PipelineID p_pipeline) override;

		/**
		 * @brief 绑定 Uniform Set（override）
		 *
		 * 录制 CmdBindUniformSet 命令。
		 *
		 * @param[in] p_uniform_set  Uniform Set ID
		 * @param[in] p_set_index    Set 索引
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void BindUniformSet(UniformSetID p_uniform_set,
							uint32_t p_set_index) override;

		/**
		 * @brief 绑定顶点缓冲区数组（override）
		 *
		 * 录制 CmdBindVertexBuffers 命令。拷贝缓冲区 ID 数组和偏移数组。
		 *
		 * @param[in] p_buffers  缓冲区 ID 数组指针
		 * @param[in] p_offsets 偏移量数组指针
		 * @param[in] p_count   缓冲区数量（≤ kMaxBindings=4）
		 *
		 * @pre state_ == kRecording
		 * @pre p_buffers != nullptr && p_offsets != nullptr
		 * @pre p_count <= CmdBindVertexBuffers::kMaxBindings
		 *
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void BindVertexBuffers(
			const BufferID *p_buffers,
			const uint64_t *p_offsets,
			uint32_t p_count) override;

		/**
		 * @brief 绑定索引缓冲区（override）
		 *
		 * 录制 CmdBindIndexBuffer 命令。
		 *
		 * @param[in] p_buffer  索引缓冲区 ID
		 * @param[in] p_format  索引格式
		 * @param[in] p_offset 字节偏移
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void BindIndexBuffer(BufferID p_buffer,
							 IndexBufferFormat p_format,
							 uint64_t p_offset) override;

		// ═══════════════════════════════════════════════════════════════════
		// 绘制命令
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 非索引绘制（override）
		 *
		 * 录制 CmdDraw 命令。
		 *
		 * @param[in] p_vertex_count    顶点数量
		 * @param[in] p_instance_count  实例化数量
		 * @param[in] p_base_vertex     基础顶点偏移
		 * @param[in] p_first_instance  首实例 ID
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void Draw(uint32_t p_vertex_count,
				  uint32_t p_instance_count,
				  uint32_t p_base_vertex,
				  uint32_t p_first_instance) override;

		/**
		 * @brief 索引绘制（override）
		 *
		 * 录制 CmdDrawIndexed 命令。
		 *
		 * @param[in] p_index_count     索引数量
		 * @param[in] p_instance_count  实例化数量
		 * @param[in] p_first_index     首索引位置
		 * @param[in] p_vertex_offset   顶点偏移
		 * @param[in] p_first_instance  首实例 ID
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void DrawIndexed(uint32_t p_index_count,
						 uint32_t p_instance_count,
						 uint32_t p_first_index,
						 int32_t p_vertex_offset,
						 uint32_t p_first_instance) override;

		// ═══════════════════════════════════════════════════════════════════
		// 动态状态设置命令
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 设置视口（override）
		 *
		 * 录制 CmdSetViewport 命令。
		 *
		 * @param[in] p_x      视口 X
		 * @param[in] p_y      视口 Y
		 * @param[in] p_width  视口宽度
		 * @param[in] p_height 视口高度
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void SetViewport(int32_t p_x, int32_t p_y,
						 uint32_t p_width, uint32_t p_height) override;

		/**
		 * @brief 设置裁剪矩形（override）
		 *
		 * 录制 CmdSetScissor 命令。
		 *
		 * @param[in] p_x      裁剪矩形 X
		 * @param[in] p_y      裁剪矩形 Y
		 * @param[in] p_width  裁剪矩形宽度
		 * @param[in] p_height 裁剪矩形高度
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void SetScissor(int32_t p_x, int32_t p_y,
						uint32_t p_width, uint32_t p_height) override;

		/**
		 * @brief 设置混合常量颜色（override）
		 *
		 * 录制 CmdSetBlendConstants 命令。
		 *
		 * @param[in] p_r  R 通道 [0, 1]
		 * @param[in] p_g  G 通道 [0, 1]
		 * @param[in] p_b  B 通道 [0, 1]
		 * @param[in] p_a  A 通道 [0, 1]
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void SetBlendConstants(float p_r, float p_g,
							   float p_b, float p_a) override;

		// ═══════════════════════════════════════════════════════════════════
		// 数据传输命令
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 清除缓冲区（override）
		 *
		 * 录制 CmdClearBuffer 命令。
		 *
		 * @param[in] p_buffer 目标缓冲区 ID
		 * @param[in] p_offset 字节偏移
		 * @param[in] p_size   字节数（0=全部）
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void ClearBuffer(BufferID p_buffer,
						 uint64_t p_offset,
						 uint64_t p_size) override;

		/**
		 * @brief 缓冲区拷贝（override）
		 *
		 * 录制 CmdCopyBuffer 命令。拷贝区域列表到 FixedVector 中。
		 *
		 * @param[in] p_src_buffer 源缓冲区 ID
		 * @param[in] p_dst_buffer 目标缓冲区 ID
		 * @param[in] p_regions    拷贝区域数组视图
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void CopyBuffer(BufferID p_src_buffer,
						BufferID p_dst_buffer,
						VectorView<BufferCopyRegion> p_regions) override;

		/**
		 * @brief 清除颜色纹理（override）
		 *
		 * 录制 CmdClearColorTexture 命令。
		 *
		 * @param[in] p_texture      目标纹理 ID
		 * @param[in] p_color_r      R 通道 [0, 1]
		 * @param[in] p_color_g      G 通道 [0, 1]
		 * @param[in] p_color_b      B 通道 [0, 1]
		 * @param[in] p_color_a      A 通道 [0, 1]
		 * @param[in] p_subresources 子资源范围
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void ClearColorTexture(
			TextureID p_texture,
			float p_color_r, float p_color_g,
			float p_color_b, float p_color_a,
			const TextureSubresourceRange &p_subresources) override;

		/**
		 * @brief 清除深度/模板纹理（override）
		 *
		 * 录制 CmdClearDepthStencilTexture 命令。
		 *
		 * @param[in] p_texture      目标纹理 ID
		 * @param[in] p_depth        深度值 [0, 1]
		 * @param[in] p_stencil      模板值 [0, 255]
		 * @param[in] p_subresources 子资源范围
		 *
		 * @pre state_ == kRecording
		 * @thread_safety 多缓冲下由独占线程调用
		 */
		void ClearDepthStencilTexture(
			TextureID p_texture,
			float p_depth, uint32_t p_stencil,
			const TextureSubresourceRange &p_subresources) override;

		/**
		 * @brief 获取资源追踪器（override）
		 *
		 * 返回内部 ResourceTracker 的常量指针，用于外部查询资源使用情况。
		 *
		 * @return 指向 resource_tracker_ 的常量指针（始终有效，非 nullptr）
		 *
		 * @par 典型用途
		 *   @code
		 *   GLCommandBuffer *gl_cmd = static_cast<GLCommandBuffer*>(cmd);
		 *   const ResourceTracker *tracker = gl_cmd->GetResourceTracker();
		 *   tracker->DumpState();  // 输出当前帧资源使用状态
		 *   uint32_t active = tracker->GetActiveCount();
		 *   @endcode
		 *
		 * @thread_safety 线程安全（返回只读引用）
		 */
		const ResourceTracker *GetResourceTracker() const override;

	private:
		// ═══════════════════════════════════════════════════════════════════
		// 内部辅助方法
		// ═══════════════════════════════════════════════════════════════════

		/**
		 * @brief 追加一条新命令到录制列表
		 *
		 * 核心录制原语。所有公共录制方法最终都调用此模板方法来分配和初始化命令数据。
		 *
		 * @tparam TCmd 具体的命令数据结构类型
		 *               （如 CmdDraw、CmdBindPipeline 等）
		 *
		 * @param[in] p_type 命令类型标识（CommandType 枚举值）
		 *
		 * @return TCmd& 新分配的命令数据的可写引用，
		 *             调用者可直接填充字段值
		 *
		 * @pre state_ == kRecording
		 *
		 * @par 内存分配流程
		 *   1. RecordedCommand *cmd = command_allocator_.Alloc()
		 *      → O(1) 从当前页获取空间（必要时分配新页）
		 *   2. cmd->type = p_type
		 *   3. new (&cmd->data) TCmd()
		 *      → placement new 在 union 内存上构造具体类型
		 *   4. commands_.PushBack(cmd)
		 *      → 记录指针到顺序数组
		 *   5. return reinterpret_cast<TCmd&>(cmd->data)
		 *
		 * @par 为什么使用 placement new
		 *   RecordedCommand::Data 是联合体，其成员共享同一块原始内存。
		 *   直接赋值会触发拷贝赋值运算符，但联合体内存未被正确构造，
		 *   导致未定义行为。placement new 确保：
		 *   - 正确调用 TCmd 的构造函数（如果有的话）
		 *   - 在已知类型的内存上进行构造
		 *
		 * @note 此方法是 private 的，由公共录制方法调用。
		 *
		 * @see ReplayCommand() 回放时读取这些数据
		 */
		template <typename TCmd>
		TCmd &Emplace(CommandType p_type);

		/**
		 * @brief 回放单条已录制命令
		 *
		 * Execute() 的核心分发逻辑。根据命令类型 switch-case 到对应的
		 * driver_->CommandXxx() 调用。
		 *
		 * @param[in] p_cmd 已录制命令的常量引用（从 commands_ 数组中取出）
		 *
		 * @par 分发映射表
		 *   | CommandType | driver_-> 调用 | GL 等效 |
		 *   |-------------|---------------|---------|
		 *   | kBeginRenderPass | CommandBeginRenderPass() | glBindFramebuffer + glClear |
		 *   | kEndRenderPass | CommandEndRenderPass() | （无操作） |
		 *   | kBindPipeline | CommandBindRenderPipeline() | glUseProgram |
		 *   | kBindUniformSet | CommandBindUniformSet() | glBindBufferBase |
		 *   | kBindVertexBuffers | CommandBindVertexBuffers() | glBindVertexBuffer |
		 *   | kBindIndexBuffer | CommandBindIndexBuffer() | glBindBuffer(EAB) |
		 *   | kDraw | CommandDraw() | glDrawArraysInstancedBaseInstance |
		 *   | kDrawIndexed | CommandDrawIndexed() | glDrawElements... |
		 *   | kSetViewport | CommandSetViewport() | glViewport |
		 *   | kSetScissor | CommandSetScissor() | glScissor |
		 *   | kSetBlendConstants | CommandSetBlendConstants() | glBlendColor |
		 *   | kClearBuffer | CommandClearBuffer() | glClearBufferData |
		 *   | kCopyBuffer | CommandCopyBuffer() | glCopyBufferSubData |
		 *   | kClearColorTexture | CommandClearColorTexture() | glClearTexSubImage |
		 *   | kClearDepthStencilTexture | CommandClearDepthStencilTexture() | glClearTexSubImage |
		 *
		 * @warning 此方法不加锁，必须在 GL 上下文线程被 Execute() 调用。
		 *       default 分支静默忽略未知命令类型（防御性编程）。
		 *
		 * @note 对于带参数的命令（如 kBeginRenderPass），需要从
		 *       p_cmd.data.xxx 中提取对应类型的引用传递给 driver_。
		 */
		void ReplayCommand(const RecordedCommand &p_cmd);

		// ═══════════════════════════════════════════════════════════════════
		// 成员变量
		// ═══════════════════════════════════════════════════════════════════

		GLRenderingDeviceDriver *driver_;										///< 关联的 OpenGL 设备驱动指针。
																				///< 回放时通过此指针调用立即模式的
																				///< CommandXxx() 方法。
																				///< 由构造函数设置，生命周期 > 本对象。
		std::atomic<CommandBufferState> state_{CommandBufferState::kRecording}; ///< 当前生命周期状态。
																				///< atomic 确保 Execute() 与 GetState() 的线程安全。
																				///< 多缓冲模式下每个 CommandBuffer 由单一线程独占，
																				///< atomic state_ 用于跨线程状态可见性。
		PagedAllocator<RecordedCommand>
			command_allocator_ = {}; ///< 命令对象页式分配器。
									 ///< 每个 RecordedCommand 从此分配，
									 ///< 提供 O(1) alloc/free，避免堆碎片。
		LocalVector<RecordedCommand *>
			commands_ = {}; ///< 已录制命令的指针数组（有序）。
							///< 元素指向 command_allocator_ 分配的对象。
							///< 顺序即为回放顺序。
		ResourceTracker
			resource_tracker_ = {}; ///< 轻量级资源追踪器（Phase 1）。
									///< 记录当前帧所有 GPU 资源的使用情况，
									///< 提供冲突检测和调试信息。
									///< Begin() 时调用 BeginFrame()，
									///< Reset() 时调用 EndFrame()。
	};

} // namespace arhud
