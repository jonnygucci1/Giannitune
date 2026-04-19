#include "PitchShifter.h"
#include <cmath>
#include <algorithm>

namespace gianni
{
    // ------------------------------------------------------------------
    //  Delay-line pitch shifter — Dattorro / two-tap crossfade.
    //
    //  State per tap:
    //     phase ∈ [0, kWindowLen).  Advances by (1 - rate) per output
    //     sample. When it leaves the interval we wrap it by ±kWindowLen.
    //
    //  Read position:
    //     readAbs = inWriteAbs - 1 - kRangeMin - phase
    //     In words: a base delay of kRangeMin samples plus an extra
    //     delay that cycles in [0, kWindowLen).
    //
    //  Weight:
    //     Hann of (phase / kWindowLen). Tap0 and tap1 are offset by
    //     kWindowLen/2 so their Hann weights sum to exactly 1 at every
    //     phase. No normalisation needed and the wrap is inaudible
    //     because the tap leaving the window is at zero weight.
    //
    //  Rate = inputPeriod / targetPeriod.
    //     rate = 1      unity   → phase frozen, pure delay
    //     rate > 1      shift up → phase decreases → total delay
    //                              shrinks → tap reads newer samples
    //     rate < 1      shift down → phase increases → delay grows
    //                              → tap reads older samples
    // ------------------------------------------------------------------

    PitchShifter::PitchShifter()  = default;
    PitchShifter::~PitchShifter() = default;

    void PitchShifter::prepare (double sampleRate, int /*maxBlockSize*/, int numChans)
    {
        sr          = sampleRate;
        numChannels = juce::jlimit(1, kMaxChannels, numChans);
        reset();
    }

    void PitchShifter::reset()
    {
        for (auto& ch : channels)
            ch.in.fill(0.0f);

        inWriteAbs         = 0;
        outReadAbs         = -(int64_t) kLatency;

        // tap phases in [0, kWindowLen), half a window apart so their
        // crossfade sum is constantly 1.
        tap0Offset         = (double) kWindowLen * 0.5;  // repurposed: stores phase, not offset
        tap1Offset         = 0.0;

        inputPeriodTarget  = 480.0f;
        targetPeriodTarget = 480.0f;
        smoothedRate       = 1.0f;
        envIn              = 0.0f;
        envOut             = 0.0f;
        voiced             = false;
        bypass             = false;
    }

    void PitchShifter::setInputPeriodSamples (float p) noexcept
    {
        inputPeriodTarget = juce::jmax(20.0f, p);
    }

    void PitchShifter::setTargetPeriodSamples (float p) noexcept
    {
        targetPeriodTarget = juce::jmax(20.0f, p);
    }

    void PitchShifter::setVoiced (bool v) noexcept  { voiced = v; }
    void PitchShifter::setBypass (bool b) noexcept  { bypass = b; }

    // ------------------------------------------------------------------
    //  Read from a channel's input ring at a fractional delay from
    //  the write head. Linear interpolation. `delay` is how many
    //  samples back from the most recently written sample.
    // ------------------------------------------------------------------
    float PitchShifter::readTap (int channel, double delay) const noexcept
    {
        const auto& cin = channels[(size_t) channel].in;

        const double srcAbs = (double) (inWriteAbs - 1) - delay;
        if (srcAbs < 0.0)
            return 0.0f;

        const int64_t srcInt = (int64_t) std::floor(srcAbs);
        const float   frac   = (float) (srcAbs - (double) srcInt);

        const int idx0 = (int) (((srcInt    % kRingSize) + kRingSize) % kRingSize);
        const int idx1 = (idx0 + 1) % kRingSize;
        return cin[(size_t) idx0] + frac * (cin[(size_t) idx1] - cin[(size_t) idx0]);
    }

