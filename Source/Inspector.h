#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Project.h"
#include "Media.h"

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
/** A row of rotary knobs, evenly spaced. */
class KnobRow : public juce::Component
{
public:
    void add (Knob& k) { knobs.add (&k); addAndMakeVisible (k); }
    void resized() override;
private:
    juce::Array<Knob*> knobs;
};

//==============================================================================
/** Collapsible section: chevron + name + summary (+ on/off switch). Click the name to open. */
class FxSection : public juce::Component
{
public:
    FxSection (const juce::String& title, bool hasSwitch, juce::Path icon = {});
    void addItem (juce::Component& c, int height);
    void setItemHeight (juce::Component& c, int height);
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
    juce::Path icon;
    bool hasSwitch;
    bool expanded = false;
    Header header { *this };
    Smoother anim;
    juce::Array<std::pair<juce::Component*, int>> items;
};

//==============================================================================
/** Right-hand panel: everything about the selected track, organised in nested, collapsible sections.
    Voice track: level, preset browser, effect rack, microphone & AI noise.
    Music track: level, speed, scale, AI vocal remover, change music. */
class Inspector : public juce::Component,
                  private juce::Timer,
                  private juce::ChangeListener
{
public:
    Inspector (Project&, AudioEngine&, juce::PropertiesFile&);
    ~Inspector() override;

    void showTrack (const juce::String& trackId);
    juce::String getTrack() const { return trackId; }
    void updateMeters();
    bool isBusy() const { return busy.load(); }

    std::function<void (const juce::String&)> toast;
    std::function<void()> onChangeMusic;
    std::function<void()> onNoiseChanged;

    /** AI noise removal for the microphone (shared by all voice tracks). */
    void setNoiseOn (bool on);
    bool isNoiseOn() const;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Content;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    const TrackInfo* track() const;
    void refresh();
    void layout();
    void loadFxFromTrack();
    void fxEdited (bool commitNow);
    void commitFx();
    void applyEngineGlobals();

    // presets
    struct PresetItem { juce::String id, name, note, category, state; bool user = false; };
    juce::Array<PresetItem> allPresets() const;
    void rebuildPresetList();
    void selectPreset (const juce::String& id);
    void saveUserPreset();
    void deleteUserPreset();
    void renameUserPreset();
    juce::String presetSummary() const;

public:
    // music jobs
    void applySpeed (double speed);
    void applyKey (int semitones);
    void cancelJob();
    // recording clean-up and smart mix (also used by the right-click menu and the transport button)
    enum class CleanKind { noise, bleed };
    void cleanVoice (CleanKind, const juce::String& trackId = {});
    void autoBalance();
    /** For the right-click menu: all presets (id -> name), and applying one to the shown track. */
    juce::StringPairArray presetNames() const;
    void choosePreset (const juce::String& id) { selectPreset (id); }
private:
    void renderMusic (double speed, int semitones, bool isSpeed);
private:

    Project& project;
    AudioEngine& engine;
    juce::PropertiesFile& props;
    juce::String trackId;
    TrackKind kind = TrackKind::vocal;
    bool hasTrack = false;

    FxParams fx;                   // the selected voice track's effects (mirrors the project)
    juce::String committedFx;
    bool fxPending = false;
    int pendingTicks = 0;

    std::unique_ptr<Content> content;
    juce::Viewport viewport;

    juce::ThreadPool jobs { juce::ThreadPoolOptions().withThreadName ("TF track jobs").withNumberOfThreads (1) };
    std::atomic<bool> busy { false }, cancel { false };

    friend class Content;
    friend struct UITest;
};
}
