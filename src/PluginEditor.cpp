#include "PluginEditor.h"
#include "Version.h"

namespace gianni
{

namespace
{
    // Window + panel geometry (handoff spec v2, §3.1).
    //   Window: 720×480 (back down from rc4's 530 — both pill groups
    //   now share a single row so we don't need the extra vertical
    //   space). Two side-by-side pill groups are distinguished by tiny
    //   labels above each and a comfortable horizontal gap between.
    constexpr int kWinW       = 720;
    constexpr int kWinH       = 480;
    constexpr int kPanelX     = 20;
    constexpr int kPanelY     = 10;
    constexpr int kPanelW     = 680;
    constexpr int kPanelH     = 460;

    // Header row y=10..68 → header bottom = 68
    constexpr int kHeaderDividerY   = 76;

    // Waveform strip y=96..162 (Pass 7: +6 down, +8 taller for legend
    //                            clearance and more readable curve)
    constexpr int kWaveformY        = 96;
    constexpr int kWaveformH        = 66;

    // Main controls row: key-ring centre at y=288 (was 268)
    constexpr int kMainY            = 184;

    // rc5: ONE pill row, two groups side-by-side.
    //   Divider at y=414
    //   Tiny labels at y=420 above each group ("SCALE" left, "VOICE TYPE" right)
    //   Pill groups at y=432..464, 32 px tall
    constexpr int kVoiceDividerY    = 414;
    constexpr int kPillLabelY       = 420;
    constexpr int kPillRowY         = 432;
    constexpr int kPillRowH         = 32;
    // Geometry for the two groups (centred horizontally, 40 px gap).
    constexpr int kPillGroupW       = 220;
    constexpr int kPillGroupGap     = 40;
    constexpr int kPillScaleX       = (kWinW - 2 * kPillGroupW - kPillGroupGap) / 2;
    constexpr int kPillTypeX        = kPillScaleX + kPillGroupW + kPillGroupGap;

    // ------------------------------------------------------------------
    //  Procedural noise — generate once, tile forever.
    //
    //  Design notes: the HTML uses SVG feTurbulence with
    //  mix-blend-mode:overlay at 50 % opacity. JUCE lacks a true
    //  overlay blend, so we approximate it with very low-alpha
    //  black + white specks on normal compositing. The tile is
    //  sparse on purpose — each pixel has a ~12 % chance of being
    //  textured at all. Anything denser reads as static, not grain.
    // ------------------------------------------------------------------
    juce::Image buildNoiseTile (int size = 192)
    {
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        juce::Random rng (0x9A17B0C);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                // Sparse speckle: most pixels transparent.
                const int roll = rng.nextInt (100);
                if (roll < 88) continue;        // 88 % stays transparent

                const bool bright = rng.nextBool();
                const juce::uint8 alpha = (juce::uint8)
                    (bright ? 6 + rng.nextInt (10)    // 6..15
                            : 8 + rng.nextInt (14));  // 8..21
                const juce::uint8 col = bright
                                        ? (juce::uint8) (200 + rng.nextInt (55))
                                        : (juce::uint8) (0  + rng.nextInt (28));
                bd.setPixelColour (x, y,
                    juce::Colour (col, col, col, alpha));
            }
        return img;
    }

    // Small helper: fill a rounded path used as a clipped region for
    // subsequent painting (noise, gradient, highlight pass).
    juce::Path roundedPanelPath()
    {
        juce::Path p;
        p.addRoundedRectangle ((float) kPanelX, (float) kPanelY,
                               (float) kPanelW, (float) kPanelH, 10.0f);
        return p;
    }
}

// =====================================================================
//  PanelBackground
// =====================================================================
void PanelBackground::paint (juce::Graphics& g)
{
    if (noise.isNull())
        noise = buildNoiseTile (200);

    const auto& pal = look.palette;

    // --- full-window chassis gradient (radial halo over the panel) -----
    {
        g.fillAll (pal.bgBase);

        juce::ColourGradient halo (
            juce::Colour::fromRGB (0x1A, 0x1C, 0x20), kWinW * 0.5f,   88.0f,
            juce::Colour::fromRGB (0x05, 0x06, 0x08), kWinW * 0.5f,  440.0f,
            true);
        g.setGradientFill (halo);
        g.fillRect (0, 0, kWinW, kWinH);
    }

    // --- panel base: vertical gradient + top-highlight overlay ---------
    const auto panelRect = juce::Rectangle<float>(
        (float) kPanelX, (float) kPanelY,
        (float) kPanelW, (float) kPanelH);

    {
        juce::ColourGradient grad (
            pal.bgPanelTop, panelRect.getCentreX(), panelRect.getY(),
            pal.bgPanelBot, panelRect.getCentreX(), panelRect.getBottom(),
            false);
        grad.addColour (0.55, pal.bgPanelMid);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (panelRect, 10.0f);
    }

    // --- top highlight (mimics CSS radial gradient at top) ------------
    {
        juce::ColourGradient topLift (
            juce::Colours::white.withAlpha (0.05f),
            panelRect.getCentreX(), panelRect.getY(),
            juce::Colours::transparentWhite,
            panelRect.getCentreX(), panelRect.getY() + 120.0f,
            true);
        g.setGradientFill (topLift);
        g.fillRoundedRectangle (panelRect, 10.0f);
    }

    // --- noise overlay — tiled, clipped to panel. Low fill opacity
    //     so the grain only whispers; anything louder reads as TV
    //     static and looks cheap.
    {
        juce::Graphics::ScopedSaveState _(g);
        g.reduceClipRegion (roundedPanelPath());
        g.setTiledImageFill (noise, 0, 0, 0.35f);
        g.fillRect (panelRect);
    }

    // --- inner vignette (darken corners slightly) -------------------
    {
        juce::ColourGradient vig (
            juce::Colours::transparentBlack,
            panelRect.getCentreX(), panelRect.getCentreY(),
            juce::Colours::black.withAlpha (0.40f),
            panelRect.getX(), panelRect.getBottom(),
            true);
        g.setGradientFill (vig);
        g.fillRoundedRectangle (panelRect, 10.0f);
    }

    // --- inset top highlight + bottom shadow (1 px chamfer) ---------
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawLine (panelRect.getX() + 8.0f,  panelRect.getY() + 0.5f,
                panelRect.getRight() - 8.0f, panelRect.getY() + 0.5f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.60f));
    g.drawLine (panelRect.getX() + 8.0f,  panelRect.getBottom() - 0.5f,
                panelRect.getRight() - 8.0f, panelRect.getBottom() - 0.5f, 1.0f);

    // --- panel outline (thin dark) ----------------------------------
    g.setColour (juce::Colours::black.withAlpha (0.80f));
    g.drawRoundedRectangle (panelRect, 10.0f, 1.0f);

    // --- wordmark + version (inline, baseline-aligned) --------------
    //     Spec v2: "Giannitune" and "v1.0" form a single inline row
    //     with align-items:baseline; gap:10px. We match this by
    //     measuring the wordmark width and placing the caption just
    //     to its right, with both baselines sharing a common y.
    {
        const juce::String brand = "Giannitune";
        auto bigFont = GianniLookAndFeel::displayFont (24.0f, -0.01f);
        g.setFont (bigFont);
        const float brandW = bigFont.getStringWidthFloat (brand);

        const int xBrand = 90;
        // Baseline is around y=48 for a 24px font centred on 26..62 row.
        const int yRow   = 28;
        const int hBrand = 28;

        g.setColour (pal.ink100);
        g.drawText (brand, xBrand, yRow, (int) brandW + 4, hBrand,
                    juce::Justification::centredLeft);

        // Small caps version — baseline aligned with the display text.
        // Pass 7 contrast: ink40 → ink60, 9 → 10 px.
        // Version caption pulls from kVersion so the header always
        // reflects the build, not a hardcoded literal.
        auto tiny = GianniLookAndFeel::tinyFont (10.0f, 0.24f);
        g.setFont (tiny);
        g.setColour (pal.ink60);
        const int xCap = xBrand + (int) std::ceil (brandW) + 10;
        const int yCap = yRow + 12;
        const juce::String versionCap
            = "V" + juce::String (gianni::kVersion).toUpperCase();
        g.drawText (versionCap, xCap, yCap, 120, 12,
                    juce::Justification::centredLeft);
    }

    // --- meta text + header divider ---------------------------------
    //   Pass 7: ink40 → ink60, 9 → 10 px.
    //   v1.2.1: dynamic from host — kHz + channel mode.
    {
        auto font = GianniLookAndFeel::tinyFont (10.0f, 0.24f);
        g.setFont (font);
        g.setColour (pal.ink60);

        // Build "XXX KHZ · MONO/STEREO" from current host values.
        // Round kHz to 1 decimal if non-integer (88.2, 176.4, etc.).
        const double khz = metaSampleRate * 0.001;
        juce::String khzStr;
        if (std::abs (khz - std::round (khz)) < 0.05)
            khzStr = juce::String ((int) std::round (khz));
        else
            khzStr = juce::String (khz, 1);
        const char* chStr = (metaNumChannels >= 2) ? "STEREO" : "MONO";
        const juce::String meta =
            khzStr + juce::String::fromUTF8 (" KHZ \xc2\xb7 ") + chStr;

        // right-anchored, to the left of the LIVE pill
        g.drawText (meta,
                    kPanelX, 36, kPanelW - 38 - 60 - 14, 14,
                    juce::Justification::centredRight);
    }
    {
        g.setColour (pal.hairline);
        g.fillRect (kPanelX + 28, kHeaderDividerY, kPanelW - 56, 1);
    }

    // --- bottom divider above the pill row --------------------------
    g.setColour (pal.hairline);
    g.fillRect (kPanelX + 28, kVoiceDividerY, kPanelW - 56, 1);

    // --- Tiny labels identifying the two pill-selector groups -------
    //   Both groups live on the same row; labels sit directly above
    //   each group. 9-px JetBrains Mono caps, dimmed (ink60).
    {
        auto font = GianniLookAndFeel::tinyFont (9.0f, 0.28f);
        g.setFont (font);
        g.setColour (pal.ink60);
        g.drawText ("SCALE",
                    juce::Rectangle<int>(kPillScaleX, kPillLabelY,
                                         kPillGroupW, 10),
                    juce::Justification::centred);
        g.drawText ("VOICE TYPE",
                    juce::Rectangle<int>(kPillTypeX, kPillLabelY,
                                         kPillGroupW, 10),
                    juce::Justification::centred);
    }
}


