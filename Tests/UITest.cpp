// Automated UI test: builds the real main window with a simulated sound card and presses the
// buttons the way a user would (button clicks, mouse drags on the timeline, keyboard), then
// measures what comes out of the speaker and what lands on disk.

#include "MainComponent.h"
#include "Settings.h"
#include "FakeDevice.h"
#include "InspectorContent.h"
#include <juce_events/juce_events.h>

namespace tf
{
static int failures = 0;
#define CHECK(cond, msg) do { if (cond) std::cout << "  PASS  " << juce::String (msg) << "\n"; else { std::cout << "  FAIL  " << juce::String (msg) << "\n"; ++failures; } } while (0)

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }
static bool waitFor (std::function<bool()> cond, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end) { if (cond()) return true; pump (20); }
    return cond();
}
static double rms (const std::vector<float>& v, size_t from = 0)
{
    double s = 0; size_t n = 0;
    for (size_t i = from; i < v.size(); ++i) { s += (double) v[i] * v[i]; ++n; }
    return std::sqrt (s / (double) juce::jmax ((size_t) 1, n));
}

/** A real button press (the click is delivered through the message loop, as for the user). */
static void click (juce::Button& b) { b.triggerClick(); pump (40); }
static void click (juce::Button* b) { if (b != nullptr) click (*b); }

/** Mouse press (and optional drag) on a component, as a user would do it. */
static void mouse (juce::Component& c, juce::Point<float> from, juce::Point<float> to, bool drag)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    const juce::ModifierKeys down (juce::ModifierKeys::leftButtonModifier);
    auto ev = [&] (juce::Point<float> p, bool dragged, juce::ModifierKeys mods)
    { return juce::MouseEvent (src, p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, from, now, 1, dragged); };
    c.mouseDown (ev (from, false, down));
    if (drag) for (int i = 1; i <= 12; ++i) c.mouseDrag (ev (from + (to - from) * ((float) i / 12.0f), true, down));
    c.mouseUp (ev (to, drag, {}));
}

struct UITest
{
    static int run();
};

static void makeSong (const juce::File& f, double seconds)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (2).withBitsPerSample (24));
    juce::AudioBuffer<float> b (2, (int) (48000 * seconds));
    juce::Random rnd (7);
    double phase = 0.0;
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const auto t = i / 48000.0;
        phase += 2 * juce::MathConstants<double>::pi * (330.0 + 60.0 * std::sin (t * 1.3)) / 48000.0;
        const float v = 0.2f * (float) std::sin (phase) + 0.08f * (rnd.nextFloat() * 2 - 1);
        b.setSample (0, i, v); b.setSample (1, i, v);
    }
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

