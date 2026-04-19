#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

namespace gianni
{
    // ----------------------------------------------------------------------
    //  PsolaShifter — Time-Domain Pitch Synchronous Overlap Add with
    //  real pitch-mark detection.
    //
    //  This is what Antares Auto-Tune actually uses under the hood (see
    //  Moulines & Charpentier 1990, and Hildebrand's Auto-Tune patents).
    //  The previous delay-line shifter could move pitch smoothly but
    //  could never *lock* to a target frequency cycle-by-cycle, which is
    //  precisely what gives Auto-Tune its character. A delay-line at
    //  retune speed 0 still sounds like a slightly-mechanical time-warp
    //  because it doesn't know where the actual vocal cycles are.
    //
    //  PSOLA fixes that by:
    //    1. Detecting real glottal pulses (pitch marks / epochs) in the
    //       input waveform.
    //    2. Extracting a 2-period Hann-windowed grain around each mark.
    //    3. Placing those grains in the output at the *target* period
    //       spacing. For retune speed 0 the target period is exactly
    //       sr / snapped_hz — the output is locked to the scale note
    //       from the first cycle.
    //
    //  Key insight from the v0.1.4 failure: using a *synthetic* grid
    //  (k * inputPeriod) for grain centres produces clicks on real
    //  vocals because the grid drifts out of phase with the actual
    //  cycles. You have to search for the real peak within each
    //  predicted period window.
    //
    //  Epoch detection
    //  ---------------
    //  Within each predicted period window, search for the sample with
    //  the maximum absolute value — that's approximately the glottal
    //  closure instant. Crude but robust for well-behaved vocals.
    //  Future work could swap in LPC-residual peak picking for noisier
    //  input.
    //
    //  Latency
    //  -------
    //  v0.2.7 reduces reported plugin latency from 1024 (21.33 ms
    //  @ 48 k) to **864 samples (18.00 ms)** — a 15.6 % reduction.
    //  The figure was established by an exhaustive empirical sweep
    //  on the user's own real vocal (see
    //  measurements/v0.2.7_latency_sweep.txt).
    //
    //  Sweep method: for each candidate latency in {512..1152},
    //  run click-hunt at block sizes {64, 128, 192, 256, 512, 1024}
    //  and count DSP-introduced clicks (input-side transient
    //  passthrough events filtered out). The click-free minimum
    //  across ALL block sizes is 864. Below 864 the small-block
    //  configs start clicking in the low-male voiced range; above
    //  864 there is no additional cleanliness but every sample costs
    //  latency.
    //
    //  At the user's REAPER setup (ASIO 192) this drops live-
    //  monitoring round-trip from ~33 ms to ~26 ms, moving toward
    //  the psychoacoustic "barely perceptible" threshold around
    //  20 ms.
    //
    //  runtimeLatency is a variable (not a constant) so hosts that
    //  use very large block sizes can be given more headroom if we
    //  ever find that 864 is too tight for them. Currently it's set
    //  to 864 unconditionally because the sweep showed that value
    //  works across the entire block-size range we tested.
    //
    //  getLatencySamples() returns runtimeLatency so setLatencySamples
    //  reports the correct value to the host for PDC.
    // ----------------------------------------------------------------------
    class PsolaShifter
    {
    public:
        PsolaShifter();
        ~PsolaShifter();

        void prepare (double sampleRate, int maxBlockSize, int numChannels);
        void reset();

        // v0.3.1: tell the shifter the maximum period we ever expect
        // the pitch detector to report. This is set based on the
        // user's voice type (narrower voice = shorter max period =
        // lower kLatency). MUST be called BEFORE prepare() because
        // prepare() computes runtimeLatency from it. If not called,
        // defaults to kMaxPeriod which gives the v0.2.9 latency.
        void setMaxExpectedPeriod (int samples) noexcept;

        // v0.3.3: Live Monitor grain mode. When set to a value < 1.0,
        // grains are shortened and double-placed to maintain 50% Hann
        // overlap at lower latency. Typical value: 0.5 (half-length
        // grains, 2× placement rate). MUST be called BEFORE prepare().
        void setGrainFraction (float fraction) noexcept;

        // v0.3.8 experiment: cap the grain half-length in samples. A
        // value of 0 disables the cap (= current behaviour, half =
        // inputPeriod). Low-frequency vocals (period > 300 samples @
        // 44.1k) produce 2*period = 600+ sample grains that are
        // susceptible to OLA under-run clicks at scale-note snap
        // transitions. Shortening them sacrifices some harmonic
        // fidelity but dramatically reduces click density.
        void setGrainHalfCap (int halfSamples) noexcept;

        // v0.3.8: period-smoother time constant, in multiples of the
        // current inputPeriod. A value of N means the smoother covers
        // N periods of the input fundamental, so smoothing strength
        // is pitch-invariant. Default 0 = legacy fixed 1 ms tau
        // (ineffective at low pitches because alpha ≈ 1.0 when
        // grain-spacing >> tau).
        void setPeriodSmootherPeriods (float periods) noexcept;

