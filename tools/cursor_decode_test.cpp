// Diagnose why some cursors (I-beam, hand) render invisible. Replicates the engine's
// DecodeCursorBGRA and reports, for ARROW/IBEAM/HAND, the decode path + pixel composition.
// Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\cursor_decode_test.cpp ^
//        /Fe:cursor_decode_test.exe /link user32.lib gdi32.lib
#include <windows.h>
#include <vector>
#include <cstdint>
#include <cstdio>

static void analyze(const wchar_t* name, LPCWSTR id, FILE* f) {
    HCURSOR hc = LoadCursorW(nullptr, id);
    ICONINFO ii{};
    if (!hc || !GetIconInfo(hc, &ii)) { fprintf(f, "%ls: GetIconInfo failed\n", name); return; }
    HDC hdc = GetDC(nullptr);
    BITMAP bm{};
    int w = 0, h = 0;
    std::vector<uint32_t> out;
    bool color = (ii.hbmColor != nullptr);
    if (color) {
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        out.assign((size_t)w * h, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(hdc, ii.hbmColor, 0, h, out.data(), &bi, DIB_RGB_COLORS);
        bool anyAlpha = false; for (uint32_t px : out) if (px & 0xFF000000u) { anyAlpha = true; break; }
        fprintf(f, "%ls: COLOR %dx%d hot(%lu,%lu) anyAlpha=%d\n", name, w, h, ii.xHotspot, ii.yHotspot, anyAlpha);
    } else {
        GetObjectW(ii.hbmMask, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight / 2;
        std::vector<uint32_t> both((size_t)w * bm.bmHeight, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(hdc, ii.hbmMask, 0, bm.bmHeight, both.data(), &bi, DIB_RGB_COLORS);
        int andOne = 0, andZero = 0, invert = 0;   // invert = AND=1 & XOR=1
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t a = both[(size_t)y * w + x] & 0xFFFFFFu;
            uint32_t xr = both[(size_t)(y + h) * w + x] & 0xFFFFFFu;
            if (a) { andOne++; if (xr) invert++; } else andZero++;
        }
        fprintf(f, "%ls: MONOCHROME %dx%d hot(%lu,%lu) andOne=%d andZero=%d invertPx=%d\n",
                name, w, h, ii.xHotspot, ii.yHotspot, andOne, andZero, invert);
    }
    ReleaseDC(nullptr, hdc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FILE* f = nullptr; fopen_s(&f, "cursor_decode_test.txt", "w");
    if (!f) return 1;
    analyze(L"ARROW", IDC_ARROW, f);
    analyze(L"IBEAM", IDC_IBEAM, f);
    analyze(L"HAND",  IDC_HAND,  f);
    fclose(f);
    return 0;
}
