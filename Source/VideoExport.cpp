#include "VideoExport.h"
#include "Media.h"
#include "Theme.h"

namespace tf::videoexport
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

static juce::String runFfmpeg (const juce::StringArray& args, double expectSeconds, std::function<void (float)> progress, float p0, float p1, std::atomic<bool>& cancel)
{
    juce::ChildProcess cp;
    if (! cp.start (args, juce::ChildProcess::wantStdOut)) return u8 ("ffmpeg চালু করা যায়নি।");
    juce::String pending;
    char buf[512];
    for (;;)
    {
        if (cancel.load()) { cp.kill(); return u8 ("বাতিল করা হয়েছে।"); }
        const int n = cp.readProcessOutput (buf, sizeof (buf));
        if (n <= 0) break;
        pending += juce::String (juce::CharPointer_UTF8 (buf), (size_t) n);
        while (pending.containsChar ('\n'))
        {
            auto line = pending.upToFirstOccurrenceOf ("\n", false, false).trim();
            pending = pending.fromFirstOccurrenceOf ("\n", false, false);
            if (line.startsWith ("out_time_us=") && expectSeconds > 0 && progress)
                progress (p0 + (p1 - p0) * (float) juce::jlimit (0.0, 1.0, line.substring (12).getLargeIntValue() / 1.0e6 / expectSeconds));
        }
    }
    cp.waitForProcessToFinish (60000);
    return cp.getExitCode() == 0 ? juce::String() : u8 ("ভিডিও তৈরি করা যায়নি (ffmpeg)।");
}

void drawLyricsFrame (juce::Graphics& g, const Options& o, double t, juce::TextLayout& cache, float& cacheWidth)
{
    const auto W = (float) o.width, H = (float) o.height;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff14171D), 0, 0, juce::Colour (0xff0B0D11), 0, H, false));
    g.fillAll();
    const float scale = juce::jmin (W, H) / 720.0f;   // the same text size for wide and tall (Reels) videos

    // title (small, top)
    g.setColour (col::muted);
    g.setFont (font (22.0f * scale, Weight::medium));
    g.drawText (o.title, juce::Rectangle<float> (0, 24.0f * scale, W, 34.0f * scale), juce::Justification::centred, true);

    const auto& lyr = o.lyrics;
    double song = o.state.musicTrack() != nullptr ? o.state.musicTimeAt (t) : t;
    song -= o.lyricsOffset;

    if (lyr.isEmpty())
    {
        g.setColour (col::ink);
        g.setFont (font (54.0f * scale, Weight::semibold));
        g.drawText (o.title, juce::Rectangle<float> (0, H * 0.4f, W, 70.0f * scale), juce::Justification::centred, true);
    }
    else if (lyr.timed && o.state.musicTrack() != nullptr)
    {
        const int idx = lyr.indexAt (song);
        const int cur = juce::jmax (0, idx);
        double songLen = 0.0;
        if (auto* m = o.state.musicTrack()) for (auto& c : m->clips) songLen = juce::jmax (songLen, c.out * m->speed);
        const double prog = idx >= 0 ? lyr.progressAt (idx, song, songLen) : 0.0;
        const float cy = H * 0.52f, gap = 92.0f * scale;
        for (int k = cur - 1; k <= cur + 2; ++k)
        {
            if (! juce::isPositiveAndBelow (k, lyr.lines.size())) continue;
            auto text = lyr.lines.getReference (k).text;
            if (text.isEmpty()) text = juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xaa  \xe2\x99\xaa"));
            const bool now = k == cur;
            auto f = font ((now ? 60.0f : 38.0f) * scale, now ? Weight::semibold : Weight::regular);
            float w = juce::GlyphArrangement::getStringWidth (f, text);
            if (w > W - 80.0f) { f = f.withHeight (f.getHeight() * (W - 80.0f) / w); w = W - 80.0f; }
            juce::Rectangle<float> r (0, cy + (float) (k - cur) * gap - 50.0f * scale, W, 100.0f * scale);
            g.setFont (f);
            g.setColour (now ? col::ink : col::muted.withAlpha (k < cur ? 0.45f : 0.75f));
            g.drawText (text, r, juce::Justification::centred, false);
            if (now && prog > 0.0)
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (juce::Rectangle<float> (W * 0.5f - w * 0.5f - 2.0f, r.getY(), (float) (w * prog) + 2.0f, r.getHeight()).toNearestInt());
                g.setColour (col::accent);
                g.drawText (text, r, juce::Justification::centred, false);
            }
        }
    }
    else
    {
        // scrolling text, same pace as the teleprompter on screen
        const float fs = 46.0f * scale, w = W - 160.0f * scale;
        if (std::abs (cacheWidth - w) > 0.5f)
        {
            juce::AttributedString as;
            as.setJustification (juce::Justification::centredTop);
            as.setLineSpacing (fs * 0.5f);
            as.setWordWrap (juce::AttributedString::WordWrap::byWord);
            juce::StringArray rows;
            for (auto& l : lyr.lines) rows.add (l.text);
            as.append (rows.joinIntoString ("\n"), font (fs, Weight::semibold), col::ink);
            cache.createLayout (as, w);
            cacheWidth = w;
        }
        const double len = juce::jmax (60.0, o.state.length() * 0.92);
        const float pps = o.scrollSpeed * juce::jmax (6.0f, (float) (cache.getHeight() / len));
        const float readY = H * 0.36f;
        const float scroll = (float) (t - o.lyricsOffset) * pps;
        g.setColour (col::accent.withAlpha (0.08f));
        g.fillRoundedRectangle (40.0f * scale, readY - fs * 0.25f, W - 80.0f * scale, fs * 1.55f, 14.0f * scale);
        cache.draw (g, juce::Rectangle<float> (80.0f * scale, readY - scroll, w, cache.getHeight()));
        auto fade = [&] (float y0, float y1, bool down)
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff12141A).withAlpha (down ? 1.0f : 0.0f), 0, y0,
                                                     juce::Colour (0xff0B0D11).withAlpha (down ? 0.0f : 1.0f), 0, y1, false));
            g.fillRect (juce::Rectangle<float> (0, y0, W, y1 - y0));
        };
        fade (70.0f * scale, 150.0f * scale, true);
        fade (H - 110.0f * scale, H, false);
        g.setColour (col::muted);
        g.setFont (font (22.0f * scale, Weight::medium));
        g.drawText (o.title, juce::Rectangle<float> (0, 24.0f * scale, W, 34.0f * scale), juce::Justification::centred, true);
    }
}

