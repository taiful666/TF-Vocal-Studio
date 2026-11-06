#include "MainComponent.h"
#include "BinaryData.h"
#include "Settings.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

//==============================================================================
MainComponent::MainComponent (juce::PropertiesFile& p, std::function<void (AudioEngine&)> configureDevices) : props (p)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    logo = juce::ImageCache::getFromMemory (TFData::icon256_png, TFData::icon256_pngSize);

    if (configureDevices) configureDevices (engine);
    auto saved = props.getXmlValue ("audioDevice");
    engine.initialise (saved.get());
    applySavedAudioSettings (engine, props);
    engine.addChangeListener (this);

    for (auto* b : { &studioTab, &libraryTab }) addAndMakeVisible (b);
    studioTab.onClick = [this] { showPage (0); };
    libraryTab.onClick = [this] { showPage (1); };

    liveBtn.setIcon (icons::mic(), false, 18.0f);
    liveBtn.setRadius (10.0f);
    liveBtn.setTooltip (u8 ("চালু করলে মাইকের কণ্ঠ ইফেক্টসহ স্পিকারে শোনা যাবে (L)। হেডফোন ছাড়া জোরে চালালে চিৎকার-আওয়াজ হতে পারে।"));
    liveBtn.onClick = [this] { toggleLive(); };
    addAndMakeVisible (liveBtn);
    updateLiveButton();

    homeBtn.setIcon (icons::home(), false, 17.0f);
    homeBtn.setTooltip (u8 ("হোম — শুরুর পর্দায় যান (অডিও, ভিডিও, লিরিক্স, খালি গলায় গান)"));
    homeBtn.onClick = [this] { showPage (0); studio.showHome(); };
    resetBtn.setIcon (icons::refresh(), false, 16.0f);
    resetBtn.setTooltip (u8 ("নতুন প্রজেক্ট শুরু করুন — এখনকারটা লাইব্রেরিতে সেভ থাকবে"));
    resetBtn.onClick = [this] { showPage (0); studio.resetProject(); };
    for (auto* b : { &homeBtn, &resetBtn }) { b->setRadius (10.0f); b->setFontSize (14.5f); addAndMakeVisible (b); }

    settingsBtn.setIcon (icons::gear(), false, 19.0f);
    settingsBtn.setTooltip (u8 ("মাইক, স্পিকার ও দেরি (latency) ঠিক করুন"));
    settingsBtn.onClick = [this] { showSettings(); };
    addAndMakeVisible (settingsBtn);

    addAndMakeVisible (studio);
    addChildComponent (library);

    studio.toast = [this] (const juce::String& t) { showToast (t); };
    library.toast = studio.toast;
    studio.onOpenLibrary = [this] { showPage (1); };
    library.onSing = [this] (const juce::File& f) { showPage (0); studio.openSong (f); };
    library.onOpenProject = [this] (const juce::File& f) { showPage (0); studio.openProject (f); };
    library.onNewProject = [this] { showPage (0); studio.resetProject(); };
    library.onProjectDeleted = [this] (const juce::File& f) { studio.projectFileDeleted (f); };

    toastAnim.onChange = [this] { repaint (getLocalBounds().withHeight (140)); };

    showPage (0);
    startTimerHz (2);
    setSize (1440, 900);
}

MainComponent::~MainComponent()
{
    saveDeviceState();
    engine.removeChangeListener (this);
}

void MainComponent::saveDeviceState()
{
    if (auto xml = engine.createDeviceState())
        props.setValue ("audioDevice", xml.get());
    props.saveIfNeeded();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    saveDeviceState();
    repaint (devicePill.expanded (4));
}

void MainComponent::timerCallback()
{
    if (toastUntil > 0 && juce::Time::getMillisecondCounterHiRes() > toastUntil)
    {
        toastUntil = 0;
        toastAnim.setTarget (0.0f);
    }
    repaint (devicePill.expanded (4));
}

