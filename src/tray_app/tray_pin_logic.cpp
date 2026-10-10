#include "tray_pin.h"
#include <cwctype>

namespace wind { namespace TrayPin {

std::wstring ExpandPath(const std::wstring& raw, const GuidResolver& resolve) {
    if (raw.empty() || raw[0] != L'{') return raw;
    const size_t close = raw.find(L'}');
    if (close == std::wstring::npos) return raw;
    std::wstring dir;
    if (!resolve || !resolve(raw.substr(0, close + 1), dir) || dir.empty()) return std::wstring();
    std::wstring rest = raw.substr(close + 1);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (!rest.empty() && rest[0] != L'\\' && rest[0] != L'/') dir += L'\\';
    return dir + rest;
}

static std::wstring Norm(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) o += (c == L'/') ? L'\\' : (wchar_t)std::towlower(c);
    while (!o.empty() && o.back() == L'\\') o.pop_back();
    return o;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = Norm(a);
    return !na.empty() && na == Norm(b);
}

bool EntryMatches(const std::wstring& rawExePath, unsigned long entryUid,
                  const std::wstring& selfExePath, unsigned long selfUid, const GuidResolver& resolve) {
    if (entryUid != selfUid) return false;
    return SamePath(ExpandPath(rawExePath, resolve), selfExePath);
}

}}  // namespace wind::TrayPin
