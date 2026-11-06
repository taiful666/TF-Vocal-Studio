#pragma once

#include "Theme.h"
#include "AudioEngine.h"

namespace tf
{
//==============================================================================
/** A large, clear microphone level bar for recording: shows whether the voice is too soft,
    just right or too loud (with a word, not only colours). */
class InputMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevel (float peak);
    void paint (juce::Graphics&) override;
    /** -1 too soft, 0 good, 1 too loud (for the tests). */
    int verdict() const noexcept { return state; }
private:
    float shownDb = -60.0f, holdDb = -60.0f, loudHold = 0.0f;
    int holdFrames = 0, state = -1;
    double lastLoud = 0.0;
};

//==============================================================================
/** Live tuner: the note being sung and how far from it (cents), green when in tune. */
class PitchMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setPitch (float hz, int songKey);
    void paint (juce::Graphics&) override;
    juce::String noteText() const { return note; }
private:
    juce::String note;
    float cents = 0.0f, shownCents = 0.0f;
    bool voiced = false, inScale = true;
    int silentFrames = 100;
};

//==============================================================================
/** Easy karaoke timing: the song plays, and one tap (button or Enter) marks where each line starts.
    The result is saved as timed (.lrc) lyrics. */
class TapTiming : public juce::Component, private juce::Timer
{
public:
    TapTiming (AudioEngine& e) : engine (e)
    {
        for (auto* b : { &tapBtn, &backBtn, &saveBtn, &cancelBtn }) { b->setRadius (12.0f); addAndMakeVisible (b); }
        tapBtn.setFontSize (20.0f);
        tapBtn.onClick = [this] { tap(); };
        backBtn.onClick = [this] { undoTap(); };
        saveBtn.onClick = [this] { finish (true); };
        cancelBtn.onClick = [this] { finish (false); };
    }
    /** lines: the plain lyrics; songTime: timeline -> song time (the music may be sped up). */
    void start (const juce::StringArray& lines, std::function<double (double)> songTime);
    void tap();
    void undoTap();
    bool isActive() const noexcept { return active; }
    int tapped() const noexcept { return (int) times.size(); }
    /** The timed lyrics as .lrc text. */
    juce::String toLrc() const;
    std::function<void (const juce::String& lrc)> onSave;
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override { repaint(); }
    void finish (bool save);
    AudioEngine& engine;
    juce::StringArray lines;
    std::vector<double> times;
    std::function<double (double)> toSong;
    bool active = false;
    friend struct UITest;
    TFButton tapBtn { juce::String::fromUTF8 ("এই লাইন শুরু (Enter)"), TFButton::Style::primary },
             backBtn { juce::String::fromUTF8 ("আগেরটা মুছুন") },
             saveBtn { juce::String::fromUTF8 ("সেভ করুন"), TFButton::Style::accent },
             cancelBtn { juce::String::fromUTF8 ("বাতিল") };
};

//==============================================================================
/** 3-2-1 count-in over the stage before recording starts (with beeps). */
class CountIn : public juce::Component, private juce::Timer
{
public:
    CountIn (AudioEngine& e) : engine (e) { setInterceptsMouseClicks (false, false); }
    void start (std::function<void()> onGo);
    void cancel();
    bool isCounting() const noexcept { return counting; }
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    AudioEngine& engine;
    std::function<void()> go;
    int count = 0;
    bool counting = false;
    double tickStart = 0.0;
};
}
