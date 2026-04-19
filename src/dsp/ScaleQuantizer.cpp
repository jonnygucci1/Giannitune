#include "ScaleQuantizer.h"
#include <cmath>

namespace gianni
{
    ScaleQuantizer::ScaleQuantizer() = default;

    void ScaleQuantizer::prepare (double sampleRate)
    {
        sr = sampleRate;
        reset();
    }

    void ScaleQuantizer::reset()
    {
        currentTargetHz  = 0.0f;
        smoothedHz       = 0.0f;
        lfoPhase         = 0.0f;
        committedMidi    = -1.0f;
        commitAgeMs      = 0.0f;
        silenceMs        = 0.0f;
        prevMidi         = -1000.0f;
        glideMsInState   = 0.0f;
        glissandoActive  = false;
    }

    void ScaleQuantizer::setScaleMask (uint16_t mask) noexcept    { scaleMask = mask; }
    void ScaleQuantizer::setRetuneSpeed (float v) noexcept        { retuneSpeed = juce::jlimit(0.0f, 100.0f, v); }
    void ScaleQuantizer::setHumanize    (float v) noexcept        { humanize    = juce::jlimit(0.0f, 100.0f, v); }

    float ScaleQuantizer::snapMidiToMask (float midi, uint16_t mask) noexcept
    {
        if (mask == 0) return midi; // no active notes — bypass

        // Split into octave + pitch class (0..12). Works for the full
        // audible MIDI range (we never see negative values in practice).
        const int   baseOctave = static_cast<int>(std::floor(midi / 12.0f));
        const float pc         = midi - static_cast<float>(baseOctave * 12);

        // Find the active pitch class with the smallest wrap-around
        // distance to pc.
        int   bestPc   = -1;
        float bestDist = 1.0e9f;
        for (int p = 0; p < 12; ++p)
        {
            if (((mask >> p) & 1u) == 0) continue;
            float d = std::abs(pc - static_cast<float>(p));
            if (d > 6.0f) d = 12.0f - d;      // wrap
            if (d < bestDist) { bestDist = d; bestPc = p; }
        }
        if (bestPc < 0) return midi;

        // Choose the nearest octave placement of that pitch class.
        float cand = static_cast<float>(baseOctave * 12 + bestPc);
        if (cand - midi >  6.0f) cand -= 12.0f;
        if (midi - cand >  6.0f) cand += 12.0f;
        return cand;
    }

    // Find the next active scale note strictly ABOVE the given MIDI value.
    // Walks upward one semitone at a time up to one octave, which is
    // always enough to find the next active note (any non-empty mask has
    // at least one bit set, so the maximum gap between consecutive active
    // notes is 11 semitones).
    float ScaleQuantizer::nextActiveAbove (float midi, uint16_t mask) noexcept
    {
        if (mask == 0) return midi + 12.0f;
        for (int i = 1; i <= 12; ++i)
        {
            const int  testMidi = static_cast<int>(std::ceil(midi)) + i - 1;
            const int  pc       = ((testMidi % 12) + 12) % 12;
            if ((mask >> pc) & 1u)
            {
                if (static_cast<float>(testMidi) > midi + 0.001f)
                    return static_cast<float>(testMidi);
            }
        }
        return midi + 12.0f;
    }

    // Next active scale note strictly BELOW the given MIDI value.
    float ScaleQuantizer::nextActiveBelow (float midi, uint16_t mask) noexcept
    {
        if (mask == 0) return midi - 12.0f;
        for (int i = 1; i <= 12; ++i)
        {
            const int  testMidi = static_cast<int>(std::floor(midi)) - i + 1;
            const int  pc       = ((testMidi % 12) + 12) % 12;
            if ((mask >> pc) & 1u)
            {
                if (static_cast<float>(testMidi) < midi - 0.001f)
                    return static_cast<float>(testMidi);
            }
        }
        return midi - 12.0f;
    }

