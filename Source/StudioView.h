#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Lyrics.h"
#include "Media.h"
#include "FxPanel.h"

namespace tf
{
//==============================================================================
/** Lyrics area: synced video, karaoke (.lrc) or a teleprompter for plain text lyrics. */
class LyricsView : public juce::Component
{
public:
    LyricsView (AudioEngine& e, juce::PropertiesFile& p);
    void setLyrics (Lyrics l);
    void setVideo (const juce::File& videoOrNone);
    bool hasVideo() const { return video.isOpen(); }
    bool hasLyrics() const { return ! lyrics.isEmpty(); }
    bool isTeleprompter() const { return ! lyrics.isEmpty() && ! lyrics.timed; }

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

    TFButton addMedia     { juce::String::fromUTF8 ("অডিও / ভিডিও যোগ করুন"), TFButton::Style::accent };
    TFButton openLibrary  { juce::String::fromUTF8 ("লাইব্রেরি থেকে বাছুন"), TFButton::Style::tool };
    TFButton writeLyrics  { juce::String::fromUTF8 ("লিরিক্স লিখুন বা পেস্ট করুন"), TFButton::Style::accent };

private:
    void tick();
    void paintKaraoke (juce::Graphics&, juce::Rectangle<float>);
    void paintTeleprompter (juce::Graphics&, juce::Rectangle<float>);
    void drawLine (juce::Graphics&, const juce::String&, float centreY, float size, float alpha, double progress, bool current);
    void rebuildLayout();
    float pixelsPerSecond() const;

    AudioEngine& engine;
    juce::PropertiesFile& props;
    Lyrics lyrics;
    VideoFrames video;
    Mode mode = Mode::lyrics;

    float scrollIdx = 0.0f;
    float fontSize = 34.0f, speed = 1.0f;
    float manualOffset = 0.0f, dragStartOffset = 0.0f;
    juce::TextLayout layout;
    float layoutWidth = 0.0f, layoutFont = 0.0f;
    double lastTick = 0;
    juce::Image lastFrame;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };
};

//==============================================================================
class WaveformView : public juce::Component, private juce::ChangeListener
{
public:
    explicit WaveformView (AudioEngine& e);
    ~WaveformView() override;
    void setSong (const juce::File& audio);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void seekTo (float x);
    double timelineLength() const;
    juce::Rectangle<float> rowArea (int row) const;

    AudioEngine& engine;
    juce::AudioThumbnailCache cache { 4 };
    juce::AudioThumbnail thumb;
    double lastPos = -1;
    juce::VBlankAttachment vblank { this, [this]
    {
        auto p = engine.getPosition();
        if (std::abs (p - lastPos) > 1e-4 || engine.isRecording()) { lastPos = p; repaint(); }
    } };
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
/** Progress overlay for long jobs (importing a video, AI separation). */
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

    void loadSong (const juce::File& f);   // any audio or video file (imports when needed)
    void loadTake (const Take& t);
    void togglePlay();
    void toggleRecord();
    void goToStart();
    void chooseFile();
    bool isBusy() const { return importing.load(); }

    std::function<void()> onOpenLibrary;
    std::function<void (const juce::String&)> toast;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateTransportButtons();
    void updateToolbar();
    void showSaveMenu();
    void showError (const juce::String& msg);
    void importAndLoad (const juce::File& f, bool copyToLibrary);
    void songLoaded (const juce::File& f);
    void openLyricsEditor();
    void saveLyrics (const juce::String& text);

    AudioEngine& engine;
    juce::PropertiesFile& props;
    Lyrics lyrics;

    juce::Rectangle<int> lyricsCard, waveCard, transportCard, timeArea, headArea;
    LyricsView lyricsView { engine, props };
    WaveformView waveform { engine };
    LyricsEditor lyricsEditor;
    ProgressOverlay overlay;

    // lyrics toolbar
    TFButton viewVideo { juce::String::fromUTF8 ("ভিডিও"), TFButton::Style::segment },
             viewLyrics { juce::String::fromUTF8 ("লিরিক্স"), TFButton::Style::segment },
             editLyricsBtn { juce::String::fromUTF8 ("লিরিক্স") },
             fontDown { juce::String::fromUTF8 ("অ−") }, fontUp { juce::String::fromUTF8 ("অ+") },
             slower { juce::String::fromUTF8 ("−") }, faster { "+" },
             changeSong { juce::String::fromUTF8 ("গান বদলান") };
    juce::Rectangle<int> viewWell, speedArea;

    TFButton toStartBtn { juce::String::fromUTF8 ("শুরুতে যান") }, stopBtn { juce::String::fromUTF8 ("থামান") },
             playBtn { juce::String::fromUTF8 ("চালান"), TFButton::Style::primary },
             recBtn { juce::String::fromUTF8 ("রেকর্ড"), TFButton::Style::record },
             listenBtn { juce::String::fromUTF8 ("রেকর্ডিং শুনুন") },
             saveBtn { juce::String::fromUTF8 ("সেভ / এক্সপোর্ট") };

    juce::Viewport rightViewport;
    FxPanel right { engine, props };

    std::unique_ptr<juce::FileChooser> chooser;
    float exportProgress = -1.0f;
    bool wasPlaying = false;
    std::atomic<bool> importing { false }, importCancel { false };
    juce::ThreadPool jobs { juce::ThreadPoolOptions().withThreadName ("TF import").withNumberOfThreads (1) };
};
}
