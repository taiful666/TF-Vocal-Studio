#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "StudioView.h"
#include "LibraryView.h"

namespace tf
{
class MainComponent : public juce::Component,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit MainComponent (juce::PropertiesFile& props);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    void openFile (const juce::File&);
    void showToast (const juce::String& text);
    bool canQuit() const { return ! engine.isRecording() && ! engine.isExporting() && ! studio.isBusy(); }
    void stopEverything() { engine.stop(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void showPage (int index);
    void showSettings();
    void saveDeviceState();
    void toggleLive();
    void updateLiveButton();

    juce::PropertiesFile& props;
    AudioEngine engine;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Image logo;
    TFButton studioTab { juce::String::fromUTF8 ("স্টুডিও"), TFButton::Style::segment },
             libraryTab { juce::String::fromUTF8 ("গানের লাইব্রেরি"), TFButton::Style::segment },
             settingsBtn { juce::String::fromUTF8 ("অডিও সেটিংস") },
             liveBtn { juce::String::fromUTF8 ("লাইভ মাইক বন্ধ") };
    juce::Rectangle<int> headerArea, tabWell, devicePill;

    StudioView studio { engine, props };
    LibraryView library { engine };

    juce::String toastText;
    Smoother toastAnim;
    double toastUntil = 0;
    juce::String lastDeviceText;
};
}
