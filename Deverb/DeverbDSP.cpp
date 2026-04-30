#include "DeverbDSP.h"
#include <cstdio>

// Definitions for postfish globals — every postfish C file declares these extern.
extern "C" {
    int input_size = 2048;
    int input_rate = 44100;
    int input_ch   = 2;

    sig_atomic_t loop_active      = 0;
    sig_atomic_t playback_active  = 0;
    sig_atomic_t playback_exit    = 0;
    sig_atomic_t playback_seeking = 0;
    sig_atomic_t master_att       = 0;
    int outfileno      = -1;
    int input_seekable = 0;
    int eventpipe[2]   = {-1, -1};

    void clean_exit(int sig) { (void)sig; }

    // mute_channel_muted: used by subband_read; nothing is muted in plugin context
    int mute_channel_muted(u_int32_t bitmap, int i) { (void)bitmap; (void)i; return 0; }
}

// Per-band default deverb depths (1.0 = off, postfish defaults)
static constexpr float kDefaultDepth[deverb_freqs] = {
    1.0f, 1.3f, 1.7f, 1.7f, 1.7f, 1.7f, 1.3f, 1.0f
};

// ------------------------------------------------------------------ lifecycle

DeverbDSP::DeverbDSP() {
    mSettings.smooth  = 400;   // 40 ms * 10
    mSettings.release = 4000;  // 400 ms * 10
    mSettings.linkp   = 1;
    for (int i = 0; i < deverb_freqs; ++i) {
        mSettings.ratio[i] = (sig_atomic_t)(1000.f / kDefaultDepth[i]);
        mInactiveDelay[i]  = 2;
        mPrevRatio[i]      = 0.f;
    }
}

DeverbDSP::~DeverbDSP() {
    for (int i = 0; i < deverb_freqs; ++i) {
        delete[] mIirS[i];
        delete[] mIirR[i];
    }
    freeTL();
}

void DeverbDSP::freeTL() {
    if (mInputTL.data) {
        for (int ch = 0; ch < mInputTL.channels; ++ch)
            free(mInputTL.data[ch]);
        free(mInputTL.data);
        mInputTL.data = nullptr;
    }
}

// ------------------------------------------------------------------ prepare

void DeverbDSP::prepare(double sampleRate, int blockSize, int numChannels) {
    mSampleRate  = sampleRate;
    mNumChannels = std::min(numChannels, (int)OUTPUT_CHANNELS);

    // Fixed subband processing block size — independent of the DAW's blockSize.
    // Determines FFT resolution and latency (2 * input_size samples = ~93ms @ 44.1kHz).
    const int subbandSize = 2048;

    input_size = subbandSize;
    input_rate = (int)sampleRate;
    input_ch   = mNumChannels;

    // Allocate time_linkage for feeding subband_read
    freeTL();
    mInputTL.data     = (float**)malloc(mNumChannels * sizeof(float*));
    mInputTL.channels = mNumChannels;
    mInputTL.samples  = subbandSize;
    mInputTL.active   = (1u << mNumChannels) - 1u;
    mInputTL.alias    = 0;
    for (int ch = 0; ch < mNumChannels; ++ch)
        mInputTL.data[ch] = (float*)calloc(subbandSize, sizeof(float));

    // Subband state
    subband_reset(&mSS);
    subband_load(&mSS, deverb_freqs, subbandSize / 16, mNumChannels);
    subband_load_freqs(&mSS, &mSW, deverb_freq_list, deverb_freqs);

    // IIR states
    for (int i = 0; i < deverb_freqs; ++i) {
        delete[] mIirS[i]; mIirS[i] = new iir_state[mNumChannels]{};
        delete[] mIirR[i]; mIirR[i] = new iir_state[mNumChannels]{};
        mInactiveDelay[i] = 2;
        mPrevRatio[i]     = 0.f;
    }

    const float smoothMs  = mSettings.smooth  > 0 ? mSettings.smooth  * 0.1f : 40.f;
    const float releaseMs = mSettings.release > 0 ? mSettings.release * 0.1f : 400.f;
    filterSet(smoothMs,  &mSmooth,      1, 2);
    filterSet(smoothMs,  &mSmoothLimit, 0, 1);
    filterSet(releaseMs, &mRelease,     0, 1);

    // Clear FIFOs
    for (int ch = 0; ch < OUTPUT_CHANNELS; ++ch) {
        mInQueue[ch].clear();
        mOutQueue[ch].clear();
    }
    mInQueued = 0;
}

