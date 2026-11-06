#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Project.h"
#include "Lyrics.h"
#include "Media.h"
#include "Timeline.h"
#include "Inspector.h"

namespace tf
{
//==============================================================================
/** A big option card on the home screen (icon, title, one line of help). */
class OptionCard : public juce::Button
{
public:
    OptionCard (const juce::String& title, const juce::String& sub, juce::Path icon, bool primary = false);
    void paintButton (juce::Graphics&, bool over, bool down) override;
private:
    juce::String sub;
    juce::Path icon;
    bool primary;
};

//==============================================================================
/** The stage: synced video, karaoke (.lrc) or a teleprompter for plain lyrics — or the home options. */
class LyricsView : public juce::Component
{
public:
    LyricsView (AudioEngine&, Project&, juce::PropertiesFile&);
    void setLyrics (Lyrics l);
    void setVideo (const juce::File& videoOrNone);
    bool hasVideo() const { return video.isOpen(); }
    bool hasLyrics() const { return ! lyrics.isEmpty(); }
    bool isTeleprompter() const { return ! lyrics.isEmpty() && ! lyrics.timed; }
    bool showingHome() const { return home; }

    enum class Mode { video, lyrics };
    void setMode (Mode m) { mode = m; repaint(); }
    Mode getMode() const { return mode; }

    void changeFontSize (float delta);
    void changeSpeed (float delta);
    float getSpeed() const { return speed; }
    void resetScroll() { manualOffset = 0.0f; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    OptionCard addAudio, addVideo, pasteLyrics, singFree;
    TFButton openLibrary { juce::String::fromUTF8 ("লাইব্রেরি থেকে গান বাছুন"), TFButton::Style::ghost };
    TFButton writeLyrics { juce::String::fromUTF8 ("লিরিক্স লিখুন বা পেস্ট করুন"), TFButton::Style::accent };

private:
    void tick();
    double songTime() const;
    void paintKaraoke (juce::Graphics&, juce::Rectangle<float>);
    void paintTeleprompter (juce::Graphics&, juce::Rectangle<float>);
    void drawLine (juce::Graphics&, const juce::String&, float centreY, float size, float alpha, double progress, bool current);
    void rebuildLayout();
    float pixelsPerSecond() const;
    void updateHome();

    AudioEngine& engine;
    Project& project;
    juce::PropertiesFile& props;
    Lyrics lyrics;
    VideoFrames video;
    Mode mode = Mode::lyrics;
    bool home = false;

    float scrollIdx = 0.0f;
    float fontSize = 34.0f, speed = 1.0f;
    float manualOffset = 0.0f, dragStartOffset = 0.0f;
    juce::TextLayout layout;
    float layoutWidth = 0.0f, layoutFont = 0.0f;
    double lastTick = 0;
    juce::Image lastFrame;
    juce::Rectangle<int> homeTitle;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };
};

//==============================================================================
/** In-place editor for typing/pasting lyrics. */
class LyricsEditor : public juce::Component
{
public:
    LyricsEditor();
    void open (const juce::String& text);
    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void (const juce::String&)> onSave;
    std::function<void()> onCancel;
    juce::TextEditor editor;
private:
    TFButton save { juce::String::fromUTF8 ("সেভ করুন"), TFButton::Style::accent },
             cancel { juce::String::fromUTF8 ("বাতিল"), TFButton::Style::tool },
             fromFile { juce::String::fromUTF8 ("ফাইল থেকে আনুন"), TFButton::Style::tool };
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** Progress overlay for long jobs (importing a video). */
class ProgressOverlay : public juce::Component, private juce::Timer
{
public:
    ProgressOverlay();
    void show (const juce::String& title);
    void setProgress (float p, const juce::String& detail = {});
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override { if (! isVisible()) stopTimer(); }
    std::function<void()> onCancel;
private:
    void timerCallback() override { phase += 0.02f; if (phase > 1.0f) phase -= 1.0f; repaint(); }
    juce::String title, detail;
    float progress = 0.0f, phase = 0.0f;
    TFButton cancel { juce::String::fromUTF8 ("বাতিল"), TFButton::Style::tool };
};

//==============================================================================
class StudioView : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    StudioView (AudioEngine&, juce::PropertiesFile&);
    ~StudioView() override;

