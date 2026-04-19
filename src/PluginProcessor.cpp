#include "PluginProcessor.h"
#include "PluginEditor.h"

// v1.2.1: preset system removed.
// Previously: 9 hardcoded presets (Hard Tune / Travis Scott / etc).
// User confirmed 2026-04-18 presets were never used. Host-level
// "Default" program behaviour is kept (single entry, derives from
// APVTS defaults / last_used.xml).

// ---------------------------------------------------------------------------
//  User-defaults persistence
//
//  When a user inserts a FRESH Giannitune instance on a track (no saved
//  state), JUCE restores APVTS defaults: Key=C, Scale=Major, RS=25,
//  Humanize=25. This is wrong for anyone who consistently sings in a
//  different key — the user has to dial Key/Scale/etc. every time they
//  add a new instance, and the first few seconds of every session are
//  processed with the wrong settings (confirmed in v0.2.5 telemetry:
//  first 10 s of session-20260410-180837.csv were under mask 2741 =
//  C major, not the intended C# minor).
//
//  Fix: after every parameter change, dump the current "live" values
//  to `%APPDATA%/Giannitune/last_used.xml`. On construction, load that
//  file (if present) and apply its values to APVTS before any host
//  interaction. The user's preferred Key/Scale/Retune/Humanize/Voice-
//  Type travel across plugin instances.
//
//  This only kicks in for FRESH inserts — when the host restores saved
//  state via setStateInformation, that takes precedence (the project's
//  own value wins over the user-global default).
// ---------------------------------------------------------------------------
namespace
{
    juce::File userDefaultsFile()
    {
        return juce::File::getSpecialLocation (
                   juce::File::userApplicationDataDirectory)
                .getChildFile ("Giannitune")
                .getChildFile ("last_used.xml");
    }

    void applyUserDefaults (juce::AudioProcessorValueTreeState& apvts)
    {
        const auto file = userDefaultsFile();
        if (! file.existsAsFile()) return;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr) return;

        const juce::StringArray persistedParams {
            gianni::pid::key,
            gianni::pid::scale,
            gianni::pid::retuneSpeed,
            gianni::pid::humanize,
            gianni::pid::voiceType
        };

        for (const auto& pname : persistedParams)
        {
            if (auto* p = apvts.getParameter (pname))
            {
                const auto value = xml->getDoubleAttribute (pname,
                                                            (double) p->getValue());
                p->setValueNotifyingHost ((float) value);
            }
        }
    }

    void saveUserDefaults (juce::AudioProcessorValueTreeState& apvts)
    {
        const juce::StringArray persistedParams {
            gianni::pid::key,
            gianni::pid::scale,
            gianni::pid::retuneSpeed,
            gianni::pid::humanize,
            gianni::pid::voiceType
        };

        juce::XmlElement xml ("GiannituneLastUsed");
        for (const auto& pname : persistedParams)
        {
            if (auto* p = apvts.getParameter (pname))
                xml.setAttribute (pname, (double) p->getValue());
        }

        const auto file = userDefaultsFile();
        file.getParentDirectory().createDirectory();
        xml.writeTo (file);
    }
}

// ---------------------------------------------------------------------------
//  GiannituneAudioProcessor
// ---------------------------------------------------------------------------
GiannituneAudioProcessor::GiannituneAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", gianni::createParameterLayout())
{
    pRetuneSpeed = apvts.getRawParameterValue(gianni::pid::retuneSpeed);
    pHumanize    = apvts.getRawParameterValue(gianni::pid::humanize);
    pKey         = apvts.getRawParameterValue(gianni::pid::key);
    pScale       = apvts.getRawParameterValue(gianni::pid::scale);
    pVoiceType   = apvts.getRawParameterValue(gianni::pid::voiceType);
    pEngaged     = apvts.getRawParameterValue(gianni::pid::engaged);

    // User-defaults load: applies the last-used Key/Scale/etc. to
    // APVTS on fresh insert. If a host restores project state via
    // setStateInformation() afterwards, THAT value wins (as it
    // should — the project's saved settings beat global defaults).
    applyUserDefaults (apvts);

    // Register as a listener on the persisted parameters so we can
    // save the user's last-used values whenever they change. We use
    // a throttled save (via AsyncUpdater) so rapid slider drags
    // don't thrash the filesystem.
    apvts.addParameterListener (gianni::pid::key,         this);
    apvts.addParameterListener (gianni::pid::scale,       this);
    apvts.addParameterListener (gianni::pid::retuneSpeed, this);
    apvts.addParameterListener (gianni::pid::humanize,    this);
    apvts.addParameterListener (gianni::pid::voiceType,   this);
}

