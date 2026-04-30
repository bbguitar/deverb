#pragma once

#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

extern "C" {
#include <fftw3.h>
#include "postfish.h"
#include "bessel.h"
#include "subband.h"
// Inlined from deverb.h — can't include by name on macOS (case collision with DeverbPlugin.h)
typedef struct {
    sig_atomic_t ratio[8];   // deverb_freqs = 8
    sig_atomic_t smooth;
    sig_atomic_t trigger;
    sig_atomic_t release;
    sig_atomic_t linkp;
    sig_atomic_t* active;
    sig_atomic_t panel_visible;
} deverb_settings;

extern time_linkage* subband_read(time_linkage* in, subband_state* f,
                                  subband_window** w, int* visible, int* active,
                                  void (*workfunc)(void*), void* arg);
} // extern "C"

// Undo postfish's C min/max macros — they break std::max/std::min in C++ headers
#undef max
#undef min

static constexpr int deverb_freqs = 8;
static constexpr float deverb_freq_list[deverb_freqs + 1] = {
    125, 250, 500, 1000, 2000, 4000, 8000, 16000, 9e10f
};

class DeverbDSP {
public:
    DeverbDSP();
    ~DeverbDSP();

    void prepare(double sampleRate, int blockSize, int numChannels);
    void reset();
    void processBlock(double** inputs, double** outputs, int nFrames);

    // Param setters (depth: 1.0 = off, 5.0 = max, matching postfish display)
    void setSmooth(float ms)          { mSettings.smooth  = (sig_atomic_t)(ms * 10.f); }
    void setRelease(float ms)         { mSettings.release = (sig_atomic_t)(ms * 10.f); }
    void setLink(bool v)              { mSettings.linkp   = v ? 1 : 0; }
    void setRatio(int band, float depthX) {
        depthX = std::max(depthX, 1.f);
        mSettings.ratio[band] = (sig_atomic_t)(1000.f / depthX);
    }

    // Called from the static subband_read callback
    void doWork();

private:
    static void workCallback(void* self) { static_cast<DeverbDSP*>(self)->doWork(); }

    subband_state   mSS{};
    iir_filter      mSmooth{}, mSmoothLimit{}, mRelease{};
    iir_state*      mIirS[deverb_freqs]{};
    iir_state*      mIirR[deverb_freqs]{};
    float           mPrevRatio[deverb_freqs]{};
    int             mInactiveDelay[deverb_freqs]{};

    deverb_settings mSettings{};
    subband_window  mSW{};

    double mSampleRate  = 44100.0;
    int    mNumChannels = 0;

    // time_linkage fed into subband_read each block
    time_linkage mInputTL{};

    // Sample FIFOs: accumulate to input_size, process, drain to output
    std::vector<float> mInQueue[OUTPUT_CHANNELS];
    std::vector<float> mOutQueue[OUTPUT_CHANNELS];
    int mInQueued = 0;

    void filterSet(float msec, iir_filter* f, int attackp, int order);
    void resetBand(int freq);
    void resetBandCh(int freq, int ch);
    void freeTL();
};
