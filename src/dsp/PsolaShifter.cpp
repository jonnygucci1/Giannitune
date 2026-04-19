#include "PsolaShifter.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

#ifdef GIANNI_PSOLA_DEBUG
  #define GD(...) std::fprintf(stderr, __VA_ARGS__)
#else
  #define GD(...) ((void) 0)
#endif

namespace gianni
{
    // ------------------------------------------------------------------
    PsolaShifter::PsolaShifter()  = default;
    PsolaShifter::~PsolaShifter() = default;

    void PsolaShifter::prepare (double sampleRate, int maxBlockSize, int numChans)
    {
        sr          = sampleRate;
        numChannels = juce::jlimit(1, kMaxChannels, numChans);

        // v0.2.8 block-size-adaptive runtime latency.
        //
        // Empirical sweep (2026-04-11 on user's real vocal):
        //   lat 800: bs64 = 22 clicks, bs128 = 19, bs192 = 1, bs256 = 0
        //   lat 832: bs64 =  5 clicks, bs128 =  4, bs192 = 0, bs256 = 0
        //   lat 864: bs64 =  1 click , bs128 =  0, bs192 = 0, bs256 = 0
        //
        // So lat=800 is safe for bs ≥ 192 (the typical live-monitoring
        // buffer sizes in REAPER, Ableton, Logic, Reason). For very
        // tight ASIO (bs 64 or 128) we fall back to 864 which is the
        // click-free floor across all block sizes.
        //
        //   bs ≤ 128           → lat = 864 samples (18.00 ms @ 48k)
        //   bs in [192..1024]  → lat = 800 samples (16.67 ms @ 48k)
        //   bs > 1024          → lat scales with block size
        //
        // The user's REAPER at ASIO 192 will see 800 samples = 16.67 ms,
        // down from v0.2.7's 864 samples = 18.00 ms. -1.33 ms.
        //
        // v0.2.7 → v0.2.8 plugin latency: 18.00 ms → 16.67 ms
        // Round-trip (incl. 2×192 ASIO):  ~29.3 ms → ~28.0 ms
        if (grainFrac >= 0.99f)
        {
            // Studio Mode: use the v0.2.9 empirically-swept values
            // that are proven click-free across all block sizes.
            // DO NOT replace with a formula — the empirical values
            // are the ground truth from exhaustive sweeps.
            if (voiceMaxPeriod >= kMaxPeriod)
            {
                if (maxBlockSize <= 128)
                    runtimeLatency = 864;
                else if (maxBlockSize <= 1024)
                    runtimeLatency = 800;
                else
                    runtimeLatency = kMaxPeriod / 2 + maxBlockSize + 32;
            }
            else
            {
                const int needed = voiceMaxPeriod
                                 + juce::jmax(1, maxBlockSize) + 128;
                const int floor_ = (maxBlockSize <= 128) ? 864 : 0;
                runtimeLatency = juce::jmax(needed, floor_);
            }
        }
        else
        {
            // Live Monitor Mode (grainFrac < 1.0): shorter grains
            // with double placement. The effective grain half is
            // scaled down, giving proportionally lower latency.
            const int effectiveMaxPeriod =
                (voiceMaxPeriod >= kMaxPeriod)
                ? kMaxPeriod / 2
                : voiceMaxPeriod;
            const int effectiveHalf =
                (int) std::round ((float) effectiveMaxPeriod * grainFrac);
            const int needed = effectiveHalf
                             + juce::jmax(1, maxBlockSize) + 128;
            const int floor_ = (maxBlockSize <= 128) ? 640 : 0;
            runtimeLatency = juce::jmax(needed, floor_);
        }

        reset();
    }

    void PsolaShifter::reset()
    {
        for (auto& ch : channels)
        {
            ch.in.fill(0.0f);
            ch.outAcc.fill(0.0f);
            ch.outNorm.fill(0.0f);
        }
        inWriteAbs     = 0;
        outReadAbs     = -(int64_t) runtimeLatency;
        nextEpochAbs   = -1.0;
        nextOutMarkAbs = 0.0;
        epochHead      = 0;
        epochCount     = 0;
        inputPeriod    = 480.0f;
        targetPeriod   = 480.0f;
        periodSmoothed = 480.0f;
        voiced         = false;
        prevVoiced     = false;
        bypass         = false;
    }