int UITest::run()
{
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tfvs_uitest");
    tmp.deleteRecursively(); tmp.createDirectory();
    const auto testStart = juce::Time::getCurrentTime() - juce::RelativeTime::seconds (2);

    TFLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
    juce::PropertiesFile::Options po; po.storageFormat = juce::PropertiesFile::storeAsXML;
    auto props = std::make_unique<juce::PropertiesFile> (tmp.getChildFile ("settings.xml"), po);

    auto song = tmp.getChildFile ("UI Test Song.wav");
    makeSong (song, 14.0);

    auto mc = std::make_unique<MainComponent> (*props, [] (AudioEngine& e)
    {
        e.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType>());
    });
    auto& engine = mc->engine;
    engine.getDeviceManager().setCurrentAudioDeviceType ("FakeType", true);
    juce::DocumentWindow win ("TF Vocal Studio UI test", col::bg, 0);
    win.setContentNonOwned (mc.get(), true);
    win.setBounds (0, 0, 1440, 900);
    win.setVisible (true);
    pump (600);

    auto* dev = dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
    CHECK (dev != nullptr && engine.isDeviceOpen(), "app started with the (simulated) sound card");
    if (dev == nullptr) return 1;
    dev->positionProbe = [&engine] { return engine.getPosition(); };

    auto& studio = mc->studio;
    studio.revealAfterExport = false;
    auto& stage = studio.stage;
    auto& tl = studio.timeline;
    auto& insp = studio.inspector;
    auto& ic = *insp.content;
    auto& project = studio.project;
    auto state = [&]() -> const ProjectState& { return project.state(); };
    auto voiceId = [&]
    {
        for (auto& t : state().tracks) if (t.kind == TrackKind::vocal) return t.id;
        return juce::String();
    };
    auto musicIndex = [&] { for (int i = 0; i < state().tracks.size(); ++i) if (state().tracks.getReference (i).kind == TrackKind::music) return i; return -1; };
    auto voiceIndex = [&] { return state().indexOf (voiceId()); };
    auto laneY = [&] (int i) { return (float) tl.laneArea (i).getCentreY(); };
    auto capture = [&] (double from, int ms)
    {
        engine.setPosition (from);
        pump (250);
        dev->captured.clear(); dev->captureStartPos = -1.0;
        dev->capture = true;
        engine.play();
        pump (ms);
        dev->capture = false;
        engine.pause();
        pump (100);
        return dev->captured;
    };
    juce::String err;

    // ------------------------------------------------------------------ 1. home screen
    std::cout << "\n1. Home screen\n";
    CHECK (stage.showingHome() && stage.addAudio.isShowing() && stage.addVideo.isShowing() && stage.pasteLyrics.isShowing() && stage.singFree.isShowing(),
           "home shows the 4 options (audio, lyrics video, write lyrics, sing without music)");
    click (stage.pasteLyrics);
    pump (100);
    CHECK (studio.lyricsEditor.isVisible(), "'লিরিক্স লিখুন' opens the lyrics editor");
    studio.lyricsEditor.editor.setText (juce::String::fromUTF8 ("আমার প্রথম লাইন\nদ্বিতীয় লাইন\nতৃতীয় লাইন"));
    click (studio.lyricsEditor.save);
    pump (200);
    CHECK (! studio.lyricsEditor.isVisible() && stage.hasLyrics() && stage.isTeleprompter() && ! stage.showingHome(), "own lyrics saved and shown (karaoke text without music)");

    studio.importAndAdd (song, false);
    CHECK (waitFor ([&] { return ! studio.importing.load() && musicIndex() >= 0; }, 20000), "audio file added as the music layer");

    // ------------------------------------------------------------------ 2. music layer: cut, move, range
    std::cout << "\n2. Music layer\n";
    tl.zoomToFit();
    pump (100);
    {
        const int mi = musicIndex();
        CHECK (mi == 0 && state().tracks.getReference (mi).clips.size() == 1, "music is its own layer at the top");
        click (tl.selectTool);
        mouse (tl, { tl.timeToX (2.0), laneY (mi) }, { tl.timeToX (3.0), laneY (mi) }, true);
        const auto sel = tl.getSelection();
        CHECK (std::abs (sel.getStart() - 2.0) < 0.1 && std::abs (sel.getEnd() - 3.0) < 0.1 && tl.cutBtn.isEnabled(), "drag selects a part of the song (" + juce::String (sel.getStart(), 2) + "-" + juce::String (sel.getEnd(), 2) + " s)");
        click (tl.loopBtn);
        pump (300);
        CHECK (engine.isLooping() && engine.isPlaying(), "'লুপ' plays only the selected part again and again");
        pump (1300);
        CHECK (engine.getPosition() >= sel.getStart() - 0.05 && engine.getPosition() <= sel.getEnd() + 0.05, "the loop stays inside the part (at " + juce::String (engine.getPosition(), 2) + " s)");
        click (tl.loopBtn);
        engine.pause();
        click (tl.cutBtn);
        pump (100);
        CHECK (state().tracks.getReference (mi).clips.size() == 2, "'কেটে ফেলুন' removes that part of the music");
        click (tl.undoBtn);
        pump (100);
        CHECK (state().tracks.getReference (mi).clips.size() == 1, "'আগের অবস্থা' brings it back");
        click (tl.moveTool);
        mouse (tl, { tl.timeToX (5.0), laneY (mi) }, { tl.timeToX (6.0), laneY (mi) }, true);
        pump (100);
        CHECK (std::abs (state().tracks.getReference (mi).clips.getReference (0).start - 1.0) < 0.1, "music clip dragged 1 s later with the mouse");
        click (tl.undoBtn);
        click (tl.selectTool);
        pump (100);
    }

    // ------------------------------------------------------------------ 3. lyrics: size, timing, speed
    std::cout << "\n3. Lyrics screen\n";
    {
        studio.saveLyrics (juce::String::fromUTF8 ("[00:01.00]এক\n[00:03.00]দুই\n[00:05.00]তিন\n[00:07.00]চার"));
        pump (200);
        engine.setPosition (3.5);
        pump (100);
        const int at = stage.currentLine();
        stage.setOffset (1.0);
        const int later = stage.currentLine();
        CHECK (at == 1 && later == 0, "timing (sync) offset moves the karaoke text (line " + juce::String (at) + " -> " + juce::String (later) + ")");
        const float fs = stage.getFontSize();
        stage.changeFontSize (6.0f);
        CHECK (stage.getFontSize() > fs, "text size bigger/smaller works");
        click (studio.lyricsBtn);
        pump (200);
        bool panel = false;
        for (auto* ch : studio.getChildren())
            if (auto* box = dynamic_cast<juce::CallOutBox*> (ch)) { panel = true; box->dismiss(); }
        pump (200);
        CHECK (panel, "'লিরিক্স' button opens the size / speed / timing panel");
        stage.setOffset (0.0);
        // plain text: scroll speed
        studio.saveLyrics (juce::String::fromUTF8 ("লাইন এক\nলাইন দুই"));
        pump (100);
        const float sp = stage.getSpeed();
        stage.changeSpeed (0.2f);
        CHECK (stage.isTeleprompter() && stage.getSpeed() > sp, "scroll speed control for plain lyrics");
    }

    // ------------------------------------------------------------------ 12. space bar plays from where you click
    std::cout << "\n12. Space bar\n";
    {
        mouse (tl, { tl.timeToX (4.0), (float) tl.rulerArea.getCentreY() }, { tl.timeToX (4.0), (float) tl.rulerArea.getCentreY() }, false);
        CHECK (std::abs (engine.getPosition() - 4.0) < 0.1, "click on the timeline moves the play position (" + juce::String (engine.getPosition(), 2) + " s)");
        if (auto* f = juce::Component::getCurrentlyFocusedComponent()) std::cout << "        focus: " << typeid (*f).name() << "\n";
        mc->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        pump (500);
        const bool played = engine.isPlaying();
        mc->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        pump (100);
        CHECK (played && ! engine.isPlaying() && engine.getPosition() > 4.2, "Space plays from there and Space again pauses (at " + juce::String (engine.getPosition(), 2) + " s)");
    }

    // ------------------------------------------------------------------ 13. record, then play: voice WITH music
    std::cout << "\n13. Record -> play: the voice is heard with the music\n";
    {
        tl.selectTrack (voiceId());
        engine.setPosition (1.0);
        pump (200);
        click (studio.recBtn);
        pump (100);
        CHECK (engine.isRecording(), "record button starts recording into the voice layer");
        pump (3000);
        click (studio.recBtn);
        CHECK (waitFor ([&] { return ! state().tracks.getReference (voiceIndex()).clips.isEmpty(); }, 3000), "recording placed on the voice layer");

        // music muted with its own button: what is left is the recorded voice (with effects)
        tl.selectTrack (state().tracks.getReference (musicIndex()).id);
        pump (50);
        click (ic.muteBtn);
        pump (100);
        auto voiceOnly = capture (1.5, 1500);
        click (ic.muteBtn);
        pump (100);
        auto both = capture (1.5, 1500);
        tl.selectTrack (voiceId());
        pump (50);
        click (ic.muteBtn);
        pump (100);
        auto musicOnly = capture (1.5, 1500);
        click (ic.muteBtn);
        pump (100);
        const double v = rms (voiceOnly, 4800), b = rms (both, 4800), m = rms (musicOnly, 4800);
        std::cout << "        level: voice " << v << ", music " << m << ", together " << b << "\n";
        CHECK (v > 0.02, "the recorded voice is heard on playback (with effects)");
        CHECK (b > m * 1.3 && b > v * 1.03, "voice and music play together (music gives way a little: smart mix)");
        // play from somewhere else too
        auto later = capture (3.0, 800);
        CHECK (rms (later, 4800) > m * 0.5, "voice + music also when playing from another place");
    }

    // ------------------------------------------------------------------ 6. separate volume (%) and mute
    std::cout << "\n6. Volume and mute per layer\n";
    {
        tl.selectTrack (voiceId());
        pump (50);
        ic.volKnob.slider.setValue (50.0);
        pump (100);
        CHECK (std::abs (state().tracks.getReference (voiceIndex()).gain - 0.5f) < 0.01f, "voice volume knob 50% -> layer volume 50%");
        ic.volKnob.slider.setValue (100.0);
        tl.selectTrack (state().tracks.getReference (musicIndex()).id);
        pump (50);
        click (ic.muteBtn);
        tl.selectTrack (voiceId());
        pump (50);
        click (ic.muteBtn);
        pump (100);
        auto silent = capture (1.5, 700);
        float peak = 0; for (size_t i = 4800; i < silent.size(); ++i) peak = juce::jmax (peak, std::abs (silent[i]));
        CHECK (peak < 1e-5f, "both layers muted = silence (peak " + juce::String (peak, 7) + ")");
        click (ic.soloBtn);
        CHECK (state().tracks.getReference (voiceIndex()).solo, "'S' (listen to this layer only) works");
        click (ic.soloBtn);
        click (ic.muteBtn);
        tl.selectTrack (state().tracks.getReference (musicIndex()).id);
        pump (50);
        click (ic.muteBtn);
        pump (100);
        tl.selectTrack (voiceId());
    }

    // ------------------------------------------------------------------ 5. drag the voice clip
    std::cout << "\n5. Move the voice with the mouse\n";
    {
        const int vi = voiceIndex();
        const double before = state().tracks.getReference (vi).clips.getReference (0).start;
        click (tl.moveTool);
        mouse (tl, { tl.timeToX (before + 1.0), laneY (vi) }, { tl.timeToX (before + 1.5), laneY (vi) }, true);
        pump (100);
        const double after = state().tracks.getReference (vi).clips.getReference (0).start;
        CHECK (std::abs (after - before - 0.5) < 0.05, "voice clip dragged 0.5 s later (" + juce::String (before, 2) + " -> " + juce::String (after, 2) + " s)");
        click (tl.undoBtn);
        click (tl.selectTool);
        pump (100);
    }

    // ------------------------------------------------------------------ 4. sing a part again
    std::cout << "\n4. Punch-in (sing a part again)\n";
    {
        const int vi = voiceIndex();
        const int clipsBefore = state().tracks.getReference (vi).clips.size();
        mouse (tl, { tl.timeToX (2.0), laneY (vi) }, { tl.timeToX (2.6), laneY (vi) }, true);
        click (tl.redoPartBtn);
        pump (100);
        CHECK (engine.isRecording(), "'এই অংশ আবার গাই' starts recording with a cue before the part");
        CHECK (waitFor ([&] { return ! engine.isRecording(); }, 8000), "it stops by itself after the part");
        pump (300);
        auto& clips = state().tracks.getReference (vi).clips;
        CHECK (clips.size() == clipsBefore + 2, "the old take is split and the new part placed between (" + juce::String (clips.size()) + " clips)");
        bool overlap = false;
        for (auto& a : clips) for (auto& b : clips) if (a.id != b.id && a.file != b.file && a.end() > b.start && a.start < b.start && a.end() - b.start < 0.02) overlap = true;
        CHECK (overlap, "the new part cross-fades into the old take");
    }

    // ------------------------------------------------------------------ 7. one-click noise remover
    std::cout << "\n7. Noise remover (one click, on the recording)\n";
    {
        tl.selectTrack (voiceId());
        pump (50);
        const auto filesBefore = state().tracks.getReference (voiceIndex()).clips.getReference (0).file;
        click (studio.noiseBtn);
        CHECK (waitFor ([&] { return ! insp.isBusy(); }, 60000), "noise removal finished");
        pump (200);
        const auto after = state().tracks.getReference (voiceIndex()).clips.getReference (0).file;
        CHECK (after != filesBefore && after.getFileName().contains ("_clean") && after.existsAsFile(), "one click: the recording is replaced by a cleaned copy (" + after.getFileName() + ")");
        click (tl.undoBtn);
        CHECK (state().tracks.getReference (voiceIndex()).clips.getReference (0).file == filesBefore, "Ctrl+Z brings the original recording back");
        click (studio.noiseBtn);
        waitFor ([&] { return ! insp.isBusy(); }, 60000);
        click (tl.undoBtn);
    }

    // ------------------------------------------------------------------ 9. presets
    std::cout << "\n9. Presets\n";
    {
        tl.selectTrack (voiceId());
        pump (100);
        ic.catButtons[0]->triggerClick();   // all
        pump (100);
        int ok = 0, total = 0;
        juce::StringArray badOnes;
        for (int i = 0; i < (int) builtInPresets().size(); ++i)
        {
            if (i >= ic.presetButtons.size()) break;
            ic.presetButtons[i]->triggerClick();
            pump (60);
            const auto& pr = builtInPresets()[(size_t) i];
            FxParams ref; pr.applyTo (ref);
            auto* ef = engine.getTrackFx (voiceId());
            ++total;
            if (state().tracks.getReference (voiceIndex()).presetId == pr.id && ef != nullptr && ef->toString() == ref.toString()) ++ok;
            else badOnes.add (pr.id);
        }
        CHECK (ok == 22 && total == 22, "every preset button applies its sound to the voice (" + juce::String (ok) + "/22)" + (badOnes.isEmpty() ? juce::String() : " " + badOnes.joinIntoString (",")));
        // live mode: the live microphone uses the same voice effects
        CHECK (engine.getArmedTrack() == voiceId(), "live microphone uses the selected voice layer's preset");
        click (ic.abBtn);
        CHECK (engine.getTrackFx (voiceId())->bypass.load(), "'আগে/পরে' switches the effects off for comparison");
        click (ic.abBtn);
        CHECK (! engine.getTrackFx (voiceId())->bypass.load(), "and on again");
        // my presets
        auto id = UserPresets (*props).add (juce::String::fromUTF8 ("আমার গলা"), engine.getTrackFx (voiceId())->toString());
        insp.rebuildPresetList();
        insp.selectPreset (id);
        pump (100);
        CHECK (state().tracks.getReference (voiceIndex()).presetId == id && ic.renameBtn.isEnabled() && ic.delBtn.isEnabled(), "my preset can be chosen, renamed and deleted");
        UserPresets (*props).rename (id, "renamed");
        props->saveIfNeeded();
        juce::PropertiesFile reopened (tmp.getChildFile ("settings.xml"), po);
        juce::Array<UserPreset> store;
        auto* found = UserPresets (reopened).find (id, store);
        CHECK (found != nullptr && found->name == "renamed", "my preset is still there after restarting");
    }

    // ------------------------------------------------------------------ 10. pitch correction strength
    std::cout << "\n10. Pitch correction\n";
    {
        for (auto& pk : ic.paramKnobs)
            if (pk.second == FxParams::tuneAmt) { pk.first->slider.setValue (80.0); }
        pump (900);
        CHECK (std::abs (engine.getTrackFx (voiceId())->get (FxParams::tuneAmt) - 80.0f) < 0.5f
               && state().tracks.getReference (voiceIndex()).fx.contains ("tuneAmt=80"), "auto-tune strength knob sets the correction (80%) and is saved");
        ic.scaleRow.box.setSelectedId (3, juce::sendNotification);   // C major
        pump (200);   // combo boxes report changes asynchronously
        CHECK (engine.getScaleMode() == 0 && engine.getEffectiveKey() == 0, "scale can be chosen (C major)");
        ic.scaleRow.box.setSelectedId (1, juce::sendNotification);   // follow the song
        pump (200);
        CHECK (engine.getScaleMode() == -2, "scale back to 'follow the song'");
    }

    // ------------------------------------------------------------------ speed
    std::cout << "\nMusic speed\n";
    {
        tl.selectTrack (state().tracks.getReference (musicIndex()).id);
        pump (100);
        ic.speedButtons[3]->triggerClick();   // 110%
        pump (100);
        click (ic.speedApply);
        const bool sped = waitFor ([&] { return ! insp.isBusy() && std::abs (state().musicTrack()->speed - 1.1) < 1e-6; }, 60000);
        if (! sped) std::cout << "        speed now " << state().musicTrack()->speed << ", slider " << ic.speedSlider.slider.getValue() << ", busy " << (int) insp.isBusy() << ", status '" << ic.speedBar.status << "'\n";
        CHECK (sped, "speed 110% applied (pitch kept)");
        ic.speedButtons[2]->triggerClick();   // 100%
        pump (100);
        click (ic.speedApply);
        waitFor ([&] { return ! insp.isBusy(); }, 5000);
        CHECK (std::abs (state().musicTrack()->speed - 1.0) < 1e-6, "back to normal speed");
    }

    // ------------------------------------------------------------------ 8. key change (music higher / lower, speed kept)
    std::cout << "\n8. Key change\n";
    {
        const auto before = state().musicTrack()->clips.getReference (0).file;
        click (ic.keyUp);
        click (ic.keyUp);
        click (ic.keyApply);
        CHECK (waitFor ([&] { return ! insp.isBusy() && state().musicTrack()->semitones == 2; }, 120000), "music 2 steps higher");
        CHECK (state().musicTrack()->clips.getReference (0).file != before && std::abs (state().musicTrack()->speed - 1.0) < 1e-6, "new audio, same speed");
        click (tl.undoBtn);
        CHECK (state().musicTrack()->semitones == 0, "undo brings the original key back");
    }

    // ------------------------------------------------------------------ 15. exports
    std::cout << "\n15. Exports\n";
    {
        auto runExport = [&] (int choice, juce::Range<double> sel) -> juce::StringArray
        {
            bool done = false;
            studio.onExportsDone = [&] { done = true; };
            studio.exportChoice (choice, sel);
            waitFor ([&] { return done; }, 180000);
            studio.onExportsDone = nullptr;
            if (! studio.exportErrors.isEmpty()) std::cout << "        error: " << studio.exportErrors.joinIntoString (" | ") << "\n";
            return studio.exportedFiles;
        };
        const bool ff = media::ffmpeg().existsAsFile();
        auto mp4 = runExport (1, {});
        CHECK (! ff || (mp4.size() == 1 && mp4[0].endsWith (".mp4") && media::probe (juce::File (mp4[0])).hasVideo), "(ক) MP4 video with lyrics");
        auto mix = runExport (2, {});
        CHECK (! ff || (mix.size() == 1 && mix[0].endsWith (".mp3") && juce::File (mix[0]).getSize() > 10000), "(খ) whole mix as MP3");
        auto voice = runExport (3, {});
        CHECK (! ff || (voice.size() == 1 && voice[0].contains (juce::String::fromUTF8 ("শুধু কণ্ঠ"))), "(গ) only the voice");
        auto music = runExport (4, {});
        CHECK (! ff || (music.size() == 1 && music[0].contains (juce::String::fromUTF8 ("শুধু মিউজিক"))), "(ঘ) only the music");
        auto two = runExport (5, {});
        CHECK (! ff || two.size() == 2, "(ঙ) voice and music as two files at once");
        auto wav = runExport (6, {});
        CHECK (wav.size() == 1 && wav[0].endsWith (".wav"), "WAV (best quality)");
        // the voice-only file really holds only the voice: it is silent where no voice was recorded
        if (ff && voice.size() == 1 && music.size() == 1)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rv (fm.createReaderFor (juce::File (voice[0]))), rm (fm.createReaderFor (juce::File (music[0])));
            auto levelAt = [] (juce::AudioFormatReader* r, double t0, double t1)
            {
                if (r == nullptr) return 0.0;
                juce::AudioBuffer<float> b (2, (int) ((t1 - t0) * r->sampleRate));
                r->read (&b, 0, b.getNumSamples(), (juce::int64) (t0 * r->sampleRate), true, true);
                return (double) b.getRMSLevel (0, 0, b.getNumSamples());
            };
            CHECK (levelAt (rv.get(), 9.0, 11.0) < 0.002 && levelAt (rv.get(), 1.5, 3.0) > 0.01, "voice file: voice where sung, silence elsewhere");
            CHECK (levelAt (rm.get(), 9.0, 11.0) > 0.05, "music file: the music");
        }
        for (auto& f : mp4) juce::File (f).deleteFile();
        for (auto* l : { &mix, &voice, &music, &two, &wav }) for (auto& f : *l) juce::File (f).deleteFile();
    }

    // ------------------------------------------------------------------ home, reset, library
    std::cout << "\nHome, new start and the library\n";
    juce::File oldProject;
    {
        click (mc->homeBtn);
        pump (100);
        CHECK (stage.showingHome() && stage.backToProject.isVisible() && stage.newStart.isVisible(), "'হোম' shows the start options from anywhere, with 'back to project' and 'new project'");
        click (stage.backToProject);
        pump (100);
        CHECK (! stage.showingHome(), "'চলমান প্রজেক্টে ফিরুন' returns to the work");
        click (mc->libraryTab);
        pump (100);
        click (mc->homeBtn);
        pump (100);
        CHECK (studio.isVisible() && stage.showingHome(), "'হোম' also works from the library page");
        oldProject = project.getFile();
        studio.newProject();   // what 'নতুন শুরু' does after its "are you sure?"
        pump (300);
        CHECK (oldProject.existsAsFile() && project.getFile() != oldProject && state().length() < 1e-9 && stage.showingHome(), "new start: a fresh project, the old one kept on disk");
        click (mc->libraryTab);
        mc->library.setTab (true);
        pump (200);
        bool listed = false;
        LibraryView::Item item;
        for (auto& it : mc->library.items) if (it.file == oldProject) { listed = true; item = it; }
        CHECK (listed, "the old project is in the library ('আমার প্রজেক্ট')");
        mc->library.onProjectDeleted = nullptr;
        studio.openProject (oldProject);
        pump (300);
        CHECK (project.getFile() == oldProject && state().length() > 1.0, "an old project opens again from the library");
        mc->library.onProjectDeleted = [&] (const juce::File& f) { studio.projectFileDeleted (f); };
        mc->library.deleteNow (item);
        pump (1200);
        CHECK (! oldProject.existsAsFile() && project.getFile() != oldProject, "a project can be deleted from the library (and is not saved back)");
        click (mc->studioTab);
        pump (100);
    }

    // ------------------------------------------------------------------ 14. sing without music
    std::cout << "\n14. Singing without music\n";
    {
        click (stage.singFree);
        pump (100);
        CHECK (engine.isRecording(), "'মিউজিক ছাড়াই গান করুন' starts recording at once");
        dev->extraNoise = 0.2f;   // the "voice"
        pump (2000);
        dev->extraNoise = 0.0f;
        mc->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));   // Space stops it
        CHECK (waitFor ([&] { return voiceIndex() >= 0 && ! state().tracks.getReference (voiceIndex()).clips.isEmpty(); }, 3000), "Space stops; the recording is on the voice layer");
        auto out = capture (0.2, 1200);
        CHECK (rms (out, 4800) > 0.001, "it plays back with effects");
        const int vi = voiceIndex();
        mouse (tl, { tl.timeToX (0.5), laneY (vi) }, { tl.timeToX (1.0), laneY (vi) }, true);
        click (tl.cutBtn);
        pump (100);
        CHECK (state().tracks.getReference (vi).clips.size() == 2, "and a part of it can be cut out");
    }

    // ------------------------------------------------------------------ settings, live mic
    std::cout << "\nOther buttons\n";
    {
        const int before = juce::TopLevelWindow::getNumTopLevelWindows();
        click (mc->settingsBtn);
        pump (300);
        CHECK (juce::TopLevelWindow::getNumTopLevelWindows() == before + 1, "'অডিও সেটিংস' opens the settings window");
        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            if (auto* d = dynamic_cast<juce::DialogWindow*> (juce::TopLevelWindow::getTopLevelWindow (i))) d->closeButtonPressed();
        pump (300);
        const bool live = engine.isMonitorEnabled();
        click (mc->liveBtn);
        CHECK (engine.isMonitorEnabled() != live, "'লাইভ মাইক' switches the live microphone");
        click (mc->liveBtn);
        click (tl.addTrackBtn);
        pump (100);
        int voices = 0; for (auto& t : state().tracks) if (t.kind == TrackKind::vocal) ++voices;
        CHECK (voices == 2, "'কণ্ঠ ট্র্যাক' adds a second voice layer (e.g. for the daughter)");
        const double z = tl.pxPerSec;
        click (tl.zoomInBtn);
        CHECK (tl.pxPerSec > z, "zoom in / out");
        click (tl.zoomOutBtn);
    }

    // clean up what the test made in the user's folders
    engine.stop();
    const auto projFile = project.getFile();
    win.clearContentComponent();
    mc.reset();
    projFile.deleteFile();
    for (auto& f : Project::projectsFolder().findChildFiles (juce::File::findFiles, false, "*.tfvs"))
        if (testStart < f.getLastModificationTime() && (f.getFileName().startsWith ("UI Test Song") || f.getFileName().startsWith (juce::String::fromUTF8 ("নতুন প্রজেক্ট")))) f.deleteFile();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    tmp.deleteRecursively();

    std::cout << "\n" << (failures == 0 ? "ALL UI TESTS PASSED" : juce::String (failures) + " UI TEST(S) FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
}

int main()
{
    std::cout << std::unitbuf;
    juce::ScopedJuceInitialiser_GUI init;
    return tf::UITest::run();
}
