#include "Settings.h"
#include "Camera.h"
#include "Inspector.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

void applySavedAudioSettings (AudioEngine& e, juce::PropertiesFile& p)
{
    e.setUserOffsetMs ((float) p.getDoubleValue ("userOffsetMs", 0.0));
    e.setPrerollSeconds (p.getDoubleValue ("preroll", 2.0));
}

//==============================================================================
/** Three pages behind tabs: Device · Recording & sync · Check. */
class SettingsPanel : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    SettingsPanel (AudioEngine& e, juce::PropertiesFile& p)
        : engine (e), props (p),
          selector (e.getDeviceManager(), e.isMicSeparate() ? 0 : 1, e.isMicSeparate() ? 0 : 2, 2, 2, false, false, false, false),
          offset (u8 ("রেকর্ডিং সিঙ্ক (কণ্ঠ আগে/পরে)"), -150, 150, e.getUserOffsetMs(),
                  [] (double v) { auto i = (int) std::round (v); return (i > 0 ? "+" : "") + bn (i) + " ms"; }),
          preroll (u8 ("\"এই অংশ আবার গাই\" — আগে কত সেকেন্ড শোনাবে"), 0, 5, p.getDoubleValue ("preroll", 2.0),
                   [] (double v) { return bn (juce::String (v, 1)) + u8 (" সেকেন্ড"); })
    {
        for (auto* b : { &tabDevice, &tabRecord, &tabCheck })
        {
            b->setFontSize (14.0f);
            addAndMakeVisible (b);
        }
        tabDevice.onClick = [this] { show (0); };
        tabRecord.onClick = [this] { show (1); };
        tabCheck.onClick  = [this] { show (2); };

        addChildComponent (selector);
        addChildComponent (offset);
        addChildComponent (preroll);
        offset.slider.setRange (-150.0, 150.0, 1.0);
        offset.onChange = [this] (double v) { engine.setUserOffsetMs ((float) v); props.setValue ("userOffsetMs", v); };
        preroll.slider.setRange (0.0, 5.0, 0.5);
        preroll.onChange = [this] (double v) { engine.setPrerollSeconds (v); props.setValue ("preroll", v); };

        // microphone (its own stream, any sound card) and camera
        micBox.setWantsKeyboardFocus (false);
        refreshMics();
        micBox.onChange = [this]
        {
            const auto name = micBox.getText();
            if (name.isEmpty() || name == engine.getMicrophone()) return;
            engine.setMicrophone (name);
            props.setValue ("micDevice", name);
        };
        addChildComponent (micBox);
        camBox.setWantsKeyboardFocus (false);
        camBox.addItem (u8 ("ক্যামেরা নেই"), 1);
        {
            auto cams = camera::devices();
            for (int i = 0; i < cams.size(); ++i) camBox.addItem (cams[i], 2 + i);
            const auto cur = props.getValue ("camera");
            camBox.setSelectedId (cur.isEmpty() || ! cams.contains (cur) ? 1 : 2 + cams.indexOf (cur), juce::dontSendNotification);
        }
        camBox.onChange = [this] { props.setValue ("camera", camBox.getSelectedId() > 1 ? camBox.getText() : juce::String()); camPicture = {}; repaint(); };
        addChildComponent (camBox);
        camTest.onClick = [this]
        {
            juce::String err;
            camPicture = camBox.getSelectedId() > 1 ? camera::snapshot (camBox.getText(), err) : juce::Image();
            camError = err;
            repaint();
        };
        addChildComponent (camTest);

        // recording aids
        countIn.sw.setToggleState (p.getBoolValue ("countIn", true), juce::dontSendNotification);
        countIn.sw.onClick = [this] { props.setValue ("countIn", countIn.sw.getToggleState()); };
        addChildComponent (countIn);
        measureBtn.setIcon (icons::wand(), false, 16.0f);
        measureBtn.onClick = [this]
        {
            measureText = u8 ("মাপা হচ্ছে… স্পিকার থেকে ৪টি ছোট \"টিক\" শব্দ হবে, চুপ থাকুন।");
            measureBtn.setEnabled (false);
            repaint();
            juce::Component::SafePointer<SettingsPanel> safe (this);
            engine.measureLatency ([safe] (bool ok, double ms, juce::String msg)
            {
                if (safe == nullptr) return;
                safe->measureBtn.setEnabled (true);
                if (ok)
                {
                    safe->offset.slider.setValue (safe->engine.getUserOffsetMs(), juce::sendNotificationSync);
                    safe->measureText = u8 ("মাপা হয়েছে: মোট দেরি ") + bn ((int) std::round (ms)) + u8 (" ms — রেকর্ডিং এখন নিজে থেকে মিলবে।");
                }
                else safe->measureText = msg;
                safe->repaint();
            });
        };
        addChildComponent (measureBtn);
        camOffset.slider.setRange (-300.0, 300.0, 10.0);
        camOffset.onChange = [this] (double v) { props.setValue ("camOffsetMs", v); };
        addChildComponent (camOffset);

        toneBtn.setIcon (icons::speaker(), false, 17.0f);
        toneBtn.onClick = [this] { engine.playTestTone(); };
        addChildComponent (toneBtn);
        resetBtn.onClick = [this] { offset.slider.setValue (0.0); };
        addChildComponent (resetBtn);

        engine.addChangeListener (this);
        show (0);
        startTimerHz (4);
        camOffset.slider.setValue (p.getDoubleValue ("camOffsetMs", 0.0), juce::dontSendNotification);
        setSize (720, 720);
    }

    ~SettingsPanel() override { engine.removeChangeListener (this); }

    void show (int p)
    {
        page = p;
        tabDevice.setSelected (p == 0);
        tabRecord.setSelected (p == 1);
        tabCheck.setSelected (p == 2);
        selector.setVisible (p == 0);
        micBox.setVisible (p == 0 && engine.isMicSeparate());
        camBox.setVisible (p == 0);
        camTest.setVisible (p == 0);
        offset.setVisible (p == 1);
        preroll.setVisible (p == 1);
        resetBtn.setVisible (p == 1);
        countIn.setVisible (p == 1);
        measureBtn.setVisible (p == 1);
        camOffset.setVisible (p == 1);
        toneBtn.setVisible (p == 2);
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::well);
        g.fillRoundedRectangle (tabWell.toFloat(), 12.0f);
        g.setColour (col::border);
        g.drawRoundedRectangle (tabWell.toFloat().reduced (0.5f), 12.0f, 1.0f);

        auto card = body.toFloat();
        g.setColour (col::panel);  g.fillRoundedRectangle (card, 16.0f);
        g.setColour (col::border); g.drawRoundedRectangle (card.reduced (0.5f), 16.0f, 1.0f);

        auto txt = body.reduced (24, 20);
        auto heading = [&] (const juce::String& h, const juce::String& sub)
        {
            g.setColour (col::ink); g.setFont (font (17.0f, Weight::semibold));
            g.drawText (h, txt.removeFromTop (28), juce::Justification::centredLeft);
            g.setColour (col::muted); g.setFont (font (13.5f));
            g.drawFittedText (sub, txt.removeFromTop (40), juce::Justification::topLeft, 2, 1.0f);
        };

        if (page == 0)
        {
            heading (u8 ("মাইক, স্পিকার ও ক্যামেরা"), u8 ("মাইক আলাদা সাউন্ড কার্ডে (যেমন FIFINE USB মাইক) থাকলেও কোনো সমস্যা নেই — আওয়াজ ভাঙবে না। সবচেয়ে কম দেরির জন্য \"Windows Audio (Low Latency Mode)\" রাখুন।"));
            auto label = [&g] (const juce::String& t, juce::Rectangle<int> r)
            {
                g.setColour (col::inkSoft); g.setFont (font (14.5f));
                g.drawText (t, r.withWidth (150), juce::Justification::centredLeft);
            };
            if (micBox.isVisible()) label (u8 ("মাইক"), micBox.getBounds().withX (body.getX() + 24));
            label (u8 ("ক্যামেরা"), camBox.getBounds().withX (body.getX() + 24));
            if (camPicture.isValid())
                g.drawImage (camPicture, camPreview.toFloat(), juce::RectanglePlacement::centred);
            else if (camError.isNotEmpty())
            {
                g.setColour (col::warn); g.setFont (font (13.0f));
                g.drawFittedText (camError, camPreview, juce::Justification::centred, 3);
            }
        }
        else if (page == 1)
        {
            heading (u8 ("রেকর্ডিং ও সিঙ্ক"), u8 ("রেকর্ড করা কণ্ঠ মিউজিকের চেয়ে পিছিয়ে শোনালে সিঙ্ক বাড়ান (+), এগিয়ে শোনালে কমান (−)। সাধারণত ০ রাখলেই ঠিক থাকে।"));
            g.setColour (measureText.isNotEmpty() ? col::okText : col::dim); g.setFont (font (13.0f));
            g.drawFittedText (measureText.isNotEmpty() ? measureText
                                                       : u8 ("\"সিঙ্ক মাপুন\" চাপলে সফটওয়্যার নিজেই স্পিকার আর মাইকের দেরি মেপে রেকর্ডিং মিলিয়ে নেবে (মাইক স্পিকারের কাছে রাখুন)।"),
                              measureNote, juce::Justification::topLeft, 3, 1.0f);
        }
        else
        {
            heading (u8 ("সব ঠিক আছে কিনা দেখুন"), u8 ("নিচের প্রতিটি লাইনে সবুজ থাকলে সবচেয়ে ভালো মান পাবেন।"));
            txt.removeFromTop (6);
            auto row = [&] (bool ok, const juce::String& label, const juce::String& value)
            {
                auto r = txt.removeFromTop (40);
                g.setColour (ok ? col::ok : col::warn);
                g.fillEllipse (r.removeFromLeft (16).toFloat().withSizeKeepingCentre (9.0f, 9.0f));
                r.removeFromLeft (8);
                g.setColour (col::inkSoft); g.setFont (font (14.5f));
                g.drawText (label, r, juce::Justification::centredLeft, true);
                g.setColour (ok ? col::okText : col::warn); g.setFont (font (14.5f, Weight::medium));
                g.drawText (value, r, juce::Justification::centredRight, true);
                g.setColour (col::border);
                g.fillRect (r.getX(), r.getBottom() - 1, txt.getRight() - r.getX(), 1);
            };
            const bool open = engine.isDeviceOpen();
            const double sr = engine.getSampleRate();
            const int bs = engine.getBufferSize();
            const double ms = engine.getRoundTripLatencyMs();
            auto* dev = engine.getDeviceManager().getCurrentAudioDevice();
            const auto type = dev != nullptr ? dev->getTypeName() : juce::String();
            row (open, u8 ("অডিও ডিভাইস"), open ? engine.getDeviceName() : u8 ("বন্ধ — ডিভাইস পাতায় বাছুন"));
            row (type.containsIgnoreCase ("Low Latency") || type.containsIgnoreCase ("Exclusive") || type.containsIgnoreCase ("ASIO"),
                 u8 ("ড্রাইভার"), type.isNotEmpty() ? type : u8 ("—"));
            row (std::abs (sr - 48000.0) < 1.0, u8 ("স্যাম্পল রেট (AI নয়েজ রিমুভের জন্য ৪৮০০০)"), bn ((int) sr) + " Hz");
            row (bs > 0 && bs <= 512, u8 ("বাফার সাইজ"), bn (bs) + u8 (" স্যাম্পল"));
            row (ms > 0 && ms < 40.0, u8 ("মোট দেরি (মাইক → স্পিকার)"), bn ((int) std::round (ms)) + " ms");
            row (engine.getXruns() == 0, u8 ("আওয়াজ ভাঙা (dropout)"), engine.getXruns() <= 0 ? u8 ("নেই") : bn (engine.getXruns()) + u8 (" বার — বাফার বাড়ান"));
            txt.removeFromTop (16);
            g.setColour (col::dim); g.setFont (font (13.0f));
            g.drawFittedText (u8 ("স্পিকার পরীক্ষা চাপলে ছোট একটা \"টুং\" আওয়াজ হবে। না শুনলে ডিভাইস পাতায় Output বদলে দেখুন।"),
                              txt.removeFromTop (40), juce::Justification::topLeft, 2, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18);
        auto top = r.removeFromTop (48);
        tabWell = top.withSizeKeepingCentre (4 + 150 * 3 + 4, 46);
        auto tw = tabWell.reduced (4);
        tabDevice.setBounds (tw.removeFromLeft (150));
        tabRecord.setBounds (tw.removeFromLeft (150));
        tabCheck.setBounds (tw);
        r.removeFromTop (14);
        body = r;

        auto in = body.reduced (20, 20).withTrimmedTop (78);
        {
            auto d = in;
            if (engine.isMicSeparate()) { micBox.setBounds (d.removeFromTop (34).withTrimmedLeft (150)); d.removeFromTop (8); }
            auto camRow = d.removeFromBottom (34);
            camTest.setBounds (camRow.removeFromRight (150));
            camRow.removeFromRight (8);
            camBox.setBounds (camRow.withTrimmedLeft (150));
            d.removeFromBottom (8);
            camPreview = d.removeFromBottom (camPicture.isValid() || camError.isNotEmpty() ? 110 : 0).withTrimmedLeft (150);
            selector.setBounds (d.withTrimmedLeft (-8));
        }
        auto rr = in.withTrimmedLeft (4).withTrimmedRight (4);
        auto mrow = rr.removeFromTop (40);
        measureBtn.setBounds (mrow.removeFromLeft (220));
        measureNote = rr.removeFromTop (44).withTrimmedTop (4);
        offset.setBounds (rr.removeFromTop (52));
        resetBtn.setBounds (rr.removeFromTop (44).removeFromRight (150).withSizeKeepingCentre (150, 36));
        preroll.setBounds (rr.removeFromTop (52));
        rr.removeFromTop (6);
        countIn.setBounds (rr.removeFromTop (44));
        rr.removeFromTop (6);
        camOffset.setBounds (rr.removeFromTop (52));
        toneBtn.setBounds (juce::Rectangle<int> (200, 44).withPosition (body.getX() + 24, body.getBottom() - 70));
    }

