#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

namespace gianni
{
    // ----------------------------------------------------------------------
    //  ScaleQuantizer — the pitch-correction core.
    //
    //  Given a detected input frequency (Hz) and a "scale mask" (which
    //  of the 12 pitch classes are active), returns the target
    //  frequency a shifter should move the signal toward.
    //
    //  The retune curve is a single-pole exponential smoother with
    //
    //      τ (ms) = 1.44 × max(retuneSpeed, 3.5)
    //
    //  which matches Antares Auto-Tune Pro's retune-speed behaviour
    //  across the full knob range, measured from reference renders.
    //
    //  Scale-boundary detection is latch-and-commit: the committed
    //  note is held until the detected pitch crosses the widened
    //  midpoint between two neighbouring scale notes. Hysteresis and
    //  debounce widen at slower retune speeds so natural vocal jitter
    //  doesn't flip the commit on every micro-deviation.
    //
    //  Glissando-aware: when the detected pitch is changing faster
    //  than a configurable rate threshold, correction is bypassed and
    //  the user's natural vocal run passes through untouched.
    //  Correction resumes when the voice settles.
    //
    //  The `humanize` LFO adds a slow sinusoidal detune around the
    //  committed target to avoid a dead-centre robotic feel when the
    //  knob is engaged.
    // ----------------------------------------------------------------------
    class ScaleQuantizer
    {
    public:
        ScaleQuantizer();

        void prepare (double sampleRate);
        void reset();

        // Bit 0 = C, bit 1 = C#, ..., bit 11 = B (absolute pitch classes).
        void setScaleMask (uint16_t mask12) noexcept;

        // 0 = hard-tune (5 ms τ), 100 = slow glide (144 ms τ).
        void setRetuneSpeed (float percent0to100) noexcept;
        void setHumanize    (float percent0to100) noexcept;

        // Advance one block. `voiced` must be the detector's voicing
        // bit; low-clarity frames are treated as silence regardless of
        // the detected Hz value.
        float process (float detectedHz, int numSamples,
                       bool voiced = true) noexcept;

        float getCurrentTargetHz() const noexcept { return currentTargetHz; }

        // Diagnostic accessors.
        float getCommittedMidi() const noexcept { return committedMidi; }
        float getSnapHz() const noexcept
        {
            if (committedMidi < 0.0f) return 0.0f;
            return 440.0f * std::pow (2.0f, (committedMidi - 69.0f) / 12.0f);
        }

    private:
        double sr               { 44100.0 };
        uint16_t scaleMask      { 0x0AB5 };   // default: C major
        float    retuneSpeed    { 25.0f };
        float    humanize       { 25.0f };
        float    currentTargetHz{ 0.0f };
        float    smoothedHz     { 0.0f };
        float    lfoPhase       { 0.0f };

        // Latch-and-commit state. committedMidi is the scale note we
        // are currently holding (MIDI units, negative = uninitialised);
        // commitAgeMs measures time since the last commit change for
        // debounce.
        float    committedMidi  { -1.0f };
        float    commitAgeMs    { 0.0f };

        // Voice-onset detection — when silence exceeds this, the next
        // voiced frame hard-snaps the smoother to the new scale target
        // instead of gliding from the stale previous value.
        float    silenceMs          { 0.0f };
        static constexpr float kOnsetSilenceThresholdMs = 75.0f;

        // Glissando detection. When the user's pitch moves faster than
        // kGlideRateSemPerSec for kGlideSettleMs, the quantizer
        // transitions into a bypass state (smoothedHz tracks detectedHz
        // so the downstream ratio is 1.0); it leaves the state
        // symmetrically when the pitch settles.
        //
        // Threshold tuning: sustained vocal jitter is < 1 sem/sec;
        // expressive glissandos are > 5 sem/sec. 3 sem/sec sits between
        // the two.
        float    prevMidi           { -1000.0f };
        float    glideMsInState     { 0.0f };
        bool     glissandoActive    { false };
        static constexpr float kGlideRateSemPerSec = 3.0f;
        static constexpr float kGlideSettleMs      = 60.0f;

        static float snapMidiToMask     (float midi, uint16_t mask) noexcept;
        static float nextActiveAbove    (float midi, uint16_t mask) noexcept;
        static float nextActiveBelow    (float midi, uint16_t mask) noexcept;
    };
}
