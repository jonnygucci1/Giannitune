#include "DelayLine.h"

namespace gianni
{
    int DelayLine::nextPowerOfTwo (int n) noexcept
    {
        if (n <= 1) return 1;
        int p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    void DelayLine::prepare (int maxDelaySamples, int numChans)
    {
        numChannels = juce::jmax (1, numChans);
        // +1 so we always have room to store one sample ahead of the
        // delay position (write-then-read semantics).
        capacity = nextPowerOfTwo (juce::jmax (2, maxDelaySamples + 1));
        mask     = capacity - 1;
        buffer.assign ((size_t) capacity * (size_t) numChannels, 0.0f);
        writeHead = 0;
        delay     = 0;
    }

    void DelayLine::setDelaySamples (int samples) noexcept
    {
        delay = juce::jlimit (0, capacity - 1, samples);
    }

    void DelayLine::reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeHead = 0;
    }

    void DelayLine::processBlock (juce::AudioBuffer<float>& buf) noexcept
    {
        const int n      = buf.getNumSamples();
        const int nChans = juce::jmin (numChannels, buf.getNumChannels());
        if (n == 0 || nChans == 0 || capacity == 0) return;

        // Store channel pointers once to avoid repeated lookups.
        float* chPtrs[8] = { nullptr };
        const int useChans = juce::jmin (nChans, 8);
        for (int c = 0; c < useChans; ++c)
            chPtrs[c] = buf.getWritePointer (c);

        for (int i = 0; i < n; ++i)
        {
            const int readIdx = (writeHead - delay) & mask;
            for (int c = 0; c < useChans; ++c)
            {
                const int flatW = writeHead * numChannels + c;
                const int flatR = readIdx   * numChannels + c;

                // Temporarily hold the input so we can write-then-read
                // correctly even when delay == 0.
                const float in = chPtrs[c][i];
                buffer[(size_t) flatW] = in;
                chPtrs[c][i] = buffer[(size_t) flatR];
            }
            writeHead = (writeHead + 1) & mask;
        }
    }
}
