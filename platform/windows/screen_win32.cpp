/**
 * @file screen_win32.cpp
 * @brief Win32 平台屏幕实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-07
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core_os
 */

#include "screen_win32.h"

#ifdef ARHUD_PLATFORM_WINDOWS

#include "io/logger.h"
#include "os/memory.h"

namespace arhud
{

    static uint32_t s_next_screen_id = 1;

    Win32Screen *Win32Screen::Create(HMONITOR hmonitor)
    {
        if (!hmonitor)
        {
            return nullptr;
        }

        Win32Screen *screen = ARHUD_NEW(Win32Screen)();
        screen->InitializeFromMonitor(hmonitor);
        return screen;
    }

    void Win32Screen::InitializeFromMonitor(HMONITOR hmonitor)
    {
        hmonitor_ = hmonitor;

        MONITORINFOEXA mi;
        mi.cbSize = sizeof(MONITORINFOEXA);
        if (GetMonitorInfoA(hmonitor, &mi))
        {
            info_.x = mi.rcMonitor.left;
            info_.y = mi.rcMonitor.top;
            info_.width = mi.rcMonitor.right - mi.rcMonitor.left;
            info_.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
            info_.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        }
        else
        {
            ARHUD_LOG_WARN("Win32Screen: GetMonitorInfoA failed");
        }

        info_.dpi = QueryDpiForMonitor(hmonitor);
        info_.scale = info_.dpi / 96.0f;
        info_.refresh_rate = QueryRefreshRateForMonitor(hmonitor);
        info_.id = s_next_screen_id++;
    }

    float Win32Screen::QueryDpiForMonitor(HMONITOR hmonitor)
    {
        HMODULE shcore = LoadLibraryA("shcore.dll");
        if (shcore)
        {
            using GetDpiForMonitorFunc = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
            auto func = reinterpret_cast<GetDpiForMonitorFunc>(
                GetProcAddress(shcore, "GetDpiForMonitor"));
            if (func)
            {
                UINT dpi_x = 0;
                UINT dpi_y = 0;
                HRESULT hr = func(hmonitor, 0, &dpi_x, &dpi_y);
                FreeLibrary(shcore);
                if (SUCCEEDED(hr) && dpi_y > 0)
                {
                    return static_cast<float>(dpi_y);
                }
            }
            else
            {
                FreeLibrary(shcore);
            }
        }

        HDC hdc = GetDC(nullptr);
        if (hdc)
        {
            float dpi = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSY));
            ReleaseDC(nullptr, hdc);
            return dpi;
        }

        return 96.0f;
    }

    float Win32Screen::QueryRefreshRateForMonitor(HMONITOR hmonitor)
    {
        MONITORINFOEXA mi;
        mi.cbSize = sizeof(MONITORINFOEXA);
        if (!GetMonitorInfoA(hmonitor, &mi))
        {
            return 60.0f;
        }

        DEVMODEA dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(DEVMODEA);

        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        {
            if (dm.dmFields & DM_DISPLAYFREQUENCY)
            {
                return static_cast<float>(dm.dmDisplayFrequency);
            }
        }

        return 60.0f;
    }

    IScreen::ScreenID Win32Screen::GetId() const
    {
        return info_.id;
    }

    const ScreenInfo &Win32Screen::GetInfo() const
    {
        return info_;
    }

    int32_t Win32Screen::GetWidth() const
    {
        return info_.width;
    }

    int32_t Win32Screen::GetHeight() const
    {
        return info_.height;
    }

    float Win32Screen::GetDpi() const
    {
        return info_.dpi;
    }

    float Win32Screen::GetRefreshRate() const
    {
        return info_.refresh_rate;
    }

    float Win32Screen::GetScale() const
    {
        return info_.scale;
    }

    bool Win32Screen::IsPrimary() const
    {
        return info_.is_primary;
    }

    void *Win32Screen::GetNativeHandle() const
    {
        return hmonitor_;
    }

}

#endif