        void setInputPeriodSamples  (float periodSamples) noexcept;
        void setTargetPeriodSamples (float periodSamples) noexcept;
        void setVoiced              (bool v) noexcept;
        void setBypass              (bool b) noexcept;

        void process (juce::AudioBuffer<float>& buffer) noexcept;
        int  getLatencySamples() const noexcept { return runtimeLatency; }

    private:
        static constexpr int kRingSize    = 16384;
        static constexpr int kMaxChannels = 2;
        // kMaxPeriod = 1024 → lowest f0 = 47 Hz @ 48 k. Comfortably
        // covers every vocal range. See the latency block comment
        // above for why we can't shrink this without rewriting the
        // grain-placement loop.
        static constexpr int kMaxPeriod   = 1024;
        // Empirical click-free latency (2026-04-11 sweep on user's
        // real vocal). See the "Latency" block above and
        // measurements/v0.2.7_latency_sweep.txt. Do not lower without
        // re-running the full click-hunt suite at all tested block
        // sizes — below 864 the low-male voiced range starts clicking.
        static constexpr int kEmpiricalLatency = 864;
        static constexpr int kMaxEpochs        = 128;

        struct Epoch
        {
            int64_t absPos;        // absolute sample index in the input stream
            float   period;        // local period at this epoch
        };

        struct Channel
        {
            std::array<float, kRingSize> in      {};
            std::array<float, kRingSize> outAcc  {};
            std::array<float, kRingSize> outNorm {};
        };

        std::array<Channel, kMaxChannels> channels {};
        int    numChannels { 2 };
        double sr          { 48000.0 };

        // runtimeLatency is set in prepare(). Default matches the
        // empirical click-free value so pre-prepare queries (like
        // getLatencySamples) return a sane number.
        int     runtimeLatency { kEmpiricalLatency };
        // Voice-type-aware maximum period. Set from PluginProcessor
        // based on the Voice Type dropdown BEFORE prepare() is
        // called. Lets the shifter compute a tighter kLatency for
        // narrower voice ranges. Default = kMaxPeriod (full range,
        // v0.2.9 behaviour).
        int     voiceMaxPeriod { kMaxPeriod };

        // Grain fraction for Live Monitor mode. 1.0 = full 2*period
        // grain (Studio quality). 0.5 = period-length grain with
        // double placement (Live Monitor, lower latency). Set from
        // PluginProcessor before prepare().
        float   grainFrac { 1.0f };

        // v0.3.8 production defaults: 128-sample grain-half cap
        // (2.9 ms grains) + 5-period pitch-invariant smoother.
        // See PluginProcessor v0.3.8 comment for measurement data
        // and rationale. Test fixtures that need v0.3.7 "classic"
        // PSOLA behaviour call setGrainHalfCap(0) and
        // setPeriodSmootherPeriods(0.0f) before prepare().
        int     grainHalfCap           { 128 };
        float   periodSmootherPeriods  { 5.0f };
        int64_t inWriteAbs { 0 };
        int64_t outReadAbs { -(int64_t) kEmpiricalLatency };
        // Fractional accumulator so the integer epoch positions don't
        // drift relative to the real cycles over time.
        double  nextEpochAbs { -1.0 };
        double  nextOutMarkAbs { 0.0 };

        // Circular buffer of recent epochs (oldest at head)
        std::array<Epoch, kMaxEpochs> epochs {};
        int epochHead  { 0 };
        int epochCount { 0 };

        // The quantizer upstream is the only "musical" smoother
        // (Antares-calibrated 5-144 ms curve). But the quantizer
        // updates once per block, so at larger block sizes the
        // targetPeriod takes discrete steps of up to tens of samples
        // per block. Those discrete jumps made bs 512 click in
        // low-pitched voiced regions. We track a separately-smoothed
        // periodSmoothed that moves toward targetPeriod with a very
        // small (~1 ms) time constant, used only for grain spacing
        // within the synthesis loop. This glues adjacent-block grain
        // sequences together without affecting the perceived retune
        // curve at all (1 ms is well below the 5 ms floor).
        float inputPeriod     { 480.0f };   // from detector
        float targetPeriod    { 480.0f };   // from quantizer, block-constant
        float periodSmoothed  { 480.0f };   // used for grain spacing
        bool  voiced       { false };
        bool  prevVoiced   { false };
        bool  bypass       { false };

        // ----- implementation ----------------------------------------
        void detectEpochs() noexcept;
        void placeGrain  (int64_t inputCenterAbs,
                          double  outputCenterAbs,
                          int     half) noexcept;

        void pushEpoch (const Epoch& e) noexcept;
        const Epoch* findClosestEpoch (double absPos) const noexcept;
        void pruneOldEpochs (int64_t minAbs) noexcept;
    };
}
