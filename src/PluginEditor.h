#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "gui/GianniLookAndFeel.h"

// =====================================================================
//  PluginEditor v1.0 — Editorial Minimal
//
//  Each visual building block from giannitune.html lives as its own
//  JUCE component. Draw order (back to front):
//
//      PanelBackground   — gradient + outer shadow + noise overlay
//      HeaderBar         — monogram / wordmark / meta / LIVE pill
//      WaveformStrip     — live-animated polyline driven by RT + AU
//      MinimalKnob  x 2  — retune + aura
//      KeyRing           — 12-note ring + centred Fraunces letter
//      VoicePill         — 3-segment Minor/Major/Chromatic toggle
//
//  All interactions route through juce::AudioProcessorValueTreeState
//  so host automation stays in sync.
// =====================================================================

namespace gianni
{

// -------------------------------------------------------------------
//  PanelBackground — the chassis. Paints the deep charcoal gradient,
//  a subtle outer vignette, and a one-time procedural noise overlay.
// -------------------------------------------------------------------
class PanelBackground : public juce::Component
{
public:
    explicit PanelBackground (GianniLookAndFeel& laf) : look (laf) {}
    void paint (juce::Graphics&) override;

    // v1.2.1: dynamic project-meta (sample rate + channel mode) in
    // the top-right of the header. Editor pushes the current values
    // each paint tick; PanelBackground renders them.
    void setMeta (double sampleRate, int numChannels) noexcept
    {
        if (sampleRate == metaSampleRate && numChannels == metaNumChannels)
            return;
        metaSampleRate    = sampleRate;
        metaNumChannels   = numChannels;
        repaint();
    }

private:
    GianniLookAndFeel& look;
    juce::Image noise;  // cached

    double metaSampleRate  { 48000.0 };   // fallback
    int    metaNumChannels { 1 };          // fallback
};

// -------------------------------------------------------------------
//  MonogramDisc — 30×30 dark disc with a glowing accent dot centre.
// -------------------------------------------------------------------
class MonogramDisc : public juce::Component
{
public:
    explicit MonogramDisc (GianniLookAndFeel& laf) : look (laf) {}
    void paint (juce::Graphics&) override;
private:
    GianniLookAndFeel& look;
};

// -------------------------------------------------------------------
//  LiveToggle — the pill in the top-right. Reads/writes the engaged
//  bool APVTS param. Fades between LIVE (green) and BYPASS (gray).
// -------------------------------------------------------------------
class LiveToggle : public juce::Button
{
public:
    explicit LiveToggle (GianniLookAndFeel& laf);

    void paintButton (juce::Graphics&, bool over, bool down) override;
};

// -------------------------------------------------------------------
//  WaveformStrip — 60 fps polyline that reacts to retune + aura.
//  No actual audio is routed here; the visual is a synthesised
//  sine-stack whose quantisation grows with "strength" (= 50 % of
//  each knob). This matches the React mockup exactly.
// -------------------------------------------------------------------
class WaveformStrip : public juce::Component,
                      private juce::Timer
{
public:
    WaveformStrip (GiannituneAudioProcessor& p, GianniLookAndFeel& laf);
    ~WaveformStrip() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    GiannituneAudioProcessor& processor;
    GianniLookAndFeel& look;
    float frameT { 0.0f };
};

// -------------------------------------------------------------------
//  MinimalKnob — dark circular face, value arc, glowing needle,
//  cap hub. Backed by a hidden juce::Slider so the SliderAttachment
//  keeps the APVTS bidirectionally synced.
// -------------------------------------------------------------------
class MinimalKnob : public juce::Component
{
public:
    // `invertDisplay` flips the visual mapping so that a higher
    // displayed percentage corresponds to a lower APVTS value. Used
    // for RETUNE, where the underlying `retune_speed` DSP param has
    // 0 = instant/strongest and 100 = slow/weakest effect — the
    // opposite of what users intuitively expect from the label.
    MinimalKnob (GianniLookAndFeel& laf,
                 juce::AudioProcessorValueTreeState& apvts,
                 const juce::String& paramID,
                 const juce::String& caption,
                 float defaultValue,
                 bool invertDisplay = false);

    void paint    (juce::Graphics&) override;
    void resized  () override;

    // Drag handling lives on the knob itself; we forward to the
    // hidden Slider so automation gets the same value updates.
    void mouseDown  (const juce::MouseEvent&) override;
    void mouseDrag  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails&) override;

private:
    GianniLookAndFeel& look;
    juce::String caption;
    float defaultV;
    bool  invertDisplay;

    // Internal parameter-backed slider (not displayed).
    juce::Slider slider { juce::Slider::LinearVertical,
                          juce::Slider::NoTextBox };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;

    // Drag state
    float dragStartValue { 0.0f };
    int   dragStartY     { 0 };
    bool  dragging       { false };
};

