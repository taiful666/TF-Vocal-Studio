#include "Camera.h"
#include "Media.h"

namespace tf::camera
{
static juce::String runAndRead (const juce::StringArray& args, int timeoutMs)
{
    juce::ChildProcess cp;
    if (! cp.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return {};
    auto out = cp.readAllProcessOutput();
    cp.waitForProcessToFinish (timeoutMs);
    return out;
}

juce::StringArray devices()
{
    juce::StringArray result;
   #if JUCE_WINDOWS
    const auto exe = media::ffmpeg();
    if (! exe.existsAsFile()) return result;
    const auto out = runAndRead ({ exe.getFullPathName(), "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy" }, 10000);
    for (auto line : juce::StringArray::fromLines (out))
    {
        // [dshow @ ...] "Integrated Webcam" (video)
        if (! line.contains ("(video)")) continue;
        auto name = line.fromFirstOccurrenceOf ("\"", false, false).upToFirstOccurrenceOf ("\"", false, false);
        if (name.isNotEmpty()) result.addIfNotAlreadyThere (name);
    }
   #endif
    return result;
}

juce::Image snapshot (const juce::String& device, juce::String& error)
{
   #if JUCE_WINDOWS
    const auto exe = media::ffmpeg();
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tfvs-camera-test.jpg");
    f.deleteFile();
    runAndRead ({ exe.getFullPathName(), "-hide_banner", "-loglevel", "error", "-y", "-f", "dshow", "-i", "video=" + device,
                  "-frames:v", "1", f.getFullPathName() }, 15000);
    auto img = juce::ImageFileFormat::loadFrom (f);
    if (! img.isValid()) error = juce::String::fromUTF8 ("ক্যামেরা থেকে ছবি পাওয়া যায়নি। অন্য কোনো সফটওয়্যার ক্যামেরা ব্যবহার করছে কিনা দেখুন।");
    f.deleteFile();
    return img;
   #else
    juce::ignoreUnused (device);
    error = "camera not supported";
    return {};
   #endif
}

//==============================================================================
Recorder::Recorder() : juce::Thread ("TF camera") {}
Recorder::~Recorder() { stop(); }

bool Recorder::launch (bool fixedSize)
{
    juce::StringArray args { media::ffmpeg().getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "info",
                             "-f", "dshow", "-rtbufsize", "512M", "-use_wallclock_as_timestamps", "1" };
    if (fixedSize) args.addArray ({ "-video_size", "1280x720", "-framerate", "30" });
    // MPEG-TS stays playable even if the process is stopped abruptly
    args.addArray ({ "-i", "video=" + deviceName,
                     "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p",
                     "-y", "-f", "mpegts", file.getFullPathName() });
    auto p = std::make_unique<juce::ChildProcess>();
    if (! p->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return false;
    const juce::ScopedLock sl (procLock);
    proc = std::move (p);
    usedFixedSize = fixedSize;
    return true;
}

juce::String Recorder::start (const juce::String& device, const juce::File& out)
{
    stop();
    if (! media::ffmpeg().existsAsFile()) return juce::String::fromUTF8 ("ffmpeg পাওয়া যায়নি — সফটওয়্যারটি আবার ইনস্টল করুন।");
    deviceName = device;
    file = out;
    file.deleteFile();
    firstFrameWall = 0.0;
    { const juce::ScopedLock sl (lock); log = {}; }
    startedWall = (double) juce::Time::currentTimeMillis() / 1000.0;
    if (! launch (true)) return juce::String::fromUTF8 ("ক্যামেরা চালু করা যায়নি।");
    recording = true;
    startThread();
    return {};
}

void Recorder::run()
{
    char buf[1024];
    juce::String pending;
    while (! threadShouldExit())
    {
        juce::ChildProcess* p = nullptr;
        { const juce::ScopedLock sl (procLock); p = proc.get(); }
        if (p == nullptr) break;
        const int n = p->readProcessOutput (buf, sizeof (buf));
        if (n <= 0)
        {
            if (! p->isRunning())
            {
                // this camera has no 1280x720: try again with its own default size (frames start a bit later)
                if (usedFixedSize && firstFrameWall.load() == 0.0 && ! threadShouldExit() && launch (false)) continue;
                break;
            }
            wait (10);
            continue;
        }
        pending += juce::String (juce::CharPointer_UTF8 (buf), (size_t) n);
        while (pending.containsAnyOf ("\r\n"))
        {
            const int i = pending.indexOfAnyOf ("\r\n");
            auto line = pending.substring (0, i).trim();
            pending = pending.substring (i + 1);
            if (line.isEmpty()) continue;
            {
                const juce::ScopedLock sl (lock);
                if (log.length() < 20000) log << line << "\n";
            }
            // "  Duration: N/A, start: 1730889012.345000, bitrate: N/A" = the first frame's wall-clock time
            if (firstFrameWall.load() == 0.0 && line.contains ("start:"))
            {
                const double s = line.fromFirstOccurrenceOf ("start:", false, false).trim().getDoubleValue();
                if (s > 1.0e9) firstFrameWall = s;
            }
        }
    }
}

double Recorder::stop()
{
    if (! recording) return 0.0;
    recording = false;
    signalThreadShouldExit();
    {
        const juce::ScopedLock sl (procLock);
        if (proc != nullptr) proc->kill();
    }
    stopThread (3000);
    { const juce::ScopedLock sl (procLock); proc.reset(); }
    const double w = firstFrameWall.load();
    return w > 0.0 ? w : startedWall + 0.6;   // ffmpeg did not tell: a typical start-up time
}

juce::String Recorder::getLastError() const
{
    const juce::ScopedLock sl (lock);
    return log.getLastCharacters (600);
}
}
