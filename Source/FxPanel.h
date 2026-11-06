#pragma once

#include "Theme.h"
#include "AudioEngine.h"

namespace tf
{
//==============================================================================
class LevelMeter : public juce::Component
{
public:
    void setLevel (float peak);
    void paint (juce::Graphics&) override;
private:
    float shown = 0.0f, hold = 0.0f;
    int holdFrames = 0;
};

//==============================================================================
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
/** Collapsible effect row: chevron + name + summary (+ on/off switch). Click to open. */
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
        explicit Header (FxSection& o);
        void paintButton (juce::Graphics&, bool over, bool down) override;
        FxSection& owner;
    };
    int contentHeight() const;

    juce::String title;
    bool hasSwitch;
    bool expanded = false;
    Header header { *this };
    Smoother anim;
    juce::Array<std::pair<juce::Component*, int>> items;
};

//==============================================================================
/** Right-hand column: mic level, smart match, presets (22 + your own), all effects. */
class FxPanel : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    FxPanel (AudioEngine&, juce::PropertiesFile&);
    ~FxPanel() override;
    void updateLayout();
    void resized() override;
    void paint (juce::Graphics&) override;
    void updateMeters();
    std::function<void (const juce::String&)> toast;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshFromParams();
    void rebuildPresetList();
    void selectPreset (const juce::String& id);
    void saveUserPreset();
    void applySmartMatch();
    void paramsChanged();

    struct PresetItem { juce::String id, name, note, category; juce::String state; bool user = false; };
    juce::Array<PresetItem> allPresets() const;

    AudioEngine& engine;
    FxParams& fx;
    juce::PropertiesFile& props;

    // mic level
    LevelMeter micMeter, outMeter;
    juce::String micStatus;
    juce::Colour micStatusColour = col::okText;
    int silentFrames = 0, loudFrames = 0;

    // smart match
    TFSwitch smartSw;
    juce::String keyText;

    // presets
    TFButton abBtn { juce::String::fromUTF8 ("আগে / পরে"), TFButton::Style::tool },
             saveBtn { juce::String::fromUTF8 ("নিজের সেভ"), TFButton::Style::tool };
    juce::OwnedArray<TFButton> catButtons, presetButtons;
    juce::Viewport presetViewport;
    juce::Component presetHolder;
    juce::String currentCategory { "all" }, selectedPreset;

    // effects
    juce::OwnedArray<FxSection> sections;
    juce::OwnedArray<LabeledSlider> sliders;
    std::vector<std::pair<LabeledSlider*, FxParams::Param>> paramSliders;
    std::vector<std::pair<FxSection*, FxParams::Switch>> switchSections;
    juce::OwnedArray<SwitchRow> switchRows;
    SwitchRow* liveTuneRow = nullptr;
    SwitchRow* noiseLiveRow = nullptr;
    FxSection* noiseSection = nullptr;
    LabeledSlider *noiseAmt = nullptr, *gain = nullptr, *voice = nullptr, *music = nullptr;

    juce::Rectangle<int> meterCard, smartCard, presetCard, fxCard, catWell;
    bool dirty = false;
};
}
