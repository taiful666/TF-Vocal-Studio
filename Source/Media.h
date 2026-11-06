#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace tf
{
//==============================================================================
/** Helpers around the bundled ffmpeg.exe (any audio/video format in, audio + video frames out). */
namespace media
{
    juce::File ffmpeg();                         // bundled next to the app (or on PATH while developing)
    bool isVideo (const juce::File&);
    /** A clean name for showing a song (download-site prefixes, ids and underscores removed). */
    juce::String displayName (const juce::File&);
    juce::String audioExtensions();              // "mp3;wav;..."
    juce::String videoExtensions();
    juce::String allPatterns();                  // "*.mp3;*.mp4;..." for file choosers

    struct ProbeInfo { double duration = 0.0; int width = 0, height = 0; bool hasVideo = false, hasAudio = false; };
    ProbeInfo probe (const juce::File&);

    /** Playable audio file for a song: the file itself if it can be decoded directly, else its converted cache. */
    juce::File cacheFor (const juce::File& media);
    bool canReadDirectly (const juce::File&, juce::AudioFormatManager&);

    /** Converts any media file to a 48 kHz stereo WAV. progress 0..1. Returns error text or empty on success. */
    juce::String extractAudio (const juce::File& in, const juce::File& outWav, double sampleRate,
                               std::function<void (float)> progress, std::atomic<bool>& cancel);

    /** Encodes a WAV as a high-quality MP3 (320 kbps) for sharing. Returns error text or empty on success. */
    juce::String encodeMp3 (const juce::File& wav, const juce::File& mp3);
}

//==============================================================================
/** Decodes video frames with ffmpeg on a background thread, kept in step with the song position. */
class VideoFrames : private juce::Thread
{
public:
    VideoFrames();
    ~VideoFrames() override;

    void open (const juce::File& video);
    void close();
    bool isOpen() const noexcept { return file != juce::File(); }

    /** Call from the UI (each frame) with the song position. Returns the frame to show (may be invalid). */
    juce::Image frameAt (double seconds, bool playing);
    double aspect() const noexcept { return aspectRatio; }

private:
    void run() override;
    void restartAt (double seconds);

    juce::File file;
    int w = 1280, h = 720;
    double fps = 25.0, aspectRatio = 16.0 / 9.0;
    juce::CriticalSection lock;
    std::deque<std::pair<double, juce::Image>> queue;
    juce::Image current;
    double currentTime = -1.0;
    std::atomic<double> requestedStart { -1.0 };
    std::atomic<bool> restartFlag { false };
    double lastAsked = -1.0;
    std::unique_ptr<juce::ChildProcess> proc;
    juce::CriticalSection procLock;
    juce::WaitableEvent spaceAvailable;
};

}
