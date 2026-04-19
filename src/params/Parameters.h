#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace gianni
{
    // -----------------------------------------------------------------
    //  Parameter IDs — one source of truth. Any string referenced from
    //  the processor, editor, or host-automation list lives here.
    // -----------------------------------------------------------------
    namespace pid
    {
        inline constexpr const char* tracking      = "tracking";
        inline constexpr const char* voiceType     = "voice_type";
        inline constexpr const char* key           = "key";
        inline constexpr const char* scale         = "scale";
        inline constexpr const char* retuneSpeed   = "retune_speed";
        inline constexpr const char* humanize      = "humanize";
        inline constexpr const char* liveMode      = "live_mode";
        inline constexpr const char* engaged       = "engaged";

        inline constexpr const char* motionEnabled = "motion_enabled";
        inline constexpr const char* motionPattern = "motion_pattern";
        inline constexpr const char* motionRate    = "motion_rate";
        inline constexpr const char* motionSync    = "motion_sync";
    }

    // Enum choices — keep indices stable across versions, or writing
    // automation will break for users.
    enum class Tracking  : int { Normal = 0, Choosy = 1 };
    enum class VoiceType : int { Soprano = 0, AltoTenor = 1, LowMale = 2, Instrument = 3, BassInstrument = 4 };
    enum class Scale     : int { Major = 0, Minor = 1, Chromatic = 2 };

    // -----------------------------------------------------------------
    //  Scale masks. Bit i of the 12-bit mask is set when pitch-class i
    //  (0 = C, 1 = C#, ..., 11 = B) is active. The *template* masks
    //  below are rooted at C; rotate them with rotateScale() to apply
    //  the user's key.
    // -----------------------------------------------------------------
    namespace scales
    {
        // Major:         C D E F G A B       (W W H W W W H)
        //                bits 0 2 4 5 7 9 11
        inline constexpr uint16_t kMajor    = 0b0000'1010'1011'0101;   // 0x0AB5

        // Natural minor: C D Eb F G Ab Bb    (W H W W H W W)
        //                bits 0 2 3 5 7 8 10
        inline constexpr uint16_t kMinor    = 0b0000'0101'1010'1101;   // 0x05AD

        // Chromatic: every semitone
        inline constexpr uint16_t kChromatic = 0b0000'1111'1111'1111;  // 0x0FFF
    }

    // Rotate a 12-bit scale mask by `keySemitones` (0 = C, 1 = C#, ...
    // 11 = B). Returns a new 12-bit mask with the scale rooted at the
    // given key.
    inline uint16_t rotateScale (uint16_t mask, int keySemitones) noexcept
    {
        keySemitones = ((keySemitones % 12) + 12) % 12;
        const uint32_t m32 = (uint32_t) mask & 0x0FFFu;
        const uint32_t rot = ((m32 << keySemitones) | (m32 >> (12 - keySemitones)));
        return (uint16_t) (rot & 0x0FFFu);
    }

    // Build the active pitch-class mask for the current key/scale
    // combination. This is what ScaleQuantizer consumes.
    inline uint16_t computeScaleMask (int keySemitones, Scale scale) noexcept
    {
        uint16_t base = scales::kMajor;
        switch (scale)
        {
            case Scale::Major:     base = scales::kMajor;     break;
            case Scale::Minor:     base = scales::kMinor;     break;
            case Scale::Chromatic: base = scales::kChromatic; break;
        }
        return rotateScale(base, keySemitones);
    }

    // Auto-Motion patterns — match what EFX 3 shipped. Names are
    // persisted, so don't rename without a migration.
    inline const juce::StringArray kMotionPatterns {
        "SyncoJump 1", "SyncoJump 2", "SyncoJump 3",
        "Octaves Up",  "Octaves Down",
        "Fifths",      "Triad",       "ArpUp",   "ArpDown"
    };

    inline const juce::StringArray kMotionRates {
        "1/4", "1/8", "1/16", "1/32"
    };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
}
