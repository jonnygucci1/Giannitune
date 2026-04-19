#include "SignalsmithShifter.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace gianni
{
    // ------------------------------------------------------------------
    //  PIMPL: owns the templated Signalsmith engine + work buffers.
    //
    //  The library's .process() API takes input and output as buffer-
    //  of-pointers (e.g. `float * const *`), and the two buffers must
    //  not alias. JUCE's AudioBuffer<float> gives us
    //  getArrayOfReadPointers / WritePointers — we use those plus a
    //  separately-owned `work` buffer to satisfy the no-alias rule.
    // ------------------------------------------------------------------
    struct SignalsmithShifter::Impl
    {
        signalsmith::stretch::SignalsmithStretch<float, std::default_random_engine> engine;
        juce::AudioBuffer<float> workIn;
        juce::AudioBuffer<float> workOut;
    };

    SignalsmithShifter::SignalsmithShifter()
        : impl(std::make_unique<Impl>()) {}
    SignalsmithShifter::~SignalsmithShifter() = default;

    void SignalsmithShifter::prepareCustom (double sampleRate, int maxBlockSize,
                                             int numChannels,
                                             int customBlockSamples,
                                             int customIntervalSamples,
                                             bool formantCompensate)
    {
        sr       = sampleRate;
        channels = juce::jlimit(1, 2, numChannels);

        const int blockSamples    = juce::jmax(128, customBlockSamples);
        const int intervalSamples = juce::jmax(32,  customIntervalSamples);
        chunkSamples              = intervalSamples;   // keep chunks aligned to STFT fires

        impl->engine.configure(channels, blockSamples, intervalSamples, splitCompute);

        // Formant handling. When `compensatePitch` is true the library
        // holds the spectral envelope stationary while the fundamental
        // shifts — useful for large corrections. Off by default.
        impl->engine.setFormantFactor(1.0f, /*compensatePitch*/ formantCompensate);
        impl->engine.setFormantBase(0.0f);   // auto-detect base frequency

        impl->workIn.setSize(channels, juce::jmax(maxBlockSize, blockSamples),
                             false, true, false);
        impl->workOut.setSize(channels, juce::jmax(maxBlockSize, blockSamples),
                              false, true, false);

        reportedLatency = impl->engine.inputLatency() + impl->engine.outputLatency();

        reset();
    }

    void SignalsmithShifter::prepare (double sampleRate, int maxBlockSize, int numChannels)
    {
        // Default configuration: block ≈ 21 ms, interval ≈ 5 ms. At
        // 44.1 k this is 1024 / 256, at 48 k roughly the same. Chosen
        // empirically for balance of spectral resolution vs latency.
        const int defaultBlock    = juce::jmax(256,
            (int) std::round(sampleRate * 0.021));
        const int defaultInterval = juce::jmax(64,
            (int) std::round(sampleRate * 0.005));

        prepareCustom(sampleRate, maxBlockSize, numChannels,
                      defaultBlock, defaultInterval, /*formantCompensate*/ false);
    }

    void SignalsmithShifter::reset()
    {
        impl->engine.reset();
        impl->workIn.clear();
        impl->workOut.clear();
        inputPeriod  = 1.0f;
        targetPeriod = 1.0f;
        voiced       = false;
        bypass       = false;
        ratioPrev    = 1.0f;
        unvoicedMs   = 0.0f;
    }

    void SignalsmithShifter::setInputPeriodSamples (float p) noexcept
    {
        inputPeriod = juce::jlimit(20.0f, 1024.0f, p);
    }

    void SignalsmithShifter::setTargetPeriodSamples (float p) noexcept
    {
        targetPeriod = juce::jlimit(20.0f, 1024.0f, p);
    }

    void SignalsmithShifter::setVoiced (bool v) noexcept { voiced = v; }
    void SignalsmithShifter::setBypass (bool b) noexcept { bypass = b; }

    void SignalsmithShifter::setTonalityLimit (float hzNorm) noexcept
    {
        tonalityLimit = juce::jlimit(0.0f, 0.5f, hzNorm);
    }

    void SignalsmithShifter::setSplitComputation (bool on) noexcept
    {
        // Must be called before prepare(); takes effect on next configure.
        splitCompute = on;
    }

    void SignalsmithShifter::process (juce::AudioBuffer<float>& buffer) noexcept
    {
        const int n = buffer.getNumSamples();
        const int nc = juce::jmin(buffer.getNumChannels(), channels);
        if (n <= 0 || nc <= 0) return;

        if (bypass)
        {
            // No-op pass-through (latency is reported to host so the DAW
            // delay-aligns the dry/wet). We still pump an equal number
            // of samples through so the output keeps rolling if the
            // user toggles bypass on/off mid-playback.
            return;
        }

        // -------------------------------------------------------------
        //  Ratio smoothing across voicing transitions.
        //
        //  The raw correction ratio is targetHz / detectedHz while the
        //  voice is active, and 1.0 (unity) otherwise. Switching those
        //  two states in a single block produces an instant pitch
        //  discontinuity, which is audible as a pop or warble on the
        //  output. We apply a one-pole smoother (τ ≈ 80 ms) on the
        //  ratio so transitions become short fades rather than hard
        //  steps, and add a brief "HOLD" window during which a
        //  momentary unvoiced reading (soft consonants, breath, detector
        //  uncertainty) does not move the ratio at all — the shifter
        //  behaves as if the voice were still there. Past the hold
        //  window, the smoother fades ratio → 1.0 at the natural τ.
        // -------------------------------------------------------------
        static constexpr float kVoicingRatioTauSec = 0.080f;

        const float rawRatio = (voiced && inputPeriod > 0.0f)
            ? juce::jlimit(0.5f, 2.0f, inputPeriod / targetPeriod)
            : 1.0f;

        const float blockSec = (float) n / (float) sr;
        const float blockMs  = blockSec * 1000.0f;
        if (voiced)
            unvoicedMs = 0.0f;
        else
            unvoicedMs += blockMs;

        float smootherTarget;
        if (voiced)
            smootherTarget = rawRatio;
        else if (unvoicedMs < kUnvoicedHoldMs)
            smootherTarget = ratioPrev;    // hold: no change
        else
            smootherTarget = 1.0f;         // long silence: fade to unity

        const float smootherAlpha = 1.0f - std::exp(-blockSec / kVoicingRatioTauSec);
        const float ratioTarget   = ratioPrev
            + smootherAlpha * (smootherTarget - ratioPrev);

        // Copy input into work buffer (library requires non-aliased IO).
        for (int ch = 0; ch < nc; ++ch)
            impl->workIn.copyFrom(ch, 0, buffer, ch, 0, n);
        for (int ch = nc; ch < channels; ++ch)
            impl->workIn.clear(ch, 0, n);

        // -------------------------------------------------------------
        //  Chunked sub-block processing.
        //
        //  The Signalsmith engine runs its STFT analysis every
        //  `intervalSamples`. We align our chunk boundary to that
        //  interval and set the transpose factor once per chunk, so
        //  the engine sees a single stable ratio per analysis — no
        //  mid-frame changes that would confuse peak tracking.
        // -------------------------------------------------------------
        std::array<const float*, 2> chunkIn  { nullptr, nullptr };
        std::array<float*,       2> chunkOut { nullptr, nullptr };

        for (int start = 0; start < n; start += chunkSamples)
        {
            const int len = std::min(chunkSamples, n - start);
            const float t = (static_cast<float>(start) + len * 0.5f)
                          / static_cast<float>(n);
            const float ratio = ratioPrev + t * (ratioTarget - ratioPrev);

            impl->engine.setTransposeFactor(ratio, tonalityLimit);

            for (int ch = 0; ch < channels; ++ch)
            {
                chunkIn [ch] = impl->workIn .getReadPointer  (ch) + start;
                chunkOut[ch] = impl->workOut.getWritePointer (ch) + start;
            }
            impl->engine.process(chunkIn.data(), len,
                                 chunkOut.data(), len);
        }

        ratioPrev        = ratioTarget;
        lastAppliedRatio = ratioTarget;

        // Copy engine output back to the in-place buffer.
        for (int ch = 0; ch < nc; ++ch)
            buffer.copyFrom(ch, 0, impl->workOut, ch, 0, n);
    }
}