    /** From the library: sing this song (a new project unless the current one is still empty). */
    void openSong (const juce::File& f);
    void openProject (const juce::File& f);
    void newProject();
    void togglePlay();
    void toggleRecord();
    void goToStart();
    void chooseFile (bool video = false);
    void undo()  { if (! engine.isRecording()) project.undo(); }
    void redo()  { if (! engine.isRecording()) project.redo(); }
    void deleteSelection() { timeline.deleteSelection(); }
    void toggleLoop();
    /** Stops playback/recording and puts any recording onto its track right away (used when closing). */
    void stopAndKeep();
    bool isBusy() const { return importing.load() || inspector.isBusy(); }
    Project& getProject() { return project; }

    std::function<void()> onOpenLibrary;
    std::function<void (const juce::String&)> toast;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void projectChanged();
    void updateTransportButtons();
    void updateToolbar();
    void showSaveMenu();
    void showAddMenu();
    void exportRange (juce::Range<double> range, bool mp3);
    void showError (const juce::String& msg);
    void importAndAdd (const juce::File& f, bool copyToLibrary);
    void musicAdded (const juce::File& source, const juce::File& audio);
    void reloadStage();
    juce::File lyricsFile() const;
    void openLyricsEditor();
    void saveLyrics (const juce::String& text);
    juce::String recordTrack();
    void startRedoPart (const juce::String& trackId, juce::Range<double> range);
    void singWithoutMusic();
    void consumeRecording();

    AudioEngine& engine;
    juce::PropertiesFile& props;
    Project project;
    Lyrics lyrics;

    juce::Rectangle<int> stageCard, transportCard, timeArea, headArea, meterArea;
    LyricsView stage { engine, project, props };
    TimelineView timeline { project, engine };
    Inspector inspector { project, engine, props };
    LyricsEditor lyricsEditor;
    ProgressOverlay overlay;

    // stage toolbar
    TFButton addBtn { juce::String::fromUTF8 ("যোগ করুন"), TFButton::Style::accent },
             viewVideo { juce::String::fromUTF8 ("ভিডিও"), TFButton::Style::segment },
             viewLyrics { juce::String::fromUTF8 ("লিরিক্স"), TFButton::Style::segment },
             editLyricsBtn { juce::String::fromUTF8 ("লিরিক্স") },
             fontDown { juce::String::fromUTF8 ("অ−") }, fontUp { juce::String::fromUTF8 ("অ+") },
             slower { juce::String::fromUTF8 ("−") }, faster { "+" };
    juce::Rectangle<int> viewWell, speedArea;

    // transport
    TFButton toStartBtn { juce::String::fromUTF8 ("শুরুতে যান") }, stopBtn { juce::String::fromUTF8 ("থামান") },
             playBtn { juce::String::fromUTF8 ("চালান"), TFButton::Style::primary },
             recBtn { juce::String::fromUTF8 ("রেকর্ড"), TFButton::Style::record },
             saveBtn { juce::String::fromUTF8 ("সেভ / এক্সপোর্ট") };
    LevelMeter masterL, masterR;

    std::unique_ptr<juce::FileChooser> chooser;
    float exportProgress = -1.0f;
    bool wasPlaying = false, wasRecording = false;
    juce::File stageSource;
    juce::String stageLyricsKey;
    std::atomic<bool> importing { false }, importCancel { false };
    juce::ThreadPool jobs { juce::ThreadPoolOptions().withThreadName ("TF import").withNumberOfThreads (1) };
};
}