    void PsolaShifter::setInputPeriodSamples  (float p) noexcept { inputPeriod  = juce::jlimit(20.0f, (float) kMaxPeriod, p); }
    void PsolaShifter::setTargetPeriodSamples (float p) noexcept { targetPeriod = juce::jlimit(20.0f, (float) kMaxPeriod, p); }
    void PsolaShifter::setVoiced              (bool v) noexcept  { voiced  = v; }
    void PsolaShifter::setBypass              (bool b) noexcept  { bypass  = b; }

    void PsolaShifter::setMaxExpectedPeriod (int samples) noexcept
    {
        voiceMaxPeriod = juce::jlimit (64, (int) kMaxPeriod, samples);
    }

    void PsolaShifter::setGrainFraction (float fraction) noexcept
    {
        grainFrac = juce::jlimit (0.3f, 1.0f, fraction);
    }

    void PsolaShifter::setGrainHalfCap (int halfSamples) noexcept
    {
        grainHalfCap = juce::jmax (0, halfSamples);
    }

    void PsolaShifter::setPeriodSmootherPeriods (float periods) noexcept
    {
        periodSmootherPeriods = juce::jmax (0.0f, periods);
    }

    // ------------------------------------------------------------------
    //  Epoch queue management
    // ------------------------------------------------------------------
    void PsolaShifter::pushEpoch (const Epoch& e) noexcept
    {
        if (epochCount >= kMaxEpochs)
        {
            // drop oldest
            epochHead = (epochHead + 1) % kMaxEpochs;
            --epochCount;
        }
        const int tail = (epochHead + epochCount) % kMaxEpochs;
        epochs[(size_t) tail] = e;
        ++epochCount;
    }

    const PsolaShifter::Epoch* PsolaShifter::findClosestEpoch (double absPos) const noexcept
    {
        if (epochCount == 0) return nullptr;

        const Epoch* best = nullptr;
        double bestDist = 1.0e18;
        for (int i = 0; i < epochCount; ++i)
        {
            const int idx = (epochHead + i) % kMaxEpochs;
            const double d = std::abs((double) epochs[(size_t) idx].absPos - absPos);
            if (d < bestDist)
            {
                bestDist = d;
                best = &epochs[(size_t) idx];
            }
        }
        return best;
    }

    void PsolaShifter::pruneOldEpochs (int64_t minAbs) noexcept
    {
        while (epochCount > 0 && epochs[(size_t) epochHead].absPos < minAbs)
        {
            epochHead = (epochHead + 1) % kMaxEpochs;
            --epochCount;
        }
    }

    // ------------------------------------------------------------------
    //  Epoch (pitch mark) detection.
    //
    //  We know the pitch period from YIN. Starting from the last epoch
    //  we placed, we predict where the NEXT pitch mark should be
    //  (lastEpoch + period) and search for the real local peak in a
    //  ±half-period window around the prediction. The real peak is the
    //  glottal closure instant; it's approximately where the signal
    //  has its maximum absolute value within that window for typical
    //  vocal waveforms.
    //
    //  If lastEpochAbs is -1 (first time), we seed by finding the peak
    //  of the most recently pushed full period of input.
    // ------------------------------------------------------------------
    void PsolaShifter::detectEpochs() noexcept
    {
        if (inputPeriod < 20.0f) return;

        const double period     = (double) inputPeriod;
        const int    halfPeriod = juce::jmax(8, (int) std::round(period * 0.5));
        const auto&  cin        = channels[0].in;

        // Seed the epoch chain the first time we have enough buffered
        // input. Pick the most POSITIVE sample in the last two periods
        // — that's approximately the glottal closure instant for a
        // typical vocal. Signed peak (not |x|) keeps subsequent cycles
        // on the same polarity.
        if (nextEpochAbs < 0.0)
        {
            if (inWriteAbs < (int64_t) (period * 2.0)) return;

            const int64_t seedStart = inWriteAbs - (int64_t) (period * 2.0);
            const int64_t seedEnd   = inWriteAbs - (int64_t) period;
            int64_t bestAbs = -1;
            float   bestVal = -1.0e9f;
            for (int64_t a = seedStart; a < seedEnd; ++a)
            {
                if (a < 0) continue;
                const int idx = (int) (((a % kRingSize) + kRingSize) % kRingSize);
                const float v = cin[(size_t) idx];
                if (v > bestVal) { bestVal = v; bestAbs = a; }
            }
            if (bestAbs < 0) return;

            pushEpoch({ bestAbs, inputPeriod });
            nextEpochAbs = (double) bestAbs + period;
        }

        // After the seed, walk forward in exact fractional period
        // steps. Regular spacing + fractional accumulation gives
        // phase-coherent epochs with no long-term drift. Any soft
        // lock that nudged individual epochs even slightly would
        // break phase coherence at grain boundaries and click.
        //
        // v0.3.0 experiment: tight epoch push REVERTED. Tried pushing
        // epochs right up to inWriteAbs - 8 instead of - halfPeriod - 2,
        // hoping to reduce latency. Result: sweep showed MORE clicks
        // at every latency level vs the original conservative push,
        // because placeGrain truncates grains whose right half extends
        // past inWriteAbs and the resulting amplitude modulation is
        // worse than the latency improvement. Reverted to the
        // halfPeriod + 2 margin which empirically works.
        while (true)
        {
            const int64_t predicted = (int64_t) std::round(nextEpochAbs);
            if (predicted + halfPeriod + 2 >= inWriteAbs) break;

            pushEpoch({ predicted, inputPeriod });
            nextEpochAbs += period;
        }
    }

