#pragma once
namespace wind {
class MagnifierEngine {
public:
    bool initialize();                                  // MagInitialize
    void setTransform(double level, int xOffset, int yOffset);
    void shutdown();                                    // reset to 1x then MagUninitialize
    bool ready() const { return ready_; }
    // True if the most recent attempt to enable input routing succeeded. False means
    // MagSetInputTransform was rejected (typically no UIAccess) - clicks will misalign.
    bool inputTransformOk() const { return inputTransformOk_; }
    unsigned long lastInputErr() const { return lastItxErr_; }   // GetLastError on failure
    // Last rect passed to MagSetInputTransform (diagnostics): source = magnified region.
    void lastInputRects(long& sl, long& st, long& sr, long& sb,
                        long& dr, long& db) const {
        sl = lastSl_; st = lastSt_; sr = lastSr_; sb = lastSb_; dr = lastDr_; db = lastDb_;
    }
private:
    bool ready_ = false;
    bool inputTransformOn_ = false;   // MagSetInputTransform currently enabled
    bool inputTransformOk_ = false;   // last enable attempt returned success
    unsigned long lastItxErr_ = 0;
    long lastSl_ = 0, lastSt_ = 0, lastSr_ = 0, lastSb_ = 0, lastDr_ = 0, lastDb_ = 0;
};
}
