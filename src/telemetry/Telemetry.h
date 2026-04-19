#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <array>

namespace gianni
{
    // ----------------------------------------------------------------------
    //  Telemetry — real-time safe DSP state logger
    //
    //  Writes a CSV-format log file of every processed block, so that
    //  after a user recording session we can read back EXACTLY what the
    //  DSP state was at every moment, correlate with user reports (e.g.
    //  "I heard a click at 0:34"), and measure performance.
    //
    //  Architecture (single-producer, single-consumer):
    //
    //      Audio thread               Writer thread (background)
    //      ─────────────              ──────────────────────────
    //      push(frame)  ──>  FIFO  <──  drain + write to file
    //                                   (wakes every 50 ms)
    //
    //  The FIFO is a lock-free ring buffer (juce::AbstractFifo). The
    //  audio-thread `push` never allocates, never locks, never blocks.
    //  If the FIFO is full, the frame is dropped — we prefer silent
    //  data loss over xruns. A drop counter is kept so the writer can
    //  log "(N frames dropped)" markers.
    //
    //  File location: $APPDATA/Giannitune/logs/session-YYYYMMDD-HHMMSS.csv
    //  on Windows, equivalent on macOS/Linux via JUCE's application-data
    //  special location. Each plugin instance opens its own file with a
    //  unique session timestamp so reopening the plugin doesn't clobber
    //  previous logs.
    //
    //  Columns (v0.4.0):
    //    wall_ms, sample_pos, block_size, sr, cpu_us,
    //    detected_hz, voiced, target_hz, applied_ratio, output_latency,
    //    retune_speed, humanize, scale_mask,
    //    rms_in, rms_out, flags
    //
    //  v0.4.0 added columns:
    //    applied_ratio   — the transpose factor actually passed to the
    //                      Signalsmith library this block. Equals
    //                      target_hz / detected_hz during voiced,
    //                      1.0 during unvoiced. Lets us see at-a-glance
    //                      whether the engine saw the same pitch move
    //                      the detector/quantizer computed.
    //    output_latency  — the engine's reported delay (samples).
    //                      Constant in steady state; logged so we can
    //                      correlate a WAV recording back to log rows.
    //
    //  "flags" is a bitmask of transient events observed this block:
    //    0x01 VOICE_ONSET     — voiced went false → true this block
    //    0x02 VOICE_OFFSET    — voiced went true  → false
    //    0x04 BIG_PITCH_JUMP  — detectedHz changed by > 1 octave
    //    0x08 BIG_TARGET_JUMP — targetHz changed by > 1 semitone
    //    0x10 CLIP            — output sample magnitude ≥ 0.99
    //    0x20 CPU_SPIKE       — cpu_us > 2 × moving average
    //
    //  "flags" is cheap to compute on the audio thread and turns into a
    //  grep-friendly CSV column.
    // ----------------------------------------------------------------------
    struct TelemetryFrame
    {
        double  wallMs       = 0.0;      // since session start
        int64_t samplePos    = 0;        // cumulative input sample index
        int32_t blockSize    = 0;
        float   sampleRate   = 0.0f;
        float   cpuMicros    = 0.0f;
        float   detectedHz   = 0.0f;
        bool    voiced       = false;
        float   targetHz     = 0.0f;
        float   appliedRatio = 1.0f;     // v0.4.0: actual transpose factor
        int32_t outputLatency= 0;        // v0.4.0: engine-reported samples
        float   retuneSpeed  = 0.0f;
        float   humanize     = 0.0f;
        uint16_t scaleMask   = 0;
        float   rmsIn        = 0.0f;
        float   rmsOut       = 0.0f;
        uint32_t flags       = 0;
    };

    // Event flag bits (kept here so the dsp_test log-analyzer can read
    // the same header from the CSV and explain them).
    namespace telemetry_flags
    {
        constexpr uint32_t voiceOnset     = 0x01;
        constexpr uint32_t voiceOffset    = 0x02;
        constexpr uint32_t bigPitchJump   = 0x04;
        constexpr uint32_t bigTargetJump  = 0x08;
        constexpr uint32_t clip           = 0x10;
        constexpr uint32_t cpuSpike       = 0x20;
    }

    class Telemetry
    {
    public:
        Telemetry();
        ~Telemetry();

        // Opens the log file and starts the writer thread. Safe to call
        // multiple times (subsequent calls are no-ops).
        void start (double sampleRate, int maxBlockSize);

        // Stops the writer thread and closes the log file.
        void stop();

        // AUDIO THREAD ONLY: enqueue one frame. Never allocates, never
        // locks. If the FIFO is full the frame is dropped and a counter
        // is bumped.
        void push (const TelemetryFrame& frame) noexcept;

        // Returns the path of the currently open session log, or an
        // empty file if not started.
        juce::File getSessionFile() const noexcept { return sessionFile; }

        // Returns the number of frames dropped because the FIFO was full.
        // Atomic read, safe from any thread.
        int getDroppedCount() const noexcept { return droppedFrames.load(); }

    private:
        // Ring buffer of TelemetryFrame. Size is a power of two so the
        // index masking is cheap.
        static constexpr int kFifoSize = 4096;
        std::array<TelemetryFrame, kFifoSize> fifo {};
        juce::AbstractFifo                    fifoCtl { kFifoSize };
        std::atomic<int>                      droppedFrames { 0 };

        // Background writer thread.
        class Writer;
        std::unique_ptr<Writer> writer;

        juce::File  sessionFile;
        bool        started { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Telemetry)
    };
}
