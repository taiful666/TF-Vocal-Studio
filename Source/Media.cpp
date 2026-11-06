#include "Media.h"

namespace tf
{
//==============================================================================
namespace media
{
    static juce::File appDir() { return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory(); }

    juce::File ffmpeg()
    {
       #if JUCE_WINDOWS
        return appDir().getChildFile ("ffmpeg.exe");
       #else
        for (auto p : { appDir().getChildFile ("ffmpeg"), juce::File ("/usr/local/bin/ffmpeg"), juce::File ("/usr/bin/ffmpeg") })
            if (p.existsAsFile()) return p;
        return {};
       #endif
    }

    juce::String audioExtensions() { return "mp3;wav;m4a;aac;flac;ogg;opus;wma;aif;aiff"; }
    juce::String videoExtensions() { return "mp4;mkv;webm;mov;avi;wmv;flv;m4v;3gp;mpg;mpeg;ts"; }

    juce::String allPatterns()
    {
        juce::StringArray p;
        for (auto& e : juce::StringArray::fromTokens (audioExtensions() + ";" + videoExtensions(), ";", "")) p.add ("*." + e);
        return p.joinIntoString (";");
    }

    bool isVideo (const juce::File& f) { return f.hasFileExtension (videoExtensions()); }

    static juce::String runAndCapture (const juce::StringArray& args, int timeoutMs)
    {
        juce::ChildProcess cp;
        if (! cp.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return {};
        auto out = cp.readAllProcessOutput();
        cp.waitForProcessToFinish (timeoutMs);
        return out;
    }

    ProbeInfo probe (const juce::File& f)
    {
        ProbeInfo info;
        auto exe = ffmpeg();
        if (! exe.existsAsFile()) return info;
        auto out = runAndCapture ({ exe.getFullPathName(), "-hide_banner", "-nostdin", "-i", f.getFullPathName() }, 15000);
        auto d = out.fromFirstOccurrenceOf ("Duration: ", false, false).upToFirstOccurrenceOf (",", false, false).trim();
        auto parts = juce::StringArray::fromTokens (d, ":", "");
        if (parts.size() == 3)
            info.duration = parts[0].getIntValue() * 3600.0 + parts[1].getIntValue() * 60.0 + parts[2].getDoubleValue();
        for (auto line : juce::StringArray::fromLines (out))
        {
            if (line.contains ("Stream #") && line.contains ("Video:") && ! line.contains ("attached pic"))
            {
                info.hasVideo = true;
                for (auto tok : juce::StringArray::fromTokens (line, " ,", ""))
                {
                    auto x = tok.indexOfChar ('x');
                    if (x > 0 && tok.substring (0, x).containsOnly ("0123456789") && tok.substring (x + 1).containsOnly ("0123456789")
                        && tok.substring (0, x).getIntValue() > 15)
                    {
                        info.width = tok.substring (0, x).getIntValue();
                        info.height = tok.substring (x + 1).getIntValue();
                        break;
                    }
                }
            }
            if (line.contains ("Stream #") && line.contains ("Audio:")) info.hasAudio = true;
        }
        return info;
    }

    juce::File cacheFor (const juce::File& m)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("TF Vocal Studio").getChildFile ("AudioCache");
        dir.createDirectory();
        auto key = m.getFullPathName() + "|" + juce::String (m.getSize()) + "|" + juce::String (m.getLastModificationTime().toMilliseconds());
        return dir.getChildFile (juce::File::createLegalFileName (m.getFileNameWithoutExtension()).substring (0, 40)
                                 + "-" + juce::String::toHexString (key.hashCode64()) + ".wav");
    }

    bool canReadDirectly (const juce::File& f, juce::AudioFormatManager& fm)
    {
        if (isVideo (f)) return false;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        return r != nullptr && r->lengthInSamples > 0;
    }

    juce::String displayName (const juce::File& f)
    {
        auto n = f.getFileNameWithoutExtension();
        // "YTDown.com_YouTube_Media_<11-char id>_Title" and similar download-site names
        if (auto k = n.indexOfIgnoreCase ("YouTube_Media_"); k >= 0)
        {
            auto rest = n.substring (k + 14);
            n = rest.length() > 12 && rest[11] == '_' ? rest.substring (12) : rest;
        }
        for (auto prefix : { "y2mate.com - ", "y2mate.is - ", "ssyoutube.com - ", "YTDown.com_", "SnapSave.io - ", "savefrom.net - " })
            if (n.startsWithIgnoreCase (prefix)) n = n.substring ((int) strlen (prefix));
        for (auto tail : { "_128k", "_320k", "_192k", "_720p", "_1080p", "_360p", "_480p", " (128k)", " (320 kbps)" })
            if (n.endsWithIgnoreCase (tail)) n = n.dropLastCharacters ((int) strlen (tail));
        n = n.replaceCharacter ('_', ' ').trim();
        while (n.contains ("  ")) n = n.replace ("  ", " ");
        return n.isEmpty() ? f.getFileNameWithoutExtension() : n;
    }

