#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace gianni
{
    // -----------------------------------------------------------------
    //  GianniLookAndFeel v1.0 — "Editorial Minimal"
    //
    //  A quiet, serif-led palette inspired by editorial audio gear.
    //  Deep charcoal chassis, warm off-white ink, olive-yellow neon
    //  accent (#C8FF3A). No skeuomorphism, no 1px chamfer games. Depth
    //  comes from gradient + inner-shadow pairs; typography carries
    //  the identity.
    //
    //  Typography plan (bundled via BinaryData — see CMake):
    //    - Fraunces Italic 300      → wordmark, key-letter
    //    - Inter Tight Medium       → VOICE pills
    //    - JetBrains Mono Medium    → captions, values, meta
    //
    //  Fonts are loaded once at static-init time from the embedded
    //  TTFs so the paint loop never touches disk.
    // -----------------------------------------------------------------
    class GianniLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        GianniLookAndFeel();
        ~GianniLookAndFeel() override = default;

        struct Palette
        {
            // Chassis / panel
            juce::Colour bgBase      { juce::Colour::fromRGB (0x0C, 0x0D, 0x10) };
            juce::Colour bgPanelTop  { juce::Colour::fromRGB (0x1A, 0x1C, 0x20) };
            juce::Colour bgPanelMid  { juce::Colour::fromRGB (0x11, 0x13, 0x16) };
            juce::Colour bgPanelBot  { juce::Colour::fromRGB (0x0C, 0x0D, 0x10) };
            juce::Colour bgBowl      { juce::Colour::fromRGB (0x08, 0x0A, 0x0C) };

            // Ink (warm cream instead of pure white — "paper on metal")
            juce::Colour ink100      { juce::Colour::fromRGB (0xF3, 0xEF, 0xE7) };
            juce::Colour ink80       { juce::Colour::fromRGB (0xC9, 0xC4, 0xB8) };
            juce::Colour ink60       { juce::Colour::fromRGB (0x8A, 0x85, 0x78) };
            juce::Colour ink40       { juce::Colour::fromRGB (0x5A, 0x57, 0x4E) };
            juce::Colour ink20       { juce::Colour::fromRGB (0x3A, 0x38, 0x2F) };

            // Lines
            juce::Colour hairline    { juce::Colours::white.withAlpha (0.06f) };
            juce::Colour lineSoft    { juce::Colours::white.withAlpha (0.10f) };

            // Accent — olive neon (not the old bright spring green).
            juce::Colour accent      { juce::Colour::fromRGB (0xC8, 0xFF, 0x3A) };
            juce::Colour accentHigh  { juce::Colour::fromRGB (0xE4, 0xFF, 0x78) };
            juce::Colour accentLow   { juce::Colour::fromRGB (0x5A, 0x7A, 0x12) };
        } palette;

        // --- Font helpers ---------------------------------------------
        //     display  = Fraunces Italic   (wordmark, key-letter)
        //     pill     = Inter Tight Med.  (Voice pills)
        //     mono     = JetBrains Mono    (captions, values, meta)
        static juce::Font displayFont (float height, float tracking = -0.01f);
        static juce::Font pillFont    (float height, float tracking =  0.14f);
        static juce::Font monoFont    (float height, float tracking =  0.12f);
        static juce::Font tinyFont    (float height = 9.0f, float tracking = 0.24f);

        // Overrides — minimal. All interesting drawing lives on the
        // individual components (Knob, KeyRing, etc.) so the L&F isn't
        // tempted to centralise layout decisions.
        void drawLabel (juce::Graphics&, juce::Label&) override;
    };
}
