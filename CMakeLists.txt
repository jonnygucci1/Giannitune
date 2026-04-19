cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
#  macOS build targets — Universal Binary (arm64 + x86_64)
#  so the plugin runs natively on both Apple Silicon (M1/M2/M3/M4) and
#  Intel Macs. Must be set BEFORE project() to apply correctly.
#  Minimum macOS 10.13 covers all modern Logic Pro / Ableton versions.
# ---------------------------------------------------------------------------
if (APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64"
        CACHE STRING "macOS Universal Binary architectures" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13"
        CACHE STRING "Minimum macOS SDK" FORCE)
endif()

# ---------------------------------------------------------------------------
#  Giannitune — Real-time vocal pitch correction plugin
#  Inspired by Antares Auto-Tune EFX 3, modernized for 2026.
# ---------------------------------------------------------------------------

project(Giannitune
    VERSION     1.2.1
    DESCRIPTION "Real-time vocal pitch correction"
    LANGUAGES   C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all build outputs in predictable places
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Silence JUCE deprecation noise from the compiler
if (MSVC)
    add_compile_options(/W3 /MP)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
#  Dependencies
# ---------------------------------------------------------------------------
include(cmake/FetchDependencies.cmake)

# ---------------------------------------------------------------------------
#  Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(Giannitune
    COMPANY_NAME              "Gianni Audio"
    COMPANY_WEBSITE           ""
    COMPANY_EMAIL             ""
    PLUGIN_MANUFACTURER_CODE  Gian          # 4 chars, must start uppercase
    PLUGIN_CODE               Gitu          # unique 4-char plugin ID
    PRODUCT_NAME              "Giannitune"
    BUNDLE_ID                 com.gianni.giannitune
    # v1.2.1: AU added for macOS (Logic Pro requirement). Windows builds
    # silently skip AU — it's macOS-only in JUCE's format list.
    FORMATS                   VST3 AU Standalone
    VST3_CATEGORIES           Fx Pitch Shift
    AU_MAIN_TYPE              kAudioUnitType_Effect
    AU_EXPORT_PREFIX          GiannituneAU
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE          # auto-install to user VST3 dir
    VST3_AUTO_MANIFEST        FALSE)

juce_generate_juce_header(Giannitune)

# ---------------------------------------------------------------------------
#  Embedded fonts (editorial-minimal design system v1.0)
#    Fraunces Italic 300  — wordmark + key-letter in the ring
#    Inter Tight Medium   — VOICE pill group
#    JetBrains Mono Med.  — captions, values, meta text
#    Fonts are SIL OFL; redistribution inside a binary is permitted.
# ---------------------------------------------------------------------------
juce_add_binary_data(GiannituneFonts
    NAMESPACE GiannituneFonts
    SOURCES
        resources/fonts/Fraunces-Italic.ttf
        resources/fonts/InterTight-Medium.ttf
        resources/fonts/JetBrainsMono-Medium.ttf
)

target_link_libraries(Giannitune PRIVATE GiannituneFonts)

# ---------------------------------------------------------------------------
#  Sources
# ---------------------------------------------------------------------------
target_sources(Giannitune PRIVATE
    src/PluginProcessor.cpp
    src/PluginProcessor.h
    src/PluginEditor.cpp
    src/PluginEditor.h

    src/params/Parameters.cpp
    src/params/Parameters.h

    src/dsp/PitchDetector.cpp
    src/dsp/PitchDetector.h
    src/dsp/ScaleQuantizer.cpp
    src/dsp/ScaleQuantizer.h
    src/dsp/PitchShifter.cpp
    src/dsp/PitchShifter.h
    src/dsp/PsolaShifter.cpp
    src/dsp/PsolaShifter.h
    src/dsp/SignalsmithShifter.cpp
    src/dsp/SignalsmithShifter.h
    src/dsp/DelayLine.cpp
    src/dsp/DelayLine.h
    src/dsp/CrystalExciter.cpp
    src/dsp/CrystalExciter.h

    src/gui/GianniLookAndFeel.cpp
    src/gui/GianniLookAndFeel.h

    src/telemetry/Telemetry.cpp
    src/telemetry/Telemetry.h
)