    juce::String extractAudio (const juce::File& in, const juce::File& outWav, double sampleRate,
                               std::function<void (float)> progress, std::atomic<bool>& cancel)
    {
        auto exe = ffmpeg();
        if (! exe.existsAsFile()) return juce::String::fromUTF8 ("ffmpeg পাওয়া যায়নি — সফটওয়্যারটি আবার ইনস্টল করুন।");
        const auto info = probe (in);
        if (! info.hasAudio) return juce::String::fromUTF8 ("এই ফাইলে কোনো আওয়াজ (অডিও) নেই।");

        auto tmp = outWav.getSiblingFile (outWav.getFileNameWithoutExtension() + ".part.wav");
        tmp.deleteFile();
        juce::ChildProcess cp;
        juce::StringArray args { exe.getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                                 "-i", in.getFullPathName(), "-vn", "-sn", "-ac", "2", "-ar", juce::String ((int) sampleRate),
                                 "-c:a", "pcm_s16le", "-progress", "pipe:1", "-nostats", tmp.getFullPathName() };
        if (! cp.start (args, juce::ChildProcess::wantStdOut))
            return juce::String::fromUTF8 ("ffmpeg চালু করা যায়নি।");

        juce::String pending;
        char buf[512];
        for (;;)
        {
            if (cancel.load()) { cp.kill(); tmp.deleteFile(); return juce::String::fromUTF8 ("বাতিল করা হয়েছে।"); }
            const int n = cp.readProcessOutput (buf, sizeof (buf));
            if (n <= 0) break;
            pending += juce::String (juce::CharPointer_UTF8 (buf), (size_t) n);
            while (pending.containsChar ('\n'))
            {
                auto line = pending.upToFirstOccurrenceOf ("\n", false, false).trim();
                pending = pending.fromFirstOccurrenceOf ("\n", false, false);
                if (line.startsWith ("out_time_us=") && info.duration > 0 && progress != nullptr)
                    progress ((float) juce::jlimit (0.0, 1.0, line.substring (12).getLargeIntValue() / 1.0e6 / info.duration));
            }
        }
        cp.waitForProcessToFinish (10000);
        if (cp.getExitCode() != 0 || ! tmp.existsAsFile() || tmp.getSize() < 1000)
        {
            tmp.deleteFile();
            return juce::String::fromUTF8 ("ফাইল থেকে অডিও বের করা যায়নি। ফাইলটি ঠিক আছে কিনা দেখুন।");
        }
        outWav.deleteFile();
        tmp.moveFileTo (outWav);
        if (progress) progress (1.0f);
        return {};
    }

    juce::String encodeMp3 (const juce::File& wav, const juce::File& mp3)
    {
        auto exe = ffmpeg();
        if (! exe.existsAsFile()) return juce::String::fromUTF8 ("ffmpeg পাওয়া যায়নি — সফটওয়্যারটি আবার ইনস্টল করুন।");
        auto tmp = mp3.getSiblingFile (mp3.getFileNameWithoutExtension() + ".part.mp3");
        tmp.deleteFile();
        juce::ChildProcess cp;
        juce::StringArray args { exe.getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                                 "-i", wav.getFullPathName(), "-c:a", "libmp3lame", "-b:a", "320k", "-id3v2_version", "3",
                                 "-metadata", "encoded_by=TF Vocal Studio", tmp.getFullPathName() };
        if (! cp.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return juce::String::fromUTF8 ("ffmpeg চালু করা যায়নি।");
        cp.readAllProcessOutput();
        cp.waitForProcessToFinish (120000);
        if (cp.getExitCode() != 0 || ! tmp.existsAsFile() || tmp.getSize() < 1000)
        {
            tmp.deleteFile();
            return juce::String::fromUTF8 ("MP3 তৈরি করা যায়নি।");
        }
        mp3.deleteFile();
        tmp.moveFileTo (mp3);
        return {};
    }
}

//==============================================================================
// VideoFrames
//==============================================================================
VideoFrames::VideoFrames() : juce::Thread ("TF video") {}
VideoFrames::~VideoFrames() { close(); }

void VideoFrames::open (const juce::File& video)
{
    close();
    auto info = media::probe (video);
    if (! info.hasVideo) return;
    aspectRatio = (info.width > 0 && info.height > 0) ? (double) info.width / info.height : 16.0 / 9.0;
    // decode at a size that looks sharp in the lyrics area but keeps the pipe light
    const int maxW = info.width > 0 ? juce::jmin (1280, info.width) : 1280;
    w = maxW & ~1; h = juce::roundToInt ((double) w / aspectRatio) & ~1;
    if (h > 720) { h = 720; w = juce::roundToInt (720.0 * aspectRatio) & ~1; }
    file = video;
    requestedStart = -1.0;
    lastAsked = -1.0;
    currentTime = -1.0;
    current = {};
    startThread (juce::Thread::Priority::normal);
}

void VideoFrames::close()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        restartFlag = true;
        { const juce::ScopedLock sl (procLock); if (proc != nullptr) proc->kill(); }
        spaceAvailable.signal();
        stopThread (4000);
    }
    file = juce::File();
    const juce::ScopedLock sl (lock);
    queue.clear();
    current = {};
}

