/**
 * @file rendering_server.cpp
 * @brief RenderingServer 单例数据成员与工厂实现（实现侧）
 *
 * @par 内容
 *   - `singleton` 静态成员定义（默认 nullptr，装配期由引擎写入）；
 *   - `Create()` 工厂：构造 RenderingServerDefault 并以接口指针返回。
 *
 * @par 所有权
 *   S0 阶段引擎（ARHudEngine）以值持有 RenderingServerDefault，本文件不参与
 *   释放；`Create()` 暂未被任何调用点使用（见 rendering_server.h 中的 @note）。
 *
 * @see rendering_server.h         接口定义（公共 API Doxygen）
 * @see rendering_server_default.h 被工厂构造的具体实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-10-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#include "rendering_server.h"

#include "os/memory.h"
#include "rendering_server_default.h"

namespace arhud {

/**
 * @brief 全局单例存储定义（实现侧）
 *
 * 默认 nullptr；由装配方赋值（S0：ARHudEngine::Initialize 建栈成功后设置，
 * Finalize 前清空）。定义集中在本文件，避免多 TU 重复符号。
 *
 * @see RenderingServer::singleton
 * @see RenderingServer::GetSingleton
 */
RenderingServer* RenderingServer::singleton = nullptr;

/**
 * @brief 创建默认实现实例（实现侧：堆分配 RenderingServerDefault）
 *
 * @return 以 RenderingServer 接口返回的新实例（调用方拥有，需自行释放）
 *
 * @note S0 未使用：引擎以值持有 RenderingServerDefault，而非经本工厂构造。
 *       所有权模型待 S7 接管后由 Finish() 释放。
 * @see RenderingServerDefault
 */
RenderingServer* RenderingServer::Create() {
  return ARHUD_NEW(RenderingServerDefault)();
}

}  // namespace arhud