// -------------------------------------------------------------------
//  KeyRing — the central 208×208 ring. Clicking any of the 12 note
//  labels sets the `key` APVTS param. The centred letter uses
//  Fraunces Italic 300; sharps rendered in accent.
// -------------------------------------------------------------------
class KeyRing : public juce::Component
{
public:
    KeyRing (GianniLookAndFeel& laf,
             juce::AudioProcessorValueTreeState& apvts);
    ~KeyRing() override;

    void paint (juce::Graphics&) override;

    void mouseMove  (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseDown  (const juce::MouseEvent&) override;

private:
    int hitTestNote (juce::Point<int>) const;   // -1 = miss
    void refreshFromParam();

    GianniLookAndFeel& look;
    juce::AudioProcessorValueTreeState& apvts;

    // Listener on `key` so host automation updates the ring too.
    struct ParamWatcher
        : public juce::AudioProcessorValueTreeState::Listener
    {
        explicit ParamWatcher (KeyRing& ownerRef) : owner (ownerRef) {}
        void parameterChanged (const juce::String&, float) override;
        KeyRing& owner;
    };
    ParamWatcher watcher { *this };

    int selectedIdx { 0 };
    int hoverIdx    { -1 };
};

// -------------------------------------------------------------------
//  VoiceTypePill — 5-segment radio toggle for the `voice_type` APVTS
//  param (SOPRANO / ALTO·TENOR / LOW MALE / INSTR / BASS). Sits below
//  the SCALE pill so the user can pick the vocal register that the
//  pitch detector + Crystal exciter are tuned for. Visual order =
//  param order, so no mapping layer (unlike VoicePill below).
// -------------------------------------------------------------------
class VoiceTypePill : public juce::Component
{
public:
    VoiceTypePill (GianniLookAndFeel& laf,
                   juce::AudioProcessorValueTreeState& apvts);
    ~VoiceTypePill() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    static constexpr int kNumSegments = 3;  // Soprano / Alto·Tenor / Low Male
    juce::Rectangle<int> segmentBounds (int idx) const;

    GianniLookAndFeel& look;
    juce::AudioProcessorValueTreeState& apvts;

    struct ParamWatcher
        : public juce::AudioProcessorValueTreeState::Listener
    {
        explicit ParamWatcher (VoiceTypePill& o) : owner (o) {}
        void parameterChanged (const juce::String&, float) override;
        VoiceTypePill& owner;
    };
    ParamWatcher watcher { *this };

    int activeIdx { 1 };   // Alto/Tenor = default
    int hoverIdx  { -1 };
};

// -------------------------------------------------------------------
//  VoicePill — 3-segment radio toggle for the `scale` APVTS param
//  (Minor / Major / Chromatic in display order).
// -------------------------------------------------------------------
class VoicePill : public juce::Component
{
public:
    VoicePill (GianniLookAndFeel& laf,
               juce::AudioProcessorValueTreeState& apvts);
    ~VoicePill() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    static constexpr int kNumSegments = 3;
    // Visual order = { "Minor", "Major", "Chromatic" }.
    // Underlying `scale` param order is { Major=0, Minor=1, Chromatic=2 }
    // — see params/Parameters.cpp. We map between the two explicitly.
    static constexpr int visualToParam (int v) noexcept
    {
        // Minor → 1, Major → 0, Chromatic → 2
        return v == 0 ? 1 : v == 1 ? 0 : 2;
    }
    static constexpr int paramToVisual (int p) noexcept
    {
        return p == 0 ? 1 : p == 1 ? 0 : 2;
    }

    juce::Rectangle<int> segmentBounds (int visualIdx) const;

    GianniLookAndFeel& look;
    juce::AudioProcessorValueTreeState& apvts;

    struct ParamWatcher
        : public juce::AudioProcessorValueTreeState::Listener
    {
        explicit ParamWatcher (VoicePill& o) : owner (o) {}
        void parameterChanged (const juce::String&, float) override;
        VoicePill& owner;
    };
    ParamWatcher watcher { *this };

    int activeVisual { 0 };
    int hoverVisual  { -1 };
};

} // namespace gianni

// =====================================================================
//  Main editor
// =====================================================================
class GiannituneAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      private juce::Timer
{
public:
    explicit GiannituneAudioProcessorEditor (GiannituneAudioProcessor&);
    ~GiannituneAudioProcessorEditor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;

    // v1.2.1: 1 Hz poll of host sample-rate + channel config to
    // keep the header meta text ("48 KHZ · MONO") in sync with
    // the actual project. Implemented via base juce::Timer — cheap.
    void timerCallback() override;

private:
    GiannituneAudioProcessor& processor;
    gianni::GianniLookAndFeel lookAndFeel;

    // Layers
    gianni::PanelBackground panel     { lookAndFeel };
    gianni::MonogramDisc    monogram  { lookAndFeel };
    gianni::LiveToggle      livePill  { lookAndFeel };
    gianni::WaveformStrip   waveform;
    gianni::MinimalKnob     retuneKnob;
    gianni::KeyRing         keyRing;
    gianni::MinimalKnob     auraKnob;
    gianni::VoicePill       voicePill;
    gianni::VoiceTypePill   voiceTypePill;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        engagedAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GiannituneAudioProcessorEditor)
};