void VideoFrames::restartAt (double seconds)
{
    {
        const juce::ScopedLock sl (lock);
        queue.clear();
        currentTime = seconds;
    }
    requestedStart = juce::jmax (0.0, seconds);
    restartFlag = true;
    { const juce::ScopedLock sl (procLock); if (proc != nullptr) proc->kill(); }
    spaceAvailable.signal();
}

juce::Image VideoFrames::frameAt (double t, bool playing)
{
    if (! isOpen()) return {};
    bool needRestart = false;
    {
        const juce::ScopedLock sl (lock);
        while (! queue.empty() && queue.front().first <= t + 0.5 / fps)
        {
            current = queue.front().second;
            currentTime = queue.front().first;
            queue.pop_front();
            spaceAvailable.signal();
        }
        const bool jumpedBack = t < currentTime - 0.35;
        const bool farBehind = queue.empty() && playing && t > currentTime + 1.5;
        const bool farAhead = ! queue.empty() && queue.front().first > t + 1.5;
        const bool seekedWhilePaused = ! playing && lastAsked >= 0 && std::abs (t - lastAsked) > 0.2;
        needRestart = requestedStart.load() < 0 || jumpedBack || farBehind || farAhead || seekedWhilePaused;
    }
    lastAsked = t;
    if (needRestart) restartAt (t);
    const juce::ScopedLock sl (lock);
    return current;
}

void VideoFrames::run()
{
    const int frameBytes = w * h * 4;
    juce::HeapBlock<char> buf ((size_t) frameBytes);

    while (! threadShouldExit())
    {
        const double start = requestedStart.load();
        restartFlag = false;
        if (start < 0) { wait (30); continue; }

        {
            const juce::ScopedLock sl (procLock);
            proc = std::make_unique<juce::ChildProcess>();
            juce::StringArray args { media::ffmpeg().getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "quiet",
                                     "-ss", juce::String (start, 3), "-i", file.getFullPathName(), "-an", "-sn", "-dn",
                                     "-vf", "scale=" + juce::String (w) + ":" + juce::String (h) + ",fps=" + juce::String (fps, 0),
                                     "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1" };
            if (! proc->start (args, juce::ChildProcess::wantStdOut)) { proc.reset(); wait (500); continue; }
        }

        juce::int64 idx = 0;
        while (! threadShouldExit() && ! restartFlag.load())
        {
            int got = 0;
            while (got < frameBytes)
            {
                const int r = proc->readProcessOutput (buf.get() + got, frameBytes - got);
                if (r <= 0) break;
                got += r;
            }
            if (got < frameBytes)
            {
                while (! threadShouldExit() && ! restartFlag.load()) wait (30);   // end of video: idle until a seek
                break;
            }
            juce::Image img (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
            {
                juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < h; ++y)
                    std::memcpy (bd.getLinePointer (y), buf.get() + (size_t) y * (size_t) w * 4, (size_t) w * 4);
            }
            const double t = start + (double) idx++ / fps;
            {
                const juce::ScopedLock sl (lock);
                if (! restartFlag.load()) queue.emplace_back (t, img);
            }
            while (! threadShouldExit() && ! restartFlag.load())
            {
                { const juce::ScopedLock sl (lock); if (queue.size() < 8) break; }
                spaceAvailable.wait (20);
            }
        }

        const juce::ScopedLock sl (procLock);
        if (proc != nullptr) { proc->kill(); proc.reset(); }
    }
}

}