void drawLyricsOverlay (juce::Graphics& g, const Options& o, double t, int width, int stripH, bool opaque)
{
    const auto W = (float) width, H = (float) stripH;
    if (opaque) g.fillAll (juce::Colour (0xff0E1014));
    else
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0B0D11).withAlpha (0.0f), 0, 0, juce::Colour (0xff0B0D11).withAlpha (0.88f), 0, H * 0.55f, false));
        g.fillAll();
    }
    const float scale = juce::jmin ((float) o.width, (float) o.height) / 720.0f;
    const auto& lyr = o.lyrics;
    double song = o.state.musicTrack() != nullptr ? o.state.musicTimeAt (t) : t;
    song -= o.lyricsOffset;
    juce::StringArray rows;
    int cur = 0;
    double prog = 0.0;
    if (lyr.isEmpty()) rows.add (o.title);
    else if (lyr.timed && o.state.musicTrack() != nullptr)
    {
        const int idx = lyr.indexAt (song);
        cur = juce::jmax (0, idx);
        double songLen = 0.0;
        if (auto* m = o.state.musicTrack()) for (auto& c : m->clips) songLen = juce::jmax (songLen, c.out * m->speed);
        prog = idx >= 0 ? lyr.progressAt (idx, song, songLen) : 0.0;
        for (int k = cur; k <= cur + 1; ++k) rows.add (juce::isPositiveAndBelow (k, lyr.lines.size()) ? lyr.lines.getReference (k).text : juce::String());
    }
    else
    {
        // plain lyrics: move through the lines at the song's pace
        const double len = juce::jmax (30.0, o.state.length() * 0.92);
        cur = juce::jlimit (0, lyr.lines.size() - 1, (int) (juce::jmax (0.0, t - o.lyricsOffset) / len * lyr.lines.size()));
        for (int k = cur; k <= cur + 1; ++k) rows.add (juce::isPositiveAndBelow (k, lyr.lines.size()) ? lyr.lines.getReference (k).text : juce::String());
    }
    const float cy = opaque ? H * 0.42f : H * 0.58f;
    for (int i = 0; i < rows.size(); ++i)
    {
        auto text = rows[i];
        if (text.isEmpty()) continue;
        const bool now = i == 0;
        auto f = font ((now ? 46.0f : 32.0f) * scale, now ? Weight::semibold : Weight::regular);
        float w = juce::GlyphArrangement::getStringWidth (f, text);
        const float maxW = W - 60.0f * scale;
        if (w > maxW) { f = f.withHeight (f.getHeight() * maxW / w); w = maxW; }
        juce::Rectangle<float> r (0, cy + (float) i * 64.0f * scale - 32.0f * scale, W, 64.0f * scale);
        g.setFont (f);
        // a soft shadow keeps the words readable over any camera picture
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawText (text, r.translated (0.0f, 2.0f * scale), juce::Justification::centred, false);
        g.setColour (now ? col::ink : col::muted);
        g.drawText (text, r, juce::Justification::centred, false);
        if (now && prog > 0.0)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (juce::Rectangle<float> (W * 0.5f - w * 0.5f - 2.0f, r.getY(), (float) (w * prog) + 2.0f, r.getHeight()).toNearestInt());
            g.setColour (col::accent);
            g.drawText (text, r, juce::Justification::centred, false);
        }
    }
}