    // ------------------------------------------------------------------
    void PitchShifter::process (juce::AudioBuffer<float>& buffer) noexcept
    {
        const int n     = buffer.getNumSamples();
        const int chans = juce::jmin(buffer.getNumChannels(), numChannels);
        if (n <= 0 || chans <= 0) return;

        // -------- 1. host bypass : pure delay ------------------------
        // We only take this exit path for explicit plugin bypass, NOT
        // for unvoiced. Switching between the delay-line path and a
        // pure-delay path mid-session creates audible clicks because
        // the two paths read from different delays (the taps have
        // drifted). Instead, for unvoiced material we stay inside the
        // delay-line and just glide `rate` back to 1 so the shifter
        // transparently passes audio through.
        if (bypass)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                auto& cin = channels[(size_t) ch].in;
                const float* src = buffer.getReadPointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    const int64_t abs = inWriteAbs + (int64_t) i;
                    const int     idx = (int) (((abs % kRingSize) + kRingSize) % kRingSize);
                    cin[(size_t) idx] = src[i];
                }
            }
            inWriteAbs += (int64_t) n;

            for (int ch = 0; ch < chans; ++ch)
            {
                float* dst = buffer.getWritePointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    const int64_t srcAbs = outReadAbs + (int64_t) i;
                    if (srcAbs < 0) { dst[i] = 0.0f; continue; }
                    const int idx = (int) (((srcAbs % kRingSize) + kRingSize) % kRingSize);
                    dst[i] = channels[(size_t) ch].in[(size_t) idx];
                }
            }
            outReadAbs += (int64_t) n;
            return;
        }

        // -------- 2. smooth the shift rate ---------------------------
        // Target rate depends on voicing: during voiced material, the
        // real shift ratio; during unvoiced, 1.0 (no shift). Because
        // we stay inside the delay-line loop either way, the transition
        // is a smooth rate glide rather than a discontinuous mode jump.
        const float dt   = (float) (n / sr);
        const float tau  = 0.050f;                              // 50 ms
        const float coef = 1.0f - std::exp(-dt / tau);
        const float targetRate = voiced
            ? (inputPeriodTarget / targetPeriodTarget)
            : 1.0f;
        smoothedRate += coef * (targetRate - smoothedRate);

        // -------- 3. interleaved push + pitch-shifted drain ----------
        //
        // CRITICAL: push input and produce output sample-by-sample.
        // The delay-line math assumes inWriteAbs advances by exactly 1
        // between every pair of output samples. If we pushed the whole
        // block first, inWriteAbs would be constant through the loop
        // and every block boundary would create a click.
        const double windowLen = (double) kWindowLen;
        const double baseDelay = (double) kRangeMin;
        const double twoPi     = 2.0 * juce::MathConstants<double>::pi;
        const double phaseStep = 1.0 - (double) smoothedRate;

        double& phase0 = tap0Offset;
        double& phase1 = tap1Offset;

        // Envelope follower time constants. 5 ms attack / 30 ms
        // release — short enough to track vocal dynamics, long enough
        // to ignore cycle-level variation in the signal.
        const float envAttack  = 1.0f - std::exp(-1.0f / (0.005f * (float) sr));
        const float envRelease = 1.0f - std::exp(-1.0f / (0.030f * (float) sr));

        for (int i = 0; i < n; ++i)
        {
            // Push this sample's input into each channel's ring.
            // Also accumulate the input envelope from channel 0.
            const float inSample0 = buffer.getReadPointer(0)[i];
            for (int ch = 0; ch < chans; ++ch)
            {
                auto& cin = channels[(size_t) ch].in;
                const int idx = (int) (((inWriteAbs % kRingSize) + kRingSize) % kRingSize);
                cin[(size_t) idx] = buffer.getReadPointer(ch)[i];
            }
            inWriteAbs += 1;

            // Update input envelope
            const float inMag = std::abs(inSample0);
            const float coefIn = (inMag > envIn) ? envAttack : envRelease;
            envIn += coefIn * (inMag - envIn);

            // Advance phases
            phase0 += phaseStep;
            phase1 += phaseStep;

            if (phase0 < 0.0)        phase0 += windowLen;
            if (phase0 >= windowLen) phase0 -= windowLen;
            if (phase1 < 0.0)        phase1 += windowLen;
            if (phase1 >= windowLen) phase1 -= windowLen;

            const double t0 = phase0 / windowLen;
            const double t1 = phase1 / windowLen;
            const float  w0 = (float) (0.5 * (1.0 - std::cos(twoPi * t0)));
            const float  w1 = (float) (0.5 * (1.0 - std::cos(twoPi * t1)));

            const double d0 = baseDelay + phase0;
            const double d1 = baseDelay + phase1;

            // Tap read for channel 0 first, so we can measure envOut
            const float s0_0 = readTap(0, d0);
            const float s1_0 = readTap(0, d1);
            const float rawOut0 = s0_0 * w0 + s1_0 * w1;

            // Update output envelope from channel 0's raw output
            const float outMag = std::abs(rawOut0);
            const float coefOut = (outMag > envOut) ? envAttack : envRelease;
            envOut += coefOut * (outMag - envOut);

            // Envelope-matching gain. Floor envOut so we don't divide
            // by ~0; clamp gain to [0, 1] so the output can only be
            // attenuated, never amplified above the raw level (that
            // would create its own kind of noise in silence).
            const float envFloor = 1.0e-4f;
            const float gain = juce::jlimit(0.0f, 1.0f,
                                            envIn / std::max(envOut, envFloor));

            buffer.getWritePointer(0)[i] = juce::jlimit(-2.0f, 2.0f, rawOut0 * gain);

            // Apply the same gain to the other channels for stereo
            // image consistency.
            for (int ch = 1; ch < chans; ++ch)
            {
                const float s0 = readTap(ch, d0);
                const float s1 = readTap(ch, d1);
                const float out = (s0 * w0 + s1 * w1) * gain;
                buffer.getWritePointer(ch)[i] = juce::jlimit(-2.0f, 2.0f, out);
            }
        }

        outReadAbs += (int64_t) n;
    }
}
