# ---------------------------------------------------------------------------
#  External dependencies — pulled via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# Put everything under libs/ so the root stays clean
set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/libs CACHE PATH "" FORCE)

# ---------------------------------------------------------------------------
#  JUCE 8  (GPLv3 while developing, commercial license needed to ship closed)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4           # pinned stable
    GIT_SHALLOW    TRUE
)

# ---------------------------------------------------------------------------
#  Signalsmith Stretch  (MIT — free for commercial use)
#  Header-only C++11 library for real-time pitch & time stretching.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

message(STATUS "Fetching JUCE ...")
FetchContent_MakeAvailable(juce)

message(STATUS "Fetching Signalsmith Stretch ...")
FetchContent_MakeAvailable(signalsmith_stretch)
