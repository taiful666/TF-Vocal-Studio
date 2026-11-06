cmake_minimum_required(VERSION 3.22)

project(TFVocalStudio VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime: the installed app needs no separate Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# JUCE (pinned version). A local copy can be supplied with -DTF_JUCE_DIR=...
# ---------------------------------------------------------------------------
if(TF_JUCE_DIR)
    add_subdirectory(${TF_JUCE_DIR} juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.5
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# RNNoise (BSD-3-Clause, xiph.org v0.1.1, vendored; VLAs replaced for MSVC)
# ---------------------------------------------------------------------------
add_library(rnnoise STATIC
    third_party/rnnoise/src/denoise.c
    third_party/rnnoise/src/rnn.c
    third_party/rnnoise/src/rnn_data.c
    third_party/rnnoise/src/pitch.c
    third_party/rnnoise/src/kiss_fft.c
    third_party/rnnoise/src/celt_lpc.c)
target_include_directories(rnnoise PUBLIC third_party/rnnoise/include PRIVATE third_party/rnnoise/src)
target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES)
if(MSVC)
    target_compile_options(rnnoise PRIVATE /W0)
else()
    target_compile_options(rnnoise PRIVATE -w)
    set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ---------------------------------------------------------------------------
# Rubber Band Library (GPL-2.0-or-later) — high-quality tempo change, single-file build
# ---------------------------------------------------------------------------
add_library(rubberband STATIC third_party/rubberband/single/RubberBandSingle.cpp)
target_include_directories(rubberband PUBLIC third_party/rubberband)
target_compile_features(rubberband PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(rubberband PRIVATE /W0 /O2)
    target_compile_definitions(rubberband PRIVATE _USE_MATH_DEFINES NOMINMAX)
else()
    target_compile_options(rubberband PRIVATE -w -O2)
endif()

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
juce_add_gui_app(TFVocalStudio
    PRODUCT_NAME "TF Vocal Studio"
    COMPANY_NAME "TF"
    BUNDLE_ID    "com.tf.vocalstudio"
    ICON_BIG     "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
    ICON_SMALL   "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon256.png"
    VERSION      ${PROJECT_VERSION})

juce_add_binary_data(TFVSData
    NAMESPACE TFData
    SOURCES
        assets/fonts/HindSiliguri-Regular.ttf
        assets/fonts/HindSiliguri-Medium.ttf
        assets/fonts/HindSiliguri-SemiBold.ttf
        assets/fonts/HindSiliguri-Bold.ttf
        assets/icon256.png)

target_sources(TFVocalStudio PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/Theme.cpp
    Source/StudioView.cpp
    Source/LibraryView.cpp
    Source/Lyrics.cpp
    Source/VocalFx.cpp
    Source/Media.cpp
    Source/Project.cpp
    Source/Stretch.cpp
    Source/Timeline.cpp
    Source/Inspector.cpp
    Source/Settings.cpp
    Source/VideoExport.cpp
    Source/AudioInput.cpp
    Source/MixTools.cpp)

target_compile_definitions(TFVocalStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:TFVocalStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:TFVocalStudio,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ASIO=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0)

# Bengali text in the sources is UTF-8; make MSVC read it as such.
if(MSVC)
    target_compile_options(TFVocalStudio PRIVATE /utf-8 /MP)
endif()

target_link_libraries(TFVocalStudio
    PRIVATE
        TFVSData
        rnnoise
        rubberband
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Automated engine test (simulated audio device). Off by default.
# ---------------------------------------------------------------------------
option(TF_BUILD_TESTS "Build the automated engine test" OFF)
if(TF_BUILD_TESTS)
    juce_add_console_app(TFEngineTest PRODUCT_NAME "TFEngineTest")
    target_sources(TFEngineTest PRIVATE Tests/EngineTest.cpp Source/AudioEngine.cpp Source/Lyrics.cpp Source/VocalFx.cpp Source/Media.cpp Source/Project.cpp Source/Stretch.cpp
        Source/Theme.cpp Source/VideoExport.cpp Source/AudioInput.cpp Source/MixTools.cpp)
    target_include_directories(TFEngineTest PRIVATE Source Tests third_party)
    if(MSVC)
        target_compile_options(TFEngineTest PRIVATE /utf-8 /MP)
    endif()
    target_compile_definitions(TFEngineTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1)
    target_link_libraries(TFEngineTest PRIVATE TFVSData rnnoise rubberband juce::juce_audio_utils juce::juce_audio_devices juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # UI test: the real main window, buttons pressed by code, simulated sound card
    juce_add_console_app(TFUITest PRODUCT_NAME "TFUITest")
    target_sources(TFUITest PRIVATE Tests/UITest.cpp
        Source/MainComponent.cpp Source/AudioEngine.cpp Source/Theme.cpp Source/StudioView.cpp Source/LibraryView.cpp
        Source/Lyrics.cpp Source/VocalFx.cpp Source/Media.cpp Source/Project.cpp Source/Stretch.cpp Source/Timeline.cpp
        Source/Inspector.cpp Source/Settings.cpp Source/VideoExport.cpp Source/AudioInput.cpp Source/MixTools.cpp)
    target_include_directories(TFUITest PRIVATE Source Tests third_party)
    if(MSVC)
        target_compile_options(TFUITest PRIVATE /utf-8 /MP)
    endif()
    target_compile_definitions(TFUITest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1 JUCE_WASAPI=1 JUCE_DIRECTSOUND=1 JUCE_ASIO=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1 JUCE_APPLICATION_VERSION_STRING="test")
    target_link_libraries(TFUITest PRIVATE TFVSData rnnoise rubberband juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
