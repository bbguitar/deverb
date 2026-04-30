#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "DeverbDSP.h"

const int kNumPresets = 1;

enum EParams {
    kSmooth = 0,
    kRelease,
    kLink,
    kRatio0, kRatio1, kRatio2, kRatio3,
    kRatio4, kRatio5, kRatio6, kRatio7,
    kBypass,
    kNumParams
};

using namespace iplug;
using namespace igraphics;

class Deverb final : public Plugin {
public:
    Deverb(const InstanceInfo& info);

#if IPLUG_DSP
    void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
    void OnReset() override;
    void OnParamChange(int paramIdx) override;
private:
    DeverbDSP mDSP;
#endif

#if IPLUG_EDITOR
public:
    int mZoomIdx = 2;  // index into kZoomLevels; default = 100%
#endif
};
