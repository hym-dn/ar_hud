/**
 * @file shader_storage_gl.h
 * @brief OpenGL ShaderStorage alias
 *
 * GLShaderStorage is a type alias for ShaderStorage because the Storage
 * layer only calls RenderingDevice methods (which internally delegate to
 * the RDD). No GL-specific code is needed at this layer — the driver
 * abstraction is fully handled by RD → RDD.
 *
 * @par Why no GL-specific subclass?
 *   ShaderStorage manages shader metadata (uniform reflection, source
 *   caching, push constant size) and delegates GPU compilation to
 *   RenderingDevice::ShaderCreateFromGLSL(). The RD internally calls
 *   RDD::ShaderCreateFromGLSL(), which is where the OpenGL-specific
 *   compilation happens. Therefore, ShaderStorage itself is
 *   API-agnostic and can be used directly as the GL implementation.
 *
 * @par Architecture:
 *   @code
 *   GLShaderStorage (= ShaderStorage)
 *       └── RenderingDevice::ShaderCreateFromGLSL()
 *             └── IRenderingDeviceDriver::ShaderCreateFromGLSL()  ← GL-specific
 *                   └── glCompileShader + glLinkProgram
 *   @endcode
 *
 * @see ShaderStorage
 * @see RenderingDevice
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-23
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup rendering
 */

#pragma once

#include "storage/shader_storage.h"

namespace arhud
{

using GLShaderStorage = ShaderStorage;

}
