#include "MainComponent.h"
#include "BinaryData.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

//==============================================================================
/** Audio settings dialog: device, driver, sample rate, buffer + recording sync offset. */
class SettingsPanel : public juce::Component
{
public:
    SettingsPanel (AudioEngine& e, juce::PropertiesFile& p)
        : engine (e), props (p),
          selector (e.getDeviceManager(), 1, 2, 2, 2, false, false, false, false),
          offset (u8 ("রেকর্ডিং সিঙ্ক ঠিক করা"), -150, 150, e.getUserOffsetMs(),
                  [] (double v) { auto i = (int) std::round (v); return (i > 0 ? "+" : "") + bn (i) + " ms"; })
    {
        addAndMakeVisible (selector);
        addAndMakeVisible (offset);
        offset.onChange = [this] (double v) { engine.setUserOffsetMs ((float) v); props.setValue ("userOffsetMs", v); };
        setSize (660, 640);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        auto r = getLocalBounds().reduced (24).removeFromBottom (120);
        g.setColour (col::muted);
        g.setFont (font (14.0f));
        g.drawFittedText (u8 ("সবচেয়ে কম দেরির জন্য: Audio device type-এ \"Windows Audio (Low Latency Mode)\" বা \"Windows Audio (Exclusive Mode)\" বাছুন, "
                              "Sample rate ৪৮০০০ রাখুন (AI নয়েজ রিমুভ এতে চলে), আর Buffer size যত ছোট রাখা যায় — শব্দ ভাঙলে একটু বাড়ান।\n"
                              "রেকর্ডিংয়ে কণ্ঠ মিউজিকের চেয়ে পিছিয়ে শোনালে সিঙ্ক বাড়ান, এগিয়ে শোনালে কমান।"),
                          r.withTrimmedTop (64), juce::Justification::topLeft, 6, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bottom = r.removeFromBottom (132);
        selector.setBounds (r);
        offset.setBounds (bottom.reduced (12, 0).removeFromTop (52));
    }

private:
    AudioEngine& engine;
    juce::PropertiesFile& props;
    juce::AudioDeviceSelectorComponent selector;
    LabeledSlider offset;
};

//==============================================================================
MainComponent::MainComponent (juce::PropertiesFile& p) : props (p)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    logo = juce::ImageCache::getFromMemory (TFData::icon256_png, TFData::icon256_pngSize);

    auto saved = props.getXmlValue ("audioDevice");
    engine.initialise (saved.get());
    engine.setUserOffsetMs ((float) props.getDoubleValue ("userOffsetMs", 0.0));
    engine.addChangeListener (this);

    for (auto* b : { &studioTab, &libraryTab }) addAndMakeVisible (b);
    studioTab.onClick = [this] { showPage (0); };
    libraryTab.onClick = [this] { showPage (1); };

    settingsBtn.setIcon (icons::gear(), false, 19.0f);
    settingsBtn.setTooltip (u8 ("মাইক, স্পিকার ও দেরি (latency) ঠিক করুন"));
    settingsBtn.onClick = [this] { showSettings(); };
    addAndMakeVisible (settingsBtn);

    addAndMakeVisible (studio);
    addChildComponent (library);

    studio.toast = [this] (const juce::String& t) { showToast (t); };
    library.toast = studio.toast;
    studio.onOpenLibrary = [this] { showPage (1); };
    library.onSing = [this] (const juce::File& f) { studio.loadSong (f); showPage (0); };
    library.onOpenTake = [this] (const Take& t) { showPage (0); studio.loadTake (t); };

    toastAnim.onChange = [this] { repaint (getLocalBounds().removeFromBottom (110)); };

    auto last = juce::File (props.getValue ("lastSong"));
    if (last.existsAsFile()) studio.loadSong (last);

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
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new SettingsPanel (engine, props));
    o.dialogTitle = u8 ("অডিও সেটিংস");
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::openFile (const juce::File& f)
{
    if (! f.existsAsFile()) return;
    studio.loadSong (f);
    showPage (0);
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
    if (k.getKeyCode() == 'R' || k.getKeyCode() == 'r')                 { if (studio.isVisible()) studio.toggleRecord(); return true; }
    if (k == juce::KeyPress::homeKey)                                    { studio.goToStart(); return true; }
    if (k == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { studio.chooseFile(); return true; }
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
    auto r = juce::Rectangle<float> (w, 46.0f).withCentre ({ (float) getWidth() * 0.5f, (float) getHeight() - 60.0f + (1.0f - a) * 16.0f });
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
    devicePill = h.removeFromRight (juce::jmin (300, juce::jmax (180, h.getWidth() / 4))).withSizeKeepingCentre (juce::jmin (300, juce::jmax (180, h.getWidth() / 4)), 40);

    const int wellW = 4 + 120 + 2 + 170 + 4;
    tabWell = juce::Rectangle<int> (wellW, 46).withCentre (headerArea.getCentre());
    auto tw = tabWell.reduced (4);
    studioTab.setBounds (tw.removeFromLeft (120));
    tw.removeFromLeft (2);
    libraryTab.setBounds (tw);

    studio.setBounds (r);
    library.setBounds (r);
}
}
