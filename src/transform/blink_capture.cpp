// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "blink_capture.h"
#include "blink_policy.h"   // pure sender/section rules (tested)
#include "logging.h"
#include <aclapi.h>         // GetSecurityInfo
#include <cstring>
#include <cwchar>
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

namespace wind {

bool BlinkCapture::prepare(const MonitorTarget& mon) {
    if (device_ && output_ && wcscmp(device_name_, mon.device) == 0) return true;
    device_.Reset(); ctx_.Reset(); output_.Reset(); staging_.Reset();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> out;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &out) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(out->GetDesc(&d)) && wcscmp(d.DeviceName, mon.device) == 0) {
                ComPtr<IDXGIOutput5> o5;
                if (FAILED(out.As(&o5))) return false;
                D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 };
                D3D_FEATURE_LEVEL got{};
                HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 1, D3D11_SDK_VERSION,
                                               device_.ReleaseAndGetAddressOf(), &got, ctx_.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    Log(LogLevel::Warn, "blink", "D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr);
                    return false;
                }
                output_ = o5;
                wcsncpy_s(device_name_, mon.device, _TRUNCATE);
                return true;
            }
            out.Reset();
        }
        adapter.Reset();
    }
    Log(LogLevel::Warn, "blink", "no DXGI output for the target monitor");
    return false;
}

