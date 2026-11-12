/**
 * @file dxgi_shared_frame_pool_win32.cpp
 * @brief Win32 共享显存帧池实现（D3D11 帧环 + DXGI flip-model + 零回读上屏）。
 *
 * @par 数据流（Mode X）
 *   @code
 *   渲染线程：GL 经 WGL_NV_DX_interop2 把 frames_[i] 当 FBO 颜色附件写入
 *   上屏线程：CopyResource(back_buffers_[n], frames_[i]) → Present1(1)
 *   @endcode
 *   像素全程留在显存；跨线程只传帧 index（由 FrameImageQueue 以 token
 *   承载）。
 *
 * @par 为何不等"拷贝完成"
 *   归还保证来自 interop：规范要求 Lock/Unlock 充当跨驱动的同步点，
 *   渲染线程下一次 Lock 该帧时，驱动保证显示侧对它的读取已结束。
 *   本池因此不需要 GPU event/fence 轮询（本机 Windows SDK 的
 *   ID3D11DeviceContext 也未暴露 query 接口）；环深度 3 让那次等待
 *   在实践中基本不发生。
 *
 * @par 为何仍需一次 GPU 拷贝
 *   Windows 有 DWM 合成器挡在中间，帧内容必须落进 swap chain 自己的
 *   back buffer 才能上屏，故保留 CopyResource（纯 GPU 侧，不占 PCIe、
 *   不经 CPU）。生产平台（QNX Screen / KMS 直扫）可把这一步省掉。
 *
 * @author yameng.he
 * @version 1.1
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup platform
 */

#include "dxgi_shared_frame_pool_win32.h"

