#include "PitchDetector.h"
#include <cmath>

namespace gianni
{
    PitchDetector::PitchDetector() = default;

    void PitchDetector::prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sr = sampleRate;
        reset();
    }

    void PitchDetector::reset()
    {
        ring.fill(0.0f);
        cmnd.fill(0.0f);
        writeIdx            = 0;
        samplesSinceAnalyse = 0;
        samplesPushed       = 0;
        voicedStreak        = 0;
        unvoicedStreak      = 0;
        lastHz              = 0.0f;
        lastPeriod          = 0.0f;
        lastClarity         = 0.0f;
        voiced              = false;
    }

    void PitchDetector::setPitchRange (float minHz, float maxHz) noexcept
    {
        // v0.3.6: voice-type range now populates the SOFT prior bounds
        // (voiceTauMin/voiceTauMax) and no longer narrows the CMNDF
        // search. YIN always runs the full kTauMin..kTauMax (100..3000
        // Hz @ 48k), so an out-of-range candidate is still available
        // as a fallback.
        const float safeMax = juce::jmax (1.0f, maxHz);
        const float safeMin = juce::jmax (1.0f, minHz);
        const int newTauMin = (int) std::floor ((float) sr / safeMax);
        const int newTauMax = (int) std::ceil  ((float) sr / safeMin);
        voiceTauMin = juce::jlimit (kTauMinHard, kTauMaxHard - 1, newTauMin);
        voiceTauMax = juce::jlimit (voiceTauMin + 1, kTauMaxHard, newTauMax);
    }

    void PitchDetector::push (const float* mono, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) writeIdx] = mono[i];
            writeIdx = (writeIdx + 1) % kBufSize;
        }
        samplesSinceAnalyse += n;
        samplesPushed       += n;

        // Re-analyse on hops. We may need multiple analyses if the host
        // delivered a very large block.
        while (samplesSinceAnalyse >= kHopSize)
        {
            samplesSinceAnalyse -= kHopSize;
            runYin();
        }
    }

    void PitchDetector::copyAnalysisFrame (float* dest) const noexcept
    {
        // v0.3.6: full-range CMNDF — copy kWindowSize + kTauMax so that
        // the worst-case difference function d(tau) at tau=kTauMax is
        // well defined. No longer uses voice-type-narrowed tauMax since
        // YIN now searches the whole range and filters by prior later.
        const int needed = kWindowSize + kTauMax;
        int start = (writeIdx - needed + kBufSize) % kBufSize;
        for (int i = 0; i < needed; ++i)
            dest[i] = ring[(size_t) ((start + i) % kBufSize)];
    }

    void PitchDetector::runYin() noexcept
    {
        // Gate 1: the ring buffer must be fully filled with real input
        // samples before YIN's difference function is meaningful.
        // Running earlier is a guaranteed subharmonic error. We still
        // wait for the HARD tauMax (not runtime tauMax) worth of
        // samples so a voice-type switch mid-session is free of
        // transients.
        if (samplesPushed < (int64_t) (kWindowSize + kTauMaxHard))
        {
            voiced      = false;
            lastClarity = 0.0f;
            return;
        }

        copyAnalysisFrame(frame.data());

        // ---- Gate 2: Transient guard (step 1 — feature compute) ------
        //
        // v0.3.5: reject noise bursts (lip smacks, plosive consonants,
        // bed-sheet rustle, mouth clicks) BEFORE YIN. These events have
        // two signatures that distinguish them from voiced sound:
        //
        //   (a) Zero-crossing rate in the analysis window is much higher
        //       than any periodic voice signal could produce. A pure
        //       sine at f0 crosses zero 2·f0/sr times per sample.
        //       At the top of the current Soprano range (1200 Hz) that's
        //       0.05 crossings/sample; harmonic content pushes it up to
        //       ~0.10-0.19 on the 8-harmonic vocal stimulus (measured
        //       2026-04-16 via transient-stress). White noise sits
        //       near 0.5. We reject anything above 0.25 — above every
        //       clean-vocal block in the calibration set, well below
        //       the burst floor (min 0.30 during bursts).
        //
        //   (b) Crest factor (peak/RMS). A clean sine has √2 ≈ 1.41, a
        //       vocal with 8 harmonics has ~3-5, broadband noise 3-4,
        //       impulsive transients (plops, mic bumps) >8. Catches the
        //       short percussive events that have moderate ZCR but very
        //       peaky envelopes.
        //
        // Either gate triggers → treat as unvoiced for the rest of this
        // frame. YIN still runs and updates lastHz for diagnostic
        // purposes, but the voiced flag stays false so the downstream
        // PSOLA chain does not engage on non-pitched material.
        //
        // Detection is backed by the `transient-stress` subcommand of
        // Giannitune_DspTest, which mixes 30 ms noise bursts into a
        // harmonic vocal stack and counts how often the detector
        // incorrectly classifies those bursts as voiced. v0.3.4 hit
        // 100 %; this guard aims to drive that to near 0 % without
        // regressing the click-hunt baseline on clean real vocals.
        // Measure ZCR/crest over ONLY the most recent kHopSize samples
        // (the 5.3 ms window of new content since the last YIN hop),
        // not the 21.3 ms analysis window. A burst of ~30 ms gets
        // "diluted" when averaged over 1024 samples but dominates a
        // 256-sample slice. Using the hop-sized window gives a much
        // sharper transient response: during a real burst the hop
        // slice is almost entirely noise, so ZCR jumps above the
        // voice ceiling within a single YIN update.
        float windowZcr   = 0.0f;
        float windowCrest = 0.0f;
        {
            // The most recent kHopSize samples occupy the tail of the
            // analysis frame (which was copied from the ring buffer
            // aligned to the write head).
            const int slice0 = kWindowSize + kTauMax - kHopSize;
            const float* slice = frame.data() + slice0;

            int zc = 0;
            float prev = slice[0];
            for (int i = 1; i < kHopSize; ++i)
            {
                const float cur = slice[i];
                if ((prev >= 0.0f) != (cur >= 0.0f)) ++zc;
                prev = cur;
            }
            windowZcr = (float) zc / (float) (kHopSize - 1);

            float sumSq = 0.0f;
            float peak  = 0.0f;
            for (int i = 0; i < kHopSize; ++i)
            {
                const float a = std::abs(slice[i]);
                if (a > peak) peak = a;
                sumSq += slice[i] * slice[i];
            }
            const float rms = std::sqrt (sumSq / (float) kHopSize);
            windowCrest = rms > 1.0e-6f ? peak / rms : 0.0f;
        }

        // ---- Step 1: difference function -----------------------------
        // d(tau) = sum_{i=0}^{W-1} (x[i] - x[i+tau])^2
        // v0.3.6: always compute over the full hard range. Cost is
        // ~480*1024 = 491k ops per hop = ~0.1 ms on modern CPUs.
        // Measured impact on CPU-load telemetry: under 0.2 % absolute.
        cmnd[0] = 1.0f;             // YIN convention: cmnd[0] = 1
        const float* x = frame.data();
        for (int tau = 1; tau <= kTauMax; ++tau)
        {
            float sum = 0.0f;
            const float* a = x;
            const float* b = x + tau;
            for (int i = 0; i < kWindowSize; ++i)
            {
                const float d = a[i] - b[i];
                sum += d * d;
            }
            cmnd[(size_t) tau] = sum;
        }

        // ---- Step 2: cumulative mean normalised difference -----------
        // cmnd(tau) = d(tau) * tau / sum_{j=1..tau} d(j)
        float runningSum = 0.0f;
        for (int tau = 1; tau <= kTauMax; ++tau)
        {
            runningSum += cmnd[(size_t) tau];
            if (runningSum > 0.0f)
                cmnd[(size_t) tau] = cmnd[(size_t) tau] * (float) tau / runningSum;
            else
                cmnd[(size_t) tau] = 1.0f;
        }

        // ---- Step 3: candidate selection --------------------------
        //
        // Delegated to one of four mode-specific pickers. The plugin
        // ships Classic (v0.3.7 — YIN-paper-correct first-below-threshold
        // over the full hard range). The remaining modes are retained
        // only for offline A/B regression study of the v0.3.5 and
        // v0.3.6 failure modes — see PitchDetector.h for the
        // theoretical justification.
        int tauEstimate = -1;
        switch (detectorMode)
        {
            case DetectorMode::Classic:
                tauEstimate = pickClassic();
                break;
            case DetectorMode::FirstBelowVoiceRange:
                tauEstimate = pickFirstBelowVoiceRange();
                break;
            case DetectorMode::DeepestFullRange:
                tauEstimate = pickDeepestFullRange();
                break;
            case DetectorMode::DeepestVoiceRange:
                tauEstimate = pickDeepestVoiceRange();
                break;
        }

        // NOTE: a Boersma-style subharmonic rescue ("if tau*2 is also a
        // local min, prefer it") was attempted here in a draft of
        // v0.3.7 but regressed the Antares test set badly (mean |delta|
        // 8.25 → 39.91 cents). Reason: for a pure periodic signal with
        // period τ₀, tau*2 IS a real CMNDF minimum — the signal is
        // trivially periodic at 2×, 3×, … τ₀ too. Blindly promoting
        // to tau*2 reports frequency/2 for every clean harmonic signal,
        // producing an octave-down error. first-below-threshold
        // (pickClassic) already picks the true fundamental under normal
        // conditions; the anti-octave-flip guard below handles the
        // transient flip case. No additional rescue is needed.

        // Anti-octave-flip (only once we have a lock to compare to).
        // When our current tauEstimate is ~2× lastPeriod (octave down)
        // OR ~0.5× lastPeriod (octave up), check whether a candidate
        // near lastPeriod is ALSO a local minimum under threshold. If
        // so, stay in the same octave — the flip almost always means
        // YIN latched to a subharmonic on a consonant or noisy frame.
        if (tauEstimate > 0 && lastPeriod > 0.0f)
        {
            const float ratio = (float) tauEstimate / lastPeriod;
            const bool octDown = ratio > 1.87f && ratio < 2.13f;
            const bool octUp   = ratio > 0.47f && ratio < 0.535f;
            if (octDown || octUp)
            {
                const int near = juce::jlimit (kTauMin + 1, kTauMax - 1,
                                               (int) std::round (lastPeriod));
                // Search ±2 % window around lastPeriod for a local min
                const int span = juce::jmax (1, (int) std::round (0.02f * lastPeriod));
                int best = -1; float bestC = kThreshold;
                for (int tau = near - span; tau <= near + span; ++tau)
                {
                    if (tau <= kTauMin || tau >= kTauMax) continue;
                    const float c = cmnd[(size_t) tau];
                    if (c >= kThreshold) continue;
                    if (c > cmnd[(size_t) (tau - 1)]) continue;
                    if (c > cmnd[(size_t) (tau + 1)]) continue;
                    if (c < bestC) { bestC = c; best = tau; }
                }
                if (best > 0 && bestC < 1.5f * cmnd[(size_t) tauEstimate])
                    tauEstimate = best;
            }
        }

        if (tauEstimate < 0)
        {
            // Unvoiced — coast before flipping the flag so a brief
            // dropout (consonant, transient, one bad YIN frame)
            // doesn't reset the downstream PSOLA chain.
            //
            // v0.3.4: raised from 3 to 4 frames. At hop=256/48k
            // that's ~21 ms of coast. Bridges a typical consonant
            // (~15 ms) without losing the pitch correction thread.
            // 6 frames (32 ms) was too long — held voiced state
            // through actual silence gaps and caused OLA-overlap
            // clicks on the next onset.
            voicedStreak = 0;
            if (++unvoicedStreak >= 4)
            {
                voiced      = false;
                lastClarity = 0.0f;
            }
            return;
        }

        // ---- Step 4: parabolic interpolation -------------------------
        // v0.3.7: clamp offset to ±0.5 samples (parabolic is only
        // valid for small excursions around the discrete minimum)
        // and clamp final period to the hard detector range. Prior
        // versions could produce refinedTau outside [kTauMin, kTauMax]
        // when the CMNDF had a near-flat region near the boundary —
        // in offline detector-ab this appeared as min det_hz = 40 Hz
        // on a signal whose true fundamental was 108 Hz.
        float refinedTau = (float) tauEstimate;
        if (tauEstimate > 1 && tauEstimate < kTauMax)
        {
            const float s0 = cmnd[(size_t) (tauEstimate - 1)];
            const float s1 = cmnd[(size_t) tauEstimate];
            const float s2 = cmnd[(size_t) (tauEstimate + 1)];
            const float denom = (s0 + s2 - 2.0f * s1);
            if (std::abs(denom) > 1.0e-6f)
            {
                float offset = 0.5f * (s0 - s2) / denom;
                offset = juce::jlimit(-0.5f, 0.5f, offset);
                refinedTau += offset;
            }
        }

        refinedTau = juce::jlimit((float) kTauMinHard, (float) kTauMaxHard, refinedTau);
        lastPeriod  = refinedTau;
        lastHz      = (float) (sr / (double) refinedTau);
        lastClarity = juce::jlimit(0.0f, 1.0f, 1.0f - cmnd[(size_t) tauEstimate]);

        // v0.3.4: clarity threshold lowered from 0.5 to 0.35 for more
        // sensitive onset. voicedStreak kept at 2 (not 1) because
        // single-frame onset caused excessive PSOLA chain resets
        // which produced clicks (9 at bs=192 vs 1 with voicedStreak=2).
        // The 2-frame requirement still gives fast onset (~10 ms)
        // while filtering out single-frame false positives from
        // noise/consonants.
        //
        // v0.3.5: transient guard (Gate 2 — decision).
        //
        //   transient  ⇔  ZCR > 0.20  OR  crest > 8.0
        //
        // Calibrated against `transient-stress` (10 s of vocal-like
        // 200 Hz + 30 ms occlusive noise bursts every 500 ms):
        //   clean-block ZCR: avg 0.04, max 0.46 (max is Burst-tail
        //                    bleeding into the 1024-sample analysis
        //                    window, which we also want to reject)
        //   burst-block ZCR: avg 0.34, max 0.53
        //
        // A ZCR-alone rule is robust because no voiced material in
        // the supported range (100 Hz..1500 Hz fundamental, up to
        // ~8 harmonics) can produce ZCR > 0.20 on a 1024-sample
        // window. We deliberately do NOT AND this with clarity —
        // a noise burst that happens to have a concurrent periodic
        // component (common: a /p/ plosive during sustained vowel)
        // still has clarity > 0.35 because YIN locks to the vowel
        // fundamental, and a clarity-conditional gate would wrongly
        // let the burst through (measured 85 % miss rate).
        // Crest > 8 is a secondary catch for impulsive transients
        // (mic bumps, pops) that don't shift ZCR enough — currently
        // unseen in the calibration set but cheap to keep as safety.
        const bool transientDetected =
              windowZcr   > 0.20f
           || windowCrest > 8.0f;

        unvoicedStreak = 0;
        if (lastClarity > 0.35f && ! transientDetected)
        {
            if (++voicedStreak >= 2)
                voiced = true;
        }
        else
        {
            voicedStreak = 0;
            voiced       = false;
        }
    }

    // ------------------------------------------------------------------
    //  Candidate-selection implementations.
    //
    //  All four share the same contract: scan cmnd[] for a τ that is a
    //  strict local minimum AND satisfies cmnd[τ] < kThreshold. Return
    //  the chosen τ, or -1 if no candidate exists under threshold.
    //  Never return τ at the boundary — local-min test requires both
    //  neighbours to exist.
    //
    //  Classic (default) implements the YIN paper's Step 4 verbatim:
    //  the FIRST local minimum under threshold wins, "walk-down"
    //  refines to the tal floor. Scanned over the FULL hard range so
    //  the voice-type prior does not silently gate the signal. This
    //  is the theoretically correct choice for harmonic signals — the
    //  CMNDF's τ/Σd(j) normaliser artificially deepens higher-τ
    //  (subharmonic) minima, so any "deepest" strategy systematically
    //  prefers harmonics. First-below-threshold breaks before those
    //  later minima come into view.
    // ------------------------------------------------------------------
    int PitchDetector::pickClassic() const noexcept
    {
        // v0.3.7: two-pass first-below-threshold.
        //
        // Pass 1: voice-type range (the user's selected Voice Type —
        // Soprano, Alto/Tenor, Low Male, Instrument, Bass Inst). This
        // matches v0.3.4/v0.3.5 behaviour for the common case where
        // the user has selected the right type for their voice. Keeps
        // voicing-% at the v0.3.4 level (~20 % on alto_tenor, ~45 %
        // on low_male) so PSOLA only engages where the user expects it.
        //
        // Pass 2: full hard range fallback (kTauMinHard..kTauMaxHard).
        // Only runs if Pass 1 found no candidate — this handles the
        // v0.3.5 bug where a user on Soprano preset with a Bass vocal
        // got silently bypassed, and the v0.3.4 bug where a user on
        // Low Male with a 108 Hz fundamental got a harmonic lock.
        // Scans widely but, since it's only invoked when voice range
        // is empty, doesn't over-voice on in-range signals.
        //
        // Per YIN paper Step 4: first-below-threshold with walk-down
        // to the local minimum. Mathematically robust against
        // harmonic CMNDF inflation at higher τ.
        const int vlo = juce::jmax (kTauMin + 1, voiceTauMin);
        const int vhi = juce::jmin (kTauMax - 1, voiceTauMax);
        for (int tau = vlo; tau <= vhi; ++tau)
        {
            if (cmnd[(size_t) tau] < kThreshold)
            {
                int t = tau;
                while (t + 1 <= vhi
                       && cmnd[(size_t) (t + 1)] < cmnd[(size_t) t])
                    ++t;
                return t;
            }
        }
        for (int tau = kTauMin + 1; tau < kTauMax; ++tau)
        {
            if (cmnd[(size_t) tau] < kThreshold)
            {
                int t = tau;
                while (t + 1 < kTauMax
                       && cmnd[(size_t) (t + 1)] < cmnd[(size_t) t])
                    ++t;
                return t;
            }
        }
        return -1;
    }

    // v0.3.5 behaviour — first-below-threshold, but search clamped to
    // the voice-type range. Failure mode: when the voice-type range
    // contains both the fundamental period AND its 2nd/3rd harmonic
    // (e.g. 108 Hz vocal in Low Male range 100-550 Hz), the harmonic
    // is reached first in the τ-scan and the detector locks onto it.
    int PitchDetector::pickFirstBelowVoiceRange() const noexcept
    {
        const int lo = juce::jmax (kTauMin + 1, voiceTauMin);
        const int hi = juce::jmin (kTauMax - 1, voiceTauMax);
        for (int tau = lo; tau <= hi; ++tau)
        {
            if (cmnd[(size_t) tau] < kThreshold)
            {
                int t = tau;
                while (t + 1 <= hi
                       && cmnd[(size_t) (t + 1)] < cmnd[(size_t) t])
                    ++t;
                return t;
            }
        }
        return -1;
    }

    // v0.3.6 behaviour — deepest local minimum under threshold.
    // Two-pass: voice range first, full range fallback. Known to pick
    // harmonics whenever the CMNDF normaliser makes them artificially
    // deeper than the fundamental dip. Retained for regression study.
    int PitchDetector::pickDeepestFullRange() const noexcept
    {
        int   tauEstimate = -1;
        float deepestCmnd = kThreshold;
        for (int tau = voiceTauMin + 1; tau <= voiceTauMax - 1; ++tau)
        {
            const float c = cmnd[(size_t) tau];
            if (c >= kThreshold) continue;
            if (c > cmnd[(size_t) (tau - 1)]) continue;
            if (c > cmnd[(size_t) (tau + 1)]) continue;
            if (c < deepestCmnd) { deepestCmnd = c; tauEstimate = tau; }
        }
        if (tauEstimate < 0)
        {
            for (int tau = kTauMin + 1; tau <= kTauMax - 1; ++tau)
            {
                const float c = cmnd[(size_t) tau];
                if (c >= kThreshold) continue;
                if (c > cmnd[(size_t) (tau - 1)]) continue;
                if (c > cmnd[(size_t) (tau + 1)]) continue;
                if (c < deepestCmnd) { deepestCmnd = c; tauEstimate = tau; }
            }
        }
        return tauEstimate;
    }

    // v0.3.6 draft — deepest-min, voice range only (no fallback).
    int PitchDetector::pickDeepestVoiceRange() const noexcept
    {
        int   tauEstimate = -1;
        float deepestCmnd = kThreshold;
        const int lo = juce::jmax (kTauMin + 1, voiceTauMin + 1);
        const int hi = juce::jmin (kTauMax - 1, voiceTauMax - 1);
        for (int tau = lo; tau <= hi; ++tau)
        {
            const float c = cmnd[(size_t) tau];
            if (c >= kThreshold) continue;
            if (c > cmnd[(size_t) (tau - 1)]) continue;
            if (c > cmnd[(size_t) (tau + 1)]) continue;
            if (c < deepestCmnd) { deepestCmnd = c; tauEstimate = tau; }
        }
        return tauEstimate;
    }
}