bool BlinkCapture::grab() {
    if (!output_) return false;
    // A fresh duplication's first frame is the desktop as it is now: the 1x frame, since the caller
    // flushed DWM after the identity write. FP16 first, so an HDR desktop keeps its range.
    const DXGI_FORMAT fmts[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
    ComPtr<IDXGIOutputDuplication> dupl;
    HRESULT hr = output_->DuplicateOutput1(device_.Get(), 0, 2, fmts, &dupl);
    if (FAILED(hr)) {
        Log(LogLevel::Warn, "blink", "DuplicateOutput1 failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    hr = dupl->AcquireNextFrame(100, &fi, &res);
    if (FAILED(hr)) {
        Log(LogLevel::Warn, "blink", "AcquireNextFrame failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ComPtr<ID3D11Texture2D> tex;
    bool ok = SUCCEEDED(res.As(&tex));
    if (ok) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (!staging_ || d.Width != stagingDesc_.Width || d.Height != stagingDesc_.Height ||
            d.Format != stagingDesc_.Format) {
            D3D11_TEXTURE2D_DESC s = d;
            s.Usage = D3D11_USAGE_STAGING;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            s.BindFlags = 0;
            s.MiscFlags = 0;
            s.MipLevels = 1;
            s.ArraySize = 1;
            ok = SUCCEEDED(device_->CreateTexture2D(&s, nullptr, staging_.ReleaseAndGetAddressOf()));
            stagingDesc_ = s;
        }
        if (ok) ctx_->CopyResource(staging_.Get(), tex.Get());
    }
    dupl->ReleaseFrame();
    return ok;
}

// The last sub-authority of a SID: the RID, which for a mandatory label SID is the integrity level.
static DWORD SidLastRid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return 0;
    const UCHAR n = *GetSidSubAuthorityCount(sid);
    return n ? *GetSidSubAuthority(sid, n - 1u) : 0;
}

bool BlinkSenderAllowed(DWORD pid, const char** why) {
    const char* unused = nullptr;
    if (!why) why = &unused;
    *why = "ok";
    if (pid == 0 || pid == GetCurrentProcessId()) { *why = "bad pid"; return false; }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) { *why = "cannot open the process"; return false; }
    // Fail closed: anything that cannot be read counts against the sender.
    DWORD rid = 0;
    bool appContainer = true;
    HANDLE tok = nullptr;
    if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE il[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE] = {};
        DWORD len = 0;
        if (GetTokenInformation(tok, TokenIntegrityLevel, il, sizeof(il), &len))
            rid = SidLastRid(reinterpret_cast<TOKEN_MANDATORY_LABEL*>(il)->Label.Sid);
        DWORD isAc = 1;
        if (GetTokenInformation(tok, TokenIsAppContainer, &isAc, sizeof(isAc), &len)) appContainer = isAc != 0;
        CloseHandle(tok);
    }
    DWORD mine = 0, theirs = 0;
    const bool sameSession = ProcessIdToSessionId(GetCurrentProcessId(), &mine) &&
                             ProcessIdToSessionId(pid, &theirs) && mine == theirs;
    wchar_t image[MAX_PATH * 2] = {};
    DWORD n = MAX_PATH * 2;
    const bool haveImage = QueryFullProcessImageNameW(p, 0, image, &n) != 0;
    CloseHandle(p);
    const std::wstring_view base = haveImage ? BlinkExeBase(std::wstring_view(image, n)) : std::wstring_view();
    if (BlinkSenderOk(rid, appContainer, sameSession, base)) return true;
    if (rid < kBlinkMediumRid) *why = "integrity below Medium";
    else if (appContainer) *why = "AppContainer";
    else if (!sameSession) *why = "another session";
    else *why = "not glint.exe";
    return false;
}

// Owner is this process's user and the mandatory label (none = Medium) is Medium or above. A
// section created by a Low IL or sandboxed process, or by another user, is never written. This does
// not check for no-read-up: a section with the default descriptor passes, and a same-user Low IL
// process can still open it for reading (closed by Glint labelling it Medium with no-read-up).
static bool SectionSecurityOk(HANDLE section) {
    alignas(TOKEN_USER) BYTE userBuf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = {};
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD len = 0;
    const bool haveUser = GetTokenInformation(tok, TokenUser, userBuf, sizeof(userBuf), &len) != 0;
    CloseHandle(tok);
    if (!haveUser) return false;
    PSID me = reinterpret_cast<TOKEN_USER*>(userBuf)->User.Sid;

    PSID owner = nullptr;
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(section, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                        &owner, nullptr, nullptr, &sacl, &sd) != ERROR_SUCCESS)
        return false;
    bool ok = owner && IsValidSid(owner) && EqualSid(owner, me);
    bool hasLabel = false;
    DWORD labelRid = 0;
    if (ok && sacl) {
        for (DWORD i = 0; i < sacl->AceCount; ++i) {
            void* ace = nullptr;
            if (!GetAce(sacl, i, &ace) || !ace) continue;
            if (static_cast<ACE_HEADER*>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
            auto* ml = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
            hasLabel = true;
            labelRid = SidLastRid(reinterpret_cast<PSID>(&ml->SidStart));
            break;
        }
    }
    if (sd) LocalFree(sd);
    return ok && BlinkSectionLabelOk(hasLabel, labelRid);
}

// The copy into the tool's section, under an SEH guard: a file-backed section can raise an in-page
// error and a hostile section can still fault. Kept apart with no C++ objects, since the build uses
// /EHsc and __try cannot share a function with objects that need unwinding (C2712).
static bool CopyFrameGuarded(unsigned char* dst, const BlinkFrameHeader* hdr, const unsigned char* src,
                             size_t srcPitch, size_t row, unsigned h) {
    __try {
        std::memcpy(dst, hdr, sizeof(*hdr));
        dst += sizeof(*hdr);
        for (unsigned y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * row, src + size_t(y) * srcPitch, row);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool BlinkCapture::openSection(const MonitorTarget& mon, DWORD toolPid) {
    closeSection();
    if (mon.w <= 0 || mon.h <= 0) return false;
    wchar_t name[64];
    swprintf_s(name, L"Local\\Glint.CaptureFrame.%lu", (unsigned long)toolPid);
    // READ_CONTROL so the owner and label can be checked before anything is written.
    HANDLE section = OpenFileMappingW(FILE_MAP_WRITE | READ_CONTROL, FALSE, name);
    if (!section) {
        Log(LogLevel::Warn, "blink", "no section for pid %lu", (unsigned long)toolPid);
        return false;
    }
    if (!SectionSecurityOk(section)) {
        Log(LogLevel::Warn, "blink", "section for pid %lu refused: not this user's or labelled below Medium",
            (unsigned long)toolPid);
        CloseHandle(section);
        return false;
    }
    void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi{};
    const bool queried = view && VirtualQuery(view, &mbi, sizeof(mbi)) != 0;
    // The grab format is not known yet: require room for the smallest frame (BGRA8) now, and
    // publish checks the exact size once the frame is in hand.
    const size_t minNeed = sizeof(BlinkFrameHeader) + size_t(mon.w) * size_t(mon.h) * 4u;
    if (!queried || !BlinkSectionOk(mbi.RegionSize, mbi.State == MEM_COMMIT, mbi.Type == MEM_MAPPED, minNeed)) {
        Log(LogLevel::Warn, "blink", "section for pid %lu refused: not mapped, not committed or too small",
            (unsigned long)toolPid);
        if (view) UnmapViewOfFile(view);
        CloseHandle(section);
        return false;
    }
    section_ = section;
    view_ = view;
    region_ = mbi.RegionSize;
    toolPid_ = toolPid;
    return true;
}

void BlinkCapture::closeSection() {
    if (view_) UnmapViewOfFile(view_);
    if (section_) CloseHandle(section_);
    view_ = nullptr;
    section_ = nullptr;
    region_ = 0;
    toolPid_ = 0;
}

bool BlinkCapture::publish(const MonitorTarget& mon) {
    if (!staging_ || !view_) { closeSection(); return false; }
    const bool fp16 = stagingDesc_.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const unsigned bpp = fp16 ? 8u : 4u;
    const unsigned w = stagingDesc_.Width, h = stagingDesc_.Height, row = w * bpp;
    const size_t need = sizeof(BlinkFrameHeader) + size_t(row) * h;
    bool ok = false;
    if (!BlinkSectionOk(region_, true, true, need)) {
        Log(LogLevel::Warn, "blink", "section for pid %lu too small for the frame", (unsigned long)toolPid_);
    } else {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            const BlinkFrameHeader hdr{ 0x46424C47u, 1u, w, h, row, fp16 ? 2u : 1u, mon.x, mon.y };
            ok = CopyFrameGuarded(static_cast<unsigned char*>(view_), &hdr,
                                  static_cast<const unsigned char*>(m.pData), m.RowPitch, row, h);
            ctx_->Unmap(staging_.Get(), 0);
            if (!ok) Log(LogLevel::Warn, "blink", "copy into the section for pid %lu faulted", (unsigned long)toolPid_);
        }
    }
    closeSection();
    return ok;
}

}  // namespace wind
