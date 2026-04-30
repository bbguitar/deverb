#include "DeverbPlugin.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

static const char* kBandLabels[deverb_freqs] = {
    "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
};

static constexpr float kDefaultDepth[deverb_freqs] = {
    1.0f, 1.3f, 1.7f, 1.7f, 1.7f, 1.7f, 1.3f, 1.0f
};

static constexpr float kZoomLevels[]   = { 0.5f, 0.75f, 1.0f, 1.1f, 1.25f, 1.5f };
static constexpr int   kNumZoomLevels  = sizeof(kZoomLevels) / sizeof(kZoomLevels[0]);
static constexpr int   kDefaultZoomIdx = 2;

constexpr int kCtrlTagAbout  = 1;
constexpr int kCtrlTagBypass = 2;

#if IPLUG_EDITOR

namespace {

// Palette — matches the variant-A teal mockup
const IColor kTeal       (255,  34, 191, 191);
const IColor kTealBg     ( 40,  34, 191, 191);
const IColor kColBG      (255,  32,  34,  42);
const IColor kColTitleBg (255,  25,  27,  34);
const IColor kColRule    (255,  42,  44,  56);
const IColor kColBorder  (255,  44,  46,  60);
const IColor kColBorderHv(255,  58,  61,  78);
const IColor kColTrack   (255,  42,  45,  58);
const IColor kColIcon    (255,  74,  96, 112);
const IColor kColIconHv  (255, 122, 154, 170);
const IColor kColFG      (255, 200, 210, 205);
const IColor kColLabel   (255, 136, 152, 168);
const IColor kColLabelDim(255,  62,  80,  96);

// ─────────────────────────────────────────────────────────────────────────
// Custom icon button
// ─────────────────────────────────────────────────────────────────────────
class IconBtnControl : public IControl {
public:
    using DrawIconFn = std::function<void(IGraphics&, const IRECT&, const IColor&)>;
    using IsActiveFn = std::function<bool()>;

    IconBtnControl(const IRECT& bounds,
                   DrawIconFn drawIcon,
                   IActionFunction onClick,
                   IsActiveFn isActive = nullptr)
        : IControl(bounds, kNoParameter)
        , mDrawIcon(std::move(drawIcon))
        , mClickFn(std::move(onClick))
        , mIsActive(std::move(isActive))
    {
    }

    void Draw(IGraphics& g) override {
        const bool active = mIsActive ? mIsActive() : false;
        const bool hover  = mMouseIsOver;

        const IColor bg     = active ? kTealBg : kColBG;
        const IColor border = active ? kTeal   : (hover ? kColBorderHv : kColBorder);
        const IColor iconC  = active ? kTeal   : (hover ? kColIconHv   : kColIcon);

        g.FillRoundRect(bg, mRECT, 5.f);
        g.DrawRoundRect(border, mRECT, 5.f, nullptr, 1.f);

        IRECT iconR = mRECT.GetCentredInside(16.f, 16.f);
        if (mDrawIcon) mDrawIcon(g, iconR, iconC);
    }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override {
        if (mClickFn) mClickFn(this);
        SetDirty(false);
    }

    void OnMouseOver(float x, float y, const IMouseMod& mod) override {
        IControl::OnMouseOver(x, y, mod);
        SetDirty(false);
    }

