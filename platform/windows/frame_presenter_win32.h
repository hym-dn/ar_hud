#ifndef ARHUD_PLATFORM_WINDOWS_FRAME_PRESENTER_WIN32_H_
#define ARHUD_PLATFORM_WINDOWS_FRAME_PRESENTER_WIN32_H_

#include "frame_presenter.h"

#ifdef ARHUD_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace arhud {
class FramePresenterWin32 : public IFramePresenter {
 public:
  Error Initialize(void* p_native_window, uint32 p_width,
                   uint32 p_height) override;
  void Finalize() override;
  void Resize(uint32 p_width, uint32 p_height) override;
  void Present(const void* p_pixels, uint32 p_width, uint32 p_height,
               uint32 p_stride) override;

  /** @brief Swap 路径支持查询（窗口像素格式含双缓冲 + GL 标志） */
  bool SupportsSwapPresent() const override;
  /** @brief 方案 B：SwapBuffers 提交 + DwmFlush 等合成完成（见
   * IFramePresenter） */
  Error SwapPresent() override;

 private:
  void RebuildBitmapInfo(uint32 p_width, uint32 p_height);

  HWND hwnd_ = nullptr;
  HDC hdc_ = nullptr;
  uint32 width_ = 0;
  uint32 height_ = 0;
  BITMAPINFO bmi_ = {};
};
}  // namespace arhud
#endif

#endif  // ARHUD_PLATFORM_WINDOWS_FRAME_PRESENTER_WIN32_H_
