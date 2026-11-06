#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Lyrics.h"

namespace tf
{
//==============================================================================
class LyricsView : public juce::Component
{
public:
    explicit LyricsView (AudioEngine& e);
    void setLyrics (Lyrics l);
    void paint (juce::Graphics&) override;
    void resized() override;

    TFButton openLibrary { juce::String::fromUTF8 ("লাইব্রেরি থেকে গান বাছুন"), TFButton::Style::accent };
    TFButton openFile    { juce::String::fromUTF8 ("ফাইল থেকে খুলুন"), TFButton::Style::tool };

private:
    void tick();
    void drawLine (juce::Graphics&, const juce::String&, float centreY, float size, float alpha, double progress, bool current);

    AudioEngine& engine;
    Lyrics lyrics;
    float scrollIdx = 0.0f;
    double lastTick = 0;
    juce::VBlankAttachment vblank { this, [this] { tick(); } };
};

//==============================================================================
class WaveformView : public juce::Component, private juce::ChangeListener
{
public:
    explicit WaveformView (AudioEngine& e);
    ~WaveformView() override;
    void setSong (const juce::File& f);
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
class LevelMeter : public juce::Component
{
public:
    void setLevel (float peak);      // linear peak, called ~30 Hz
    void paint (juce::Graphics&) override;
private:
    float shown = 0.0f, hold = 0.0f;
    int holdFrames = 0;
};

//==============================================================================
/** Row with a label (and optional note) and a switch on the right. */
class SwitchRow : public juce::Component
{
public:
    SwitchRow (const juce::String& label, const juce::String& note = {});
    TFSwitch sw;
    void paint (juce::Graphics&) override;
    void resized() override;
    void setNote (const juce::String& n) { note = n; repaint(); }
private:
    juce::String text, note;
};

//==============================================================================
/** Collapsible effect row: chevron + name + summary (+ on/off switch). Click to expand. */
class FxSection : public juce::Component
{
public:
    FxSection (const juce::String& title, bool hasSwitch);
    void addItem (juce::Component& c, int height);
    void setExpanded (bool e, bool animate = true);
    bool isExpanded() const { return expanded; }
    int getDesiredHeight() const;
    void refreshHeader() { header.repaint(); }

    std::function<juce::String()> summary;
    std::function<void()> onLayoutChange;
    std::function<void (FxSection*)> onExpand;
    TFSwitch sw;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Header : public juce::Button
    {
        explicit Header (FxSection& o) : juce::Button ("header"), owner (o) { setMouseCursor (juce::MouseCursor::PointingHandCursor); setMouseClickGrabsKeyboardFocus (false); }
        void paintButton (juce::Graphics&, bool over, bool down) override;
        FxSection& owner;
    };

    juce::String title;
    bool hasSwitch;
    bool expanded = false;
    Header header { *this };
    Smoother anim;
    juce::Array<std::pair<juce::Component*, int>> items;
    int contentHeight() const;
};

//==============================================================================
class RightColumn : public juce::Component
{
public:
    RightColumn (AudioEngine&, juce::PropertiesFile&);
    void updateLayout();
    void resized() override;
    void paint (juce::Graphics&) override;
    void updateMeters();
    void refreshAvailability();

private:
    AudioEngine& engine;
    juce::PropertiesFile& props;

    // meters card
    LevelMeter micMeter, outMeter;
    juce::String micStatus;
    juce::Colour micStatusColour = col::okText;
    int silentFrames = 0, loudFrames = 0;
    juce::Rectangle<int> meterCard, fxCard;

    // effects
    FxSection noiseSec { juce::String::fromUTF8 ("AI নয়েজ রিমুভ"), true };
    FxSection gainSec  { juce::String::fromUTF8 ("মাইক গেইন"), false };
    FxSection mixSec   { juce::String::fromUTF8 ("ভলিউম মিক্স"), false };

    LabeledSlider noiseAmt { juce::String::fromUTF8 ("পরিমাণ"), 0, 100, 80, [] (double v) { return bn ((int) std::round (v)) + "%"; } };
    SwitchRow noiseMonitor { juce::String::fromUTF8 ("স্পিকারেও পরিষ্কার কণ্ঠ শুনুন"), juce::String::fromUTF8 ("চালু করলে ১০ ms দেরি বাড়ে") };
    LabeledSlider gain { juce::String::fromUTF8 ("মাইকের জোর"), -12, 24, 0, [] (double v) { return (v > 0.05 ? "+" : "") + bn (juce::String (v, 1)) + " dB"; } };
    LabeledSlider voice { juce::String::fromUTF8 ("কণ্ঠ"), 0, 100, 80, [] (double v) { return bn ((int) std::round (v)) + "%"; } };
    LabeledSlider music { juce::String::fromUTF8 ("মিউজিক"), 0, 100, 65, [] (double v) { return bn ((int) std::round (v)) + "%"; } };
    SwitchRow monitor { juce::String::fromUTF8 ("স্পিকারে নিজের কণ্ঠ শুনুন") };

    std::array<FxSection*, 3> sections { &noiseSec, &gainSec, &mixSec };
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

    void loadSong (const juce::File& f);
    void loadTake (const Take& t);
    void togglePlay();
    void toggleRecord();
    void goToStart();
    void chooseFile();

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
    void showSaveMenu();
    void showError (const juce::String& msg);

    AudioEngine& engine;
    juce::PropertiesFile& props;
    Lyrics lyrics;

    juce::Rectangle<int> lyricsCard, waveCard, transportCard, timeArea;
    LyricsView lyricsView { engine };
    WaveformView waveform { engine };

    TFButton toStartBtn { juce::String::fromUTF8 ("শুরুতে যান") }, stopBtn { juce::String::fromUTF8 ("থামান") },
             playBtn { juce::String::fromUTF8 ("চালান"), TFButton::Style::primary },
             recBtn { juce::String::fromUTF8 ("রেকর্ড"), TFButton::Style::record },
             listenBtn { juce::String::fromUTF8 ("রেকর্ডিং শুনুন") },
             saveBtn { juce::String::fromUTF8 ("সেভ / এক্সপোর্ট") };

    juce::Viewport rightViewport;
    RightColumn right { engine, props };

    std::unique_ptr<juce::FileChooser> chooser;
    float exportProgress = -1.0f;
    bool wasPlaying = false;
};
}
