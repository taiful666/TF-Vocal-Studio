#include "Camera.h"
#include "Media.h"
#include "Theme.h"

namespace tf::camera
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

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

juce::StringArray dshowInput (const juce::String& device, bool fixedSize)
{
    juce::StringArray a { "-f", "dshow", "-rtbufsize", "512M" };
    if (fixedSize) a.addArray ({ "-video_size", "1280x720", "-framerate", "30" });
    a.addArray ({ "-i", "video=" + device });
    return a;
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
    if (! img.isValid()) error = u8 ("ক্যামেরা থেকে ছবি পাওয়া যায়নি। অন্য কোনো সফটওয়্যার ক্যামেরা ব্যবহার করছে কিনা দেখুন।");
    f.deleteFile();
    return img;
   #else
    juce::ignoreUnused (device);
    error = "camera not supported";
    return {};
   #endif
}

//==============================================================================
Session::Session() : juce::Thread ("TF camera") {}
Session::~Session() { stop(); }

bool Session::launch (bool fixedSize)
{
    juce::StringArray args { media::ffmpeg().getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "info",
                             "-use_wallclock_as_timestamps", "1" };
    args.addArray (input (fixedSize));
    if (recordFile != juce::File())
        // MPEG-TS stays playable even if the process is stopped abruptly
        // every frame as it comes (camera timing kept), written out at once: stopping can never leave an empty file
        args.addArray ({ "-map", "0:v", "-fps_mode", "passthrough", "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
                         "-crf", "18", "-pix_fmt", "yuv420p", "-flush_packets", "1",
                         "-y", "-f", "mpegts", recordFile.getFullPathName() });
    // the live picture: one small JPEG, replaced (atomically) about 10 times a second
    args.addArray ({ "-map", "0:v", "-vf", "fps=10,scale=480:-2", "-q:v", "5",
                     "-y", "-f", "image2", "-update", "1", "-atomic_writing", "1", previewFile.getFullPathName() });
    auto p = std::make_unique<juce::ChildProcess>();
    // only the log (stderr) is read; it tells when the first frame was taken
    if (! p->start (args, juce::ChildProcess::wantStdErr)) return false;
    const juce::ScopedLock sl (procLock);
    proc = std::move (p);
    usedFixedSize = fixedSize;
    return true;
}

juce::String Session::start (InputArgs in, const juce::File& preview, const juce::File& record)
{
    stop();
    if (! media::ffmpeg().existsAsFile()) return u8 ("ffmpeg পাওয়া যায়নি — সফটওয়্যারটি আবার ইনস্টল করুন।");
    input = std::move (in);
    previewFile = preview;
    recordFile = record;
    previewFile.getParentDirectory().createDirectory();
    if (recordFile != juce::File()) { recordFile.getParentDirectory().createDirectory(); recordFile.deleteFile(); }
    firstFrameWall = 0.0;
    failed = false;
    { const juce::ScopedLock sl (lock); log = {}; }
    startedWall = (double) juce::Time::currentTimeMillis() / 1000.0;
    if (! launch (true)) return u8 ("ক্যামেরা চালু করা যায়নি।");
    running = true;
    startThread();
    return {};
}

void Session::run()
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
                // this camera has no 1280x720: try again with its own default size
                if (usedFixedSize && firstFrameWall.load() == 0.0 && ! threadShouldExit() && launch (false)) continue;
                if (! threadShouldExit()) failed = true;
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

double Session::stop()
{
    if (! running.exchange (false)) return 0.0;
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

juce::String Session::getLog() const
{
    const juce::ScopedLock sl (lock);
    return log.getLastCharacters (600);
}

//==============================================================================
void Preview::setActive (bool on)
{
    setVisible (on);
    if (on) startTimerHz (12);
    else { stopTimer(); picture = {}; lastModified = {}; }
}

void Preview::timerCallback()
{
    const auto f = session.getPreviewFile();
    const auto m = f.getLastModificationTime();
    if (f.existsAsFile() && m != lastModified)
    {
        auto img = juce::ImageFileFormat::loadFrom (f);
        if (img.isValid()) { picture = img; lastModified = m; staleTicks = 0; repaint(); return; }
    }
    if (++staleTicks == 36) repaint();   // 3 s without a new picture: say so
}

void Preview::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    juce::Path clip;
    clip.addRoundedRectangle (r, 12.0f);
    g.setColour (juce::Colour (0xff0B0D11));
    g.fillPath (clip);
    if (picture.isValid())
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (clip);
        g.drawImage (picture, r, juce::RectanglePlacement::fillDestination);
    }
    else
    {
        g.setColour (col::muted);
        g.setFont (font (13.5f));
        g.drawFittedText (session.hasFailed() ? u8 ("ক্যামেরা চালু হয়নি — অন্য সফটওয়্যার ক্যামেরা ব্যবহার করছে কিনা দেখুন")
                                              : u8 ("ক্যামেরা চালু হচ্ছে…"),
                          getLocalBounds().reduced (12), juce::Justification::centred, 3);
    }
    if (session.isRecording())
    {
        auto badge = juce::Rectangle<float> (r.getX() + 10.0f, r.getY() + 10.0f, 74.0f, 24.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (badge, 12.0f);
        g.setColour (col::rec);
        g.fillEllipse (badge.getX() + 9.0f, badge.getCentreY() - 4.5f, 9.0f, 9.0f);
        g.setColour (juce::Colours::white);
        g.setFont (font (12.5f, Weight::semibold));
        g.drawText (u8 ("রেকর্ড"), badge.withTrimmedLeft (24.0f), juce::Justification::centredLeft);
    }
    else if (picture.isValid() && staleTicks >= 36)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillPath (clip);
        g.setColour (col::warn);
        g.setFont (font (13.0f));
        g.drawFittedText (u8 ("ক্যামেরার ছবি আসছে না"), getLocalBounds().reduced (10), juce::Justification::centred, 2);
    }
    g.setColour (col::border);
    g.strokePath (clip, juce::PathStrokeType (1.0f));
}
}
