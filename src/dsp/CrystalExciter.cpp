#include "CrystalExciter.h"
#include <cmath>

namespace gianni
{
    namespace
    {
        // Envelope attack/release coefficient for a one-pole follower.
        // Maps to: env = env + alpha * (target - env)
        float envCoef (float timeMs, double sampleRate) noexcept
        {
            if (timeMs <= 0.0f) return 1.0f;
            return 1.0f - std::exp (-1.0f / (timeMs * 0.001f * (float) sampleRate));
        }
    }

    void CrystalExciter::prepare (double sampleRate, int maxBlockSize, int numChans)
    {
        sr          = sampleRate;
        numChannels = juce::jmax (1, numChans);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) juce::jmax (1, maxBlockSize);
        spec.numChannels      = (juce::uint32) numChannels;

        bpFilter.prepare (spec);
        hpFilter.prepare (spec);

        // Build stage 1/2 filters for the current voice type default
        // (Alto/Tenor on first prepare).
        updateFilterCoefsForVoiceType();

        // Per-stage scratch buffers.
        stage1Buf.setSize (numChannels, juce::jmax (1, maxBlockSize),
                           false, true, false);
        stage2Buf.setSize (numChannels, juce::jmax (1, maxBlockSize),
                           false, true, false);
        stage1Env.assign ((size_t) numChannels, 0.0f);

        // Envelope time-constants.
        stage1EnvAtk   = envCoef (kStage1EnvAtkMs,   sampleRate);
        stage1EnvRel   = envCoef (kStage1EnvRelMs,   sampleRate);
        stage3VoiceAtk = envCoef (kStage3VoiceAtkMs, sampleRate);
        stage3VoiceRel = envCoef (kStage3VoiceRelMs, sampleRate);
        // Per-sample phase-increment smoothing for Stage 3: ~30 ms
        // time constant so target-note jumps ramp over a few grain-
        // like periods instead of discontinuously.
        stage3IncSmoothing = envCoef (30.0f, sampleRate);

        // Stage 3 LFO: 0.5 Hz slow drift, advanced per sample.
        lfoInc = 2.0f * juce::MathConstants<float>::pi * kStage3LfoHz
                 / (float) sampleRate;

