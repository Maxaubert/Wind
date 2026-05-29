#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

// Shared, header-only helpers for the present spike (clickprobe + harness). Inline so both
// single-TU executables can include this without a common compilation unit.
namespace spike {

inline long long QpcNow()  { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
inline long long QpcFreq() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }

// Full path in %TEMP% for a spike file name (so it works regardless of the working directory).
inline std::string TempPath(const char* name) {
    char dir[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, dir);
    std::string p(dir, (n && n <= MAX_PATH) ? n : 0);
    p += name;
    return p;
}

// Append a printf-formatted line to a %TEMP% file.
inline void LogLine(const char* file, const char* fmt, ...) {
    std::string path = TempPath(file);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Overwrite a %TEMP% file with a single printf-formatted line.
inline void WriteLine(const char* file, const char* fmt, ...) {
    std::string path = TempPath(file);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "w") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Read the entire contents of a %TEMP% file (empty string if missing).
inline std::string ReadAll(const char* file) {
    std::string path = TempPath(file);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return {};
    std::string out; char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

inline void DeleteTemp(const char* file) { DeleteFileA(TempPath(file).c_str()); }

} // namespace spike