    void OnMouseOut() override {
        IControl::OnMouseOut();
        SetDirty(false);
    }

private:
    DrawIconFn mDrawIcon;
    IActionFunction mClickFn;
    IsActiveFn mIsActive;
};

// ─────────────────────────────────────────────────────────────────────────
// Icon drawing — each takes a 16×16 icon rect and a colour
// ─────────────────────────────────────────────────────────────────────────
void DrawZoomOutIcon(IGraphics& g, const IRECT& r, const IColor& c) {
    const float cx = r.L + 6.5f;
    const float cy = r.T + 6.5f;
    g.DrawCircle(c, cx, cy, 4.f, nullptr, 1.5f);
    g.DrawLine(c, cx - 2.f, cy, cx + 2.f, cy, nullptr, 1.5f);
    g.DrawLine(c, r.L + 10.f, r.T + 10.f, r.L + 14.5f, r.T + 14.5f, nullptr, 1.5f);
}

void DrawZoomInIcon(IGraphics& g, const IRECT& r, const IColor& c) {
    const float cx = r.L + 6.5f;
    const float cy = r.T + 6.5f;
    g.DrawCircle(c, cx, cy, 4.f, nullptr, 1.5f);
    g.DrawLine(c, cx - 2.f, cy, cx + 2.f, cy, nullptr, 1.5f);
    g.DrawLine(c, cx, cy - 2.f, cx, cy + 2.f, nullptr, 1.5f);
    g.DrawLine(c, r.L + 10.f, r.T + 10.f, r.L + 14.5f, r.T + 14.5f, nullptr, 1.5f);
}

void DrawBypassIcon(IGraphics& g, const IRECT& r, const IColor& c) {
    const float cx = r.L + 8.f;
    const float cy = r.T + 8.5f;
    g.DrawLine(c, cx, r.T + 2.5f, cx, r.T + 6.f, nullptr, 1.5f);
    // Power-symbol arc — gap at the top, sweeping clockwise the long way around.
    g.DrawArc(c, cx, cy, 4.5f, 30.f, 330.f, nullptr, 1.5f);
}

void DrawInfoIcon(IGraphics& g, const IRECT& r, const IColor& c) {
    const float cx = r.L + 8.f;
    const float cy = r.T + 8.f;
    g.DrawCircle(c, cx, cy, 5.5f, nullptr, 1.5f);
    g.DrawLine(c, cx, cy - 1.f, cx, cy + 3.f, nullptr, 1.5f);
    g.FillCircle(c, cx, cy - 3.f, 1.f);
}

// ─────────────────────────────────────────────────────────────────────────
// Logo glyph — impulse spike + exponential decay tail, fits any square rect
// ─────────────────────────────────────────────────────────────────────────
void DrawLogoGlyph(IGraphics& g, const IRECT& r) {
    const IColor dim(220, 110, 122, 134);
    const float S    = std::min(r.W(), r.H());
    const float pad  = S * 0.10f;
    const float L    = r.L + (r.W() - S) * 0.5f + pad;
    const float T    = r.T + (r.H() - S) * 0.5f + pad;
    const float R    = L + S - pad * 2.f;
    const float B    = T + S - pad * 2.f;
    const float H    = B - T;

    const float spikeX = L + (R - L) * 0.08f;
    const float spikeW = std::max(1.f, S * 0.045f);
    g.DrawLine(kTeal, spikeX, B, spikeX, T, nullptr, spikeW);

    const int   barCount = std::max(4, (int)std::round(S * 0.12f));
    const float barStart = spikeX + S * 0.07f;
    const float barSpan  = (R - S * 0.02f) - barStart;
    const float barW     = std::max(0.8f, S * 0.025f);
    for (int i = 0; i < barCount; ++i) {
        const float t = (float)i / (float)(barCount - 1);
        const float x = barStart + t * barSpan;
        const float h = H * 0.82f * std::pow(0.72f, (float)i);
        g.DrawLine(dim, x, B, x, B - h, nullptr, barW);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// About overlay
// ─────────────────────────────────────────────────────────────────────────
class AboutOverlayControl : public IControl {
public:
    explicit AboutOverlayControl(const IRECT& bounds) : IControl(bounds, kNoParameter) {
        Hide(true);
    }

    void Draw(IGraphics& g) override {
        g.FillRect(IColor(220, 8, 10, 14), mRECT);

        const float cardW = std::min(320.f, mRECT.W() - 30.f);
        const float cardH = std::min(280.f, mRECT.H() - 30.f);
        IRECT card = mRECT.GetCentredInside(cardW, cardH);
        g.FillRoundRect(kColBG, card, 8.f);
        g.DrawRoundRect(kTeal,  card, 8.f, nullptr, 1.5f);

        IRECT inner = card.GetPadded(-22.f);
        float y = inner.T;

        // Logo glyph — square, centred
        const float logoSz = 48.f;
        IRECT logoR = IRECT(inner.L + (inner.W() - logoSz) * 0.5f, y,
                            inner.L + (inner.W() + logoSz) * 0.5f, y + logoSz);
        DrawLogoGlyph(g, logoR);
        y += logoSz + 8.f;

        // Plugin name
        const float titleH = 22.f;
        g.DrawText(IText(18.f, kTeal, "Roboto-Regular", EAlign::Center, EVAlign::Middle),
                   "DEVERB", IRECT(inner.L, y, inner.R, y + titleH));
        y += titleH + 2.f;

        // Subtitle
        const float subH = 14.f;
        g.DrawText(IText(9.f, kColLabel, "Roboto-Regular", EAlign::Center, EVAlign::Middle),
                   "DEREVERBERATION", IRECT(inner.L, y, inner.R, y + subH));
        y += subH + 14.f;

        // Body
        IRECT bodyR(inner.L, y, inner.R, card.B - 22.f - 18.f);
        g.DrawText(IText(10.f, kColFG, "Roboto-Regular", EAlign::Center, EVAlign::Top),
                   "Multiband spectral dereverberation\n"
                   "for mix cleanup and dialogue cleaning.\n"
                   "Postfish \xc2\xb7 wwmedia.io",
                   bodyR);

        // Dismiss hint
        IRECT hintR(card.L, card.B - 22.f, card.R, card.B - 6.f);
        g.DrawText(IText(9.f, kColLabelDim, "Roboto-Regular", EAlign::Center, EVAlign::Middle),
                   "Click anywhere to dismiss", hintR);
    }

    void OnMouseDown(float, float, const IMouseMod&) override {
        Hide(true);
        GetUI()->SetAllControlsDirty();
    }
};

} // namespace


#endif // IPLUG_EDITOR

// ─────────────────────────────────────────────────────────────────────────

Deverb::Deverb(const InstanceInfo& info)
    : Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
    GetParam(kSmooth)->InitDouble("Smooth",   40.,  0., 2000., 0.1, "ms");
    GetParam(kRelease)->InitDouble("Release", 400., 0., 2000., 0.1, "ms");
    GetParam(kLink)->InitBool("Link Channels", true);
    GetParam(kBypass)->InitBool("Bypass", false);

    for (int i = 0; i < deverb_freqs; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "%s", kBandLabels[i]);
        GetParam(kRatio0 + i)->InitDouble(label, kDefaultDepth[i], 1.0, 5.0, 0.01, "x");
    }

#if IPLUG_EDITOR
    mMakeGraphicsFunc = [&]() {
        return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                            kZoomLevels[kDefaultZoomIdx]);
    };

    mLayoutFunc = [&](IGraphics* pGraphics) {
        pGraphics->AttachPanelBackground(kColBG);
        pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);

        const IVStyle style = DEFAULT_STYLE
            .WithColor(kBG,  kColBG)
            .WithColor(kFG,  kTeal)
            .WithColor(kOFF, kColTrack)
            .WithColor(kON,  kTeal)
            .WithColor(kHL,  kTeal)
            .WithColor(kSH,  IColor(255, 12, 13, 18))
            .WithColor(kPR,  kTeal)
            .WithDrawFrame(true)
            .WithDrawShadows(false)
            .WithValueText(IText(11.f, kTeal, "Roboto-Regular", EAlign::Center, EVAlign::Middle))
            .WithLabelText(IText(11.f, kColLabel, "Roboto-Regular", EAlign::Center, EVAlign::Middle));

        const IVStyle toggleStyle = style
            .WithColor(kBG,  kColBG)
            .WithColor(kON,  kTeal)
            .WithColor(kOFF, kColTrack);

        const IRECT full = pGraphics->GetBounds();

        // ── Title bar (46px) ────────────────────────────────────────
        const IRECT titleR = full.GetFromTop(46.f);
        pGraphics->AttachControl(new IPanelControl(titleR, kColTitleBg));
        pGraphics->AttachControl(new IPanelControl(titleR.GetFromBottom(1.f), kColRule));

        // Plugin name — left aligned
        IRECT nameR = titleR.GetReducedFromLeft(14.f);
        pGraphics->AttachControl(new ITextControl(nameR, "DEVERB",
            IText(15.f, kTeal, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));

        // Icon row — laid out from the right edge
        const float btnSize = 28.f;
        const float btnGap  = 4.f;
        const float by      = titleR.MH() - btnSize / 2.f;
        float bx = titleR.R - 14.f;

        // INFO (rightmost)
        IRECT infoR(bx - btnSize, by, bx, by + btnSize);
        pGraphics->AttachControl(new IconBtnControl(infoR, DrawInfoIcon,
            [pGraphics](IControl*) {
                if (auto* o = pGraphics->GetControlWithTag(kCtrlTagAbout)) {
                    o->Hide(false);
                    pGraphics->SetAllControlsDirty();
                }
            }));
        bx = infoR.L - btnGap;

        // BYPASS — bound to kBypass; "active" visual = NOT bypassed
        IRECT bypR(bx - btnSize, by, bx, by + btnSize);
        pGraphics->AttachControl(new IconBtnControl(bypR, DrawBypassIcon,
            [this](IControl*) {
                const double v = GetParam(kBypass)->Bool() ? 0.0 : 1.0;
                SendParameterValueFromUI(kBypass, v);
                OnParamChangeUI(kBypass);
            },
            [this]() { return !GetParam(kBypass)->Bool(); }
        ), kCtrlTagBypass);
        bx = bypR.L - 8.f;

        // Divider
        IRECT divR(bx - 1.f, titleR.MH() - 9.f, bx, titleR.MH() + 9.f);
        pGraphics->AttachControl(new IPanelControl(divR, kColBorder));
        bx -= 1.f + 8.f;

        // ZOOM IN
        IRECT zinR(bx - btnSize, by, bx, by + btnSize);
        pGraphics->AttachControl(new IconBtnControl(zinR, DrawZoomInIcon,
            [this, pGraphics](IControl*) {
                if (mZoomIdx < kNumZoomLevels - 1) {
                    ++mZoomIdx;
                    pGraphics->Resize(PLUG_WIDTH, PLUG_HEIGHT, kZoomLevels[mZoomIdx]);
                }
            }));
        bx = zinR.L - btnGap;

        // ZOOM OUT
        IRECT zoutR(bx - btnSize, by, bx, by + btnSize);
        pGraphics->AttachControl(new IconBtnControl(zoutR, DrawZoomOutIcon,
            [this, pGraphics](IControl*) {
                if (mZoomIdx > 0) {
                    --mZoomIdx;
                    pGraphics->Resize(PLUG_WIDTH, PLUG_HEIGHT, kZoomLevels[mZoomIdx]);
                }
            }));

        // ── Body ───────────────────────────────────────────────────
        IRECT body = full.GetReducedFromTop(46.f).GetPadded(-8.f);

        // Timing group
        const float timingH = 88.f;
        IRECT timingGroup = body.GetFromTop(timingH);
        body.ReduceFromTop(timingH + 6.f);

        pGraphics->AttachControl(new IVGroupControl(timingGroup, "TIMING", 8.f, style));
        IRECT timingInner = timingGroup.GetPadded(-14.f).GetReducedFromTop(14.f);
        const float sliderH = 32.f;
        pGraphics->AttachControl(new IVSliderControl(
            timingInner.GetFromTop(sliderH),
            kSmooth, "Smooth", style, true, EDirection::Horizontal));
        pGraphics->AttachControl(new IVSliderControl(
            timingInner.GetReducedFromTop(sliderH + 4.f).GetFromTop(sliderH),
            kRelease, "Release", style, true, EDirection::Horizontal));

        // Depth group
        const float linkH = 32.f;
        IRECT depthGroup = body.GetReducedFromBottom(linkH + 4.f);
        IRECT linkRow    = body.GetFromBottom(linkH);

        pGraphics->AttachControl(new IVGroupControl(depthGroup, "DEPTH  ·  1x = OFF  ·  5x = MAX", 8.f, style));

        IRECT fadersArea  = depthGroup.GetPadded(-14.f).GetReducedFromTop(14.f);
        const float labelH = 18.f;
        IRECT faderTracks = fadersArea.GetReducedFromBottom(labelH);
        IRECT labelRow    = fadersArea.GetFromBottom(labelH);

        const int nBands = deverb_freqs;
        for (int i = 0; i < nBands; ++i) {
            IRECT faderCell = faderTracks.GetGridCell(0, i, 1, nBands).GetPadded(-3.f);
            pGraphics->AttachControl(new IVSliderControl(
                faderCell, kRatio0 + i, "", style, false, EDirection::Vertical));

            IRECT labelCell = labelRow.GetGridCell(0, i, 1, nBands);
            pGraphics->AttachControl(new ITextControl(labelCell, kBandLabels[i],
                IText(11.f, kColLabelDim, "Roboto-Regular", EAlign::Center, EVAlign::Middle)));
        }

        // Link toggle
        pGraphics->AttachControl(new IVToggleControl(
            linkRow.GetCentredInside(220.f, 26.f),
            kLink, "LINK CHANNELS", toggleStyle));

        // ── About overlay (last → drawn on top) ────────────────────
        pGraphics->AttachControl(new AboutOverlayControl(full), kCtrlTagAbout);
    };