GiannituneAudioProcessor::~GiannituneAudioProcessor()
{
    apvts.removeParameterListener (gianni::pid::key,         this);
    apvts.removeParameterListener (gianni::pid::scale,       this);
    apvts.removeParameterListener (gianni::pid::retuneSpeed, this);
    apvts.removeParameterListener (gianni::pid::humanize,    this);
    apvts.removeParameterListener (gianni::pid::voiceType,   this);
}

void GiannituneAudioProcessor::parameterChanged (const juce::String& /*id*/,
                                                 float /*newValue*/)
{
    // Throttle via the AsyncUpdater: parameter changes can fire
    // hundreds of times per second on a slider drag, but we only
    // need to save once per idle moment.
    triggerAsyncUpdate();
}

void GiannituneAudioProcessor::handleAsyncUpdate()
{
    saveUserDefaults (apvts);
}

void GiannituneAudioProcessor::prepareToPlay (double sampleRate, int maxBlockSize)
{
    pitchDetector.prepare(sampleRate, maxBlockSize);

    // v0.3.1: tell the shifter the max period we expect based on
    // the user's current Voice Type selection. This lets prepare()
    // compute a tighter kLatency for narrow voice types. Narrower
    // voice = smaller max period = lower plugin latency.
    //
    // Voice Type → min expected frequency:
    //   Soprano         200 Hz
    //   Alto/Tenor      130 Hz  (default)
    //   Low Male        100 Hz
    //   Instrument      100 Hz
    //   Bass Instrument 100 Hz
    const int initialVoiceType = pVoiceType != nullptr
        ? (int) pVoiceType->load()
        : (int) gianni::VoiceType::AltoTenor;
    const float voiceFMin[] = { 200.0f, 130.0f, 100.0f, 100.0f, 100.0f };
    const float fMin = voiceFMin[juce::jlimit(0, 4, initialVoiceType)];
    const int maxPeriodForVoice = (int) std::ceil (sampleRate / (double) fMin);
    pitchShifter.setMaxExpectedPeriod(maxPeriodForVoice);

    // v0.3.4: Live Mode reverted to same PSOLA as Studio. The
    // experimental PSOLA-Lite (v0.3.3 short grains + double
    // placement) caused fundamental cancellation and weaker effect.
    // Live Mode toggle is kept but currently does not change the
    // DSP — both paths use full PSOLA. The parameter remains for
    // future use once a correct low-latency variant is developed.
    pitchShifter.setGrainFraction(1.0f);  // always full grains

    // v0.3.8: grain-length cap + pitch-invariant period smoother.
    //
    // User reported persistent clicking/kratzen on real Bass vocal in
    // v0.3.7, worst on Low Male voice type (correct fundamental
    // detection), cleanest on Soprano (harmonic-locked, short grains).
    // Pattern: kratzen scales with GRAIN LENGTH. At 108 Hz,
    // classic PSOLA uses grain half = 408 samples (18.5 ms @ 44.1 k);
    // fast target-period changes (per-block scale snaps at RS=0) then
    // produce OLA phase misalignment within long grains that the
    // legacy 1 ms period-smoother cannot absorb (it's effectively
    // alpha ≈ 1 at low pitches — a no-op).
    //
    // Offline psola-ab measurements on user's take at RS=0:
    //   classic       : d>.10 = 156,985 samples
    //   cap128+sm10   : d>.10 = 127,872 samples (−19 %)
    //   cap96+sm10    : d>.10 = 122,625 samples (−22 %)
    //
    // grainHalfCap = 128 bounds grain at 256 samples total length
    // (5.8 ms @ 44.1 k). For high pitches (tau < 128) classic PSOLA
    // is unchanged. For low pitches (tau > 128) grains are shorter
    // than one period — there's a gap between adjacent grains that
    // falls back to delayed raw input. Net effect: partial correction
    // on low-pitch content (wet/dry blend per period), full correction
    // on high-pitch content. At small shifts (108 → 110 Hz) the
    // fallback is very close to the grain so the blend is inaudible.
    //
    // periodSmootherPeriods = 5 sets the smoother time constant to 5
    // input periods — 46 ms at 108 Hz, 5 ms at 1000 Hz. alpha ≈ 0.18
    // at any pitch, giving real per-grain smoothing regardless of
    // fundamental frequency. Absorbs ~80 % of a target-period step
    // within 5 grains (~45 ms @ 108 Hz) — well under the Antares
    // retune-curve floor of 5 ms set by the quantizer, so the overall
    // retune character is preserved.
    pitchShifter.setGrainHalfCap(128);
    pitchShifter.setPeriodSmootherPeriods(5.0f);

    pitchShifter.prepare(sampleRate, maxBlockSize, /*numChannels*/ 2);

    // v0.4.0: SignalsmithShifter is the active engine. PsolaShifter
    // above is prepared for legacy test-harness compatibility but
    // never called in processBlock — we call signalsmithShifter.
    signalsmithShifter.setSplitComputation(true);
    signalsmithShifter.prepare(sampleRate, maxBlockSize, /*numChannels*/ 2);

    quantizer   .prepare(sampleRate);

    monoMix.setSize(1, maxBlockSize, false, true, true);
    monoMix.clear();

    // v1.2: wet/dry infrastructure retired. Signalsmith processes the
    // main buffer in-place; the only latency reported to the host is
    // the shifter's own STFT latency. DelayLine + wetBuffer retained
    // as members but not wired into the signal path — may come back
    // for a future hybrid or parallel-effect design.
    const int ssLatency = signalsmithShifter.getLatencySamples();
    crystalExciter.prepare (sampleRate, maxBlockSize, /*channels*/ 2);
    crystalExciter.reset();

    setLatencySamples (ssLatency);

    // Session state reset.
    samplePos      = 0;
    prevVoiced     = false;
    prevDetectedHz = 0.0f;
    prevTargetHz   = 0.0f;
    cpuMicrosEMA   = 0.0f;
    sessionStartMs = juce::Time::getMillisecondCounterHiRes();

    // Force voice-type re-apply on next processBlock (the lastVoiceType
    // cache might be stale from before prepareToPlay, especially after
    // a sample-rate change which affects the tau↔Hz conversion).
    lastVoiceType = -1;

    // Start the telemetry writer. Opens the CSV log file and launches
    // the background writer thread. Safe to call multiple times (no-op
    // after the first).
    telemetry.start (sampleRate, maxBlockSize);
}