        reset();
    }

    void CrystalExciter::reset()
    {
        bpFilter.reset();
        hpFilter.reset();
        stage1Buf.clear();
        stage2Buf.clear();
        std::fill (stage1Env.begin(), stage1Env.end(), 0.0f);
        phase2 = phase3 = 0.0f;
        phaseInc2 = phaseInc3 = 0.0f;
        targetInc2 = targetInc3 = 0.0f;
        voiceEnv = 0.0f;
        lfoPhase = 0.0f;
    }

    void CrystalExciter::setAmount (float a) noexcept
    {
        amount = juce::jlimit (0.0f, 1.0f, a);
        updateStageGains();
    }

    void CrystalExciter::updateStageGains()
    {
        // Stage 1: 0 → 1 across AURA 0 → 0.6
        stage1Gain = juce::jlimit (0.0f, 1.0f, amount / 0.6f);
        // Stage 2: 0 → 1 across AURA 0.3 → 1.0
        stage2Gain = juce::jlimit (0.0f, 1.0f, (amount - 0.3f) / 0.7f);
        // Stage 3: 0 → 1 across AURA 0.6 → 1.0
        stage3Gain = juce::jlimit (0.0f, 1.0f, (amount - 0.6f) / 0.4f);
    }

    void CrystalExciter::setTargetHz (float hz) noexcept
    {
        targetHz = (hz > 0.0f) ? hz : 0.0f;
        // Compute target phase increments for 2× and 3× harmonics.
        if (targetHz > 0.0f)
        {
            const float twoPiOverSr = 2.0f * juce::MathConstants<float>::pi / (float) sr;
            targetInc2 = 2.0f * targetHz * twoPiOverSr;
            targetInc3 = 3.0f * targetHz * twoPiOverSr;
        }
        else
        {
            targetInc2 = targetInc3 = 0.0f;
        }
    }

    void CrystalExciter::setVoiced (bool v) noexcept
    {
        voiced = v;
    }

    void CrystalExciter::setVoiceType (int typeIdx) noexcept
    {
        const int t = juce::jlimit (0, 2, typeIdx);
        if (t == currentVoiceType) return;
        currentVoiceType = t;
        // Pick a band/HP pair that matches each voice's formant region.
        // Values chosen so the exciter enhances PRESENCE and AIR where
        // that register actually has spectral content — not where it
        // just has noise.
        switch (t)
        {
            case 0:  // Soprano
                stage1BpLoHz = 4000.0f;  stage1BpHiHz = 12000.0f;
                stage2HpHz   = 4000.0f;
                break;
            case 1:  // Alto / Tenor (pop-vocal default)
                stage1BpLoHz = 3000.0f;  stage1BpHiHz = 10000.0f;
                stage2HpHz   = 3000.0f;
                break;
            case 2:  // Low Male (chest-range)
                stage1BpLoHz = 2000.0f;  stage1BpHiHz =  8000.0f;
                stage2HpHz   = 2000.0f;
                break;
            default:
                return;
        }
        updateFilterCoefsForVoiceType();
    }

    void CrystalExciter::updateFilterCoefsForVoiceType()
    {
        // Stage 1 band-pass: geometric-mean centre, derived Q.
        const float centreHz = std::sqrt (stage1BpLoHz * stage1BpHiHz);
        const float bwRatio  = stage1BpHiHz / stage1BpLoHz;
        const float q        = std::sqrt (bwRatio) / (bwRatio - 1.0f);
        *bpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeBandPass (
                              sr, centreHz, q);

        // Stage 2 high-pass.
        *hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
                              sr, stage2HpHz, 0.707f);
    }

    void CrystalExciter::processBlock (juce::AudioBuffer<float>& buf) noexcept
    {
        const int n      = buf.getNumSamples();
        const int nChans = juce::jmin (numChannels, buf.getNumChannels());
        if (n == 0 || nChans == 0) return;

        // Short-circuit when AURA is essentially zero.
        if (amount <= 1.0e-4f) return;

        // ---- Stage 1: Dynamic Presence Expander ---------------------
        //   Copy buf → stage1Buf, bandpass it, compute a per-channel
        //   envelope, boost below-threshold content (upward expansion).
        if (stage1Gain > 1.0e-3f)
        {
            for (int c = 0; c < nChans; ++c)
                stage1Buf.copyFrom (c, 0, buf, c, 0, n);

            // Bandpass the sidechain.
            {
                juce::dsp::AudioBlock<float> blk (stage1Buf);
                blk = blk.getSubBlock (0, (size_t) n);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                bpFilter.process (ctx);
            }

            // Per-sample envelope follower + upward expansion.
            const float thresholdLin = juce::Decibels::decibelsToGain (kStage1ThresholdDb);
            const float maxBoost     = juce::Decibels::decibelsToGain (kStage1MaxBoostDb);
            for (int c = 0; c < nChans; ++c)
            {
                float* sc  = stage1Buf.getWritePointer (c);
                float* dry = buf      .getWritePointer (c);
                float env  = stage1Env[(size_t) c];
                for (int i = 0; i < n; ++i)
                {
                    const float a = std::abs (sc[i]);
                    const float coef = (a > env) ? stage1EnvAtk : stage1EnvRel;
                    env += coef * (a - env);

                    // Upward expand: if env is below threshold, gain
                    // rises linearly toward maxBoost; above threshold,
                    // gain stays at 1. Graceful, transient-preserving.
                    float gain = 1.0f;
                    if (env < thresholdLin && env > 1.0e-6f)
                    {
                        const float ratio = thresholdLin / env;
                        gain = juce::jmin (maxBoost, 1.0f + (ratio - 1.0f) * 0.3f);
                    }
                    dry[i] += sc[i] * (gain - 1.0f) * stage1Gain;
                }
                stage1Env[(size_t) c] = env;
            }
        }

        // ---- Stage 2: Asymmetric Harmonic Saturator -----------------
        //   Copy buf → stage2Buf, highpass it, pass through asymmetric
        //   tanh clipper (even+odd harmonics), mix back at 15 %.
        if (stage2Gain > 1.0e-3f)
        {
            for (int c = 0; c < nChans; ++c)
                stage2Buf.copyFrom (c, 0, buf, c, 0, n);

            // Highpass the sidechain.
            {
                juce::dsp::AudioBlock<float> blk (stage2Buf);
                blk = blk.getSubBlock (0, (size_t) n);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                hpFilter.process (ctx);
            }

            // Asymmetric soft-clipper. The +bias in the argument +
            // subtraction of the bias'd DC produces an asymmetric
            // transfer curve — generates both even and odd harmonics
            // (a plain tanh is symmetric, generates only odd).
            const float drive = 1.0f + kStage2DriveMax * stage2Gain;
            const float bias  = kStage2BiasAsym * stage2Gain;
            const float dcRemoval = std::tanh (drive * bias);
            const float mix   = kStage2MixLevel * stage2Gain;
            for (int c = 0; c < nChans; ++c)
            {
                float* sc  = stage2Buf.getWritePointer (c);
                float* dry = buf      .getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    const float shaped = std::tanh (drive * (sc[i] + bias))
                                       - dcRemoval;
                    dry[i] += shaped * mix;
                }
            }
        }

        // ---- Stage 3: Pitch-Locked Harmonic Synthesis ---------------
        //   Two sine oscillators at 2× and 3× targetHz, amplitude-
        //   gated by a voice envelope, slowly detuned by a LFO.
        //   Silent when !voiced or targetHz == 0.
        if (stage3Gain > 1.0e-3f)
        {
            const float twoPi = 2.0f * juce::MathConstants<float>::pi;
            const float detuneRange = kStage3DetuneCents / 1200.0f * std::log (2.0f);

            for (int i = 0; i < n; ++i)
            {
                // Input envelope on channel 0 (or mono sum — for v1
                // we use channel 0; real stereo vocals aren't common).
                const float in = buf.getReadPointer (0)[i];
                const float a  = std::abs (in);
                const float voiceTarget = voiced ? a : 0.0f;
                const float coef = (voiceTarget > voiceEnv)
                                   ? stage3VoiceAtk : stage3VoiceRel;
                voiceEnv += coef * (voiceTarget - voiceEnv);

                // Smoothly ramp phase increments toward their targets
                // so note-jumps don't click.
                phaseInc2 += stage3IncSmoothing * (targetInc2 - phaseInc2);
                phaseInc3 += stage3IncSmoothing * (targetInc3 - phaseInc3);

                // LFO → detune multiplier ~ 1 ± detuneRange.
                lfoPhase += lfoInc;
                if (lfoPhase >= twoPi) lfoPhase -= twoPi;
                const float lfoVal = std::sin (lfoPhase);
                const float detune = 1.0f + detuneRange * lfoVal;

                // Advance oscillators with detune applied.
                phase2 += phaseInc2 * detune;
                phase3 += phaseInc3 * (1.0f - detuneRange * lfoVal);  // opposite detune on 3rd for extra organic-ness
                if (phase2 >= twoPi) phase2 -= twoPi;
                if (phase3 >= twoPi) phase3 -= twoPi;

                // Harmonic samples, gated by voice env × stage3Gain.
                const float hGate = voiceEnv * stage3Gain;
                const float h2    = std::sin (phase2) * kStage3Harm2Mix * hGate;
                const float h3    = std::sin (phase3) * kStage3Harm3Mix * hGate;

                // Sum into every output channel.
                for (int c = 0; c < nChans; ++c)
                    buf.getWritePointer (c)[i] += h2 + h3;
            }
        }
    }
}