    // ------------------------------------------------------------------
    //  Place a single grain. Grain length = 2*half (usually 2*period).
    //  Hann-windowed read from the input ring, OLA into the output
    //  acc/norm accumulators. Samples that fall before the current
    //  drain pointer are skipped — we can't un-drain them.
    // ------------------------------------------------------------------
    void PsolaShifter::placeGrain (int64_t inputCenterAbs,
                                   double  outputCenterAbs,
                                   int     half) noexcept
    {
        const int64_t outCenterInt = (int64_t) std::round(outputCenterAbs);
        // Sub-sample phase offset between the grain's output centre
        // and the rounded integer write position. We apply the same
        // offset to the input read so grain content stays in exact
        // phase with the rest of the signal, even if output marks
        // land on fractional sample positions.
        const double  outFrac      = outputCenterAbs - (double) outCenterInt;
        const float twoPi    = juce::MathConstants<float>::twoPi;
        const float invLenM1 = 1.0f / (float) (2 * half - 1);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& cin  = channels[(size_t) ch].in;
            auto& acc  = channels[(size_t) ch].outAcc;
            auto& norm = channels[(size_t) ch].outNorm;

            for (int k = -half; k < half; ++k)
            {
                const int64_t dstAbs = outCenterInt + (int64_t) k;
                if (dstAbs < outReadAbs) continue;                     // already drained

                // Fractional source read: inputCenterAbs + k + outFrac
                // lets us place grains at sub-sample output positions
                // without losing phase to integer rounding.
                const double  srcAbsD = (double) inputCenterAbs + (double) k + outFrac;
                const int64_t srcInt  = (int64_t) std::floor(srcAbsD);
                const float   srcFrac = (float) (srcAbsD - (double) srcInt);

                if (srcInt < 0 || srcInt + 1 >= inWriteAbs) continue;

                const int idx0 = (int) (((srcInt        % kRingSize) + kRingSize) % kRingSize);
                const int idx1 = (int) ((((srcInt + 1)  % kRingSize) + kRingSize) % kRingSize);
                const float s0 = cin[(size_t) idx0];
                const float s1 = cin[(size_t) idx1];
                const float src = s0 + srcFrac * (s1 - s0);

                const float w = 0.5f * (1.0f
                    - std::cos(twoPi * (float) (k + half) * invLenM1));

                const int dstIdx = (int) (((dstAbs % kRingSize) + kRingSize) % kRingSize);
                acc [(size_t) dstIdx] += src * w;
                norm[(size_t) dstIdx] += w;
            }
        }
    }

    // ------------------------------------------------------------------
    void PsolaShifter::process (juce::AudioBuffer<float>& buffer) noexcept
    {
        const int n     = buffer.getNumSamples();
        const int chans = juce::jmin(buffer.getNumChannels(), numChannels);
        if (n <= 0 || chans <= 0) return;

        // -------- 1. bypass : pure delay -------------------------------
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

        // -------- 2. push input ----------------------------------------
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

        // -------- 3. voiced transition reset ---------------------------
        //
        // On voice onset we clear the OLA accumulators and the epoch
        // chain, and restart output marks at the current drain
        // pointer — PLUS half a grain so the FIRST grain's left edge
        // (Hann window at amplitude 0) lands on the drain pointer,
        // not its center (amplitude 1). Without this offset the drain
        // reads the peak of the first Hann window on the very first
        // sample after onset, which is a massive discontinuity versus
        // the preceding silence/fallback value and produces a loud
        // pop on every voice onset.
        //
        // We use inputPeriod as "half" because PsolaShifter grains are
        // 2*round(inputPeriod) samples long (see placeGrain).
        if (voiced && ! prevVoiced)
        {
            for (auto& ch : channels)
            {
                ch.outAcc.fill(0.0f);
                ch.outNorm.fill(0.0f);
            }
            nextEpochAbs   = -1.0;
            epochHead      = 0;
            epochCount     = 0;
            const int onsetHardMax = (grainHalfCap > 0)
                ? juce::jmin (grainHalfCap, kMaxPeriod / 2)
                : kMaxPeriod / 2;
            const int onsetHalf = juce::jlimit(32, onsetHardMax,
                (int) std::round(inputPeriod * grainFrac));
            nextOutMarkAbs = (double) outReadAbs + (double) onsetHalf;
            // Snap period-smoother to the current target so the first
            // grain spacing is correct from the very first sample.
            periodSmoothed = targetPeriod;
        }

        // -------- 4. voiced synthesis ----------------------------------
        if (voiced)
        {
            detectEpochs();
            pruneOldEpochs(inWriteAbs - kRingSize + 256);

            // v0.3.0 adaptive synth lookahead — REVERTED, see commit
            // log. Tried using `min(inputPeriod, kMaxPeriod/2)` instead
            // of the constant `kMaxPeriod/2`. Sweep showed it paired
            // with the tight epoch push made clicks worse at every
            // latency. Back to the conservative constant.
            const int64_t maxOutMark = inWriteAbs - kMaxPeriod / 2 - 4;

            // v0.3.8: pitch-invariant period smoother.
            //
            // Legacy (v0.2.8 - v0.3.7): fixed 1 ms tau. The smoother
            // is evaluated once per grain — at 108 Hz, grain spacing
            // is 9.25 ms, so alpha = 1 - exp(-9.25 / 1) ≈ 1.0.
            // The smoother is EFFECTIVELY A NO-OP at low pitches.
            // This was the hidden cause of the v0.3.7 low-male
            // kratzen: the block-to-block scale-snap delta hit the
            // grain placer full-force, producing OLA under-run at
            // rapid target-period changes.
            //
            // New: tau scales with inputPeriod. With periodSmootherPeriods=5,
            // the smoother time constant is 5 periods = 46 ms at 108 Hz
            // or 5 ms at 1000 Hz. alpha ≈ 1 - exp(-1/5) ≈ 0.18 at any
            // pitch — pitch-invariant smoothing strength.
            //
            // Default periodSmootherPeriods = 0 falls back to the legacy
            // 1 ms tau so unchanged tests stay bit-exact.
            const float tauSec = (periodSmootherPeriods > 0.0f)
                ? periodSmootherPeriods * (inputPeriod / (float) sr)
                : 1.0e-3f;

            // grainFrac < 1.0 activates PSOLA-Lite (Live Monitor):
            // shorter grains (period*grainFrac per half) with double
            // placement (two grains per target period at period/2
            // spacing). The Hann window at 50% overlap of the shorter
            // grain sums to exactly 1.0, so OLA reconstruction is
            // correct. Character stays epoch-locked (unlike delay-line).
            const bool doublePlacement = grainFrac < 0.99f;

            int iters = 0;
            while (nextOutMarkAbs <= (double) maxOutMark && iters++ < 2048)
            {
                // Advance the per-grain smoother by one "output period"
                // worth of time (i.e. the time between consecutive
                // grain placements).
                const float dtGrain = periodSmoothed / (float) sr;
                const float alpha   = 1.0f - std::exp(-dtGrain / tauSec);
                periodSmoothed += alpha * (targetPeriod - periodSmoothed);

                // v0.3.8: optional cap on grain half-length.
                // grainHalfCap=0 → legacy behaviour (half = inputPeriod).
                // Nonzero → shorter grains for low-frequency vocals,
                // trades some harmonic fidelity for dramatically fewer
                // OLA under-run clicks at scale-note transitions.
                const int hardMax = (grainHalfCap > 0)
                    ? juce::jmin (grainHalfCap, kMaxPeriod / 2)
                    : kMaxPeriod / 2;
                const int half = juce::jlimit(32, hardMax,
                    (int) std::round(inputPeriod * grainFrac));

                if (nextOutMarkAbs + half < (double) outReadAbs)
                    nextOutMarkAbs = (double) outReadAbs;

                const Epoch* ep = findClosestEpoch(nextOutMarkAbs);
                if (ep == nullptr) break;

                if (ep->absPos - half < inWriteAbs - kRingSize + 4)
                {
                    nextOutMarkAbs += (double) periodSmoothed;
                    continue;
                }

                // Place primary grain.
                placeGrain(ep->absPos, nextOutMarkAbs, half);

                // In Live Monitor mode (short grains), place a SECOND
                // copy of the same grain shifted by half a target
                // period. This provides the overlap coverage that the
                // shorter grain can't reach on its own. The pair of
                // Hann-windowed copies at half-spacing sums to unity,
                // giving clean OLA reconstruction.
                if (doublePlacement)
                {
                    const double secondPos = nextOutMarkAbs
                                           + (double) periodSmoothed * 0.5;
                    if (secondPos <= (double) maxOutMark)
                        placeGrain(ep->absPos, secondPos, half);
                }

                nextOutMarkAbs += (double) periodSmoothed;
            }
        }

        // -------- 5. drain output --------------------------------------
        //
        // Each output sample is a smooth blend of the OLA accumulator
        // (where we have one — `norm` high) and the delayed raw input
        // (where we don't — `norm` approaching zero, e.g. unvoiced
        // consonants, between-word pauses, the tail of the last grain
        // in a voiced phrase). Before v0.2.3 this was a hard if/else
        // on `norm > 0.001`, which popped every time we crossed the
        // threshold during unvoiced stretches — the click-hunt
        // regression counted 72 events in a 68 s real vocal. The
        // linear crossfade below eliminates that by guaranteeing
        // sample-level continuity through the transition.
        //
        // Blend weight ramp:
        //   norm ≤ 0.05 → pure delayed input (fallback)
        //   norm ≥ 0.25 → pure OLA
        //   linear in between (a 0.2-wide crossfade region)
        //
        // We only compute acc/norm when norm is above the floor to
        // avoid 1/tiny-norm amplification artefacts.
        constexpr float kNormFloor   = 0.05f;
        constexpr float kNormFull    = 0.25f;
        constexpr float kNormSpanInv = 1.0f / (kNormFull - kNormFloor);

        for (int ch = 0; ch < chans; ++ch)
        {
            float* dst = buffer.getWritePointer(ch);
            auto&  cin  = channels[(size_t) ch].in;
            auto&  acc  = channels[(size_t) ch].outAcc;
            auto&  norm = channels[(size_t) ch].outNorm;

            for (int i = 0; i < n; ++i)
            {
                const int64_t abs = outReadAbs + (int64_t) i;
                if (abs < 0) { dst[i] = 0.0f; continue; }

                const int idx = (int) (((abs % kRingSize) + kRingSize) % kRingSize);
                const float w = norm[(size_t) idx];

                float olaSample = 0.0f;
                if (w > kNormFloor)
                    olaSample = acc[(size_t) idx] / w;

                const float blend = juce::jlimit(0.0f, 1.0f,
                                                 (w - kNormFloor) * kNormSpanInv);
                const float fallback = cin[(size_t) idx];
                const float out = blend * olaSample + (1.0f - blend) * fallback;

                dst[i] = juce::jlimit(-2.0f, 2.0f, out);
                acc [(size_t) idx] = 0.0f;
                norm[(size_t) idx] = 0.0f;
            }
        }
        outReadAbs += (int64_t) n;

        prevVoiced = voiced;
    }
}