#endif
}

#if IPLUG_DSP

void Deverb::OnReset() {
    mDSP.prepare(GetSampleRate(), GetBlockSize(), NOutChansConnected());
}

void Deverb::OnParamChange(int paramIdx) {
    switch (paramIdx) {
    case kSmooth:
        mDSP.setSmooth(static_cast<float>(GetParam(kSmooth)->Value()));
        break;
    case kRelease:
        mDSP.setRelease(static_cast<float>(GetParam(kRelease)->Value()));
        break;
    case kLink:
        mDSP.setLink(GetParam(kLink)->Bool());
        break;
    case kBypass:
        // Soft bypass — ProcessBlock honours this each call.
        break;
    default:
        if (paramIdx >= kRatio0 && paramIdx <= kRatio7)
            mDSP.setRatio(paramIdx - kRatio0,
                          static_cast<float>(GetParam(paramIdx)->Value()));
        break;
    }
}

void Deverb::ProcessBlock(sample** inputs, sample** outputs, int nFrames) {
    if (GetParam(kBypass)->Bool()) {
        const int nCh = NOutChansConnected();
        for (int ch = 0; ch < nCh; ++ch) {
            std::copy_n(inputs[ch], nFrames, outputs[ch]);
        }
        return;
    }
    mDSP.processBlock(inputs, outputs, nFrames);
}

#endif