#include "io/logger.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace arhud {
namespace {

/** @brief 进程内共享的 D3D11 底座（多屏复用同一适配器与 device） */
ID3D11Device* g_dx_device = nullptr;
ID3D11DeviceContext* g_dx_context = nullptr;
IDXGIFactory2* g_dx_factory = nullptr;
uint32 g_dx_refs = 0;

/**
 * @brief 释放 COM 引用并置空（本文件唯一的 Release 出口）
 *
 * @tparam T COM 接口指针类型
 * @param[in,out] p_ptr 待释放指针
 */
template <typename T>
inline void SafeRelease(T** p_ptr) {
  if (p_ptr != nullptr && *p_ptr != nullptr) {
    (*p_ptr)->Release();
    *p_ptr = nullptr;
  }
}

/**
 * @brief 记录 GPU 拓扑（一次性诊断，混合显卡定位用）
 *
 * @par 为什么值得留
 *   Mode X 的成立前提是"D3D11 device 所在适配器 == GL context 所在
 *   适配器 == 目标窗口所在显示器"。三者不一致时（笔记本双显卡的常态）
 *   症状是帧不上屏/提交失败，光看日志无法归因。故 device 创建后立刻
 *   打出：适配器型号、LUID、每个输出的桌面区域及其是否驱动目标窗口。
 *
 * @param[in] p_device 刚创建的 D3D11 device
 * @param[in] p_hwnd   目标窗口（用于比对它的显示器是否挂在本适配器上）
 */
void LogAdapterTopology(ID3D11Device* p_device, HWND p_hwnd) {
  IDXGIDevice* dxgi_device = nullptr;
  const HRESULT qi = p_device->QueryInterface(
      __uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
  if (FAILED(qi) || dxgi_device == nullptr) {
    return;
  }

  IDXGIAdapter1* adapter = nullptr;
  if (FAILED(dxgi_device->GetAdapter(
          reinterpret_cast<IDXGIAdapter**>(&adapter))) ||
      adapter == nullptr) {
    SafeRelease(&dxgi_device);
    return;
  }

  DXGI_ADAPTER_DESC1 desc = {};
  adapter->GetDesc1(&desc);

  char name_ascii[64] = {};
  for (int i = 0; i < 63 && desc.Description[i] != 0; ++i) {
    name_ascii[i] = static_cast<char>(desc.Description[i] & 0xFF);
  }

  // 目标窗口所在的显示器
  const HMONITOR target_monitor =
      p_hwnd != nullptr ? MonitorFromWindow(p_hwnd, MONITOR_DEFAULTTONEAREST)
                        : nullptr;

  // 输出数经 EnumOutputs 探得（GetOutputCount 属 DXGI 1.6 接口，
  // 不在 IDXGIAdapter 上，避免为此多求一个 QI）。
  // 逐个输出：desktop 区域 + 是否驱动目标窗口所在的显示器。
  // 一行都没有 = 本适配器不驱动任何显示器（笔记本独显直连未开启的常态），
  // 此时在它上面为跨适配器窗口建 swap chain 属 unsupported topology。
  UINT outputs = 0;
  UINT found = 0;
  for (UINT i = 0; i < 8; ++i) {
    IDXGIOutput* output = nullptr;
    if (FAILED(adapter->EnumOutputs(i, &output)) || output == nullptr) {
      break;
    }
    ++outputs;
    DXGI_OUTPUT_DESC od = {};
    output->GetDesc(&od);
    if (target_monitor != nullptr && od.Monitor == target_monitor) {
      ++found;
    }
    ARHUD_LOG_INFO(
        "DxgiSharedFramePool:   output[%u] desktop=(%d,%d)-(%d,%d) "
        "drives_target_window=%d",
        i, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
        od.DesktopCoordinates.right, od.DesktopCoordinates.bottom,
        (target_monitor != nullptr && od.Monitor == target_monitor) ? 1 : 0);
    SafeRelease(&output);
  }

  ARHUD_LOG_INFO(
      "DxgiSharedFramePool: adapter=%s vendor=0x%04X device=0x%04X "
      "outputs=%u drives_target_window=%u flags=0x%08X luid=(%u,%u)",
      name_ascii, desc.VendorId, desc.DeviceId, outputs, found, desc.Flags,
      static_cast<uint32>(desc.AdapterLuid.HighPart),
      static_cast<uint32>(desc.AdapterLuid.LowPart));

  SafeRelease(&adapter);
  SafeRelease(&dxgi_device);
}

}  // namespace

Error DxgiSharedFramePoolWin32::AcquireSharedDevice(ID3D11Device** o_device,
                                                    ID3D11DeviceContext** o_ctx,
                                                    IDXGIFactory2** o_factory) {
  if (g_dx_device == nullptr) {
    // 不得设 D3D11_CREATE_DEVICE_SINGLETHREADED：WGL_NV_DX_interop 要求
    // device 为 multithreaded（运行时对 immediate context 加锁），
    // 本实现的多线程共用正依赖这一保证。
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1};
    constexpr UINT level_count =
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
    // 注意：不得设 D3D11_CREATE_DEVICE_SINGLETHREADED ——
    // WGL_NV_DX_interop 要求 device 为 multithreaded（运行时对
    // immediate context 加锁），本实现的跨线程共用正依赖这一保证。
    // BGRA_SUPPORT：DWM 合成路径内部为 BGRA；对
    // DXGI_FORMAT_R8G8B8A8_UNORM 链非必需，实测两者皆可，保留以贴近
    // DWM 的默认期望。
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL got_level = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   level_count, D3D11_SDK_VERSION, &device,
                                   &got_level, &context);
    if (FAILED(hr) || device == nullptr || context == nullptr) {
      ARHUD_LOG_WARN(
          "DxgiSharedFramePool: D3D11CreateDevice failed hr=0x%08X "
          "(no shared-VRAM present path, falling back to readback)",
          static_cast<uint32>(hr));
      SafeRelease(&device);
      SafeRelease(&context);
      return Error::kUnavailable;
    }

    IDXGIDevice* dxgi_device = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr) || dxgi_device == nullptr) {
      ARHUD_LOG_WARN("DxgiSharedFramePool: IDXGIDevice query failed hr=0x%08X",
                     static_cast<uint32>(hr));
      SafeRelease(&device);
      SafeRelease(&context);
      return Error::kUnavailable;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_device->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(hr) && adapter != nullptr) {
      hr = adapter->GetParent(__uuidof(IDXGIFactory2),
                              reinterpret_cast<void**>(&factory));
    }
    SafeRelease(&adapter);
    SafeRelease(&dxgi_device);

    if (FAILED(hr) || factory == nullptr) {
      ARHUD_LOG_WARN(
          "DxgiSharedFramePool: IDXGIFactory2 unavailable hr=0x%08X",
          static_cast<uint32>(hr));
      SafeRelease(&device);
      SafeRelease(&context);
      return Error::kUnavailable;
    }

    g_dx_device = device;
    g_dx_context = context;
    g_dx_factory = factory;

    ARHUD_LOG_INFO("DxgiSharedFramePool: D3D11 device created (feature level "
                   "0x%05X)",
                   static_cast<uint32>(got_level));
    LogAdapterTopology(device, hwnd_);
  }

  ++g_dx_refs;
  *o_device = g_dx_device;
  *o_ctx = g_dx_context;
  *o_factory = g_dx_factory;
  holds_device_ = true;
  return Error::kOK;
}