void MainComponent::toggleLive()
{
    engine.setMonitorEnabled (! engine.isMonitorEnabled());
    updateLiveButton();
    showToast (engine.isMonitorEnabled() ? u8 ("লাইভ মাইক চালু — আপনার কণ্ঠ স্পিকারে যাচ্ছে") : u8 ("লাইভ মাইক বন্ধ"));
}

void MainComponent::updateLiveButton()
{
    const bool on = engine.isMonitorEnabled();
    liveBtn.setStyle (on ? TFButton::Style::record : TFButton::Style::tool);
    liveBtn.setSelected (on);
    liveBtn.setText (on ? u8 ("লাইভ মাইক চালু") : u8 ("লাইভ মাইক বন্ধ"));
}

void MainComponent::showPage (int index)
{
    studioTab.setSelected (index == 0);
    libraryTab.setSelected (index == 1);
    studio.setVisible (index == 0);
    library.setVisible (index == 1);
    grabKeyboardFocus();
}

void MainComponent::showSettings()
{
    showAudioSettings (this, engine, props);
}

void MainComponent::openFile (const juce::File& f)
{
    if (! f.existsAsFile()) return;
    showPage (0);
    if (f.hasFileExtension ("tfvs")) studio.openProject (f);
    else studio.openSong (f);
}