private:
    void timerCallback() override { if (page == 2) repaint(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    AudioEngine& engine;
    juce::PropertiesFile& props;
    int page = 0;
    TFButton tabDevice { u8 ("ডিভাইস"), TFButton::Style::segment },
             tabRecord { u8 ("রেকর্ডিং ও সিঙ্ক"), TFButton::Style::segment },
             tabCheck  { u8 ("পরীক্ষা"), TFButton::Style::segment },
             toneBtn   { u8 ("স্পিকার পরীক্ষা") },
             resetBtn  { u8 ("০ তে ফেরান") };
    juce::Rectangle<int> tabWell, body, camPreview, measureNote;
    juce::AudioDeviceSelectorComponent selector;
    LabeledSlider offset, preroll;
    juce::ComboBox micBox, camBox;
    TFButton camTest { u8 ("ক্যামেরা দেখুন") }, measureBtn { u8 ("সিঙ্ক নিজে থেকে মাপুন"), TFButton::Style::accent };
    juce::Image camPicture;
    juce::String camError, measureText;
    SwitchRow countIn { u8 ("রেকর্ডের আগে ৩-২-১ গণনা"), u8 ("রেকর্ড চাপলে ৩ সেকেন্ড গুনে তারপর শুরু হয়") };
    LabeledSlider camOffset { u8 ("ক্যামেরার ভিডিও আগে/পরে"), -300, 300, 0, [] (double v) { auto i = (int) std::round (v); return (i > 0 ? "+" : "") + bn (i) + " ms"; } };

    void refreshMics()
    {
        micBox.clear (juce::dontSendNotification);
        auto names = engine.getMicrophoneNames();
        for (int i = 0; i < names.size(); ++i) micBox.addItem (names[i], i + 1);
        const int idx = names.indexOf (engine.getMicrophone());
        if (idx >= 0) micBox.setSelectedId (idx + 1, juce::dontSendNotification);
    }
};

void showAudioSettings (juce::Component* centreAround, AudioEngine& e, juce::PropertiesFile& p)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new SettingsPanel (e, p));
    o.dialogTitle = u8 ("অডিও সেটিংস");
    o.dialogBackgroundColour = col::bg;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    // centre on the main window when the system knows its screen (it always does on Windows)
    if (centreAround != nullptr && centreAround->isShowing()
        && juce::Desktop::getInstance().getDisplays().getDisplayForRect (centreAround->getScreenBounds()) != nullptr)
        o.componentToCentreAround = centreAround;
    o.launchAsync();
}
}