void DxgiSharedFramePoolWin32::ReleaseSharedDevice() {
  if (!holds_device_) {
    return;
  }
  holds_device_ = false;

  if (g_dx_refs > 0) {
    --g_dx_refs;
  }
  if (g_dx_refs == 0) {
    // 进程内最后一个池退出时才归还底座（引用计数，平凡可析构的全局指针）
    SafeRelease(&g_dx_factory);
    SafeRelease(&g_dx_context);
    SafeRelease(&g_dx_device);
  }

  device_ = nullptr;
  context_ = nullptr;
  factory_ = nullptr;
}

Error DxgiSharedFramePoolWin32::CreateFrames(uint32 p_width,
                                             uint32 p_height) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = p_width;
  desc.Height = p_height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  // 与既有 FBO 颜色格式同族（GL 侧 GL_RGBA8）；flip-model 亦支持该格式，
  // 与 back buffer 之间的 CopyResource 无需格式转换。
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  // 扩展硬要求：可注册给 GL 的 DX11 资源必须 D3D11_USAGE_DEFAULT。
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = 0;
  // 单进程单 device 内共享，无需 SHARED/SHARED_NTHANDLE；跨进程
  // （生产平台 DMA-BUF 类比）时在此加 flag + keyed mutex。
  desc.MiscFlags = 0;

  for (uint32 i = 0; i < kRingFrames; ++i) {
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr) || texture == nullptr) {
      ARHUD_LOG_WARN("DxgiSharedFramePool: CreateTexture2D[%u] failed "
                     "hr=0x%08X (%ux%u)",
                     i, static_cast<uint32>(hr), p_width, p_height);
      DestroyFrames();
      return Error::kFailed;
    }
    frames_[i] = texture;
  }
  frame_count_ = kRingFrames;
  return Error::kOK;
}

void DxgiSharedFramePoolWin32::DestroyFrames() {
  for (uint32 i = 0; i < kRingFrames; ++i) {
    SafeRelease(&frames_[i]);
  }
  frame_count_ = 0;
}

Error DxgiSharedFramePoolWin32::CreateSwapChain(HWND p_hwnd, uint32 p_width,
                                                uint32 p_height) {
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = p_width;
  desc.Height = p_height;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = D3D11_USAGE_DEFAULT;
  desc.BufferCount = kSwapChainBuffers;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  // 与 GDI 回读路径行为一致：alpha 不参与窗口合成（HUD 内容不透明）。
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = 0;

  IDXGISwapChain1* swap_chain = nullptr;
  HRESULT hr = factory_->CreateSwapChainForHwnd(
      device_, p_hwnd, &desc, nullptr, nullptr, &swap_chain);
  if (FAILED(hr) || swap_chain == nullptr) {
    ARHUD_LOG_WARN(
        "DxgiSharedFramePool: CreateSwapChainForHwnd failed hr=0x%08X "
        "(DWM composition off or window without DXGI support)",
        static_cast<uint32>(hr));
    SafeRelease(&swap_chain);
    return Error::kUnavailable;
  }
  swap_chain_ = swap_chain;

  // back buffer 引用一次性取齐：每帧 CopyResource 的目标。
  // 必须在 ResizeBuffers/swap chain 释放前全部归还（DXGI 硬要求）。
  for (uint32 i = 0; i < kSwapChainBuffers; ++i) {
    ID3D11Texture2D* back = nullptr;
    hr = swap_chain_->GetBuffer(i, __uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(&back));
    if (FAILED(hr) || back == nullptr) {
      ARHUD_LOG_WARN("DxgiSharedFramePool: GetBuffer(%u) failed hr=0x%08X", i,
                     static_cast<uint32>(hr));
      DestroySwapChain();
      return Error::kFailed;
    }
    back_buffers_[i] = back;
  }

  back_index_ = 0;
  return Error::kOK;
}

void DxgiSharedFramePoolWin32::DestroySwapChain() {
  // back buffer 引用必须先于 swap chain 释放归还
  for (uint32 i = 0; i < kSwapChainBuffers; ++i) {
    SafeRelease(&back_buffers_[i]);
  }
  SafeRelease(&swap_chain_);
  back_index_ = 0;
}

