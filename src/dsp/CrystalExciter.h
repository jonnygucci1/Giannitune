#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace gianni
{
    // ---------------------------------------------------------------
    //  CrystalExciter — Giannitune's signature vocal exciter.
    //
    //  Driven by the AURA knob. Unlike a generic Aphex/SPL/BBE exciter,
    //  Crystal leverages the fact that Giannitune's quantiser already
    //  knows what note the voice is being corrected to. That lets us
    //  do something no other exciter can: synthesise harmonics at
    //  exact musical intervals of the quantised target pitch. The
    //  synthesised partials are always in-key because the target is
    //  always in-key.
    //
    //  Three parallel stages, all fading in with AURA but on different
    //  curves so the sound evolves as the knob is pushed:
    //
    //    Stage 1 — Dynamic Presence Expander       (AURA 0 → 60 %)
    //       3-10 kHz band-pass → upward expansion →
    //       lifts quiet HF detail (consonants, air) above a
    //       threshold, leaving loud sustained content untouched.
    //       Fresh-Air-style, "brings out articulation without
    //       brightness harshness".
    //
    //    Stage 2 — Asymmetric Harmonic Saturator   (AURA 30 → 100 %)
    //       3 kHz high-pass → asymmetric tanh+bias clipper →
    //       15 % parallel mix. Aphex-style: asymmetric clipping
    //       generates BOTH even- and odd-order harmonics, which
    //       reads as "analog warmth" rather than the digital-
    //       symmetric character of plain tanh.
    //
    //    Stage 3 — Pitch-Locked Harmonic Synthesis (AURA 60 → 100 %)
    //       Sine oscillators at 2× and 3× targetHz, amplitude-gated
    //       by the voice envelope, slowly detuned ±5 cents by a
    //       0.5 Hz LFO for organic movement. 5 % back-mix per
    //       partial. GENUINELY UNIQUE — only possible because the
    //       plugin already knows the target pitch.
    //
    //  Realtime-safe after prepare(): no allocations, O(n) per block.
    // ---------------------------------------------------------------
    class CrystalExciter
    {
    public:
        CrystalExciter()  = default;
        ~CrystalExciter() = default;

        CrystalExciter (const CrystalExciter&) = delete;
        CrystalExciter& operator= (const CrystalExciter&) = delete;

        void prepare (double sampleRate, int maxBlockSize, int numChannels);
        void reset();

        // 0 = bypassed, 1 = maximum AURA.
        void setAmount (float amount0To1) noexcept;

        // Feed from the quantiser per block. Crystal uses these to
        // drive Stage 3's pitch-locked oscillators. Hz = 0 or !voiced
        // means "don't synthesise" (oscillators gate to silence).
        void setTargetHz (float hz) noexcept;
        void setVoiced   (bool  v) noexcept;

        // v1.1: voice-type-aware band tuning. Different vocal registers
        // live in different spectral regions — boosting 5-12 kHz on a
        // bass voice just adds noise hiss, boosting 2-4 kHz on a
        // soprano misses the brilliance zone. Stage 1 and Stage 2
        // filter frequencies re-adjust when the Voice Type changes.
        //   0 = Soprano        (BP 4-12 kHz, HP 4 kHz)
        //   1 = Alto/Tenor     (BP 3-10 kHz, HP 3 kHz)  ← default
        //   2 = Low Male       (BP 2-8  kHz, HP 2 kHz)
        //   3 = Instrument     (BP 3-10 kHz, HP 3 kHz)
        //   4 = Bass Instr.    (BP 1-6  kHz, HP 1 kHz)
        void setVoiceType (int typeIdx) noexcept;

        void processBlock (juce::AudioBuffer<float>& buffer) noexcept;

    private:
        double sr          { 48000.0 };
        int    numChannels { 2 };
        float  amount      { 0.0f };

        // Stage-by-stage gain scales computed from `amount` via the
        // overlapping fade curves documented in the class header.
        float stage1Gain   { 0.0f };    // 0 → 1 across AURA 0 → 60 %
        float stage2Gain   { 0.0f };    // 0 → 1 across AURA 30 → 100 %
        float stage3Gain   { 0.0f };    // 0 → 1 across AURA 60 → 100 %

        // -------- Stage 1: Dynamic Presence Expander ---------------
        //   Per-channel band-pass (3 → 10 kHz) and envelope follower.
        juce::dsp::ProcessorDuplicator<
            juce::dsp::IIR::Filter<float>,
            juce::dsp::IIR::Coefficients<float>> bpFilter;
        juce::AudioBuffer<float> stage1Buf;
        std::vector<float> stage1Env;   // per-channel RMS follower

        // -------- Stage 2: Asymmetric Harmonic Saturator -----------
        //   Per-channel high-pass sidechain for the clipper.
        juce::dsp::ProcessorDuplicator<
            juce::dsp::IIR::Filter<float>,
            juce::dsp::IIR::Coefficients<float>> hpFilter;
        juce::AudioBuffer<float> stage2Buf;

        // -------- Stage 3: Pitch-Locked Harmonic Synthesis ---------
        //   Two sine oscillators (2nd + 3rd harmonic of targetHz).
        //   Smoothed phase-increment + voice envelope + detune LFO.
        float targetHz       { 0.0f };
        bool  voiced         { false };
        // Per-harmonic phase accumulators
        float phase2         { 0.0f };
        float phase3         { 0.0f };
        // Smoothed per-sample phase increments (ramp to avoid clicks
        // at target-note transitions).
        float phaseInc2      { 0.0f };
        float phaseInc3      { 0.0f };
        float targetInc2     { 0.0f };
        float targetInc3     { 0.0f };
        // Voice-envelope follower. Gates the oscillator output so
        // Stage 3 only produces sound while the voice is actually
        // present (silence-aware).
        float voiceEnv       { 0.0f };
        // Slow LFO for organic detune.
        float lfoPhase       { 0.0f };
        float lfoInc         { 0.0f };

        // Stage 1/2 band frequencies — voice-type-dependent, updated
        // via setVoiceType(). Defaults below = Alto/Tenor (the safe
        // "pop vocal" preset we use at startup and after state restore).
        float stage1BpLoHz { 3000.0f };
        float stage1BpHiHz { 10000.0f };
        float stage2HpHz   { 3000.0f };
        int   currentVoiceType { 1 };  // cache so we only rebuild coefs on change

        // Tunables — fixed for v1.
        static constexpr float kStage1EnvAtkMs   = 5.0f;
        static constexpr float kStage1EnvRelMs   = 60.0f;
        static constexpr float kStage1MaxBoostDb = 6.0f;
        static constexpr float kStage1ThresholdDb = -30.0f;
        static constexpr float kStage2DriveMax   = 5.0f;
        static constexpr float kStage2BiasAsym   = 0.18f;
        static constexpr float kStage2MixLevel   = 0.15f;

        static constexpr float kStage3VoiceAtkMs = 8.0f;
        static constexpr float kStage3VoiceRelMs = 120.0f;
        static constexpr float kStage3Harm2Mix   = 0.06f;
        static constexpr float kStage3Harm3Mix   = 0.035f;
        static constexpr float kStage3LfoHz      = 0.5f;
        static constexpr float kStage3DetuneCents = 5.0f;

        // Envelope / smoothing coefficients computed in prepare().
        float stage1EnvAtk   { 0.0f };
        float stage1EnvRel   { 0.0f };
        float stage3VoiceAtk { 0.0f };
        float stage3VoiceRel { 0.0f };
        float stage3IncSmoothing { 0.0f };

        void updateStageGains();
        void updateFilterCoefsForVoiceType();
    };
}
