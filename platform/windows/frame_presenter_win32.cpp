#include "frame_presenter_win32.h"

#include "io/logger.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace arhud {
Error FramePresenterWin32::Initialize(void* p_native_window, uint32 p_width,
                                      uint32 p_height) {
  hwnd_ = static_cast<HWND>(p_native_window);
  if (hwnd_ == nullptr) {
    return Error::kFailed;
  }
  hdc_ = GetDC(hwnd_);
  if (hdc_ == nullptr) {
    return Error::kFailed;
  }
  width_ = p_width;
  height_ = p_height;
  RebuildBitmapInfo(p_width, p_height);
  return Error::kOK;
}

void FramePresenterWin32::Finalize() {
  if (hdc_ != nullptr && hwnd_ != nullptr) {
    ReleaseDC(hwnd_, hdc_);
  }
  hdc_ = nullptr;
  hwnd_ = nullptr;
  width_ = 0;
  height_ = 0;
}

void FramePresenterWin32::Resize(uint32 p_width, uint32 p_height) {
  width_ = p_width;
  height_ = p_height;
  RebuildBitmapInfo(p_width, p_height);
}

void FramePresenterWin32::Present(const void* p_pixels, uint32 p_width,
                                  uint32 p_height, uint32 p_stride) {
  if (hdc_ == nullptr || p_pixels == nullptr) {
    return;
  }
  if (p_width != width_ || p_height != height_) {
    RebuildBitmapInfo(p_width, p_height);
    width_ = p_width;
    height_ = p_height;
  }
  // 每次现取 DC（跨线程安全：GetDC 在调用线程上获取有效 DC）
  HDC hdc = GetDC(hwnd_);
  if (hdc == nullptr) {
    ARHUD_LOG_WARN("FramePresenterWin32: GetDC failed in Present");
    return;
  }
  int result =
      StretchDIBits(hdc, 0, 0, p_width, p_height, 0, 0, p_width, p_height,
                    p_pixels, &bmi_, DIB_RGB_COLORS, SRCCOPY);
  GdiFlush();  // 强制 GDI 命令提交到 DWM
  ReleaseDC(hwnd_, hdc);
  if (result == 0) {
    ARHUD_LOG_WARN("FramePresenterWin32: StretchDIBits returned 0");
  }
}

void FramePresenterWin32::RebuildBitmapInfo(uint32 p_width, uint32 p_height) {
  bmi_ = {};
  bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi_.bmiHeader.biWidth = static_cast<LONG>(p_width);
  bmi_.bmiHeader.biHeight = static_cast<LONG>(
      p_height);  // bottom-up（匹配 GL glReadPixels 的从下往上读取）
  bmi_.bmiHeader.biPlanes = 1;
  bmi_.bmiHeader.biBitCount = 32;
  bmi_.bmiHeader.biCompression = BI_RGB;
  bmi_.bmiHeader.biSizeImage = p_width * p_height * 4;
}

bool FramePresenterWin32::SupportsSwapPresent() const {
  if (hwnd_ == nullptr) {
    return false;
  }

  // 每次现取 DC（与 Present() 相同的跨线程纪律：DC 缓存是 per-thread 的）
  HDC hdc = GetDC(hwnd_);
  if (hdc == nullptr) {
    return false;
  }

  bool supported = false;
  int pixel_format = GetPixelFormat(hdc);
  if (pixel_format != 0) {
    PIXELFORMATDESCRIPTOR pfd = {};
    if (DescribePixelFormat(hdc, pixel_format, sizeof(pfd), &pfd) != 0) {
      // Swap 路径的两个前提都由像素格式保证：
      //   PFD_DOUBLEBUFFER   — SwapBuffers 有前后缓冲可交换
      //   PFD_SUPPORT_OPENGL — 渲染线程能把帧画进 backbuffer
      supported = (pfd.dwFlags & PFD_DOUBLEBUFFER) != 0 &&
                  (pfd.dwFlags & PFD_SUPPORT_OPENGL) != 0;
    }
  }

  ReleaseDC(hwnd_, hdc);
  return supported;
}

Error FramePresenterWin32::SwapPresent() {
  if (hwnd_ == nullptr) {
    return Error::kFailed;
  }

  // 每次现取 DC（DC 缓存 per-thread，跨线程复用 Initialize 时缓存的
  // hdc_ 不可靠——与 Present() 同一模式）
  HDC hdc = GetDC(hwnd_);
  if (hdc == nullptr) {
    ARHUD_LOG_WARN("FramePresenterWin32::SwapPresent: GetDC failed");
    return Error::kFailed;
  }

  // ── 1. SwapBuffers：提交 backbuffer 给 DWM ──
  // 纯 GDI 调用（gdi32），不需要 current 的 GL context ——
  // present 线程独立于渲染线程调它的合法性来源。
  // 非阻塞（提交语义，实际翻入由 DWM 合成节拍决定）。
  if (!SwapBuffers(hdc)) {
    // 失败主因：窗口像素格式无 PFD_DOUBLEBUFFER（可见窗口未
    // 走 GL 像素格式路径），或 DC 无效。属初始化期配置错误，
    // 每帧告警一次由上层节流。
    ReleaseDC(hwnd_, hdc);
    ARHUD_LOG_WARN(
        "FramePresenterWin32::SwapPresent: SwapBuffers failed "
        "(window without double-buffered GL pixel format?)");
    return Error::kFailed;
  }
  ReleaseDC(hwnd_, hdc);

  // ── 2. DwmFlush：等 DWM 完成一次合成 ──
  // 返回 = SwapBuffers 提交的帧已被 DWM 原子收编，渲染线程可
  // 安全复用 backbuffer 画下一帧（方案 B 的防撕裂保证）。
  //
  // HRESULT 处理：
  //   S_OK                       正常，含收编保证
  //   DWM_E_COMPOSITIONDISABLED  DWM 合成关闭（远程桌面 / Basic
  //                              主题 / 无头会话等）
  //   其他失败                   同类环境异常
  //
  // 失败时的决策：降级而非报错——
  //   - 帧序不受影响（本线程仍按 FIFO swap，不乱序）
  //   - 丢掉的只是"backbuffer 复用时机"的合成确认，退化为
  //     best-effort 节拍；渲染线程的帧槽背压（frame_count_=2）
  //     仍是兜底限速器
  //   - 该环境下往往整个合成模型都变了（如独占全屏直扫），
  //     撕裂风险本来就不成立
  HRESULT hr = DwmFlush();
  if (FAILED(hr)) {
    ARHUD_LOG_WARN(
        "FramePresenterWin32::SwapPresent: DwmFlush failed hr=0x%08X "
        "(DWM composition off?) — buffer-reuse sync degraded, "
        "falling back to frame-slot backpressure pacing",
        static_cast<uint32>(hr));
  }

  return Error::kOK;
}
}  // namespace arhud

#endif
