#pragma once
namespace wind {
// Whether Windows HDR ("Use HDR") is actually ON right now (DisplayConfig
// ADVANCED_COLOR_INFO_2 activeColorMode). False if the API is unavailable (older Windows).
bool GetHdrEnabled();
// SDR white level (nits) for the active HDR path, so HDR->SDR tonemapping matches the desktop.
// Returns a default if the query fails.
double GetSDRWhiteNits();
}