target_include_directories(Giannitune PRIVATE
    src
    ${signalsmith_stretch_SOURCE_DIR}
)

target_compile_definitions(Giannitune PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_MODAL_LOOPS_PERMITTED=0
)

target_link_libraries(Giannitune PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_gui_extra
    signalsmith-linear           # transitively provides stft.h include path
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
#  Standalone DSP test executable
#
#  Lets us run PitchShifter / PitchDetector / ScaleQuantizer through
#  synthetic input from the command line, write WAVs, and inspect them.
#  Way faster iteration than asking the user to test every PSOLA tweak
#  in their DAW.
# ---------------------------------------------------------------------------
# Optional diagnostic binary. Only built if test/dsp_test.cpp exists
# (ie. internal dev tree). Public build (clone from GitHub) does not
# include test/ and CMake will simply skip this target.
if (EXISTS "${CMAKE_SOURCE_DIR}/test/dsp_test.cpp")
    add_executable(Giannitune_DspTest
        test/dsp_test.cpp
        src/dsp/PitchShifter.cpp
        src/dsp/PsolaShifter.cpp
        src/dsp/SignalsmithShifter.cpp
        src/dsp/PitchDetector.cpp
        src/dsp/ScaleQuantizer.cpp
        src/dsp/DelayLine.cpp
        src/dsp/CrystalExciter.cpp
        src/telemetry/Telemetry.cpp
    )
    target_include_directories(Giannitune_DspTest PRIVATE
        src ${signalsmith_stretch_SOURCE_DIR})
    target_compile_definitions(Giannitune_DspTest PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(Giannitune_DspTest PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        signalsmith-linear
      PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    if (MSVC)
        target_link_options(Giannitune_DspTest PRIVATE /STACK:8388608)
    endif()
endif()

# Standalone Signalsmith micro-test — also internal-only.
if (EXISTS "${CMAKE_SOURCE_DIR}/tools/signalsmith_config_sweep.cpp")
    add_executable(Giannitune_SignalsmithTest
        tools/signalsmith_config_sweep.cpp
    )
    target_include_directories(Giannitune_SignalsmithTest
        PRIVATE ${signalsmith_stretch_SOURCE_DIR})
    target_link_libraries(Giannitune_SignalsmithTest PRIVATE signalsmith-linear)
endif()

# Standalone end-to-end pitch-fidelity check (YIN + Quantizer + Signalsmith).
# Diagnostic binary. Only built if tools/pipeline_pitch_test.cpp exists
# and opt-in via -DGIANNITUNE_BUILD_PIPELINE_TEST=ON. Not needed for
# the main plugin — configure will silently skip if unavailable.
option(GIANNITUNE_BUILD_PIPELINE_TEST
    "Build the standalone end-to-end pipeline-pitch diagnostic tool" OFF)
if (GIANNITUNE_BUILD_PIPELINE_TEST
    AND EXISTS "${CMAKE_SOURCE_DIR}/tools/pipeline_pitch_test.cpp")
    add_executable(Giannitune_PipelinePitchTest
        tools/pipeline_pitch_test.cpp
        src/dsp/PitchDetector.cpp
        src/dsp/ScaleQuantizer.cpp
        src/dsp/SignalsmithShifter.cpp
    )
    target_include_directories(Giannitune_PipelinePitchTest PRIVATE
        src
        ${signalsmith_stretch_SOURCE_DIR}
    )
    target_link_libraries(Giannitune_PipelinePitchTest PRIVATE
        juce::juce_audio_basics
        juce::juce_core
        signalsmith-linear
      PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
endif()