// ------------------------------------------------------------------ reset

void DeverbDSP::reset() {
    subband_reset(&mSS);
    for (int i = 0; i < deverb_freqs; ++i)
        resetBand(i);
    for (int ch = 0; ch < OUTPUT_CHANNELS; ++ch) {
        mInQueue[ch].clear();
        mOutQueue[ch].clear();
    }
    mInQueued = 0;
}

// ------------------------------------------------------------------ filter helpers

void DeverbDSP::filterSet(float msec, iir_filter* f, int attackp, int order) {
    if (msec <= 0.f) msec = 1.f;
    float corner_freq = 500.f / msec;
    const long limit_ahead = input_size * 2 - mSS.qblocksize * 3;
    if (limit_ahead > 0 && impulse_freq2(limit_ahead) * 1.01f > corner_freq && attackp)
        corner_freq = impulse_freq2(limit_ahead);
    const float alpha = corner_freq / (float)input_rate;
    f->g     = mkbessel((double)alpha, order, f->c);
    f->alpha = alpha;
    f->Hz    = alpha * (float)input_rate;
    f->ms    = msec;
}

void DeverbDSP::resetBand(int freq) {
    for (int ch = 0; ch < mNumChannels; ++ch) {
        memset(&mIirS[freq][ch], 0, sizeof(iir_state));
        memset(&mIirR[freq][ch], 0, sizeof(iir_state));
    }
    mInactiveDelay[freq] = 2;
}

void DeverbDSP::resetBandCh(int freq, int ch) {
    memset(&mIirS[freq][ch], 0, sizeof(iir_state));
    memset(&mIirR[freq][ch], 0, sizeof(iir_state));
}

// ------------------------------------------------------------------ doWork (IIR on bands)
// Called by subband_read after FFT decomposition fills mSS.lap.

void DeverbDSP::doWork() {
    const float smoothMs  = mSettings.smooth  * 0.1f;
    const float releaseMs = mSettings.release * 0.1f;
    if (smoothMs  > 0.f && smoothMs  != mSmooth.ms) {
        filterSet(smoothMs,  &mSmooth,      1, 2);
        filterSet(smoothMs,  &mSmoothLimit, 0, 1);
    }
    if (releaseMs > 0.f && releaseMs != mRelease.ms)
        filterSet(releaseMs, &mRelease, 0, 1);

    const int ahead = (int)impulse_ahead2(mSmooth.alpha);
    const int n     = input_size;

    for (int i = 0; i < deverb_freqs; ++i) {
        const int   ratio   = mSettings.ratio[i];
        const float multEnd = (mInactiveDelay[i] > 0) ? 0.f : 1.f - 1000.f / (float)ratio;

        if (ratio == 1000 && mPrevRatio[i] == 0.f) {
            resetBand(i);
            mPrevRatio[i] = 0.f;
            continue;
        }
        if (mInactiveDelay[i] > 0) --mInactiveDelay[i];

        std::vector<float> fast(n, 0.f);
        std::vector<float> slow(n);
        int firstLink = 0;

        for (int ch = 0; ch < mNumChannels; ++ch) {
            if (!mSS.lap || !mSS.lap[i] || !mSS.lap[i][ch]) {
                resetBandCh(i, ch);
                continue;
            }

            float* x = mSS.lap[i][ch] + ahead;

            if (mSettings.linkp) {
                if (firstLink == 0) {
                    const float scale = 1.f / mNumChannels;
                    for (int l = 0; l < mNumChannels; ++l) {
                        if (!mSS.lap[i][l]) continue;
                        const float* lx = mSS.lap[i][l] + ahead;
                        for (int k = 0; k < n; ++k)
                            fast[k] += lx[k] * lx[k];
                    }
                    for (int k = 0; k < n; ++k) fast[k] *= scale;
                }
            } else {
                for (int k = 0; k < n; ++k) fast[k] = x[k] * x[k];
            }

            if (!mSettings.linkp || firstLink == 0) {
                compute_iir_freefall_limited(fast.data(), n, &mIirS[i][ch],
                                             &mSmooth, &mSmoothLimit);
                memcpy(slow.data(), fast.data(), n * sizeof(float));
                compute_iir_freefallonly1(slow.data(), n, &mIirR[i][ch], &mRelease);

                float* band = mSS.lap[i][ch];
                if (multEnd == mPrevRatio[i]) {
                    for (int k = 0; k < n; ++k) {
                        const float g = fromdB_a(
                            (todB_a(slow[k]) - todB_a(fast[k])) * .5f * multEnd);
                        if (g < 1.f) band[k] *= g;
                    }
                } else {
                    float mult      = mPrevRatio[i];
                    const float step = (multEnd - mult) / n;
                    for (int k = 0; k < n; ++k) {
                        const float g = fromdB_a(
                            (todB_a(slow[k]) - todB_a(fast[k])) * .5f * mult);
                        if (g < 1.f) band[k] *= g;
                        mult += step;
                    }
                }

                if (mSettings.linkp && firstLink == 0) {
                    for (int l = 0; l < mNumChannels; ++l) {
                        if (l != ch) {
                            memcpy(&mIirS[i][l], &mIirS[i][ch], sizeof(iir_state));
                            memcpy(&mIirR[i][l], &mIirR[i][ch], sizeof(iir_state));
                        }
                    }
                }
                ++firstLink;
            }
        }
        mPrevRatio[i] = multEnd;
    }
}

