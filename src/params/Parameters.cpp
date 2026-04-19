#include "Parameters.h"

namespace gianni
{
    using APVTS  = juce::AudioProcessorValueTreeState;
    using PFloat = juce::AudioParameterFloat;
    using PBool  = juce::AudioParameterBool;
    using PInt   = juce::AudioParameterInt;
    using PChoice = juce::AudioParameterChoice;

    APVTS::ParameterLayout createParameterLayout()
    {
        APVTS::ParameterLayout layout;

        // --- Voice Type ---
        //   v1.1-rc5: reduced from 5 to 3 choices. "Instrument" and
        //   "Bass Instrument" were UX dead-weight — their detector
        //   ranges overlapped Alto/Tenor and Low Male respectively and
        //   the user never needed a separate preset for them. Saved
        //   states with idx 3 or 4 get clamped to 2 (Low Male) on
        //   restore — acceptable since the audible detection range
        //   subset is compatible.
        layout.add(std::make_unique<PChoice>(
            juce::ParameterID{ pid::voiceType, 1 }, "Voice Type",
            juce::StringArray{ "Soprano", "Alto/Tenor", "Low Male" }, 1));

        // --- Key ---
        layout.add(std::make_unique<PChoice>(
            juce::ParameterID{ pid::key, 1 }, "Key",
            juce::StringArray{ "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" }, 0));

        // --- Scale ---
        layout.add(std::make_unique<PChoice>(
            juce::ParameterID{ pid::scale, 1 }, "Scale",
            juce::StringArray{ "Major", "Minor", "Chromatic" }, 0));

        // --- Retune (v1.1: wet/dry mix, not smoothing time) ---
        //   APVTS ID stays `retune_speed` for automation compatibility
        //   with saved v1.0 sessions, but the label + semantic change.
        //     APVTS   0 = UI 100 % = full wet (pure T-Pain)
        //     APVTS 100 = UI   0 % = minimum wet (dry-dominant)
        //   Default 0 → user-intuitive "full T-Pain" on first insert.
        layout.add(std::make_unique<PFloat>(
            juce::ParameterID{ pid::retuneSpeed, 1 }, "Retune",
            juce::NormalisableRange<float>{ 0.0f, 100.0f, 0.1f }, 0.0f));

        // --- Aura (v1.1: air exciter amount, not pitch humanize) ---
        //   APVTS ID stays `humanize` for automation compatibility.
        //     APVTS   0 = UI 100 % = full air exciter
        //     APVTS 100 = UI   0 % = exciter bypassed
        //   Default 100 → "no effect" on first insert so the user
        //   hears the pure tune + can layer Aura to taste.
        layout.add(std::make_unique<PFloat>(
            juce::ParameterID{ pid::humanize, 1 }, "Aura",
            juce::NormalisableRange<float>{ 0.0f, 100.0f, 0.1f }, 100.0f));

        // --- Engaged (global bypass toggle driven by the Live pill) -
        //     Default true: plugin is active on insert. Host bypass
        //     and this param both route through the same gate in
        //     processBlock so automation works either way.
        layout.add(std::make_unique<PBool>(
            juce::ParameterID{ pid::engaged, 1 }, "Engaged", true));

        // v0.4.0 cleanup: removed parameters that were listener-wired
        // but never consumed by the DSP, to avoid cluttering the host's
        // generic parameter list and confusing the user:
        //   - "tracking" (Normal/Choosy)  — never read by detector
        //   - "live_mode"                  — PsolaShifter-only feature,
        //                                    ignored by the v0.4.0
        //                                    Signalsmith engine
        //   - "motion_enabled/pattern/rate/sync" — Auto-Motion DSP
        //                                          never implemented
        //   - 12 per-note toggles — superseded by Key + Scale
        //
        // The ids for the removed params stay in Parameters.h so any
        // old saved state that references them can still deserialise
        // without throwing (JUCE just ignores unknown ids on restore).

        return layout;
    }
}
