"""Win32 GUI helpers for driving Stellarium during verification (Windows only).

Pure ctypes so it works under plain Python 3.10 without extra deps.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SW_RESTORE = 9
HWND_TOPMOST = wt.HWND(-1)
HWND_NOTOPMOST = wt.HWND(-2)
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040
PW_RENDERFULLCONTENT = 0x00000002


class RECT(ctypes.Structure):
    _fields_ = [("left", wt.LONG), ("top", wt.LONG), ("right", wt.LONG), ("bottom", wt.LONG)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def find_windows(title_substr):
    """Return [(hwnd, title)] for visible top-level windows whose title contains substr."""
    result = []
    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        title = buf.value
        if title_substr.lower() in title.lower():
            result.append((hwnd, title))
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    return result


def get_rect(hwnd):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r


def show_topmost(hwnd, x=0, y=0, w=0, h=0):
    """Restore, place, and raise a window as topmost so it is visible above others."""
    user32.ShowWindow(hwnd, SW_RESTORE)
    time.sleep(0.2)
    if w and h:
        user32.SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW)
    else:
        user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)
    time.sleep(0.3)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)


def drop_topmost(hwnd):
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)


def screenshot_window(hwnd, path):
    """PrintWindow-based capture; falls back to screen copy of the window rect."""
    r = get_rect(hwnd)
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        raise ValueError("window has no area")
    hdc_screen = user32.GetDC(None)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)
    ok = user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    if not ok:
        user32.PrintWindow(hwnd, hdc_mem, 0)

    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h  # top-down
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bmi), 0)

    from PIL import Image
    import os
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img.save(path)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(None, hdc_screen)
    return path, (w, h)


def screenshot_screen(path):
    from PIL import ImageGrab
    import os
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    ImageGrab.grab().save(path)
    return path


def focus(hwnd):
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.4)


if __name__ == "__main__":
    import sys
    wins = find_windows("stellarium")
    print(wins)
    if wins and len(sys.argv) > 1:
        hwnd = wins[0][0]
        show_topmost(hwnd, 0, 0, 1600, 900)
        time.sleep(1)
        print(screenshot_window(hwnd, sys.argv[1]))