    float ScaleQuantizer::process (float detectedHz, int numSamples,
                                    bool voiced) noexcept
    {
        const float blockMs = 1000.0f * (float) numSamples / (float) sr;

        // Treat any low-clarity frame as silence: YIN sometimes reports
        // detectedHz > 0 during breath/noise; tracking it pollutes the
        // smoother state.
        if (!voiced || detectedHz <= 0.0f)
        {
            silenceMs += blockMs;
            return currentTargetHz;
        }

        // On voice onset after > 75 ms silence, skip the usual τ-glide
        // and snap smoothedHz directly to the scale target so the new
        // note starts at pitch instead of gliding from the previous
        // note's leftover value.
        const bool trueOnset = (silenceMs >= kOnsetSilenceThresholdMs);
        silenceMs = 0.0f;

        // Hz → MIDI. Arithmetic midpoints in MIDI space are geometric
        // frequency midpoints — the natural reference for scale
        // boundaries.
        const float midi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

        // ----- Glissando detection -----------------------------------
        //
        // Measure how fast the user's pitch is changing, in semitones
        // per second. When the rate stays above threshold, we've
        // entered a glissando and correction is bypassed until the
        // voice settles again. This avoids the audible warble that
        // comes from an exponential smoother chasing a fast-moving
        // target through many scale boundaries in a short span.
        const float midiRate = (prevMidi > -100.0f)
            ? std::abs(midi - prevMidi) * 1000.0f / blockMs
            : 0.0f;
        prevMidi = midi;

        const bool fastGlide = (midiRate > kGlideRateSemPerSec);

        // Debounced state transition — need sustained activity to flip
        // into or out of glissando mode.
        if (fastGlide == glissandoActive)
            glideMsInState = 0.0f;
        else
        {
            glideMsInState += blockMs;
            if (glideMsInState >= kGlideSettleMs)
            {
                glissandoActive = fastGlide;
                glideMsInState  = 0.0f;
            }
        }

        // ----- Latch-and-commit target selection ---------------------
        //
        // Commit to a scale note; hold it until:
        //   (a) the detected MIDI crosses out of the hysteresis-
        //       widened basin of attraction, AND
        //   (b) the commit is at least kMinCommitAgeMs old.
        //
        // Hysteresis width and debounce scale with retune speed: a
        // hard-tuned setting (RS=0) keeps the ±5¢ / 25 ms behaviour
        // that sounds grippy; at slow retune (RS=100) we widen to
        // ±25¢ / 100 ms so natural vocal jitter doesn't trigger
        // audible glide transitions.
        const float rsFactor = juce::jlimit(0.0f, 1.0f, retuneSpeed / 100.0f);
        const float kHysteresisSemitones = 0.05f + 0.20f * rsFactor;
        constexpr float kBigJumpSemitones = 12.0f;
        const float kMinCommitAgeMs = 25.0f + 75.0f * rsFactor;

        commitAgeMs += blockMs;

        if (committedMidi < 0.0f
            || std::abs(midi - committedMidi) > kBigJumpSemitones)
        {
            committedMidi = snapMidiToMask(midi, scaleMask);
            commitAgeMs   = 0.0f;
        }
        else if (commitAgeMs >= kMinCommitAgeMs)
        {
            const float upNote   = nextActiveAbove(committedMidi, scaleMask);
            const float downNote = nextActiveBelow(committedMidi, scaleMask);

            const float upBoundary   = 0.5f * (committedMidi + upNote)
                                     + kHysteresisSemitones;
            const float downBoundary = 0.5f * (committedMidi + downNote)
                                     - kHysteresisSemitones;

            if (midi > upBoundary || midi < downBoundary)
            {
                const float newCommit = snapMidiToMask(midi, scaleMask);
                if (std::abs(newCommit - committedMidi) > 0.001f)
                {
                    committedMidi = newCommit;
                    commitAgeMs   = 0.0f;
                }
            }
        }

        const float snappedMidi = committedMidi;
        const float snappedHz   = 440.0f * std::pow(2.0f, (snappedMidi - 69.0f) / 12.0f);

        // ----- Glissando passthrough ---------------------------------
        //
        // When the glissando state is active and the user picked a
        // retune speed above the hard-tune threshold, output tracks
        // the user's voice directly. The correction resumes naturally
        // once the voice settles.
        //
        // At the hard-tune end of the knob (RS near 0) we deliberately
        // skip this: each commit should snap instantly, which is the
        // character users pick that setting for.
        constexpr float kGlissandoBypassMinRS = 5.0f;
        if (glissandoActive && retuneSpeed > kGlissandoBypassMinRS)
        {
            smoothedHz = detectedHz;     // ratio will be 1.0 downstream
            currentTargetHz = detectedHz;
            return currentTargetHz;
        }

        // ----- Smoother cold-start / onset snap ----------------------
        //
        // Skip the usual exponential approach in three cases:
        //   1. cold start (smoothedHz hasn't been initialised yet)
        //   2. octave-scale commit change (> 2× ratio)
        //   3. voice onset after significant silence (trueOnset)
        if (smoothedHz <= 0.0f
            || snappedHz > smoothedHz * 2.0f
            || snappedHz < smoothedHz * 0.5f
            || trueOnset)
        {
            smoothedHz = snappedHz;
        }

        // ----- Retune curve ------------------------------------------
        //
        //   τ (ms) = 1.44 × max(retuneSpeed, 3.5)
        //
        // Produces time constants across the knob range that match
        // Antares Auto-Tune Pro on measured reference renders:
        //
        //       RS     τ (ms)
        //        0       5.04   (floor)
        //       10      14.4
        //       25      36.0
        //       50      72.0
        //      100     144.0
        //
        // The 3.5-unit floor prevents audible discontinuities at scale
        // boundary crossings when the knob is all the way down.
        const float effectiveSpeed = juce::jmax(3.5f, retuneSpeed);
        const float tauSec         = 1.44e-3f * effectiveSpeed;
        const float dt             = static_cast<float>(numSamples) / static_cast<float>(sr);
        const float coef           = 1.0f - std::exp(-dt / tauSec);
        smoothedHz += coef * (snappedHz - smoothedHz);

        // ----- Humanize LFO -------------------------------------------
        //
        // Slow sinusoidal detune around the committed target, peaked
        // by the `humanize` knob. Zero when the knob is off.
        const float lfoHz     = 5.0f;
        const float cents     = 8.0f * (humanize / 100.0f);
        lfoPhase += static_cast<float>(numSamples) * lfoHz / static_cast<float>(sr);
        if (lfoPhase > 1.0f) lfoPhase -= std::floor(lfoPhase);
        const float detune = std::sin(lfoPhase * juce::MathConstants<float>::twoPi) * cents;
        const float humanizedHz = smoothedHz * std::pow(2.0f, detune / 1200.0f);

        currentTargetHz = humanizedHz;
        return currentTargetHz;
    }
}