Error DxgiSharedFramePoolWin32::Initialize(void* p_native_window,
                                           uint32 p_width, uint32 p_height) {
  if (available_) {
    return Error::kAlreadyExists;
  }

  hwnd_ = static_cast<HWND>(p_native_window);
  if (hwnd_ == nullptr || p_width == 0 || p_height == 0) {
    return Error::kInvalidParameter;
  }

  Error err = AcquireSharedDevice(&device_, &context_, &factory_);
  if (err != Error::kOK) {
    return err;
  }

  err = CreateFrames(p_width, p_height);
  if (err != Error::kOK) {
    ReleaseSharedDevice();
    return err;
  }

  err = CreateSwapChain(hwnd_, p_width, p_height);
  if (err != Error::kOK) {
    DestroyFrames();
    ReleaseSharedDevice();
    return err;
  }

  width_ = p_width;
  height_ = p_height;
  available_ = true;
  ARHUD_LOG_INFO("DxgiSharedFramePool: shared-VRAM present path ready "
                 "(%ux%u, ring=%u, swapchain buffers=%u)",
                 p_width, p_height, frame_count_, kSwapChainBuffers);
  return Error::kOK;
}

void DxgiSharedFramePoolWin32::Finalize() {
  DestroySwapChain();
  DestroyFrames();
  SafeRelease(&sample_staging_);  // 诊断采样 staging
  ReleaseSharedDevice();

  width_ = 0;
  height_ = 0;
  hwnd_ = nullptr;
  available_ = false;
}

void DxgiSharedFramePoolWin32::Resize(uint32 p_width, uint32 p_height) {
  if (!available_ || p_width == 0 || p_height == 0) {
    return;
  }
  if (p_width == width_ && p_height == height_) {
    return;
  }

  // @pre 调用方保证无在途帧（FrameImageQueue::HasInFlight == false），
  //      且渲染侧的 interop 别名已解除（RD 侧外部纹理已销毁）——
  //      ResizeBuffers 要求所有 back buffer 引用先归还。
  DestroySwapChain();
  DestroyFrames();
  // staging 尺寸随帧尺寸失效，重建留待下次采样
  SafeRelease(&sample_staging_);

  Error err = CreateFrames(p_width, p_height);
  if (err == Error::kOK) {
    err = CreateSwapChain(hwnd_, p_width, p_height);
  }

  if (err != Error::kOK) {
    available_ = false;
    width_ = 0;
    height_ = 0;
    ARHUD_LOG_WARN(
        "DxgiSharedFramePool: resize failed (%ux%u) — shared present path "
        "disabled; caller must fall back to readback",
        p_width, p_height);
    return;
  }

  width_ = p_width;
  height_ = p_height;
  ARHUD_LOG_INFO("DxgiSharedFramePool: resized to %ux%u", p_width, p_height);
}

ISharedFramePool::Frame DxgiSharedFramePoolWin32::GetFrame(
    uint32 p_index) const {
  Frame frame;
  if (!available_ || p_index >= frame_count_) {
    return frame;
  }

  frame.index = p_index;
  frame.native_device = device_;
  frame.native_resource = frames_[p_index];
  frame.width = width_;
  frame.height = height_;
  frame.format = SharedFrameFormat::kRgba8Unorm;
  return frame;
}

/**
 * @brief 提交一帧上屏（上屏线程，零 GL）
 *
 * @par p_timeout_ms 的现状
 *   本实现不等待 GPU（归还保证在 interop Lock 上兑现，见文件头），
 *   故该参数暂未使用；保留在契约里是为了 Vulkan/QNX 后端要真的等
 *   release fence 时不改签名。
 */
