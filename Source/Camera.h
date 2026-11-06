#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace tf
{
/** Webcam (or a phone used as a webcam over USB, e.g. DroidCam / Iriun) through the bundled ffmpeg.
    One ffmpeg process owns the camera: it always writes a small live picture (for the preview on the
    stage) and, while recording, also a high-quality H.264 video. The wall-clock time of the first
    frame is kept, so the video lines up with the song. Windows (DirectShow) only. */
namespace camera
{
    /** Video capture devices ffmpeg can see. Empty on other systems or when none is connected. */
    juce::StringArray devices();
    /** One still picture from the camera (for checking the framing in the settings). */
    juce::Image snapshot (const juce::String& device, juce::String& error);
    /** ffmpeg input arguments for a DirectShow camera. */
    juce::StringArray dshowInput (const juce::String& device, bool fixedSize);

    class Session : private juce::Thread
    {
    public:
        using InputArgs = std::function<juce::StringArray (bool fixedSize)>;
        Session();
        ~Session() override;

        /** Starts the camera: the live picture goes to `preview` (a JPEG updated ~10x per second);
            if `record` is not empty the video is recorded there too (.ts). Returns an error, or empty. */
        juce::String start (InputArgs input, const juce::File& preview, const juce::File& record = {});
        /** Stops; a recording stays playable. Returns the wall-clock time of the first frame (0 if unknown). */
        double stop();
        bool isRunning() const noexcept   { return running.load(); }
        bool isRecording() const noexcept { return running.load() && recordFile != juce::File(); }
        bool hasFailed() const noexcept   { return failed.load(); }
        juce::File getPreviewFile() const { return previewFile; }
        juce::File getRecordFile() const  { return recordFile; }
        juce::String getLog() const;

    private:
        void run() override;
        bool launch (bool fixedSize);
        InputArgs input;
        std::unique_ptr<juce::ChildProcess> proc;
        juce::CriticalSection procLock, lock;
        juce::File previewFile, recordFile;
        std::atomic<double> firstFrameWall { 0.0 };
        std::atomic<bool> running { false }, failed { false };
        double startedWall = 0.0;
        bool usedFixedSize = true;
        juce::String log;
    };

    /** The live camera picture on the stage (reads the session's preview picture). */
    class Preview : public juce::Component, private juce::Timer
    {
    public:
        explicit Preview (const Session& s) : session (s) { setInterceptsMouseClicks (false, false); }
        void setActive (bool on);
        void paint (juce::Graphics&) override;
        bool hasPicture() const { return picture.isValid(); }
    private:
        void timerCallback() override;
        const Session& session;
        juce::Image picture;
        juce::Time lastModified;
        int staleTicks = 0;
    };
}
}
