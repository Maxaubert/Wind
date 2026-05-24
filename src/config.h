#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton     = 2;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward)
    int    zoomOutButton    = 1;
    int    recenterVk       = 0;     // 0 = unbound
    double maxLevel         = 8.0;
    double fullRangeSeconds = 1.2;
    double sensitivity      = 1.0;
    int    tickHzCap        = 144;
    int    diagnostics      = 0;     // 1 = log frame-timing to wind_diag.log
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys.
Config ParseConfig(const std::string& text);

// I/O (implemented in Task 10): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
