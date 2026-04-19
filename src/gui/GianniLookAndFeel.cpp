#include "GianniLookAndFeel.h"
#include "BinaryData.h"

namespace gianni
{
    // -----------------------------------------------------------------
    //  Typeface cache — load the embedded TTFs once at first access.
    //  Using a function-local static guarantees thread-safe init
    //  and lazy construction (C++11 magic statics).
    // -----------------------------------------------------------------
    namespace
    {
        struct Typefaces
        {
            juce::Typeface::Ptr display;
            juce::Typeface::Ptr pill;
            juce::Typeface::Ptr mono;
        };

        const Typefaces& getTypefaces()
        {
            static const Typefaces cache = []
            {
                Typefaces t;
                t.display = juce::Typeface::createSystemTypefaceFor (
                    GiannituneFonts::FrauncesItalic_ttf,
                    GiannituneFonts::FrauncesItalic_ttfSize);
                t.pill = juce::Typeface::createSystemTypefaceFor (
                    GiannituneFonts::InterTightMedium_ttf,
                    GiannituneFonts::InterTightMedium_ttfSize);
                t.mono = juce::Typeface::createSystemTypefaceFor (
                    GiannituneFonts::JetBrainsMonoMedium_ttf,
                    GiannituneFonts::JetBrainsMonoMedium_ttfSize);
                return t;
            }();
            return cache;
        }

        // Helper: build a Font from a Typeface with explicit height +
        // extra-kerning factor. Skips JUCE's name-based lookup entirely
        // (which can miss when the OS font list doesn't include the
        // embedded face).
        juce::Font makeFont (juce::Typeface::Ptr tf, float height, float tracking)
        {
            jassert (tf != nullptr);
            juce::Font f (juce::FontOptions (tf).withHeight (height));
            f.setExtraKerningFactor (tracking);
            return f;
        }
    }

    juce::Font GianniLookAndFeel::displayFont (float h, float tracking)
    {
        return makeFont (getTypefaces().display, h, tracking);
    }
    juce::Font GianniLookAndFeel::pillFont (float h, float tracking)
    {
        return makeFont (getTypefaces().pill, h, tracking);
    }
    juce::Font GianniLookAndFeel::monoFont (float h, float tracking)
    {
        return makeFont (getTypefaces().mono, h, tracking);
    }
    juce::Font GianniLookAndFeel::tinyFont (float h, float tracking)
    {
        return makeFont (getTypefaces().mono, h, tracking);
    }

    // -----------------------------------------------------------------
    GianniLookAndFeel::GianniLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, palette.bgBase);
        setColour (juce::Label::textColourId,                 palette.ink80);
        setColour (juce::PopupMenu::backgroundColourId,       palette.bgPanelMid);
        setColour (juce::PopupMenu::textColourId,             palette.ink80);
        setColour (juce::PopupMenu::highlightedBackgroundColourId,
                                                              palette.accent.withAlpha (0.12f));
        setColour (juce::PopupMenu::highlightedTextColourId,  palette.accentHigh);
    }

    // Use the label's own font (components set their font explicitly).
    void GianniLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& l)
    {
        g.fillAll (l.findColour (juce::Label::backgroundColourId));
        if (l.isBeingEdited()) return;

        const auto alpha = l.isEnabled() ? 1.0f : 0.5f;
        g.setFont (l.getFont());
        g.setColour (l.findColour (juce::Label::textColourId)
                       .withMultipliedAlpha (alpha));
        g.drawFittedText (l.getText(), l.getLocalBounds(),
                          l.getJustificationType(), 1, 1.0f);
    }
}
