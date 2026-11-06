#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "StudioView.h"
#include "LibraryView.h"
#include "Updater.h"

namespace tf
{
class MainComponent : public juce::Component,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    /** configureDevices: lets the automated UI test add its simulated sound card before start-up. */
    explicit MainComponent (juce::PropertiesFile& props, std::function<void (AudioEngine&)> configureDevices = {});
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    void openFile (const juce::File&);
    void showToast (const juce::String& text);
    bool canQuit() const { return ! engine.isRecording() && ! engine.isExporting() && ! studio.isBusy(); }
    void stopEverything() { studio.stopAndKeep(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void showPage (int index);
    void showSettings();
    void saveDeviceState();
    void toggleLive();
    void updateLiveButton();
    void checkForUpdates (bool userAsked);
    void installUpdate();

    juce::PropertiesFile& props;
    AudioEngine engine;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Image logo;
    TFButton studioTab { juce::String::fromUTF8 ("স্টুডিও"), TFButton::Style::segment },
             libraryTab { juce::String::fromUTF8 ("গানের লাইব্রেরি"), TFButton::Style::segment },
             settingsBtn { juce::String::fromUTF8 ("অডিও সেটিংস") },
             liveBtn { juce::String::fromUTF8 ("লাইভ মাইক বন্ধ") },
             homeBtn { juce::String::fromUTF8 ("হোম") },
             resetBtn { juce::String::fromUTF8 ("নতুন শুরু") },
             updateBtn { juce::String::fromUTF8 ("আপডেট"), TFButton::Style::accent };
    Updater updater;
    Updater::Release available;
    bool updateCheckScheduled = false;
    juce::Rectangle<int> headerArea, tabWell, devicePill;

    StudioView studio { engine, props };
    LibraryView library { engine };

    juce::String toastText;
    Smoother toastAnim;
    double toastUntil = 0;
    juce::String lastDeviceText;
    friend struct UITest;
};
}