Error DxgiSharedFramePoolWin32::CommitFrame(uint32 p_index,
                                            uint32 p_timeout_ms) {
  (void)p_timeout_ms;

  if (!available_) {
    return Error::kUnavailable;
  }
  if (p_index >= frame_count_) {
    return Error::kInvalidParameter;
  }
  if (swap_chain_ == nullptr || context_ == nullptr) {
    return Error::kFailed;
  }
  if (back_buffers_[back_index_] == nullptr) {
    return Error::kFailed;
  }

  // flip-model 要求应用按序轮转 back buffer（本线程 FIFO 保证有序），
  // 与 DXGI 内部的 current index 同步推进。
  context_->CopyResource(back_buffers_[back_index_], frames_[p_index]);

  // syncInterval=1：由 DXGI 对齐 vblank（各屏在自己的上屏线程里并行等，
  // 相比 Mode R 的"无节拍"，这才是帧率上限的来源）。
  //
  // @par pPresentParameters 必须非空（本机实测踩实的一个硬坑）
  //   SDK 头里该参数标 `_In_`（不是 `_In_opt_`），传 nullptr 会在
  //   dxgi.dll 内部按结构体读取字段（偏移 0x10 = pScrollRect）而
  //   EXCEPTION_ACCESS_VIOLATION——与 swap effect（FLIP_SEQUENTIAL /
  //   FLIP_DISCARD）、位图模型（DISCARD）、调用线程、CS_OWNDC 全部无关，
  //   只要走 Present1 就崩。传"全零结构体"= 无脏矩形、无滚动，
  //   语义与预期的 nullptr 完全一致。用最小复现程序对照验证：
  //   Present1(1,0,nullptr) 崩 / Present1(1,0,&零值) 连续 10 帧 S_OK。
  DXGI_PRESENT_PARAMETERS params = {};
  const HRESULT hr = swap_chain_->Present1(1, 0, &params);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
      hr == DXGI_ERROR_DEVICE_HUNG) {
    available_ = false;
    ARHUD_LOG_ERROR("kFailed",
                    "DxgiSharedFramePool: device lost hr=0x%08X — shared "
                    "present path disabled",
                    static_cast<uint32>(hr));
    return Error::kFailed;
  }
  if (FAILED(hr)) {
    ARHUD_LOG_WARN("DxgiSharedFramePool: Present1 failed hr=0x%08X",
                   static_cast<uint32>(hr));
    return Error::kFailed;
  }

  back_index_ = (back_index_ + 1) % kSwapChainBuffers;
  return Error::kOK;
}

/**
 * @brief 诊断采样：把指定帧拷到 staging 纹理读回求平均亮度
 *
 * @par 为什么值得存在
 *   零拷贝路径上"像素从不进 CPU"，于是"画面是否真的画进去了"没有
 *   现成出口可看。本方法临时借一条 staging 读回（等价于该帧走一次
 *   Mode R 的代价）把它量化成平均亮度，只在上屏线程被
 *   `ARHUD_DEBUG_SAMPLE=1` 触发，不参与正常呈现。
 *
 * @param[in]  p_index 帧 index
 * @param[out] o_mean  平均亮度 0-255（R+G+B 通道均值）
 *
 * @return true 采样成功
 */
bool DxgiSharedFramePoolWin32::SampleFrameLuminance(uint32 p_index,
                                                    float* o_mean) {
  if (!available_ || p_index >= frame_count_ || o_mean == nullptr ||
      device_ == nullptr || context_ == nullptr ||
      frames_[p_index] == nullptr) {
    return false;
  }

  if (sample_staging_ == nullptr) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT hr =
        device_->CreateTexture2D(&desc, nullptr, &sample_staging_);
    if (FAILED(hr) || sample_staging_ == nullptr) {
      ARHUD_LOG_WARN("DxgiSharedFramePool: staging for luminance sample "
                     "failed hr=0x%08X",
                     static_cast<uint32>(hr));
      return false;
    }
  }

  context_->CopyResource(sample_staging_, frames_[p_index]);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context_->Map(sample_staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }

  // 稀疏采样（约 64x64 个样点），够量化"有没有画面"，又不必遍历全帧
  const uint8* base = static_cast<const uint8*>(mapped.pData);
  const uint32 step_x = width_ > 64 ? width_ / 64 : 1;
  const uint32 step_y = height_ > 64 ? height_ / 64 : 1;
  uint64 sum = 0;
  uint64 channels = 0;
  for (uint32 y = 0; y < height_; y += step_y) {
    const uint8* row = base + static_cast<size_t>(y) * mapped.RowPitch;
    for (uint32 x = 0; x < width_; x += step_x) {
      const uint8* px = row + static_cast<size_t>(x) * 4;
      sum += px[0] + px[1] + px[2];
      channels += 3;
    }
  }
  context_->Unmap(sample_staging_, 0);

  if (channels == 0) {
    return false;
  }
  *o_mean = static_cast<float>(sum) / static_cast<float>(channels);
  return true;
}

}  // namespace arhud
#endif  // ARHUD_PLATFORM_WINDOWS