// =====================================================================
//  MonogramDisc — 30×30
// =====================================================================
void MonogramDisc::paint (juce::Graphics& g)
{
    const auto& pal = look.palette;
    const auto r = getLocalBounds().toFloat();
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    const float R  = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f - 0.5f;

    // disc gradient
    juce::ColourGradient grad (
        juce::Colour::fromRGB (0x2A, 0x2D, 0x32), cx, cy - R,
        juce::Colour::fromRGB (0x16, 0x18, 0x1B), cx, cy + R, false);
    g.setGradientFill (grad);
    g.fillEllipse (cx - R, cy - R, R * 2.0f, R * 2.0f);

    // accent dot with glow
    const float dotR = 3.0f;
    // outer glow halo
    juce::ColourGradient glow (
        pal.accent.withAlpha (0.55f), cx, cy,
        juce::Colours::transparentBlack, cx + 10.0f, cy, true);
    g.setGradientFill (glow);
    g.fillEllipse (cx - 10.0f, cy - 10.0f, 20.0f, 20.0f);
    // core
    g.setColour (pal.accent);
    g.fillEllipse (cx - dotR, cy - dotR, dotR * 2.0f, dotR * 2.0f);
}


// =====================================================================
//  LiveToggle
// =====================================================================
LiveToggle::LiveToggle (GianniLookAndFeel& laf)
    : juce::Button ("live")
{
    juce::ignoreUnused (laf);
    setClickingTogglesState (true);
    setToggleState (true, juce::dontSendNotification);
}

void LiveToggle::paintButton (juce::Graphics& g, bool over, bool down)
{
    juce::ignoreUnused (down);

    auto* laf = dynamic_cast<GianniLookAndFeel*> (&getLookAndFeel());
    if (laf == nullptr) return;
    const auto& pal = laf->palette;

    // Spec v2: label is ALWAYS "LIVE". Engagement is shown by
    // colour + glow, never by swapping the word for "BYPASS".
    const bool engaged = getToggleState();
    const auto r = getLocalBounds().toFloat();
    const float radius = r.getHeight() * 0.5f;

    // Soft fill (no blinding halo — spec v2 explicitly calls out the
    // old glow as too strong).
    if (engaged)
    {
        juce::ColourGradient grad (
            pal.accent.withAlpha (0.14f), r.getCentreX(), r.getY(),
            pal.accent.withAlpha (0.05f), r.getCentreX(), r.getBottom(),
            false);
        g.setGradientFill (grad);
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (over ? 0.05f : 0.03f));
    }
    g.fillRoundedRectangle (r, radius);

    // Thin ring
    g.setColour (engaged ? pal.accent.withAlpha (0.32f)
                         : juce::Colours::white.withAlpha (0.07f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);

    // Dot + text
    const float dotSize = 5.5f;
    const float dotX = r.getX() + 10.0f;
    const float dotY = r.getCentreY() - dotSize * 0.5f;

    if (engaged)
    {
        // gentle halo, not a full-glow blob
        g.setColour (pal.accent.withAlpha (0.32f));
        g.fillEllipse (dotX - 1.5f, dotY - 1.5f,
                       dotSize + 3.0f, dotSize + 3.0f);
    }
    g.setColour (engaged ? pal.accent : pal.ink40);
    g.fillEllipse (dotX, dotY, dotSize, dotSize);

    // Label — always "LIVE" (Pass 7: 9 → 10 px)
    g.setFont (GianniLookAndFeel::tinyFont (10.0f, 0.22f));
    g.setColour (engaged ? pal.accentHigh : pal.ink60);
    g.drawText ("LIVE",
                (int) (dotX + dotSize + 7.0f), (int) r.getY(),
                (int) (r.getRight() - dotX - dotSize - 7.0f - 10.0f),
                (int) r.getHeight(),
                juce::Justification::centredLeft);
}


