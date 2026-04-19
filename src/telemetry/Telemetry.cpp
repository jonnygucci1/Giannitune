#include "Telemetry.h"
#include "../Version.h"
#include <cmath>

namespace gianni
{
    // ------------------------------------------------------------------
    //  Writer thread
    //
    //  Wakes every ~50 ms, drains the SPSC FIFO, writes rows to the
    //  session CSV. Flushes every ~1 s so a crash preserves almost all
    //  the log. When asked to stop, drains one last time and closes.
    // ------------------------------------------------------------------
    class Telemetry::Writer : public juce::Thread
    {
    public:
        Writer (Telemetry&                                      t,
                const juce::File&                               file,
                std::array<TelemetryFrame, Telemetry::kFifoSize>& f,
                juce::AbstractFifo&                             c,
                std::atomic<int>&                               drops)
            : juce::Thread ("GiannituneTelemetry"),
              owner        (t),
              outFile      (file),
              fifo         (f),
              fifoCtl      (c),
              dropCounter  (drops)
        {}

        void run() override
        {
            juce::FileOutputStream os (outFile);
            if (! os.openedOk())
            {
                // Can't open the log file — nothing productive to do.
                // Exit quietly; the plugin still works fine.
                return;
            }
            // Buffer writes for fewer syscalls.
            os.setPosition (outFile.getSize());

            // Header
            writeHeader (os);

            juce::int64 lastFlushMs = juce::Time::getMillisecondCounter();
            int lastLoggedDrops = 0;

            while (! threadShouldExit())
            {
                drainInto (os);

                // Emit a "dropped N frames" marker if any new drops
                // occurred since last time.
                const int drops = dropCounter.load();
                if (drops > lastLoggedDrops)
                {
                    const int delta = drops - lastLoggedDrops;
                    lastLoggedDrops = drops;
                    os << "# " << delta << " frames dropped "
                       << "(fifo overflow, cumulative " << drops << ")\n";
                }

                const auto now = juce::Time::getMillisecondCounter();
                if (now - lastFlushMs >= 1000)
                {
                    os.flush();
                    lastFlushMs = now;
                }
                wait (50);   // sleep 50 ms or until notified
            }

            // Final drain + flush on shutdown.
            drainInto (os);
            os << "# session end\n";
            os.flush();
        }

        // Called from stop() on the main thread to unblock the wait.
        void nudge() { notify(); }

    private:
        void writeHeader (juce::FileOutputStream& os)
        {
            os << "# Giannitune telemetry v" << kVersion
               << "  (" << kBuildNote << ")\n";
            os << "# session start "
               << juce::Time::getCurrentTime().toString (true, true, true)
               << "\n";
            os << "# flags bits: 0x01 voice_onset, 0x02 voice_offset, "
               << "0x04 big_pitch_jump, 0x08 big_target_jump, "
               << "0x10 clip, 0x20 cpu_spike\n";
            os << "wall_ms,sample_pos,block_size,sr,cpu_us,"
               << "detected_hz,voiced,target_hz,applied_ratio,output_latency,"
               << "retune_speed,humanize,scale_mask,"
               << "rms_in,rms_out,flags\n";
            os.flush();
        }

        // Copy frames out of the fifo and format them into the stream.
        // juce::AbstractFifo's prepareToRead gives us up to two contiguous
        // ranges (ring wrap); we process each.
        void drainInto (juce::FileOutputStream& os)
        {
            int start1, size1, start2, size2;
            const int ready = fifoCtl.getNumReady();
            if (ready <= 0) return;

            fifoCtl.prepareToRead (ready, start1, size1, start2, size2);

            for (int i = 0; i < size1; ++i)
                emitRow (os, fifo[(size_t) (start1 + i)]);
            for (int i = 0; i < size2; ++i)
                emitRow (os, fifo[(size_t) (start2 + i)]);

            fifoCtl.finishedRead (ready);
        }

        static void emitRow (juce::FileOutputStream& os, const TelemetryFrame& f)
        {
            // Fixed precision keeps rows parseable. We print enough
            // digits to distinguish cents-level pitch differences
            // (4 decimals on Hz).
            os << juce::String (f.wallMs, 3)       << ","
               << (juce::int64) f.samplePos        << ","
               << f.blockSize                      << ","
               << juce::String ((double) f.sampleRate, 1) << ","
               << juce::String ((double) f.cpuMicros, 1)  << ","
               << juce::String ((double) f.detectedHz, 4) << ","
               << (f.voiced ? 1 : 0)               << ","
               << juce::String ((double) f.targetHz, 4)   << ","
               << juce::String ((double) f.appliedRatio, 6)<< ","
               << f.outputLatency                  << ","
               << juce::String ((double) f.retuneSpeed, 2)<< ","
               << juce::String ((double) f.humanize, 2)   << ","
               << (int) f.scaleMask                << ","
               << juce::String ((double) f.rmsIn, 6)      << ","
               << juce::String ((double) f.rmsOut, 6)     << ","
               << (int) f.flags
               << "\n";
        }

        Telemetry&                                        owner;
        juce::File                                        outFile;
        std::array<TelemetryFrame, Telemetry::kFifoSize>& fifo;
        juce::AbstractFifo&                               fifoCtl;
        std::atomic<int>&                                 dropCounter;
    };

    // ------------------------------------------------------------------
    Telemetry::Telemetry()  = default;
    Telemetry::~Telemetry() { stop(); }

    // ------------------------------------------------------------------
    void Telemetry::start (double /*sampleRate*/, int /*maxBlockSize*/)
    {
        if (started) return;

        // %APPDATA%/Giannitune/logs/session-YYYYMMDD-HHMMSS.csv on Windows.
        const auto logsDir = juce::File::getSpecialLocation (
                                juce::File::userApplicationDataDirectory)
                             .getChildFile ("Giannitune")
                             .getChildFile ("logs");
        logsDir.createDirectory();

        const auto now = juce::Time::getCurrentTime();
        const juce::String stamp =
            juce::String::formatted ("session-%04d%02d%02d-%02d%02d%02d.csv",
                                     now.getYear(),
                                     now.getMonth() + 1,
                                     now.getDayOfMonth(),
                                     now.getHours(),
                                     now.getMinutes(),
                                     now.getSeconds());

        sessionFile = logsDir.getChildFile (stamp);

        // Reset FIFO and drop counter. Safe because the writer thread
        // is not running yet.
        fifoCtl.reset();
        droppedFrames.store (0);

        writer = std::make_unique<Writer> (*this, sessionFile,
                                           fifo, fifoCtl, droppedFrames);
        writer->startThread();
        started = true;
    }

    // ------------------------------------------------------------------
    void Telemetry::stop()
    {
        if (! started) return;

        if (writer != nullptr)
        {
            writer->signalThreadShouldExit();
            writer->nudge();
            writer->stopThread (2000);
            writer.reset();
        }
        started = false;
    }

    // ------------------------------------------------------------------
    //  AUDIO-THREAD push — non-blocking, no allocations, no locks.
    //  If the ring is full we bump the drop counter and return.
    // ------------------------------------------------------------------
    void Telemetry::push (const TelemetryFrame& frame) noexcept
    {
        int start1, size1, start2, size2;
        fifoCtl.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            fifo[(size_t) start1] = frame;
            fifoCtl.finishedWrite (1);
        }
        else
        {
            droppedFrames.fetch_add (1, std::memory_order_relaxed);
            fifoCtl.finishedWrite (0);
        }
    }
}
