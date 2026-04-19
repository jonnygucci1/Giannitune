#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "params/Parameters.h"
#include "dsp/PitchDetector.h"
#include "dsp/ScaleQuantizer.h"
#include "dsp/PitchShifter.h"
#include "dsp/PsolaShifter.h"
#include "dsp/SignalsmithShifter.h"
#include "dsp/DelayLine.h"
#include "dsp/CrystalExciter.h"
#include "telemetry/Telemetry.h"

class GiannituneAudioProcessor : public juce::AudioProcessor,
                                 public juce::AudioProcessorValueTreeState::Listener,
                                 public juce::AsyncUpdater
{
public:
    GiannituneAudioProcessor();
    ~GiannituneAudioProcessor() override;

    // --- Parameter listener + throttled save (user-defaults persistence) --
    void parameterChanged (const juce::String& id, float newValue) override;
    void handleAsyncUpdate() override;

    // --- AudioProcessor -------------------------------------------------
    void prepareToPlay (double sampleRate, int maxBlockSize) override;
    void releaseResources() override { telemetry.stop(); }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    using juce::AudioProcessor::processBlock;  // silence double-hiding warning

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }

    // --- Presets / profiles --------------------------------------------
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- Public accessors used by the editor ---------------------------
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }

private:
    juce::AudioProcessorValueTreeState apvts;

    gianni::PitchDetector      pitchDetector;
    gianni::ScaleQuantizer     quantizer;
    // v0.4.0: PsolaShifter kept but NOT ON THE SIGNAL PATH — retained
    // so existing test harnesses keep compiling. The active engine is
    // SignalsmithShifter. Remove PsolaShifter entirely in a later clean-up.
    gianni::PsolaShifter       pitchShifter;
    gianni::SignalsmithShifter signalsmithShifter;

    // v1.2: wet/dry retired. Signalsmith processes in-place on the
    // host buffer. CrystalExciter runs post-shifter, shaping the
    // final output. DelayLine + wetBuffer kept for potential future
    // re-wire but dormant on the signal path.
    gianni::DelayLine          dryDelay;       // unused in v1.2
    juce::AudioBuffer<float>   wetBuffer;      // unused in v1.2
    gianni::CrystalExciter     crystalExciter;

    // Pre-allocated mono mix buffer for the detector — sized in
    // prepareToPlay so processBlock never allocates.
    juce::AudioBuffer<float> monoMix;

    // Cached parameter pointers (raw atomic float*) — looked up once
    // in the constructor instead of by string per block.
    std::atomic<float>* pRetuneSpeed  { nullptr };
    std::atomic<float>* pHumanize     { nullptr };
    std::atomic<float>* pKey          { nullptr };
    std::atomic<float>* pScale        { nullptr };
    std::atomic<float>* pVoiceType    { nullptr };
    std::atomic<float>* pEngaged      { nullptr };

public:
    // ---- Live pitch read-outs for the editor ---------------------------
    // Audio thread writes these at the end of each block; editor's
    // timer reads them at ~30 fps to draw the pitch meter. No locking
    // needed because single-writer/multi-reader of float/bool is lock-
    // free on every platform we target.
    std::atomic<float>  liveDetectedHz  { 0.0f };
    std::atomic<float>  liveTargetHz    { 0.0f };
    std::atomic<bool>   liveVoiced      { false };

    // ---- Pitch-target ring (for the editor's scope strip) ------------
    //   Instead of drawing raw audio (which was too busy and scrolled
    //   faster than the eye could parse), we publish the quantiser's
    //   TARGET pitch once every `kPitchDecim` processBlock calls. The
    //   editor renders it as a log-Hz curve that jumps sharply whenever
    //   the scale snaps to a new note — "unsere Keysprünge" rather than
    //   a waveform. 256 entries × ~20 ms push period ≈ 5 s of history.
    //   Unvoiced frames are pushed as 0.0f so silent gaps show a mid-
    //   line rest instead of a frozen last value.
    static constexpr int kPitchRingSize = 256;
    static constexpr int kPitchDecim    = 4;   // every 4th processBlock
    // Two parallel rings pushed in lockstep: `detected` = what the
    // detector heard, `target` = where the quantiser is snapping to.
    // The editor overlays them so the gap between the two reads as
    // "correction amount" — Antares/Melodyne-style visualisation.
    float              pitchDetectedRing[kPitchRingSize] = { };
    float              pitchTargetRing  [kPitchRingSize] = { };
    std::atomic<int>   pitchRingWriteIdx { 0 };
    int                pitchBlockCounter { 0 };

private:

    // Last-applied voice type — so we only reconfigure the detector
    // range when the user actually changes the dropdown, not every
    // block.
    int lastVoiceType { -1 };

    // v1.2.1: preset system removed. Single "Default" program.

    // ---- Telemetry -----------------------------------------------------
    // Runs an internal writer thread that logs per-block DSP state to
    // a CSV file the user can send back for post-mortem analysis. See
    // src/telemetry/Telemetry.h for format and lifecycle notes.
    gianni::Telemetry telemetry;

    // State carried between blocks to compute "flags" (voice-onset,
    // big pitch jump, cpu spike, etc.) without storing full history.
    int64_t samplePos       { 0 };
    double  sessionStartMs  { 0.0 };
    bool    prevVoiced      { false };
    float   prevDetectedHz  { 0.0f };
    float   prevTargetHz    { 0.0f };
    float   cpuMicrosEMA    { 0.0f };  // exponential moving average

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GiannituneAudioProcessor)
};