// =====================================================================
//  WaveformStrip
// =====================================================================
WaveformStrip::WaveformStrip (GiannituneAudioProcessor& p, GianniLookAndFeel& laf)
    : processor (p), look (laf)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (60);
}

WaveformStrip::~WaveformStrip() { stopTimer(); }

void WaveformStrip::timerCallback()
{
    frameT += 1.0f;
    repaint();
}

void WaveformStrip::paint (juce::Graphics& g)
{
    const auto& pal = look.palette;
    const auto bounds = getLocalBounds().toFloat();

    // Rail background
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (bounds, 6.0f);

    // Inner shadow (top)
    g.setColour (juce::Colours::black.withAlpha (0.80f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawLine (bounds.getX() + 4.0f, bounds.getY() + 1.5f,
                bounds.getRight() - 4.0f, bounds.getY() + 1.5f, 1.5f);

    // Reserve a 16-px band at the top of the rail for the scope
    // legend; the waveform only draws below it so extreme peaks
    // never collide with the text.
    const float labelBandH  = 16.0f;
    const float waveAreaTop = bounds.getY()      + labelBandH;
    const float waveAreaBot = bounds.getBottom() - 8.0f;
    const float midY        = (waveAreaTop + waveAreaBot) * 0.5f;

    // Mid-line guide (dashed)
    {
        juce::Path dash;
        dash.startNewSubPath (bounds.getX() + 10.0f, midY);
        dash.lineTo          (bounds.getRight() - 10.0f, midY);
        const float dashes[] = { 1.0f, 3.0f };
        juce::Path stroked;
        juce::PathStrokeType (0.6f).createDashedStroke (
            stroked, dash, dashes, 2);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillPath (stroked);
    }

    const bool engaged = processor.getAPVTS()
                             .getRawParameterValue (pid::engaged)
                             ->load (std::memory_order_relaxed) >= 0.5f;
    const float engagedAlpha = engaged ? 1.0f : 0.35f;

    // ---- Read the processor's pitch rings ---------------------------
    //   `detected` = what the singer hit (raw YIN estimate)
    //   `target`   = what the quantiser is snapping to
    //   Correction in cents = 1200 · log2(target / detected).
    //     > 0   → plugin is pulling pitch UP (singer was flat)
    //     < 0   → plugin is pulling pitch DOWN (singer was sharp)
    //     ≈ 0   → singer is on the selected note; plugin is idle
    //   This is the classic tuning-meter view: a single line that
    //   rests at the centreline during clean performance and bulges
    //   visibly whenever the plugin actually does something.
    constexpr int kRing = GiannituneAudioProcessor::kPitchRingSize;
    constexpr int N     = kRing;

    const int writeIdx = processor.pitchRingWriteIdx.load (std::memory_order_acquire);

    std::array<float, kRing> cents {};
    std::array<float, kRing> validity {};   // 1 when voiced, 0 otherwise
    for (int i = 0; i < N; ++i)
    {
        const int src = (writeIdx + i) % kRing;
        const float det = processor.pitchDetectedRing[src];
        const float tgt = processor.pitchTargetRing  [src];
        if (det > 0.0f && tgt > 0.0f)
        {
            const float c = 1200.0f * std::log2 (tgt / det);
            // clamp to ±100 cents (one semitone) — beyond that the
            // correction is so large it's almost certainly the scale
            // jumping notes, not pitch correction on a single note.
            cents[(size_t) i]    = juce::jlimit (-100.0f, 100.0f, c);
            validity[(size_t) i] = 1.0f;
        }
        else
        {
            cents[(size_t) i]    = 0.0f;
            validity[(size_t) i] = 0.0f;
        }
    }

    // ---- Cents → y-coordinate mapping -------------------------------
    //   Rail displays ±100 cents vertically. Centreline y = midY.
    //   Grid at ±50 cents (half-semitone — "you're between two notes")
    //   and ±25 cents (perceptually noticeable detune).
    const float drawX0 = bounds.getX() + 14.0f;
    const float drawW  = bounds.getWidth() - 28.0f;
    const float amp    = (waveAreaBot - waveAreaTop) * 0.45f;
    // Pass 7: tighten the cent-range mapping. Typical vocal correction
    // sits well under ±100 cents; mapping ±50 to the rail edges makes
    // small corrections visibly active instead of squashed flat.
    constexpr float kMaxCents = 50.0f;

    auto centsToY = [&] (float c)
    {
        const float t = juce::jlimit (-1.0f, 1.0f, c / kMaxCents);
        return midY - t * amp;       // positive cents = UP
    };

    // ---- Grid lines (zero centreline + ±25/±50) --------------------
    {
        // Zero — slightly brighter because it's the "in-tune" anchor
        g.setColour (juce::Colours::white.withAlpha (0.10f * engagedAlpha));
        g.drawLine (drawX0, midY, drawX0 + drawW, midY, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.04f * engagedAlpha));
        for (const float c : { -50.0f, 50.0f })
        {
            const float y = centsToY (c);
            g.drawLine (drawX0, y, drawX0 + drawW, y, 0.6f);
        }
    }

    // ---- Light LPF on cents so the line breathes instead of
    //       ping-ponging between consecutive samples ----------------
    std::array<float, kRing> smoothed {};
    for (int i = 0; i < N; ++i)
    {
        const float vl = cents[(size_t) juce::jmax (0, i - 1)];
        const float vc = cents[(size_t) i];
        const float vr = cents[(size_t) juce::jmin (N - 1, i + 1)];
        smoothed[(size_t) i] = (vl + 2.0f * vc + vr) * 0.25f;
    }

    // ---- Build the filled correction band + core line --------------
    //   The filled band goes from centerline (y = midY) to the
    //   correction value. Positive correction → green blob ABOVE mid,
    //   negative → blob BELOW. Completely vanishes during silence or
    //   clean in-tune notes (cents ≈ 0).
    juce::Path fillUp;
    juce::Path fillDn;
    juce::Path line;
    bool  lineStarted = false;

    for (int i = 0; i < N; ++i)
    {
        const float t = (float) i / (float) (N - 1);
        const float x = drawX0 + t * drawW;
        const float c = smoothed[(size_t) i];
        const float y = centsToY (c);

        if (! lineStarted)
        {
            fillUp.startNewSubPath (x, midY);
            fillDn.startNewSubPath (x, midY);
            line  .startNewSubPath (x, y);
            lineStarted = true;
        }
        else
        {
            line.lineTo (x, y);
        }

        // split fill so up/down can use different tints
        if (c >= 0.0f)
        {
            fillUp.lineTo (x, y);
            fillDn.lineTo (x, midY);
        }
        else
        {
            fillUp.lineTo (x, midY);
            fillDn.lineTo (x, y);
        }
    }
    fillUp.lineTo (drawX0 + drawW, midY);
    fillUp.closeSubPath();
    fillDn.lineTo (drawX0 + drawW, midY);
    fillDn.closeSubPath();

    // fill
    g.setColour (pal.accent.withAlpha (0.18f * engagedAlpha));
    g.fillPath (fillUp);
    g.setColour (pal.accent.withAlpha (0.18f * engagedAlpha));
    g.fillPath (fillDn);

    // ---- Scope legend --------------------------------------------
    //   Sits inside the 16-px top band reserved above. Uses
    //   String::fromUTF8 on the ± glyph — passing the raw byte string
    //   directly caused JUCE to render it as "Â±" (Latin-1 decode).
    {
        auto tinyL = GianniLookAndFeel::tinyFont (10.0f, 0.26f);
        g.setFont (tinyL);
        const int lblY = (int) bounds.getY() + 3;

        g.setColour (pal.ink80.withAlpha (0.90f * engagedAlpha));
        g.drawText (juce::String::fromUTF8 ("\xc2\xb1 CENTS"),
                    juce::Rectangle<int>((int) drawX0, lblY, 90, 12),
                    juce::Justification::centredLeft);

        g.setColour (pal.accent.withAlpha (0.90f * engagedAlpha));
        g.drawText ("TUNING",
                    juce::Rectangle<int>((int) (drawX0 + drawW - 90),
                                          lblY, 90, 12),
                    juce::Justification::centredRight);
    }

    // Rename the later stroke section's source path
    juce::Path pitchLine = line;

    // ---- Stroke correction curve -----------------------------------
    //   Gradient: older history dim on the left, newest on the right.
    //   Soft glow underlayer + bright core.
    juce::ColourGradient grad (
        pal.accent.withAlpha (0.20f * engagedAlpha),
        drawX0, midY,
        pal.accentHigh.withAlpha (0.95f * engagedAlpha),
        drawX0 + drawW, midY,
        false);

    // Glow underlayer
    g.setColour (pal.accent.withAlpha (0.35f * engagedAlpha));
    g.strokePath (pitchLine, juce::PathStrokeType (3.6f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    // Core
    g.setGradientFill (grad);
    g.strokePath (pitchLine, juce::PathStrokeType (1.6f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
}


// =====================================================================
//  MinimalKnob
// =====================================================================
MinimalKnob::MinimalKnob (GianniLookAndFeel& laf,
                          juce::AudioProcessorValueTreeState& apvts,
                          const juce::String& paramID,
                          const juce::String& capt,
                          float defaultValue,
                          bool invert)
    : look (laf), caption (capt), defaultV (defaultValue),
      invertDisplay (invert)
{
    // Hidden slider that drives the attachment.
    slider.setRange (0.0, 100.0, 0.1);
    slider.setVisible (false);
    addChildComponent (slider);

    attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, paramID, slider);

    // Repaint whenever the parameter changes (host automation,
    // user drag, state restore all funnel through this callback).
    slider.onValueChange = [this] { repaint(); };

    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setWantsKeyboardFocus (false);
}

void MinimalKnob::resized() {}

void MinimalKnob::mouseDown (const juce::MouseEvent& e)
{
    dragStartValue = (float) slider.getValue();
    dragStartY     = e.getPosition().getY();
    dragging       = true;
    repaint();
    juce::ignoreUnused (e);
}

void MinimalKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    int dy = dragStartY - e.getPosition().getY();           // positive = up
    // Invert the drag direction when display is flipped, so that
    // "drag up" always increases the *visible* percentage.
    if (invertDisplay) dy = -dy;
    const float range = 100.0f;
    const float sensitivity = e.mods.isShiftDown() ? 900.0f : 260.0f;
    float v = dragStartValue + ((float) dy / sensitivity) * range;
    v = juce::jlimit (0.0f, 100.0f, v);
    slider.setValue (v, juce::sendNotificationSync);
}

void MinimalKnob::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void MinimalKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    slider.setValue (defaultV, juce::sendNotificationSync);
}

void MinimalKnob::mouseWheelMove (const juce::MouseEvent& e,
                                  const juce::MouseWheelDetails& w)
{
    const float range = 100.0f;
    const float step  = range / (e.mods.isShiftDown() ? 400.0f : 120.0f);
    float dir = w.deltaY >= 0.0f ? 1.0f : -1.0f;
    if (invertDisplay) dir = -dir;
    float v = (float) slider.getValue() + dir * step;
    v = juce::jlimit (0.0f, 100.0f, v);
    slider.setValue (v, juce::sendNotificationSync);
}

void MinimalKnob::paint (juce::Graphics& g)
{
    const auto& pal = look.palette;
    const auto bounds = getLocalBounds().toFloat();

    // The knob bbox is the top 128 px square; the remaining space is
    // for value + caption text.
    constexpr float kSquare = 128.0f;
    const float bx = (bounds.getWidth()  - kSquare) * 0.5f;
    auto square = juce::Rectangle<float>(bx, 0.0f, kSquare, kSquare);

    const float cx = square.getCentreX();
    const float cy = square.getCentreY();
    const float arcR = kSquare * 0.5f - 6.0f;
    const float bodyR = kSquare * 0.5f - 16.0f;

    // Raw = underlying DSP value; displayed = what the user reads and
    // what drives the arc + needle position. For an "inverted" knob
    // like RETUNE, higher visible % corresponds to a lower raw value.
    const float rawValue      = (float) slider.getValue();
    const float displayValue  = invertDisplay ? (100.0f - rawValue) : rawValue;
    const float pct           = displayValue / 100.0f;

    // Angles: JUCE uses 0=12 o'clock, clockwise positive.
    // Spec uses -225°..+45° (= 225°..495° on JUCE scale), sweep 270°.
    // Convert: -225° in spec is same as starting at 7:30 o'clock going
    // clockwise 270° to 4:30 o'clock. In JUCE rads with 0=top CW:
    //   start = 1.25π, end = 2.75π (= 0.75π + 2π)
    const float angStart = juce::MathConstants<float>::pi * 1.25f;
    const float angEnd   = juce::MathConstants<float>::pi * 2.75f;
    const float curAng   = angStart + pct * (angEnd - angStart);

    // ---- Arc track (background) ------------------------------------
    {
        juce::Path track;
        track.addCentredArc (cx, cy, arcR, arcR, 0.0f,
                             angStart, angEnd, true);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.strokePath (track, juce::PathStrokeType (3.0f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    // ---- Value arc (gradient + glow) ------------------------------
    if (pct > 0.001f)
    {
        juce::Path arc;
        arc.addCentredArc (cx, cy, arcR, arcR, 0.0f,
                           angStart, curAng, true);

        // outer glow
        g.setColour (pal.accent.withAlpha (0.35f));
        g.strokePath (arc, juce::PathStrokeType (6.0f,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        // core with gradient
        juce::ColourGradient grad (
            pal.accentHigh, cx - arcR, cy - arcR,
            pal.accent,     cx + arcR, cy + arcR, false);
        g.setGradientFill (grad);
        g.strokePath (arc, juce::PathStrokeType (2.5f,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        // end-cap dot
        const float ex = cx + std::cos (curAng - juce::MathConstants<float>::halfPi) * arcR;
        const float ey = cy + std::sin (curAng - juce::MathConstants<float>::halfPi) * arcR;
        // glow
        g.setColour (pal.accent.withAlpha (0.60f));
        g.fillEllipse (ex - 4.5f, ey - 4.5f, 9.0f, 9.0f);
        // core
        g.setColour (pal.accentHigh);
        g.fillEllipse (ex - 2.5f, ey - 2.5f, 5.0f, 5.0f);
    }

    // ---- Body disc (radial gradient + inner shadows) ---------------
    {
        // outer drop shadow
        g.setColour (juce::Colours::black.withAlpha (0.60f));
        g.fillEllipse (cx - bodyR, cy - bodyR + 4.0f,
                       bodyR * 2.0f, bodyR * 2.0f);

        // body gradient — centre at 50 %, 30 % of the body
        juce::ColourGradient body (
            juce::Colour::fromRGB (0x2A, 0x2E, 0x33), cx, cy - bodyR * 0.4f,
            juce::Colour::fromRGB (0x0F, 0x11, 0x13),
            cx + bodyR * 0.7f, cy + bodyR * 0.7f, true);
        body.addColour (0.5, juce::Colour::fromRGB (0x1A, 0x1D, 0x20));
        g.setGradientFill (body);
        g.fillEllipse (cx - bodyR, cy - bodyR,
                       bodyR * 2.0f, bodyR * 2.0f);

        // rim — thin full-circle stroke (NO horizontal highlight line,
        //       which read as an unwanted "Querstreifen" on top of the
        //       body; the radial gradient alone carries the top-light)
        g.setColour (juce::Colours::white.withAlpha (dragging ? 0.10f : 0.05f));
        g.drawEllipse (cx - bodyR, cy - bodyR,
                       bodyR * 2.0f, bodyR * 2.0f, 1.0f);
    }

    // ---- Needle (glowing vertical bar rotating with curAng) --------
    {
        const float needleLen = bodyR * 0.64f;      // 32% of body height
        const float needleTop = bodyR - needleLen - bodyR * 0.10f;
        const float needleBot = needleTop + needleLen;

        juce::Path needle;
        needle.addRoundedRectangle (-1.25f, -needleBot,
                                    2.5f, needleLen, 1.25f);

        // Rotation: curAng is measured from 12 o'clock clockwise in
        // our convention. The path is defined with its tip pointing
        // up (-y), so rotating by curAng points the tip outward.
        auto tf = juce::AffineTransform::rotation (curAng)
                     .translated (cx, cy);

        // glow halo
        g.setColour (pal.accent.withAlpha (0.75f));
        g.fillPath (needle, tf);
        g.setColour (pal.accent.withAlpha (0.35f));
        juce::Path glow;
        glow.addRoundedRectangle (-3.0f, -needleBot - 2.0f,
                                  6.0f, needleLen + 4.0f, 3.0f);
        g.fillPath (glow, tf);

        // core gradient fill via two passes
        juce::ColourGradient needleGrad (
            pal.accentHigh, 0.0f, -needleBot,
            pal.accent,     0.0f, -needleTop, false);
        g.setGradientFill (needleGrad);
        juce::Path core;
        core.addRoundedRectangle (-1.25f, -needleBot,
                                  2.5f, needleLen, 1.25f);
        g.fillPath (core, tf);
    }

    // ---- Hub (dark gradient disc in the centre) --------------------
    {
        const float hubR = bodyR * 0.25f;
        juce::ColourGradient hub (
            juce::Colour::fromRGB (0x3A, 0x3E, 0x44),
            cx - hubR * 0.2f, cy - hubR * 0.3f,
            juce::Colour::fromRGB (0x0A, 0x0B, 0x0D),
            cx + hubR, cy + hubR, true);
        hub.addColour (0.5, juce::Colour::fromRGB (0x1C, 0x1F, 0x23));
        g.setGradientFill (hub);
        g.fillEllipse (cx - hubR, cy - hubR, hubR * 2.0f, hubR * 2.0f);
        // no horizontal highlight — same "Querstreifen" rule as body
    }

    // ---- Value + caption below the bbox ----------------------------
    //   Pass 7: bumped for DAW-distance readability.
    //     value  11 → 14 px, always ink100 (was ink80 when static)
    //     caption 9 → 10.5 px, ink60 → ink80
    {
        auto valFont = GianniLookAndFeel::monoFont (14.0f, 0.10f);
        g.setFont (valFont);
        g.setColour (pal.ink100);
        g.drawText (juce::String ((int) std::round (displayValue)) + "%",
                    juce::Rectangle<int>(0, (int) kSquare + 10,
                                         (int) bounds.getWidth(), 18),
                    juce::Justification::centred);
    }
    {
        auto lblFont = GianniLookAndFeel::tinyFont (10.5f, 0.24f);
        g.setFont (lblFont);
        g.setColour (pal.ink80);
        g.drawText (caption.toUpperCase(),
                    juce::Rectangle<int>(0, (int) kSquare + 32,
                                         (int) bounds.getWidth(), 14),
                    juce::Justification::centred);
    }
}


// =====================================================================
//  KeyRing
// =====================================================================
static const juce::StringArray kNotes {
    juce::String::fromUTF8 ("C"),  juce::String::fromUTF8 ("C\xe2\x99\xaf"),
    juce::String::fromUTF8 ("D"),  juce::String::fromUTF8 ("D\xe2\x99\xaf"),
    juce::String::fromUTF8 ("E"),
    juce::String::fromUTF8 ("F"),  juce::String::fromUTF8 ("F\xe2\x99\xaf"),
    juce::String::fromUTF8 ("G"),  juce::String::fromUTF8 ("G\xe2\x99\xaf"),
    juce::String::fromUTF8 ("A"),  juce::String::fromUTF8 ("A\xe2\x99\xaf"),
    juce::String::fromUTF8 ("B")
};

KeyRing::KeyRing (GianniLookAndFeel& laf,
                  juce::AudioProcessorValueTreeState& apvtsRef)
    : look (laf), apvts (apvtsRef)
{
    apvts.addParameterListener (pid::key, &watcher);
    refreshFromParam();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

KeyRing::~KeyRing()
{
    apvts.removeParameterListener (pid::key, &watcher);
}

void KeyRing::ParamWatcher::parameterChanged (const juce::String&, float)
{
    // Called off the message thread in some hosts — marshal to UI.
    juce::MessageManager::callAsync ([self = &owner]
    {
        self->refreshFromParam();
        self->repaint();
    });
}

void KeyRing::refreshFromParam()
{
    auto* p = apvts.getRawParameterValue (pid::key);
    if (p != nullptr)
        selectedIdx = juce::jlimit (0, 11,
                                    (int) std::round (p->load (std::memory_order_relaxed)));
}

int KeyRing::hitTestNote (juce::Point<int> mp) const
{
    const auto r = getLocalBounds().toFloat();
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    const float R  = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
    const float rLabel = R - 14.0f;

    const float fx = (float) mp.x - cx;
    const float fy = (float) mp.y - cy;
    const float dist = std::sqrt (fx * fx + fy * fy);
    if (std::abs (dist - rLabel) > 14.0f) return -1;

    // Angle convention: 0 rad = 3 o'clock, increasing CCW in atan2.
    // Our layout places C at 12 o'clock (=> -π/2). We want index 0
    // (C) at 12 o'clock and progressing CW → add π/2 then wrap.
    float a = std::atan2 (fy, fx) + juce::MathConstants<float>::halfPi;
    while (a < 0) a += juce::MathConstants<float>::twoPi;
    const int idx = (int) std::round (a / juce::MathConstants<float>::twoPi * 12.0f) % 12;
    return idx;
}

void KeyRing::mouseMove (const juce::MouseEvent& e)
{
    const int idx = hitTestNote (e.getPosition());
    if (idx != hoverIdx) { hoverIdx = idx; repaint(); }
}

void KeyRing::mouseExit (const juce::MouseEvent&)
{
    if (hoverIdx != -1) { hoverIdx = -1; repaint(); }
}

void KeyRing::mouseDown (const juce::MouseEvent& e)
{
    const int idx = hitTestNote (e.getPosition());
    if (idx < 0) return;

    if (auto* param = apvts.getParameter (pid::key))
    {
        const float normalised = (float) idx / 11.0f;
        param->beginChangeGesture();
        param->setValueNotifyingHost (normalised);
        param->endChangeGesture();
    }
}

void KeyRing::paint (juce::Graphics& g)
{
    const auto& pal = look.palette;
    const auto r = getLocalBounds().toFloat();
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    const float R  = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;

    const float rOuter = R - 2.0f;
    const float rLabel = R - 14.0f;
    const float rBowl  = R - 32.0f;

    // --- Outer disc with gradient -----------------------------------
    {
        juce::ColourGradient grad (
            juce::Colour::fromRGB (0x1C, 0x1F, 0x23),
            cx, cy - rOuter * 0.2f,
            juce::Colour::fromRGB (0x0A, 0x0C, 0x0E),
            cx + rOuter, cy + rOuter, true);
        g.setGradientFill (grad);
        g.fillEllipse (cx - rOuter, cy - rOuter, rOuter * 2.0f, rOuter * 2.0f);

        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawEllipse (cx - rOuter, cy - rOuter,
                       rOuter * 2.0f, rOuter * 2.0f, 1.0f);
    }

    // --- Inner bowl (deeper, anchors the centre letter) -------------
    {
        juce::ColourGradient bowl (
            juce::Colour::fromRGB (0x0D, 0x0F, 0x12),
            cx, cy - rBowl * 0.16f,
            juce::Colour::fromRGB (0x06, 0x07, 0x09),
            cx + rBowl, cy + rBowl, true);
        g.setGradientFill (bowl);
        g.fillEllipse (cx - rBowl, cy - rBowl, rBowl * 2.0f, rBowl * 2.0f);

        // dark rim
        g.setColour (juce::Colours::black.withAlpha (0.90f));
        g.drawEllipse (cx - rBowl, cy - rBowl,
                       rBowl * 2.0f, rBowl * 2.0f, 1.0f);
        // faint highlight ring
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.drawEllipse (cx - rBowl + 1.0f, cy - rBowl + 1.0f,
                       rBowl * 2.0f - 2.0f, rBowl * 2.0f - 2.0f, 1.0f);
    }

    // --- 12 note labels + active accent dot -------------------------
    //     Pass 7: 11→13 naturals, 9.5→11 sharps. Inactive colour
    //     ink60 → ink80 so the ring still reads from a metre away.
    auto monoNormal = GianniLookAndFeel::monoFont (13.0f, 0.0f);
    auto monoSharp  = GianniLookAndFeel::monoFont (11.0f, 0.0f);

    for (int i = 0; i < 12; ++i)
    {
        const float a = ((float) i / 12.0f) * juce::MathConstants<float>::twoPi
                      - juce::MathConstants<float>::halfPi;
        const float lx = cx + std::cos (a) * rLabel;
        const float ly = cy + std::sin (a) * rLabel;

        const bool active = (i == selectedIdx);
        const bool hover  = (i == hoverIdx);
        const bool isSharp = (i == 1 || i == 3 || i == 6 || i == 8 || i == 10);

        // accent dot (opacity crossfade by repaint — no flying needed)
        {
            const float dx = cx + std::cos (a) * (rLabel - 16.0f);
            const float dy = cy + std::sin (a) * (rLabel - 16.0f);
            if (active)
            {
                g.setColour (pal.accent.withAlpha (0.55f));
                g.fillEllipse (dx - 5.0f, dy - 5.0f, 10.0f, 10.0f);
                g.setColour (pal.accent);
                g.fillEllipse (dx - 2.0f, dy - 2.0f, 4.0f, 4.0f);
            }
        }

        g.setFont (isSharp ? monoSharp : monoNormal);
        // Pass 7 contrast tier:
        //   active  → accentHigh (bright neon)
        //   hover   → ink100
        //   resting → ink80 (was ink60 — up one step for DAW distance)
        g.setColour (active ? pal.accentHigh
                   : hover  ? pal.ink100
                            : pal.ink80);

        const auto hitR = juce::Rectangle<float>(
            lx - 13.0f, ly - 13.0f, 26.0f, 26.0f).toNearestInt();
        g.drawText (kNotes[i], hitR, juce::Justification::centred);
    }

    // --- Centre caption "KEY" ---------------------------------------
    //   Pass 7: 9 → 10.5 px, ink60 → ink80 so it reads alongside the
    //   note labels at proper viewing distance.
    {
        auto cap = GianniLookAndFeel::monoFont (10.5f, 0.35f);
        g.setFont (cap);
        g.setColour (pal.ink80);
        g.drawText ("KEY",
                    juce::Rectangle<int>((int) cx - 40, (int) cy - 38, 80, 14),
                    juce::Justification::centred);
    }

    // --- Big Fraunces letter (+ optional sharp as a superscript) ----
    //     The bare letter is ALWAYS centred on (cx, cy+14) regardless
    //     of whether the selected note has an accidental. The sharp
    //     floats up-and-to-the-right like a superscript — so choosing
    //     "G#" keeps the G in the same spot "G" alone would occupy,
    //     and just fades the ♯ in above it.
    {
        const juce::String& sel = kNotes[selectedIdx];
        const juce::juce_wchar sharpChar = 0x266F;
        const bool hasAccidental = sel.length() > 1;
        const juce::String letter (sel.substring (0, 1));

        auto bigFont   = GianniLookAndFeel::displayFont (44.0f, 0.0f);
        auto sharpFont = GianniLookAndFeel::displayFont (22.0f, 0.0f);

        // Letter (fixed position)
        g.setFont (bigFont);
        g.setColour (pal.ink100);
        g.drawText (letter,
                    juce::Rectangle<int>((int) cx - 30,
                                         (int) cy - 14, 60, 50),
                    juce::Justification::centred);

        // Sharp superscript — sits at the upper-right of the letter so
        // it reads as an accidental badge rather than a second letter
        if (hasAccidental)
        {
            g.setFont (sharpFont);
            g.setColour (pal.accent);
            g.drawText (juce::String::charToString (sharpChar),
                        juce::Rectangle<int>((int) cx + 14, (int) cy - 20,
                                             22, 22),
                        juce::Justification::centredLeft);
        }
    }
}


// =====================================================================
//  VoicePill
// =====================================================================
// Visual order — also the label set. Maps to the `scale` APVTS param
// via visualToParam() in the header.
//   Visual 0 "MINOR"    → param index 1 (scale::Minor)
//   Visual 1 "MAJOR"    → param index 0 (scale::Major)
//   Visual 2 "HARMONIC" → param index 2 (scale::Chromatic)
// The underlying choice is still the 12-tone chromatic mask; "Harmonic"
// is the brand-correct display name.
static const juce::StringArray kVoicesVisual { "MINOR", "MAJOR", "HARMONIC" };

VoicePill::VoicePill (GianniLookAndFeel& laf,
                      juce::AudioProcessorValueTreeState& apvtsRef)
    : look (laf), apvts (apvtsRef)
{
    apvts.addParameterListener (pid::scale, &watcher);
    auto* p = apvts.getRawParameterValue (pid::scale);
    if (p != nullptr)
        activeVisual = paramToVisual (juce::jlimit (0, 2,
                                      (int) std::round (p->load (std::memory_order_relaxed))));
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

VoicePill::~VoicePill()
{
    apvts.removeParameterListener (pid::scale, &watcher);
}

void VoicePill::ParamWatcher::parameterChanged (const juce::String&, float)
{
    juce::MessageManager::callAsync ([self = &owner]
    {
        auto* p = self->apvts.getRawParameterValue (pid::scale);
        if (p != nullptr)
            self->activeVisual = paramToVisual (juce::jlimit (0, 2,
                                  (int) std::round (p->load (std::memory_order_relaxed))));
        self->repaint();
    });
}

juce::Rectangle<int> VoicePill::segmentBounds (int visualIdx) const
{
    const auto r = getLocalBounds().reduced (3);
    const int w  = r.getWidth() / kNumSegments;
    return juce::Rectangle<int>(r.getX() + visualIdx * w, r.getY(),
                                w, r.getHeight());
}

void VoicePill::mouseMove (const juce::MouseEvent& e)
{
    int hit = -1;
    for (int i = 0; i < kNumSegments; ++i)
        if (segmentBounds (i).contains (e.getPosition())) { hit = i; break; }
    if (hit != hoverVisual) { hoverVisual = hit; repaint(); }
}

void VoicePill::mouseExit (const juce::MouseEvent&)
{
    if (hoverVisual != -1) { hoverVisual = -1; repaint(); }
}

void VoicePill::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < kNumSegments; ++i)
    {
        if (! segmentBounds (i).contains (e.getPosition())) continue;
        if (auto* param = apvts.getParameter (pid::scale))
        {
            const int pIdx = visualToParam (i);
            const float normalised = (float) pIdx / 2.0f;
            param->beginChangeGesture();
            param->setValueNotifyingHost (normalised);
            param->endChangeGesture();
        }
        return;
    }
}

void VoicePill::paint (juce::Graphics& g)
{
    const auto& pal = look.palette;
    const auto r = getLocalBounds().toFloat();
    const float radius = r.getHeight() * 0.5f;

    // outer container (recessed)
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (juce::Colours::black.withAlpha (0.70f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.drawLine (r.getX() + 6.0f, r.getY() + 1.0f,
                r.getRight() - 6.0f, r.getY() + 1.0f, 1.0f);

    // segments
    auto font = GianniLookAndFeel::pillFont (11.0f, 0.14f);
    g.setFont (font);

    for (int i = 0; i < kNumSegments; ++i)
    {
        const auto seg = segmentBounds (i).toFloat();
        const bool active = (i == activeVisual);
        const bool hover  = (i == hoverVisual);
        const float segRadius = seg.getHeight() * 0.5f;

        if (active)
        {
            // gradient fill
            juce::ColourGradient grad (
                juce::Colour::fromRGB (0x33, 0x36, 0x3B),
                seg.getCentreX(), seg.getY(),
                juce::Colour::fromRGB (0x1E, 0x21, 0x25),
                seg.getCentreX(), seg.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (seg.reduced (1.0f), segRadius);

            // accent ring
            g.setColour (pal.accent.withAlpha (0.35f));
            g.drawRoundedRectangle (seg.reduced (0.5f), segRadius - 0.5f, 1.0f);

            // top highlight
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawLine (seg.getX() + 8.0f, seg.getY() + 2.0f,
                        seg.getRight() - 8.0f, seg.getY() + 2.0f, 0.8f);
        }
        else if (hover)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (seg.reduced (1.0f), segRadius);
        }

        g.setColour (active ? pal.ink100 : (hover ? pal.ink80 : pal.ink60));
        g.drawText (kVoicesVisual[i], seg.toNearestInt(),
                    juce::Justification::centred);
    }
}


// =====================================================================
//  VoiceTypePill — 5 segments bound to `voice_type` APVTS.
//
//  Visual order matches the param's choice list (Parameters.cpp), so
//  no visualToParam() remapping is needed.
// =====================================================================
// rc5: reduced from 5 to 3 — "Instrument" and "Bass Instrument" were
// dropped from the APVTS param entirely (see Parameters.cpp). Keeps the
// row compact enough to fit alongside the SCALE pill in one row.
static const juce::StringArray kVoiceTypeLabels {
    "SOPRANO", "ALTO/TEN", "LOW MALE"
};

VoiceTypePill::VoiceTypePill (GianniLookAndFeel& laf,
                              juce::AudioProcessorValueTreeState& apvtsRef)
    : look (laf), apvts (apvtsRef)
{
    apvts.addParameterListener (pid::voiceType, &watcher);
    auto* p = apvts.getRawParameterValue (pid::voiceType);
    if (p != nullptr)
        activeIdx = juce::jlimit (0, 4,
                                  (int) std::round (p->load (std::memory_order_relaxed)));
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

VoiceTypePill::~VoiceTypePill()
{
    apvts.removeParameterListener (pid::voiceType, &watcher);
}

void VoiceTypePill::ParamWatcher::parameterChanged (const juce::String&, float)
{
    juce::MessageManager::callAsync ([self = &owner]
    {
        auto* p = self->apvts.getRawParameterValue (pid::voiceType);
        if (p != nullptr)
            self->activeIdx = juce::jlimit (0, 4,
                              (int) std::round (p->load (std::memory_order_relaxed)));
        self->repaint();
    });
}

juce::Rectangle<int> VoiceTypePill::segmentBounds (int idx) const
{
    const auto r = getLocalBounds().reduced (3);
    const int w  = r.getWidth() / kNumSegments;
    return juce::Rectangle<int>(r.getX() + idx * w, r.getY(),
                                w, r.getHeight());
}

void VoiceTypePill::mouseMove (const juce::MouseEvent& e)
{
    int hit = -1;
    for (int i = 0; i < kNumSegments; ++i)
        if (segmentBounds (i).contains (e.getPosition())) { hit = i; break; }
    if (hit != hoverIdx) { hoverIdx = hit; repaint(); }
}

void VoiceTypePill::mouseExit (const juce::MouseEvent&)
{
    if (hoverIdx != -1) { hoverIdx = -1; repaint(); }
}

void VoiceTypePill::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < kNumSegments; ++i)
    {
        if (! segmentBounds (i).contains (e.getPosition())) continue;
        if (auto* param = apvts.getParameter (pid::voiceType))
        {
            const float normalised = (float) i / (float) (kNumSegments - 1);
            param->beginChangeGesture();
            param->setValueNotifyingHost (normalised);
            param->endChangeGesture();
        }
        return;
    }
}

void VoiceTypePill::paint (juce::Graphics& g)
{
    const auto& pal = look.palette;
    const auto r = getLocalBounds().toFloat();
    const float radius = r.getHeight() * 0.5f;

    // Outer recessed container (identical look to VoicePill so the
    // user reads the two rows as "same control family").
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (juce::Colours::black.withAlpha (0.70f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.drawLine (r.getX() + 6.0f, r.getY() + 1.0f,
                r.getRight() - 6.0f, r.getY() + 1.0f, 1.0f);

    // Segments. Smaller pill-font than VoicePill because 5 labels on
    // similar row width means less horizontal space per segment.
    auto font = GianniLookAndFeel::pillFont (10.0f, 0.12f);
    g.setFont (font);

    for (int i = 0; i < kNumSegments; ++i)
    {
        const auto seg = segmentBounds (i).toFloat();
        const bool active = (i == activeIdx);
        const bool hover  = (i == hoverIdx);
        const float segRadius = seg.getHeight() * 0.5f;

        if (active)
        {
            juce::ColourGradient grad (
                juce::Colour::fromRGB (0x33, 0x36, 0x3B),
                seg.getCentreX(), seg.getY(),
                juce::Colour::fromRGB (0x1E, 0x21, 0x25),
                seg.getCentreX(), seg.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (seg.reduced (1.0f), segRadius);

            g.setColour (pal.accent.withAlpha (0.35f));
            g.drawRoundedRectangle (seg.reduced (0.5f), segRadius - 0.5f, 1.0f);

            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawLine (seg.getX() + 8.0f, seg.getY() + 2.0f,
                        seg.getRight() - 8.0f, seg.getY() + 2.0f, 0.8f);
        }
        else if (hover)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (seg.reduced (1.0f), segRadius);
        }

        g.setColour (active ? pal.ink100 : (hover ? pal.ink80 : pal.ink60));
        g.drawText (kVoiceTypeLabels[i], seg.toNearestInt(),
                    juce::Justification::centred);
    }
}

} // namespace gianni


// =====================================================================
//  GiannituneAudioProcessorEditor
// =====================================================================
GiannituneAudioProcessorEditor::GiannituneAudioProcessorEditor (
    GiannituneAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      waveform  (p, lookAndFeel),
      // v1.1 semantics (see PluginProcessor.cpp):
      //   retune_speed APVTS → dry/wet MIX (APVTS 0 = full wet)
      //   humanize     APVTS → air EXCITER (APVTS 0 = full air)
      // invertDisplay=true on both so UI % grows with effect strength.
      // Double-click-reset values match the param defaults so "reset"
      // is meaningful: RETUNE resets to FULL WET (user's favourite
      // "geiler Autotune"), AURA resets to OFF.
      retuneKnob (lookAndFeel, p.getAPVTS(), gianni::pid::retuneSpeed,
                  "Retune", 0.0f,   /*invert=*/true),
      keyRing    (lookAndFeel, p.getAPVTS()),
      auraKnob   (lookAndFeel, p.getAPVTS(), gianni::pid::humanize,
                  "Aura",   100.0f, /*invert=*/true),
      voicePill     (lookAndFeel, p.getAPVTS()),
      voiceTypePill (lookAndFeel, p.getAPVTS())
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (panel);        // draws wordmark + meta + dividers
    addAndMakeVisible (monogram);
    addAndMakeVisible (livePill);
    addAndMakeVisible (waveform);
    addAndMakeVisible (retuneKnob);
    addAndMakeVisible (keyRing);
    addAndMakeVisible (auraKnob);
    addAndMakeVisible (voicePill);
    addAndMakeVisible (voiceTypePill);

    engagedAttach =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            p.getAPVTS(), gianni::pid::engaged, livePill);

    // The waveform already polls the `engaged` param every frame
    // (60 Hz timer) so no extra wiring is needed for the fade.

    // v1.2.1: push initial meta NOW so the first paint shows correct
    // values before the 1 Hz timer fires.
    panel.setMeta (processor.getSampleRate(),
                   processor.getTotalNumInputChannels());
    startTimer (1000);   // 1 Hz meta-refresh

    setSize (gianni::kWinW, gianni::kWinH);
    setResizable (false, false);
}

void GiannituneAudioProcessorEditor::timerCallback()
{
    // v1.2.1: sync header meta with host. getSampleRate returns 0
    // before prepareToPlay; guard so we don't show "0 kHz".
    const double sr = processor.getSampleRate();
    if (sr > 0.0)
        panel.setMeta (sr, processor.getTotalNumInputChannels());
}

GiannituneAudioProcessorEditor::~GiannituneAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void GiannituneAudioProcessorEditor::paint (juce::Graphics&)
{
    // The panel child handles the whole background; leave empty.
}

void GiannituneAudioProcessorEditor::resized()
{
    panel.setBounds (0, 0, gianni::kWinW, gianni::kWinH);

    // --- Header ----------------------------------------------------
    //   Target: every header element's optical centre sits at y≈42,
    //   the y-height of the italic wordmark (lowercase "e" middle).
    //   Previously the 30-px monogram disc was at y=32 → its centre
    //   landed at y=47, visibly ~5 px below the wordmark — fixed.

    // Monogram 30×30, centre at y=42
    monogram.setBounds (48, 27, 30, 30);

    // LIVE pill: right-anchored, centre at y=42
    constexpr int pillW = 60;
    constexpr int pillH = 26;
    constexpr int pillX = gianni::kPanelX + gianni::kPanelW - 28 - pillW;
    livePill.setBounds (pillX, 29, pillW, pillH);

    // --- Waveform strip y=90..148 ----------------------------------
    waveform.setBounds (48, gianni::kWaveformY,
                        gianni::kWinW - 48 * 2, gianni::kWaveformH);

    // --- Main controls row -----------------------------------------
    //   Knob bbox is 128 px square for the dial, + 56 px for the
    //   value + caption stack (both grew in Pass 7).
    const int knobW = 140;
    const int knobH = 128 + 54;
    retuneKnob.setBounds (168 - knobW / 2, gianni::kMainY + 8,
                          knobW, knobH);
    auraKnob.setBounds   (552 - knobW / 2, gianni::kMainY + 8,
                          knobW, knobH);

    // Key ring 208×208 centred at (360, 288) — moved down with the
    // rest of the main row in Pass 7.
    keyRing.setBounds (360 - 104, 288 - 104, 208, 208);

    // --- Pill row (two groups side-by-side) ------------------------
    //   Left  group: SCALE      (Minor / Major / Harmonic)
    //   Right group: VOICE TYPE (Soprano / Alto·Tenor / Low Male)
    //   Tiny labels above each (drawn in PanelBackground::paint).
    const int vpH = gianni::kPillRowH - 2;
    const int y   = gianni::kPillRowY + 1;
    voicePill    .setBounds (gianni::kPillScaleX, y,
                              gianni::kPillGroupW, vpH);
    voiceTypePill.setBounds (gianni::kPillTypeX,  y,
                              gianni::kPillGroupW, vpH);
}
