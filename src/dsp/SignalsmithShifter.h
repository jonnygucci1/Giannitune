#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>

namespace signalsmith { namespace stretch {
    template<typename, class> struct SignalsmithStretch;
}}

namespace gianni
{
    // ----------------------------------------------------------------------
    //  SignalsmithShifter — wrapper around the Signalsmith Stretch
    //  phase-vocoder library, adapted for live-voice pitch correction.
    //
    //  Responsibilities on top of the raw library:
    //    - a voicing gate with a hold window and ratio smoother, so
    //      transient YIN voicing flickers don't cause audible pitch
    //      discontinuities on the output
    //    - chunked processing aligned to the library's STFT interval,
    //      so each analysis fire reads a single stable ratio rather
    //      than one that changes mid-frame
    //    - an in-place process() API compatible with JUCE AudioBuffer
    //      (the library itself requires non-aliased I/O; we handle the
    //      copy to a separate work buffer internally)
    //
    //  Default configuration: block ≈ 21 ms, interval ≈ 5 ms. Input
    //  and output buffers cannot be the same (library requirement).
    // ----------------------------------------------------------------------
    class SignalsmithShifter
    {
    public:
        SignalsmithShifter();
        ~SignalsmithShifter();

        // Non-copyable, non-movable (holds a unique_ptr to the library
        // engine and sized work buffers).
        SignalsmithShifter(const SignalsmithShifter&) = delete;
        SignalsmithShifter& operator= (const SignalsmithShifter&) = delete;

        void prepare (double sampleRate, int maxBlockSize, int numChannels);

        // Lower-level variant — lets callers pick block / interval /
        // formant-compensation explicitly. Useful for benchmarking or
        // non-default deployments. prepare() calls this with sensible
        // defaults.
        void prepareCustom (double sampleRate, int maxBlockSize, int numChannels,
                            int customBlockSamples, int customIntervalSamples,
                            bool formantCompensate);

        void reset();

        // Pitch-correction API. inputPeriod comes from the YIN
        // detector, targetPeriod from the quantizer. The actual ratio
        // passed to the library is inputPeriod / targetPeriod.
        void setInputPeriodSamples  (float periodSamples) noexcept;
        void setTargetPeriodSamples (float periodSamples) noexcept;
        void setVoiced              (bool v) noexcept;
        void setBypass              (bool b) noexcept;

        // tonalityLimit: if > 0, the frequency map is non-linear above
        // this limit (normalised 0..0.5 as fraction of sample-rate) so
        // formants above it stay roughly in place. Typical: 0.17 (≈ 8 kHz
        // at 48 k) for vocal formant preservation. 0 disables.
        void setTonalityLimit (float hzNormalised) noexcept;

        // Must be called BEFORE prepare(). On: distributes STFT
        // processing across each interval (lower peak CPU, one extra
        // interval of latency). Off: all work at the start of each
        // interval, lower latency.
        void setSplitComputation (bool on) noexcept;

        void process (juce::AudioBuffer<float>& buffer) noexcept;

        int  getLatencySamples() const noexcept { return reportedLatency; }

        // Last transpose factor passed to the library. Equals
        // targetHz/detectedHz during voiced frames, 1.0 otherwise.
        // Useful for UI meters / diagnostics.
        float getLastAppliedRatio() const noexcept { return lastAppliedRatio; }

    private:
        // PIMPL keeps the heavy Signalsmith template instantiation out
        // of every TU that just wants a forward declaration.
        struct Impl;
        std::unique_ptr<Impl> impl;

        double sr              { 48000.0 };
        int    channels        { 2 };
        int    reportedLatency { 0 };
        float  inputPeriod     { 1.0f };
        float  targetPeriod    { 1.0f };
        bool   voiced          { false };
        bool   bypass          { false };
        bool   splitCompute    { false };
        float  tonalityLimit   { 0.17f };   // ~8 kHz at 48 k, vocal-friendly
        float  lastAppliedRatio { 1.0f };

        // Runtime-configured chunk size. Matches intervalSamples after
        // prepare() so each library STFT fire reads one stable ratio.
        static constexpr int kDefaultChunkSamples = 256;
        int   chunkSamples { kDefaultChunkSamples };
        float ratioPrev { 1.0f };

        // Voicing-transition smoother state. See comment in process().
        float unvoicedMs { 0.0f };
        static constexpr float kUnvoicedHoldMs = 75.0f;
    };
}