// ------------------------------------------------------------------ processBlock

void DeverbDSP::processBlock(double** inputs, double** outputs, int nFrames) {
    if (!mIirS[0] || !mSS.out.data || input_size == 0) {
        for (int ch = 0; ch < mNumChannels; ++ch)
            for (int s = 0; s < nFrames; ++s) outputs[ch][s] = inputs[ch][s];
        return;
    }

    // Accumulate input into per-channel FIFOs
    for (int ch = 0; ch < mNumChannels; ++ch)
        for (int s = 0; s < nFrames; ++s)
            mInQueue[ch].push_back((float)inputs[ch][s]);
    mInQueued += nFrames;

    // Process full subband blocks
    while (mInQueued >= input_size) {
        // Load one block into the time_linkage
        for (int ch = 0; ch < mNumChannels; ++ch) {
            memcpy(mInputTL.data[ch], mInQueue[ch].data(), input_size * sizeof(float));
            mInQueue[ch].erase(mInQueue[ch].begin(), mInQueue[ch].begin() + input_size);
        }
        mInQueued -= input_size;
        mInputTL.samples = input_size;
        mInputTL.active  = (1u << mNumChannels) - 1u;

        int   active[OUTPUT_CHANNELS]  = {};
        int   visible[OUTPUT_CHANNELS] = {};
        subband_window* w[OUTPUT_CHANNELS] = {};
        for (int ch = 0; ch < mNumChannels; ++ch) {
            active[ch]  = 1;
            visible[ch] = 0;
            w[ch]       = &mSW;
        }

        // FFT decompose → workCallback(this) → doWork() → IFFT overlap-add
        time_linkage* out = subband_read(&mInputTL, &mSS, w, visible, active,
                                          workCallback, this);

        // Enqueue output (may be 0 during the two priming frames)
        if (out && out->samples > 0)
            for (int ch = 0; ch < mNumChannels; ++ch)
                for (int s = 0; s < out->samples; ++s)
                    mOutQueue[ch].push_back(out->data[ch][s]);
    }

    // Pull from output FIFOs
    for (int ch = 0; ch < mNumChannels; ++ch) {
        const int avail = (int)mOutQueue[ch].size();
        for (int s = 0; s < nFrames; ++s) {
            outputs[ch][s] = s < avail ? (double)mOutQueue[ch][s] : 0.0;
        }
        if (avail >= nFrames)
            mOutQueue[ch].erase(mOutQueue[ch].begin(), mOutQueue[ch].begin() + nFrames);
        else
            mOutQueue[ch].clear();
    }
}
