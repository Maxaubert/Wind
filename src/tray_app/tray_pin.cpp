#include "tray_pin.h"
#include "../logging.h"
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

namespace wind { namespace TrayPin {

static bool ResolveKnownFolder(const std::wstring& guidText, std::wstring& out) {
    GUID id{};
    if (FAILED(CLSIDFromString(guidText.c_str(), &id))) return false;
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) { CoTaskMemFree(p); return false; }
    out = p;
    CoTaskMemFree(p);
    return true;
}

static bool ReadString(HKEY key, const wchar_t* name, std::wstring& out) {
    wchar_t buf[1024];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &cb) != ERROR_SUCCESS)
        return false;
    out = buf;
    return true;
}

static bool ReadDword(HKEY key, const wchar_t* name, DWORD& out) {
    DWORD cb = sizeof(out);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &out, &cb) == ERROR_SUCCESS;
}

Result ApplyPinned(bool pinned, unsigned long uid) {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return Result::Error;

    HKEY root = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                                  KEY_READ, &root);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return Result::NoEntry;
    if (rc != ERROR_SUCCESS) return Result::Error;

    Result res = Result::NoEntry;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = ARRAYSIZE(name);
        const LONG er = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (er == ERROR_NO_MORE_ITEMS) break;
        if (er != ERROR_SUCCESS) continue;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE, &sub) != ERROR_SUCCESS) continue;
        std::wstring exe;
        DWORD entryUid = 0;
        const bool mine = ReadString(sub, L"ExecutablePath", exe) && ReadDword(sub, L"UID", entryUid) &&
                          EntryMatches(exe, entryUid, self, uid, ResolveKnownFolder);
        if (mine) {
            DWORD cur = 0;
            ReadDword(sub, L"IsPromoted", cur);   // absent = Windows default = overflow
            const DWORD want = pinned ? 1 : 0;
            if ((cur != 0) == pinned) {
                res = Result::AlreadyOk;
            } else {
                HKEY w = nullptr;
                if (RegOpenKeyExW(root, name, 0, KEY_SET_VALUE, &w) == ERROR_SUCCESS) {
                    if (RegSetValueExW(w, L"IsPromoted", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&want), sizeof(want)) == ERROR_SUCCESS) {
                        wind::Log(wind::LogLevel::Info, "tray", "taskbar pin: IsPromoted=%lu", want);
                        res = Result::Applied;
                    } else {
                        res = Result::Error;
                    }
                    RegCloseKey(w);
                } else {
                    res = Result::Error;
                }
            }
        }
        RegCloseKey(sub);
        if (mine) break;
    }
    RegCloseKey(root);
    return res;
}

}}  // namespace wind::TrayPin
