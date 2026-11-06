#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Project.h"
#include "Lyrics.h"
#include "Media.h"
#include "Timeline.h"
#include "Inspector.h"
#include "RecordAids.h"
#include "Camera.h"

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
    float getFontSize() const { return fontSize; }
    /** Lyrics timing: + = lyrics come later, − = earlier (seconds). */
    void setOffset (double seconds);
    double getOffset() const { return offset; }
    void resetScroll() { manualOffset = 0.0f; }
    /** The karaoke line being sung now (-1 before the first), after the timing offset. */
    int currentLine() const { return lyrics.indexAt (songTime() - offset); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    OptionCard addAudio, addVideo, pasteLyrics, singFree;
    TFButton openLibrary { juce::String::fromUTF8 ("লাইব্রেরি থেকে গান বাছুন"), TFButton::Style::ghost };
    TFButton writeLyrics { juce::String::fromUTF8 ("লিরিক্স লিখুন বা পেস্ট করুন"), TFButton::Style::accent };
    TFButton backToProject { juce::String::fromUTF8 ("চলমান প্রজেক্টে ফিরুন"), TFButton::Style::tool };
    TFButton newStart { juce::String::fromUTF8 ("নতুন প্রজেক্ট (রিসেট)"), TFButton::Style::ghost };
    /** Shows the home options even when the project has content (Home button). */
    void setForceHome (bool f);

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
    bool home = false, forceHome = false;

    float scrollIdx = 0.0f;
    float fontSize = 34.0f, speed = 1.0f;
    float manualOffset = 0.0f, dragStartOffset = 0.0f;
    double offset = 0.0;
    juce::TextLayout layout;
    float layoutWidth = 0.0f, layoutFont = 0.0f;
    double lastTick = 0;
    juce::Image lastFrame;
    juce::Rectangle<int> homeTitle;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };
    friend struct UITest;
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
    camera::Recorder camRec;
    bool cameraOn = false;
    juce::File pendingCamera;
    TFButton camBtn { juce::String::fromUTF8 ("ক্যামেরা") };
    friend struct UITest;
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
/** How a file is saved: fades, loudness, tall video, camera. */
struct ExportSettings { double fadeIn = 0.0, fadeOut = 0.0; bool normalise = true, vertical = false, camera = true; };

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
    /** The library deleted this project file: if it is open, start a fresh project without saving it back. */
    void projectFileDeleted (const juce::File& f);
    void newProject();
    /** Home button: the start options, from anywhere. */
    void showHome();
    /** Reset button: keep the current project in the library and start a fresh one. */
    void resetProject();
    bool projectHasContent() const;
    void togglePlay();
    void toggleRecord();
    bool isCountingIn() const { return countIn.isCounting(); }
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
    std::function<void()> onOpenSettings;
    friend struct UITest;
    std::function<void (const juce::String&)> toast;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void splitAtPlayhead() { timeline.splitAtPlayhead(); }

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void projectChanged();
    void updateTransportButtons();
    void updateToolbar();
    void showAddMenu();
    void exportRange (juce::Range<double> range, bool mp3);
    enum class ExportFormat { mp3, wav, mp4 };
    struct ExportJob
    {
        ProjectState state; juce::Range<double> range; ExportFormat format; juce::String suffix;
        juce::File dest;                // where to save (empty: the Exports folder)
        ExportSettings settings;
    };
    /** choice: 1 video (wide), 9 video (tall, Reels), 2 whole song MP3, 3 voice only, 4 music only,
        5 voice + music as two files, 6 WAV, 7 selected part MP3. dest: file chosen in the save window. */
    void exportChoice (int choice, juce::Range<double> selection, ExportSettings settings = {}, juce::File dest = {});
    void showExportPanel();
    juce::File cameraFor (const ProjectState&, juce::Range<double>, double& cameraStart) const;
    void startExports (std::vector<ExportJob>);
    void runNextExport();
    void exportFinished();
    void showError (const juce::String& msg);
    void importAndAdd (const juce::File& f, bool copyToLibrary);
    void musicAdded (const juce::File& source, const juce::File& audio);
    void reloadStage();
    juce::File lyricsFile() const;
    void openLyricsEditor();
    void saveLyrics (const juce::String& text);
    juce::String recordTrack();
    void startRedoPart (const juce::String& trackId, juce::Range<double> range);
    void startRecordingNow();
    void singWithoutMusic();
    void consumeRecording();
    void updateNoiseButton();
    void showLyricsPanel();

    AudioEngine& engine;
    juce::PropertiesFile& props;
    Project project;
    Lyrics lyrics;

    juce::Rectangle<int> stageCard, transportCard, timeArea, headArea, splitterArea;
    int timelineHeight = -1, dragStartHeight = 0;
    bool draggingSplitter = false;
    LyricsView stage { engine, project, props };
    TimelineView timeline { project, engine };
    Inspector inspector { project, engine, props };
    LyricsEditor lyricsEditor;
    ProgressOverlay overlay;

    // stage toolbar
    TFButton addBtn { juce::String::fromUTF8 ("যোগ করুন"), TFButton::Style::accent },
             viewVideo { juce::String::fromUTF8 ("ভিডিও"), TFButton::Style::segment },
             viewLyrics { juce::String::fromUTF8 ("লিরিক্স"), TFButton::Style::segment },
             lyricsBtn { juce::String::fromUTF8 ("লিরিক্স") },
             noiseBtn { juce::String::fromUTF8 ("নয়েজ সরান") };
    juce::Rectangle<int> viewWell;

    // transport
    TFButton toStartBtn { juce::String::fromUTF8 ("শুরুতে যান") }, stopBtn { juce::String::fromUTF8 ("থামান") },
             playBtn { juce::String::fromUTF8 ("চালান"), TFButton::Style::primary },
             recBtn { juce::String::fromUTF8 ("রেকর্ড"), TFButton::Style::record },
             saveBtn { juce::String::fromUTF8 ("সেভ / এক্সপোর্ট") };
    InputMeter inMeter;
    PitchMeter pitchMeter;
    CountIn countIn { engine };

    std::unique_ptr<juce::FileChooser> chooser;
    camera::Recorder camRec;
    bool cameraOn = false;
    juce::File pendingCamera;
    TFButton camBtn { juce::String::fromUTF8 ("ক্যামেরা") };
    float exportProgress = -1.0f;
    std::vector<ExportJob> exportQueue;
    juce::StringArray exportedFiles, exportErrors;
public:
    std::function<void()> onExportsDone;   // for the automated test
    // camera (singers on video)
    void setCameraEnabled (bool on);
    bool isCameraEnabled() const { return cameraOn; }
    bool revealAfterExport = true;
private:
    bool wasPlaying = false, wasRecording = false;
    juce::File stageSource;
    juce::String stageLyricsKey;
    std::atomic<bool> importing { false }, importCancel { false };
    juce::ThreadPool jobs { juce::ThreadPoolOptions().withThreadName ("TF import").withNumberOfThreads (1) };
};
}
