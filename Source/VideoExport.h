#pragma once

#include "Project.h"
#include "Lyrics.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace tf
{
/** Makes an MP4 from a rendered mix: the song's own video (lyrics video) when there is one,
    otherwise the lyrics drawn on screen (karaoke or scrolling text). Uses the bundled ffmpeg. */
namespace videoexport
{
    struct Options
    {
        ProjectState state;
        juce::File mixWav, out;
        Lyrics lyrics;
        double lyricsOffset = 0.0;      // + = lyrics later
        float scrollSpeed = 1.0f;
        juce::String title;
        double rangeStart = 0.0, length = 0.0;   // timeline seconds covered by the mix
        int width = 1280, height = 720, fps = 25;
        bool useSourceVideo = true;
    };

    /** Draws one lyrics frame (also used by the tests). t = timeline seconds. */
    void drawLyricsFrame (juce::Graphics&, const Options&, double t, juce::TextLayout& cache, float& cacheWidth);

    /** Blocking (run on a background thread). Returns error text, or empty on success. */
    juce::String render (const Options&, std::function<void (float)> progress, std::atomic<bool>& cancel);
}
}
