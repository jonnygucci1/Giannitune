#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace gianni
{
    // ---------------------------------------------------------------
    //  DelayLine — integer-sample ring buffer for latency-matching
    //  the dry signal against the Signalsmith shifter's wet output.
    //
    //  The v1.1 wet/dry architecture sends the same input down two
    //  parallel paths:
    //     1. Detector → Quantizer → Signalsmith (wet, ~21 ms latent)
    //     2. This delay line (dry, aligned to SS latency)
    //  The two are then mixed by the RETUNE knob. If they weren't
    //  sample-aligned the mix would comb-filter at high frequencies
    //  and sound hollow.
    //
    //  Stereo-safe: configure with numChannels at prepare() time.
    //  Integer-only delay — sub-sample alignment is not needed
    //  because Signalsmith reports an integer getLatencySamples().
    //
    //  Realtime-safe after prepare(): no allocations in processBlock.
    // ---------------------------------------------------------------
    class DelayLine
    {
    public:
        DelayLine()  = default;
        ~DelayLine() = default;

        DelayLine (const DelayLine&) = delete;
        DelayLine& operator= (const DelayLine&) = delete;

        // Allocate buffers for up to `maxDelaySamples` of delay at
        // `numChannels` channels. Must be called before setDelay().
        void prepare (int maxDelaySamples, int numChannels);

        // Set the delay in samples. Clamped to [0, maxDelaySamples).
        // Cheap — only updates the read-offset, no buffer resize.
        void setDelaySamples (int samples) noexcept;

        // Reset the ring contents to zero. Safe mid-stream.
        void reset() noexcept;

        // Process in-place: writes buffer content into the ring at
        // the current write head, reads delayed content out into the
        // same buffer positions. After this call, buffer contains
        // the input delayed by `delaySamples`.
        void processBlock (juce::AudioBuffer<float>& buffer) noexcept;

        int  getDelaySamples() const noexcept { return delay; }

    private:
        // Capacity is a power of two so the modulo is a bitmask.
        int               capacity    { 0 };
        int               mask        { 0 };
        int               delay       { 0 };
        int               writeHead   { 0 };
        int               numChannels { 1 };
        std::vector<float> buffer;  // interleaved [sample][channel]

        static int nextPowerOfTwo (int n) noexcept;
    };
}
