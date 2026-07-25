#pragma once
#include <windows.h>
#include <unordered_map>
namespace wind {
class CursorBlanker {
public:
    CursorBlanker();
    const std::unordered_map<HCURSOR, HCURSOR>& originals() const { return originals_; }
    bool blanked() const { return blanked_; }
    void blank();
    void restore();
private:
    std::unordered_map<HCURSOR, HCURSOR> originals_;
    bool blanked_ = false;
};
}