juce::String render (const Options& o, std::function<void (float)> progress, std::atomic<bool>& cancel)
{
    const auto exe = media::ffmpeg();
    if (! exe.existsAsFile()) return u8 ("ffmpeg পাওয়া যায়নি — সফটওয়্যারটি আবার ইনস্টল করুন।");
    if (! o.mixWav.existsAsFile()) return u8 ("মিক্স পাওয়া যায়নি।");
    const auto tmpOut = o.out.getSiblingFile (o.out.getFileNameWithoutExtension() + ".part.mp4");
    tmpOut.deleteFile();
    const juce::String size = juce::String (o.width) + ":" + juce::String (o.height);
    const juce::String common = "fps=" + juce::String (o.fps) + ",scale=" + size + ":force_original_aspect_ratio=decrease,pad=" + size
                              + ":(ow-iw)/2:(oh-ih)/2:color=0x0E1014,format=yuv420p";
    juce::String err;

    auto* m = o.state.musicTrack();
    const bool vertical = o.height > o.width;
    if (o.camera.existsAsFile())
    {
        // the singers on camera, the lyrics as a strip over / under the picture
        const int W = o.width, H = o.height;
        const int camH = vertical ? (int) std::lround (H * 0.70) / 2 * 2 : H;
        const int stripH = vertical ? H - camH : (int) std::lround (H * 0.36);
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TFVS-cam-" + juce::String (juce::Time::currentTimeMillis()));
        dir.createDirectory();
        const int frames = juce::jmax (1, (int) std::ceil (o.length * o.fps));
        juce::Image img (juce::Image::ARGB, W, stripH, true, juce::SoftwareImageType());
        juce::PNGImageFormat png;
        for (int i = 0; i < frames && err.isEmpty(); ++i)
        {
            if (cancel.load()) err = u8 ("বাতিল করা হয়েছে।");
            img.clear (img.getBounds());
            {
                juce::Graphics g (img);
                drawLyricsOverlay (g, o, o.rangeStart + (double) i / o.fps, W, stripH, vertical);
            }
            juce::FileOutputStream fos (dir.getChildFile ("o" + juce::String (i).paddedLeft ('0', 6) + ".png"));
            if (! fos.openedOk() || ! png.writeImageToStream (img, fos)) err = u8 ("ভিডিওর ছবি লেখা যায়নি — ডিস্কে জায়গা আছে কিনা দেখুন।");
            if (progress && (i % 25) == 0) progress (0.6f * (float) i / (float) frames);
        }
        if (err.isEmpty())
        {
            const double d = o.cameraStart - o.rangeStart;   // > 0: the camera starts later than the video
            juce::String vf = "[0:v]setpts=PTS-STARTPTS";
            if (d < -0.0005) vf << ",trim=start=" << juce::String (-d, 3) << ",setpts=PTS-STARTPTS";
            if (d > 0.0005) vf << ",tpad=start_duration=" << juce::String (d, 3) << ":color=0x0E1014";
            vf << ",tpad=stop_mode=clone:stop_duration=3600,fps=" << o.fps
               << ",scale=" << W << ":" << camH << ":force_original_aspect_ratio=increase,crop=" << W << ":" << camH
               << ",pad=" << W << ":" << H << ":0:0:color=0x0E1014[cam];"
               << "[cam][1:v]overlay=0:" << (H - stripH) << ":format=auto,format=yuv420p[v]";
            err = runFfmpeg ({ exe.getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                               "-i", o.camera.getFullPathName(),
                               "-framerate", juce::String (o.fps), "-i", dir.getChildFile ("o%06d.png").getFullPathName(),
                               "-i", o.mixWav.getFullPathName(),
                               "-filter_complex", vf, "-map", "[v]", "-map", "2:a",
                               "-c:v", "libx264", "-preset", "veryfast", "-crf", "19", "-c:a", "aac", "-b:a", "256k",
                               "-t", juce::String (o.length, 3), "-movflags", "+faststart", "-progress", "pipe:1", "-nostats",
                               tmpOut.getFullPathName() },
                             o.length, progress, 0.6f, 1.0f, cancel);
        }
        dir.deleteRecursively();
    }
    else if (o.useSourceVideo && m != nullptr && media::isVideo (m->source) && ! m->clips.isEmpty() && media::probe (m->source).hasVideo)
    {
        // the singer sees the song's own video: use it, in step with the music clip and its speed
        auto c = m->clips.getReference (0);
        for (auto& x : m->clips) if (x.start < c.start) c = x;
        const double pad = juce::jmax (0.0, c.start - o.rangeStart);
        const double from = (c.in + juce::jmax (0.0, o.rangeStart - c.start)) * m->speed;   // song seconds
        juce::String vf = "[0:v]trim=start=" + juce::String (from, 3) + ",setpts=(PTS-STARTPTS)/" + juce::String (m->speed, 4);
        if (pad > 0.0005) vf << ",tpad=start_duration=" << juce::String (pad, 3) << ":color=black";
        if (vertical)
        {
            // tall video: the song's video in the middle, a blurred, enlarged copy fills the rest
            vf << ",fps=" << o.fps << ",split[a][b];[a]scale=" << size << ":force_original_aspect_ratio=increase,crop=" << size
               << ",boxblur=24:2,eq=brightness=-0.15[bg];[b]scale=" << size << ":force_original_aspect_ratio=decrease[fg];"
               << "[bg][fg]overlay=(W-w)/2:(H-h)/2,format=yuv420p[v]";
        }
        else vf << "," << common << "[v]";
        err = runFfmpeg ({ exe.getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                           "-i", m->source.getFullPathName(), "-i", o.mixWav.getFullPathName(),
                           "-filter_complex", vf, "-map", "[v]", "-map", "1:a",
                           "-c:v", "libx264", "-preset", "veryfast", "-crf", "21", "-c:a", "aac", "-b:a", "256k",
                           "-t", juce::String (o.length, 3), "-movflags", "+faststart", "-progress", "pipe:1", "-nostats",
                           tmpOut.getFullPathName() },
                         o.length, progress, 0.0f, 1.0f, cancel);
    }
    else
    {
        // lyrics drawn frame by frame (correct Bengali shaping), then encoded with the mix
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TFVS-video-" + juce::String (juce::Time::currentTimeMillis()));
        dir.createDirectory();
        const int frames = juce::jmax (1, (int) std::ceil (o.length * o.fps));
        juce::Image img (juce::Image::RGB, o.width, o.height, true, juce::SoftwareImageType());
        juce::JPEGImageFormat jpg;
        jpg.setQuality (0.9f);
        juce::TextLayout cache;
        float cacheW = 0.0f;
        for (int i = 0; i < frames && err.isEmpty(); ++i)
        {
            if (cancel.load()) err = u8 ("বাতিল করা হয়েছে।");
            {
                juce::Graphics g (img);
                drawLyricsFrame (g, o, o.rangeStart + (double) i / o.fps, cache, cacheW);
            }
            juce::FileOutputStream fos (dir.getChildFile ("f" + juce::String (i).paddedLeft ('0', 6) + ".jpg"));
            if (! fos.openedOk() || ! jpg.writeImageToStream (img, fos)) err = u8 ("ভিডিওর ছবি লেখা যায়নি — ডিস্কে জায়গা আছে কিনা দেখুন।");
            if (progress && (i % 25) == 0) progress (0.7f * (float) i / (float) frames);
        }
        if (err.isEmpty())
            err = runFfmpeg ({ exe.getFullPathName(), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                               "-framerate", juce::String (o.fps), "-i", dir.getChildFile ("f%06d.jpg").getFullPathName(),
                               "-i", o.mixWav.getFullPathName(), "-vf", common.fromFirstOccurrenceOf (",", false, false),
                               "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-tune", "stillimage", "-c:a", "aac", "-b:a", "256k",
                               "-t", juce::String (o.length, 3), "-movflags", "+faststart", "-progress", "pipe:1", "-nostats",
                               tmpOut.getFullPathName() },
                             o.length, progress, 0.7f, 1.0f, cancel);
        dir.deleteRecursively();
    }

    if (err.isEmpty() && (! tmpOut.existsAsFile() || tmpOut.getSize() < 2000)) err = u8 ("ভিডিও তৈরি করা যায়নি।");
    if (err.isNotEmpty()) { tmpOut.deleteFile(); return err; }
    o.out.deleteFile();
    tmpOut.moveFileTo (o.out);
    if (progress) progress (1.0f);
    return {};
}
}