bool GiannituneAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void GiannituneAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals _;

    // --- CPU timer start (high-resolution counter in microseconds) ----
    // Used for telemetry and for the CPU spike flag. Tracking is cheap
    // (two clock reads + one subtraction). Kept even in release because
    // the overhead is under 100 ns and the data is valuable.
    const double cpuStartMs = juce::Time::getMillisecondCounterHiRes();

    const int n        = buffer.getNumSamples();
    const int numChans = buffer.getNumChannels();

    // ---- 0. Engage gate --------------------------------------------
    //   When `engaged` is false (Live pill toggled off) we pass the
    //   input through untouched. Detection + meter atomics stay at
    //   their last values so the GUI gently fades rather than
    //   snapping to zero.
    const bool isEngaged = pEngaged == nullptr
                         || pEngaged->load (std::memory_order_relaxed) >= 0.5f;
    if (! isEngaged)
    {
        // Decay live meter state so the UI reads "idle"
        liveVoiced.store (false, std::memory_order_relaxed);
        return;  // leave buffer untouched
    }

    // ---- 1. Mono-sum into our preallocated mono buffer ---------------
    if (n > monoMix.getNumSamples())
        return;

    float* mono = monoMix.getWritePointer(0);
    if (numChans == 1)
    {
        const float* l = buffer.getReadPointer(0);
        for (int i = 0; i < n; ++i) mono[i] = l[i];
    }
    else
    {
        const float* l = buffer.getReadPointer(0);
        const float* r = buffer.getReadPointer(1);
        for (int i = 0; i < n; ++i) mono[i] = 0.5f * (l[i] + r[i]);
    }

    // Input RMS for telemetry (before any DSP touches the buffer).
    double rmsInSq = 0.0;
    for (int i = 0; i < n; ++i) rmsInSq += (double) mono[i] * (double) mono[i];
    const float rmsIn = (float) std::sqrt (rmsInSq / juce::jmax (1, n));

    // ---- 2. Voice-type range (cheap, only reconfigures on change) -----
    //
    // Voice Type maps to a frequency search range for YIN. Constrains
    // the detector to realistic candidates for the singer, which
    // rejects out-of-range octave errors and improves accuracy in
    // the target range.
    //
    // Current YIN detector has a hard lower floor of ~100 Hz (driven
    // by kTauMaxHard = 480 in PitchDetector — any lower requires a
    // larger analysis window which would increase detector latency).
    // The per-voice ranges below respect this floor; sub-100 Hz
    // support for true basso profundo or bass instruments is
    // backlogged as a separate architectural upgrade.
    //
    //   Soprano:         200-1200 Hz  (female head voice)
    //   Alto / Tenor:    130-900  Hz  (typical mid vocal range)
    //   Low Male:        100-550  Hz  (baritone — user's category)
    //   Instrument:      100-1500 Hz  (broad catch-all)
    //   Bass Instrument: 100-400  Hz  (low instrument, same fMin as
    //                                  Low Male for now)
    const int voiceTypeIdx = pVoiceType != nullptr
        ? (int) pVoiceType->load()
        : (int) gianni::VoiceType::AltoTenor;
    if (voiceTypeIdx != lastVoiceType)
    {
        struct { float minHz; float maxHz; } ranges[] = {
            { 200.0f, 1200.0f },  // Soprano
            { 130.0f,  900.0f },  // Alto / Tenor
            { 100.0f,  550.0f }   // Low Male
        };
        const int idx = juce::jlimit (0, 2, voiceTypeIdx);
        pitchDetector.setPitchRange (ranges[idx].minHz, ranges[idx].maxHz);
        // v1.1: AURA / CrystalExciter is now voice-type-aware — the
        // exciter's two filter bands re-tune to the formant region of
        // the selected register so the brightness lands where the
        // voice actually has spectral content.
        crystalExciter.setVoiceType (idx);
        lastVoiceType = voiceTypeIdx;
    }

    // ---- 3. Run pitch detection --------------------------------------
    pitchDetector.push(mono, n);
    const float detectedHz     = pitchDetector.getPitchHz();
    const float detectedPeriod = pitchDetector.getPeriodSamples();
    const bool  voiced         = pitchDetector.isVoiced();

    // ---- 3. Compute the scale mask from current Key + Scale ---------
    // No more 12 individual note toggles — the user picks a key and
    // scale and we derive the pitch-class mask live. Cheap (lookup +
    // bit rotation) so it's fine to do per block.
    const int   keyIdx    = pKey   != nullptr ? (int) pKey->load()   : 0;
    const int   scaleIdx  = pScale != nullptr ? (int) pScale->load() : 0;
    const auto  scaleEnum = static_cast<gianni::Scale>(juce::jlimit(0, 2, scaleIdx));
    const uint16_t mask   = gianni::computeScaleMask(juce::jlimit(0, 11, keyIdx), scaleEnum);

    // v1.2 parameter semantics:
    //   `retune_speed` APVTS → quantiser τ (ms) smoother (Antares-style)
    //                          APVTS 0 → UI 100 % → τ=5 ms (instant snap)
    //                          APVTS 100 → UI 0 % → τ=144 ms (slow glide)
    //   `humanize`     APVTS → CrystalExciter "AURA" intensity
    //                          APVTS 0 → UI 100 % → full exciter
    //                          APVTS 100 → UI 0 % → exciter bypassed
    //
    // Key change from v1.1 (wet/dry era): humanize is PERMANENTLY
    // disconnected from the quantiser's pitch-LFO path. It only drives
    // the Crystal exciter. This eliminates one of the two ratio-drift
    // sources that made Signalsmith blubber in v1.0. The other source —
    // smoother-induced ratio drift — we're betting the library can
    // handle cleanly now that humanize isn't also modulating it.
    const float rawRetune = juce::jlimit (0.0f, 100.0f,
                              pRetuneSpeed != nullptr ? pRetuneSpeed->load() : 0.0f);
    const float rawAura   = juce::jlimit (0.0f, 100.0f,
                              pHumanize    != nullptr ? pHumanize   ->load() : 0.0f);

    // AURA (UI 0..100 %) → CrystalExciter amount.
    //   APVTS 0   → UI 100 % → auraAmt 1.0 (full exciter)
    //   APVTS 100 → UI 0 %   → auraAmt 0.0 (bypassed)
    const float auraAmt = 1.0f - rawAura * 0.01f;

    quantizer.setScaleMask   (mask);
    quantizer.setRetuneSpeed (rawRetune);  // v1.2: actual τ (not forced to 0)
    quantizer.setHumanize    (0.0f);       // v1.2: LFO permanently off — AURA is exciter-only

    const float targetHz = quantizer.process(detectedHz, n, voiced);

    // ---- 4. Signalsmith pitch-shift (in-place on buffer) -----------------
    //
    // v1.2 reverts to the v1.0 single-path topology: Signalsmith
    // processes the main buffer directly. No wet/dry mix, no separate
    // work buffer, no dry-path delay line. Relies on:
    //   - humanize LFO being OFF (we guarantee it above)
    //   - quantiser τ-smoother producing smoothly-varying target
    //   - Signalsmith's measurement-based phase prediction handling
    //     slowly-varying ratios cleanly
    if (voiced && detectedHz > 0.0f && detectedPeriod > 0.0f)
    {
        const float targetPeriod = (targetHz > 0.0f)
            ? static_cast<float>(getSampleRate() / static_cast<double>(targetHz))
            : detectedPeriod;

        signalsmithShifter.setInputPeriodSamples (detectedPeriod);
        signalsmithShifter.setTargetPeriodSamples (targetPeriod);
        signalsmithShifter.setVoiced (true);
    }
    else
    {
        signalsmithShifter.setVoiced (false);
    }

    signalsmithShifter.process (buffer);

    // ---- 4c. Crystal Exciter (controlled by AURA knob) ------------------
    //   Feeds both the audio AND the quantiser's targetHz so Stage 3
    //   (pitch-locked harmonic synthesis) can generate musical partials.
    crystalExciter.setAmount   (auraAmt);
    crystalExciter.setTargetHz (targetHz);
    crystalExciter.setVoiced   (voiced);
    crystalExciter.processBlock (buffer);

    // ---- 4b. Output soft-limiter (v0.3.5) ---------------------------
    //
    // Transparent below |x| = 0.9 (−0.9 dBFS), smooth tanh-asymptotic
    // approach to 1.0 above. Mathematically:
    //   y = x                                       for |x| < 0.9
    //   y = sgn(x) · (0.9 + 0.1 · tanh((|x|-0.9)/0.1))  else
    // At |x|=0.9 both pieces meet with slope 1 (tanh derivative at 0
    // is 1), so the curve is C¹ continuous and sonically inaudible
    // on normal-level signals. Above 0.9 the ceiling asymptotes to
    // 1.0 but is mathematically unreachable, so we guarantee no
    // digital clipping on the plugin output path.
    //
    // Why here and not inside PsolaShifter: PsolaShifter can, at
    // pathological grain/phase alignments, produce a sample briefly
    // >1.0 through OLA summation — a signal-dependent transient
    // that wouldn't trip any of the compile-time asserts. The
    // limiter catches those *and* any unexpected gain from a future
    // DSP change on the same output path. It's the single
    // truly safe place to put it: after all processing, before the
    // host sees the output.
    //
    // Normal-level signals (user's measured RMS max 0.08, peak ~0.3)
    // sit at least 10 dB below the 0.9 threshold and are bit-exact
    // preserved.
    const int numProcessedChans = buffer.getNumChannels();
    for (int ch = 0; ch < numProcessedChans; ++ch)
    {
        float* d = buffer.getWritePointer(ch);
        for (int i = 0; i < n; ++i)
        {
            const float x = d[i];
            const float a = std::abs(x);
            if (a < 0.9f) continue;   // fast path, no change
            const float sgn    = (x < 0.0f) ? -1.0f : 1.0f;
            const float shaped = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
            d[i] = sgn * shaped;
        }
    }

    // ---- 5. Telemetry ------------------------------------------------
    //
    // Compute output RMS and peak AFTER the shifter has run. These
    // plus the cpu-time measurement plus a flags bitmask are pushed
    // to the lock-free FIFO. The writer thread picks up the frame
    // within 50 ms and appends it to the session CSV.
    double rmsOutSq = 0.0;
    float  peakOut  = 0.0f;
    {
        const float* l = buffer.getReadPointer(0);
        for (int i = 0; i < n; ++i)
        {
            const float s = l[i];
            rmsOutSq += (double) s * (double) s;
            const float a = std::abs (s);
            if (a > peakOut) peakOut = a;
        }
    }
    const float rmsOut = (float) std::sqrt (rmsOutSq / juce::jmax (1, n));

    // ---- Publish target-pitch to the scope ring ---------------------
    //   `targetHz` is what the quantiser decided this block — i.e. the
    //   note we're snapping the voice to. Dropping one value every
    //   `kPitchDecim` processBlocks gives the editor a calm, slow scope
    //   that jumps on every scale-snap and rests at the midline during
    //   silence. Unvoiced blocks push 0.0f so silent gaps don't freeze
    //   the line at its last pitch.
    if (++pitchBlockCounter >= kPitchDecim)
    {
        pitchBlockCounter = 0;
        const int   w = pitchRingWriteIdx.load (std::memory_order_relaxed);
        pitchDetectedRing[w] = voiced ? detectedHz : 0.0f;
        pitchTargetRing  [w] = voiced ? targetHz   : 0.0f;
        pitchRingWriteIdx.store ((w + 1) % kPitchRingSize,
                                  std::memory_order_release);
    }

    const double cpuEndMs = juce::Time::getMillisecondCounterHiRes();
    const float cpuMicros = (float) ((cpuEndMs - cpuStartMs) * 1000.0);

    // Update moving average for spike detection. 30-block tau (~160 ms
    // @48k/256), plenty of time to ride over a transient.
    const float emaCoef = 1.0f / 30.0f;
    cpuMicrosEMA += emaCoef * (cpuMicros - cpuMicrosEMA);

    // Compose flags
    uint32_t flags = 0;
    if (voiced && ! prevVoiced)           flags |= gianni::telemetry_flags::voiceOnset;
    if (! voiced && prevVoiced)           flags |= gianni::telemetry_flags::voiceOffset;
    if (detectedHz > 0.0f && prevDetectedHz > 0.0f)
    {
        const float ratio = detectedHz / prevDetectedHz;
        if (ratio > 2.0f || ratio < 0.5f)
            flags |= gianni::telemetry_flags::bigPitchJump;
    }
    if (targetHz > 0.0f && prevTargetHz > 0.0f)
    {
        const float tratio = targetHz / prevTargetHz;
        if (tratio > 1.06f || tratio < 0.944f)   // > ~1 semitone
            flags |= gianni::telemetry_flags::bigTargetJump;
    }
    if (peakOut >= 0.99f)                 flags |= gianni::telemetry_flags::clip;
    if (cpuMicrosEMA > 1.0f && cpuMicros > 2.0f * cpuMicrosEMA)
        flags |= gianni::telemetry_flags::cpuSpike;

    gianni::TelemetryFrame frame;
    frame.wallMs       = juce::Time::getMillisecondCounterHiRes() - sessionStartMs;
    frame.samplePos    = samplePos;
    frame.blockSize    = n;
    frame.sampleRate   = (float) getSampleRate();
    frame.cpuMicros    = cpuMicros;
    frame.detectedHz   = detectedHz;
    frame.voiced       = voiced;
    frame.targetHz     = targetHz;
    frame.appliedRatio = signalsmithShifter.getLastAppliedRatio();
    frame.outputLatency= signalsmithShifter.getLatencySamples();
    // Telemetry logs the RAW APVTS values (0..100) so CSVs reflect
    // the user's knob positions directly. `retuneSpeed` / `humanize`
    // were renamed to `rawRetune` / `rawAura` in the v1.1 rewire.
    frame.retuneSpeed  = rawRetune;
    frame.humanize     = rawAura;
    frame.scaleMask    = mask;
    frame.rmsIn        = rmsIn;
    frame.rmsOut       = rmsOut;
    frame.flags        = flags;
    telemetry.push (frame);

    // Publish live readings for the editor's pitch meter. Lock-free:
    // single writer (audio thread), many readers (GUI timer).
    liveDetectedHz.store (detectedHz,          std::memory_order_relaxed);
    liveTargetHz  .store (targetHz,            std::memory_order_relaxed);
    liveVoiced    .store (voiced,              std::memory_order_relaxed);

    samplePos     += (int64_t) n;
    prevVoiced     = voiced;
    prevDetectedHz = detectedHz;
    prevTargetHz   = targetHz;
}

juce::AudioProcessorEditor* GiannituneAudioProcessor::createEditor()
{
    return new GiannituneAudioProcessorEditor(*this);
}

void GiannituneAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

void GiannituneAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (xml->hasTagName(apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml(*xml);
            apvts.replaceState(tree);
        }
    }
}

// ---------------------------------------------------------------------------
//  Program management (v1.2.1: presets removed, single host-level "Default")
// ---------------------------------------------------------------------------
int GiannituneAudioProcessor::getNumPrograms()                    { return 1; }
int GiannituneAudioProcessor::getCurrentProgram()                 { return 0; }
void GiannituneAudioProcessor::setCurrentProgram (int)            {}
const juce::String GiannituneAudioProcessor::getProgramName (int) { return "Default"; }

// ---------------------------------------------------------------------------
//  Factory — required by every JUCE plugin
// ---------------------------------------------------------------------------
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GiannituneAudioProcessor();
}