void MainComponent::showToast (const juce::String& text)
{
    toastText = text;
    toastUntil = juce::Time::getMillisecondCounterHiRes() + 2600.0;
    toastAnim.setTarget (1.0f);
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;

    if (k == juce::KeyPress::spaceKey)                                  { studio.togglePlay(); return true; }
    if ((k.getKeyCode() == 'R' || k.getKeyCode() == 'r') && ! k.getModifiers().isCommandDown()) { if (studio.isVisible()) studio.toggleRecord(); return true; }
    if ((k.getKeyCode() == 'L' || k.getKeyCode() == 'l') && ! k.getModifiers().isCommandDown()) { toggleLive(); return true; }
    if (k == juce::KeyPress::homeKey)                                    { studio.goToStart(); return true; }
    if (k == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { showPage (0); studio.chooseFile(); return true; }
    if (k == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) { studio.undo(); return true; }
    if (k == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)
        || k == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { studio.redo(); return true; }
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { if (studio.isVisible()) studio.deleteSelection(); return true; }
    if (k == juce::KeyPress ('l', juce::ModifierKeys::commandModifier, 0)) { studio.toggleLoop(); return true; }
    if (k == juce::KeyPress ('1', juce::ModifierKeys::commandModifier, 0)) { showPage (0); return true; }
    if (k == juce::KeyPress ('2', juce::ModifierKeys::commandModifier, 0)) { showPage (1); return true; }
    return false;
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    g.setColour (col::header);
    g.fillRect (headerArea);
    g.setColour (col::border);
    g.fillRect (headerArea.withTop (headerArea.getBottom() - 1));

    // logo + name
    auto h = headerArea.reduced (28, 0);
    auto logoR = juce::Rectangle<float> ((float) h.getX(), (float) h.getCentreY() - 20.0f, 40.0f, 40.0f);
    g.drawImage (logo, logoR, juce::RectanglePlacement::centred);
    g.setColour (col::ink);
    g.setFont (font (17.0f, Weight::bold));
    g.drawText ("TF Vocal Studio", juce::Rectangle<float> (logoR.getRight() + 12.0f, logoR.getY() - 2.0f, 200.0f, 24.0f), juce::Justification::centredLeft);
    g.setColour (col::muted);
    g.setFont (font (13.0f));
    g.drawText (u8 ("ঘরের স্টুডিও"), juce::Rectangle<float> (logoR.getRight() + 12.0f, logoR.getY() + 20.0f, 200.0f, 20.0f), juce::Justification::centredLeft);

    // tab well
    g.setColour (col::well);
    g.fillRoundedRectangle (tabWell.toFloat(), 12.0f);
    g.setColour (col::border);
    g.drawRoundedRectangle (tabWell.toFloat().reduced (0.5f), 12.0f, 1.0f);

    // device pill
    if (devicePill.isEmpty()) return;
    auto pr = devicePill.toFloat();
    g.setColour (juce::Colour (0xff181B22));
    g.fillRoundedRectangle (pr, 10.0f);
    g.setColour (juce::Colour (0xff262A33));
    g.drawRoundedRectangle (pr.reduced (0.5f), 10.0f, 1.0f);
    const bool open = engine.isDeviceOpen();
    g.setColour (open ? col::ok : col::rec);
    g.fillEllipse (pr.getX() + 12.0f, pr.getCentreY() - 4.0f, 8.0f, 8.0f);
    auto text = open ? engine.getDeviceName() : u8 ("অডিও ডিভাইস বন্ধ");
    auto ms = open ? bn ((int) std::round (engine.getRoundTripLatencyMs())) + " ms" : juce::String();
    g.setFont (font (14.0f, Weight::medium));
    auto msW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), ms);
    g.setColour (col::muted);
    g.drawText (ms, pr.withTrimmedRight (12.0f), juce::Justification::centredRight, false);
    g.setColour (col::ink);
    g.drawText (text, pr.withTrimmedLeft (28.0f).withTrimmedRight (msW + 22.0f), juce::Justification::centredLeft, true);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    auto a = toastAnim.get();
    if (a <= 0.001f || toastText.isEmpty()) return;
    g.setFont (font (15.0f, Weight::medium));
    auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), toastText) + 64.0f;
    auto r = juce::Rectangle<float> (w, 46.0f).withCentre ({ (float) getWidth() * 0.5f, 100.0f - (1.0f - a) * 12.0f });
    g.setOpacity (a);
    g.setColour (juce::Colours::black.withAlpha (0.45f * a));
    g.fillRoundedRectangle (r.translated (0, 3), 14.0f);
    g.setColour (juce::Colour (0xff262B35).withAlpha (a));
    g.fillRoundedRectangle (r, 14.0f);
    g.setColour (col::track.withAlpha (a));
    g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.0f);
    drawIcon (g, icons::check(), juce::Rectangle<float> (18, 18).withCentre ({ r.getX() + 26.0f, r.getCentreY() }), col::okText.withAlpha (a), false, 2.6f);
    g.setColour (col::ink.withAlpha (a));
    g.drawText (toastText, r.withTrimmedLeft (44.0f).withTrimmedRight (16.0f), juce::Justification::centredLeft, false);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (68);

    auto h = headerArea.reduced (28, 0);
    settingsBtn.setBounds (h.removeFromRight (150).withSizeKeepingCentre (150, 44));
    h.removeFromRight (10);
    liveBtn.setBounds (h.removeFromRight (168).withSizeKeepingCentre (168, 44));
    h.removeFromRight (12);

    // home + new start right after the logo
    auto left = headerArea.reduced (28, 0).withTrimmedLeft (230);
    homeBtn.setBounds (left.removeFromLeft (96).withSizeKeepingCentre (96, 44));
    left.removeFromLeft (8);
    resetBtn.setBounds (left.removeFromLeft (128).withSizeKeepingCentre (128, 44));

    // tabs centred when there is room, otherwise right after those
    const int wellW = 4 + 120 + 2 + 170 + 4;
    const int logoEnd = resetBtn.getRight() + 16;
    int tabX = headerArea.getCentreX() - wellW / 2;
    const int pillMin = 170;
    if (tabX < logoEnd || tabX + wellW + 12 + pillMin > h.getRight()) tabX = logoEnd;
    tabWell = juce::Rectangle<int> (tabX, headerArea.getCentreY() - 23, wellW, 46);
    auto tw = tabWell.reduced (4);
    studioTab.setBounds (tw.removeFromLeft (120));
    tw.removeFromLeft (2);
    libraryTab.setBounds (tw);

    const int pillW = juce::jlimit (0, 280, h.getRight() - (tabWell.getRight() + 16));
    devicePill = pillW >= 120 ? juce::Rectangle<int> (h.getRight() - pillW, headerArea.getCentreY() - 20, pillW, 40) : juce::Rectangle<int>();

    studio.setBounds (r);
    library.setBounds (r);
}
}
