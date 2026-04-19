#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

namespace gianni
{
    // ----------------------------------------------------------------------
    //  PitchShifter — Delay-line (Dattorro / two-tap crossfade) pitch shifter.
    //
    //  Why not TD-PSOLA
    //  ----------------
    //  Classical TD-PSOLA places grains on real input pitch marks
    //  (glottal closure instants). Without a real pitch-mark detector,
    //  using a regular period grid drifts out of phase with the actual
    //  signal on anything harmonically rich (= every real vocal), which
    //  produces a click at every grain boundary. Implementing proper
    //  GCI detection is a multi-day job and still fragile.
    //
    //  The delay-line approach sidesteps the problem entirely: it doesn't
    //  look at the input's phase at all. It keeps a ring of recent input
    //  samples and reads from *two* positions ("taps") that sweep through
    //  the buffer at a rate equal to the pitch-shift ratio. A Hann
    //  crossfade between the two taps hides the discontinuity when a tap
    //  wraps around. Works on any input (sine, noise, vocal, full mix).
    //  Quality is excellent at small ratios (±few semitones = pitch
    //  correction), softer at large ratios (octave-up harmonizer), which
    //  is exactly what we need.
    //
    //  Streaming model
    //  ---------------
    //   1. push host input → input ring buffer
    //   2. advance two read pointers at `rate = inputPeriod / targetPeriod`
    //      samples per output sample (1.0 = unity, 2.0 = octave up, etc)
    //   3. Read each tap with linear interpolation and crossfade them with
    //      a Hann window whose phase is driven by the tap's position
    //      within the active window
    //   4. Taps wrap backwards (shift up) or forwards (shift down) in the
    //      buffer when they drift out of their allowed window
    //
    //  Only one buffer per channel: the input ring. No OLA accumulators,
    //  no normalisation, no pitch marks. Hugely simpler than PSOLA.
    // ----------------------------------------------------------------------
    class PitchShifter
    {
    public:
        PitchShifter();
        ~PitchShifter();

        void prepare (double sampleRate, int maxBlockSize, int numChannels);
        void reset();

        // Update per block. inputPeriod / targetPeriod both in samples.
        // The shifter drives `rate = inputPeriod / targetPeriod`.
        void setInputPeriodSamples (float periodSamples) noexcept;
        void setTargetPeriodSamples (float periodSamples) noexcept;

        void setVoiced (bool v) noexcept;
        void setBypass (bool b) noexcept;

        void process (juce::AudioBuffer<float>& buffer) noexcept;

        int getLatencySamples() const noexcept { return kLatency; }

    private:
        static constexpr int kRingSize    = 16384;
        static constexpr int kMaxChannels = 2;

        // Tap oscillation window. Taps live in [kRangeMin, kRangeMin +
        // kWindowLen] and wrap by exactly kWindowLen when they leave
        // that range. That equality is the key invariant: after a
        // single wrap the tap is always inside the range, and the
        // Hann weighting goes to zero at both edges so the wrap is
        // silent.
        //
        // Window length trade-off: longer windows = slower crossfade
        // (no flutter) but bigger delay smearing across amplitude
        // transitions (the output reads from old loud content when
        // current input is quiet). 512 = ~10 ms keeps the tap within
        // a ~10 ms slice of the input — short enough that amplitude
        // transients don't bleed too far across the crossfade.
        static constexpr int kWindowLen = 512;         // ~10.7 ms window
        static constexpr int kRangeMin  = 8;           // min delay in samples

        // Plugin latency reported to the host. Tap0 starts at the
        // middle of the oscillation window so this is the typical
        // delay the output sees at unity.
        static constexpr int kLatency   = kRangeMin + kWindowLen / 2;

        struct Channel
        {
            std::array<float, kRingSize> in {};
        };

        std::array<Channel, kMaxChannels> channels {};
        int    numChannels { 2 };
        double sr          { 48000.0 };

        int64_t inWriteAbs { 0 };
        int64_t outReadAbs { -(int64_t) kLatency };

        // The two tap offsets measured backwards from the current input
        // write head, in fractional samples. They live in
        // [kRangeMin, kRangeMin + kWindowLen] and are half a window
        // apart so that as one approaches an edge (weight → 0), the
        // other is at the centre (weight → 1). Sum of Hann windows
        // offset by half a period is identically 1.
        double tap0Offset  { (double) kRangeMin + (double) kWindowLen * 0.5 };
        double tap1Offset  { (double) kRangeMin };

        // Shift ratio is the amount each tap offset changes per output
        // sample. rate > 1 means the tap moves away from the write head
        // (shift down); rate < 1 means it moves toward the write head
        // (shift up). Ratio = inputPeriod / targetPeriod.
        float  inputPeriodTarget  { 480.0f };
        float  targetPeriodTarget { 480.0f };
        float  smoothedRate       { 1.0f };

        // Envelope followers used to match the output's amplitude to
        // the input's amplitude in real time. The delay line's output
        // at any moment contains content from the last ~10 ms of input,
        // so when the input drops in level fast (end of a word) the
        // output still carries the previous loud content — audible as
        // crackling in quiet sections. Matching envelopes cancels that.
        float  envIn  { 0.0f };
        float  envOut { 0.0f };

        bool   voiced  { false };
        bool   bypass  { false };

        // Hann-weighted read from one tap. absOffset is how many samples
        // back from the write head the tap currently points to.
        float readTap (int channel, double absOffset) const noexcept;
    };
}
