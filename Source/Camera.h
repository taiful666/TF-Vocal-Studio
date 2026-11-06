#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

namespace tf
{
/** Webcam (or a phone used as a webcam over USB, e.g. DroidCam / Iriun) recorded with the bundled
    ffmpeg as high-quality H.264, alongside the voice. The wall-clock time of the first frame is kept,
    so the video can be lined up with the song exactly. Windows (DirectShow) only. */
namespace camera
{
    /** Video capture devices ffmpeg can see. Empty on other systems or when none is connected. */
    juce::StringArray devices();
    /** One still picture from the camera (for checking the framing in the settings). */
    juce::Image snapshot (const juce::String& device, juce::String& error);

    class Recorder : private juce::Thread
    {
    public:
        Recorder();
        ~Recorder() override;
        /** Starts recording into `file` (.ts). Returns an error text, or empty on success. */
        juce::String start (const juce::String& device, const juce::File& file);
        /** Stops; the file stays playable. Returns the wall-clock time of the first frame (0 if unknown). */
        double stop();
        bool isRecording() const noexcept { return recording; }
        juce::File getFile() const { return file; }
        juce::String getLastError() const;
    private:
        void run() override;
        bool launch (bool fixedSize);
        std::unique_ptr<juce::ChildProcess> proc;
        juce::CriticalSection procLock;
        juce::String deviceName;
        bool usedFixedSize = true, recording = false;
        juce::File file;
        std::atomic<double> firstFrameWall { 0.0 };
        double startedWall = 0.0;
        juce::CriticalSection lock;
        juce::String log;
    };
}
}
