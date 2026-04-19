#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace gianni
{
    // ----------------------------------------------------------------------
    //  PitchDetector — YIN-based monophonic F0 estimator.
    //
    //  Reference: de Cheveigné & Kawahara, "YIN, a fundamental frequency
    //  estimator for speech and music" (J. Acoust. Soc. Am. 2002).
    //
    //  Streaming usage:
    //      detector.prepare(sr, maxBlock);
    //      // each block:
    //      detector.push(monoIn, n);
    //      float hz   = detector.getPitchHz();
    //      float per  = detector.getPeriodSamples();
    //      bool voiced = detector.isVoiced();
    //
    //  The detector keeps an internal ring buffer of recent input and
    //  re-analyses every `hopSize` samples. All work is allocation-free
    //  and realtime-safe after prepare().
    //
    //  Latency component: ~ analysisWindowSize + tauMax samples need to
    //  be present before the FIRST estimate is valid. After that, each
    //  estimate reflects the most recent analysis window.
    // ----------------------------------------------------------------------
    class PitchDetector
    {
    public:
        // v0.3.7: candidate-selection mode.
        //
        // Classic (default) implements YIN Step 4 per the original paper:
        // "The first local minimum of d' that passes below an absolute
        //  threshold t is selected." Scanned over the FULL hard range
        // (kTauMinHard..kTauMaxHard) so voice-type selection is a hint,
        // not a hard gate. This is the theoretically correct choice for
        // harmonic-rich signals: the CMNDF normalisation τ/Σd(j) makes
        // higher-τ minima (subharmonics 2τ₀, 3τ₀, …) artificially deep,
        // so any "deepest-min" heuristic systematically picks harmonics.
        //
        // The other modes are retained for offline A/B analysis in
        // dsp_test — they reproduce the v0.3.5 and v0.3.6 behaviours
        // for regression study. Plugin ships Classic.
        enum class DetectorMode
        {
            Classic,                // v0.3.7 default: first-below, full hard range
            FirstBelowVoiceRange,   // v0.3.5 behaviour: first-below, voice range only
            DeepestFullRange,       // v0.3.6 behaviour: deepest-min, voice→full fallback
            DeepestVoiceRange       // v0.3.6 draft: deepest-min, voice range only
        };

        PitchDetector();

        void prepare (double sampleRate, int maxBlockSize);
        void reset();

        // Offline/A-B only. Default = Classic.
        void setDetectorMode (DetectorMode m) noexcept { detectorMode = m; }
        DetectorMode getDetectorMode() const noexcept  { return detectorMode; }

        // Push a mono block. Triggers re-analysis when enough new samples
        // have arrived (every hopSize). Realtime-safe.
        void push (const float* mono, int numSamples) noexcept;

        // Set the expected pitch range in Hz. YIN's tau search is
        // constrained to the corresponding sample-period range, which
        // both improves detector accuracy (ignores out-of-range
        // candidates) and rejects octave errors. Voice-type-aware;
        // called from PluginProcessor when the voiceType parameter
        // changes. Values are clamped to the detector's hard limits.
        void setPitchRange (float minHz, float maxHz) noexcept;

        float getPitchHz()        const noexcept { return lastHz; }
        float getPeriodSamples()  const noexcept { return lastPeriod; }
        float getClarity()        const noexcept { return lastClarity; }
        bool  isVoiced()          const noexcept { return voiced; }

    private:
        // --- analysis configuration ---
        // 1024-sample analysis window @48k = ~21ms.
        // kTauMaxHard = 480 → fMin floor ≈ 100 Hz @48k (hard limit).
        // kTauMinHard = 16  → fMax ceiling ≈ 3000 Hz @48k (whistle).
        //
        // These are the HARD limits of what the detector can search.
        // At runtime, voiceTauMin/voiceTauMax (see below) narrow the
        // candidate-selection window based on the voice type the user
        // selected; the CMNDF itself is computed over the full hard
        // range so out-of-range candidates stay available as fallback
        // for the Classic / DeepestFullRange modes.
        static constexpr int kWindowSize = 1024;
        static constexpr int kTauMaxHard = 480;
        static constexpr int kTauMinHard = 16;
        static constexpr int kHopSize    = 256;     // re-analyse every ~5ms@48k
        static constexpr int kBufSize    = 2048;    // > kWindowSize + kTauMaxHard
        // v0.3.4: lowered from 0.12 to 0.08. A/B analysis of
        // Giannitune vs Antares renders showed Antares detects 6698
        // voiced frames vs our 4002 on the same vocal — we're missing
        // 40% of the content that Antares pitch-corrects. The 0.12
        // threshold was too conservative for real vocals with
        // breathiness and background bleed. 0.08 accepts weaker
        // pitch candidates as voiced, matching Antares' sensitivity.
        static constexpr float kThreshold = 0.08f;

        // v0.3.6/v0.3.7: voice-type is a SOFT prior, not a hard cutoff.
        // The CMNDF runs across the FULL detector range (kTauMinHard
        // .. kTauMaxHard, 100..3000 Hz). The voice-type range below
        // is used only to bias candidate selection.
        static constexpr int kTauMin { kTauMinHard };
        static constexpr int kTauMax { kTauMaxHard };
        // Defaults match Alto/Tenor (130..900 Hz @ 48 kHz).
        int voiceTauMin { 53 };   // 48000 / 900  ≈ 53 samples
        int voiceTauMax { 370 };  // 48000 / 130  ≈ 369 samples

        std::array<float, kBufSize>                    ring  {};
        std::array<float, kTauMaxHard + 1>             cmnd  {};   // CMNDF scratch
        std::array<float, kWindowSize + kTauMaxHard>   frame {};
        int  writeIdx           { 0 };
        int  samplesSinceAnalyse{ 0 };

        // Total samples pushed since prepare(). YIN's result is only
        // trustworthy once we have ≥ kWindowSize + kTauMaxHard samples
        // in the ring — otherwise the difference function operates
        // partly on initialisation zeros and latches onto subharmonics
        // (the classic "YIN octave error" at startup).
        int64_t samplesPushed   { 0 };

        // Hysteresis: require a few consecutive valid detections before
        // flipping to voiced, and allow a short coast before flipping
        // back to unvoiced. Stops voicing flicker on breathy sections.
        int  voicedStreak       { 0 };
        int  unvoicedStreak     { 0 };

        DetectorMode detectorMode { DetectorMode::Classic };

        double sr               { 48000.0 };
        float  lastHz           { 0.0f };
        float  lastPeriod       { 0.0f };
        float  lastClarity      { 0.0f };
        bool   voiced           { false };

        void runYin() noexcept;

        // Copy analysis window out of the ring buffer linearly so YIN can
        // index x[i] / x[i+tau] without wrap-around.
        void copyAnalysisFrame (float* dest) const noexcept;

        // Candidate-selection helpers (one per mode).
        int pickClassic()              const noexcept; // first-below, full range
        int pickFirstBelowVoiceRange() const noexcept; // v0.3.5
        int pickDeepestFullRange()     const noexcept; // v0.3.6
        int pickDeepestVoiceRange()    const noexcept; // v0.3.6 draft
    };
}
