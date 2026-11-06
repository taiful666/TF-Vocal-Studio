// The inspector's inner parts (shared with the automated UI test). Include only from Inspector.cpp and tests.
#pragma once
#include "Inspector.h"
#include "VocalFx.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
static juce::String pct (double v) { return bn ((int) std::round (v)) + "%"; }
static juce::String panText (double v)
{
    if (std::abs (v) < 1.0) return u8 ("মাঝে");
    return (v < 0 ? u8 ("বামে ") : u8 ("ডানে ")) + bn ((int) std::round (std::abs (v)));
}

//==============================================================================
/** Small text block whose content is computed when painted. */
struct InfoText : public juce::Component
{
    std::function<juce::String()> text;
    juce::Colour colour = col::muted;
    float size = 13.0f;
    InfoText() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        g.setColour (colour);
        g.setFont (font (size));
        g.drawFittedText (text ? text() : juce::String(), getLocalBounds(), juce::Justification::centredLeft, 3, 1.0f);
    }
};

/** A progress bar with a status line. */
struct JobBar : public juce::Component
{
    float progress = 0.0f;
    juce::String status;
    JobBar() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::inkSoft);
        g.setFont (font (12.5f));
        g.drawText (status, r.removeFromTop (18.0f), juce::Justification::centredLeft, true);
        auto bar = r.removeFromTop (8.0f).withTrimmedTop (2.0f);
        g.setColour (col::border);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (col::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress)), 3.0f);
    }
};

/** Two buttons side by side. */
struct ButtonPair : public juce::Component
{
    juce::Array<juce::Component*> items;
    void add (juce::Component& c) { items.add (&c); addAndMakeVisible (c); }
    void resized() override
    {
        if (items.isEmpty()) return;
        auto r = getLocalBounds();
        const int gap = 8, w = (r.getWidth() - gap * (items.size() - 1)) / items.size();
        for (auto* c : items) { c->setBounds (r.removeFromLeft (w)); r.removeFromLeft (gap); }
    }
};

//==============================================================================
/** The scrollable content of the inspector. */
class Inspector::Content : public juce::Component
{
public:
    explicit Content (Inspector& o) : owner (o)
    {
        // ---------------- track card (both kinds)
        addAndMakeVisible (meter);
        volKnob.slider.onDragStart = [this] { owner.project.beginGesture(); };
        volKnob.slider.onDragEnd   = [this] { owner.project.endGesture(); };
        panKnob.slider.onDragStart = volKnob.slider.onDragStart;
        panKnob.slider.onDragEnd   = volKnob.slider.onDragEnd;
        volKnob.onChange = [this] (double v) { setTrackValue ([v] (TrackInfo& t) { t.gain = (float) v / 100.0f; }); };
        panKnob.onChange = [this] (double v) { setTrackValue ([v] (TrackInfo& t) { t.pan = (float) v / 100.0f; }); };
        trackKnobs.add (volKnob);
        trackKnobs.add (panKnob);
        addAndMakeVisible (trackKnobs);
        for (auto* b : { &muteBtn, &soloBtn }) { b->setRadius (9.0f); b->setFontSize (13.5f); addAndMakeVisible (b); }
        muteBtn.setIcon (icons::speaker(), false, 16.0f);
        muteBtn.getProperties().set ("iconOnly", true);
        muteBtn.onClick = [this] { setTrackValue ([] (TrackInfo& t) { t.mute = ! t.mute; }, true); };
        soloBtn.onClick = [this] { setTrackValue ([] (TrackInfo& t) { t.solo = ! t.solo; }, true); };
        muteBtn.setTooltip (u8 ("এই ট্র্যাক চুপ করুন"));
        soloBtn.setTooltip (u8 ("শুধু এই ট্র্যাক শুনুন"));

        buildVocal();
        buildMusic();
    }

    // ======================================================================
    void buildVocal()
    {
        auto& fx = owner.fx;
        // ---- presets
        presetSec = addSection (vocalPresets, u8 ("কণ্ঠের প্রিসেট"), false, icons::sparkle());
        presetSec->summary = [this] { return owner.presetSummary(); };
        for (auto* b : { &abBtn, &saveBtn, &renameBtn, &delBtn }) { b->setRadius (9.0f); b->setFontSize (12.5f); }
        renameBtn.onClick = [this] { owner.renameUserPreset(); };
        renameBtn.setTooltip (u8 ("আমার প্রিসেটের নাম বদলান"));
        delBtn.setTooltip (u8 ("আমার প্রিসেট মুছে ফেলুন"));
        abBtn.setTooltip (u8 ("একবার চাপলে ইফেক্ট ছাড়া শুনবেন, আবার চাপলে ইফেক্টসহ — তুলনা করার জন্য"));
        abBtn.onClick = [this]
        {
            owner.fx.bypass = ! owner.fx.bypass.load();
            if (auto* ef = owner.engine.getTrackFx (owner.trackId)) ef->bypass = owner.fx.bypass.load();
            syncAB();
        };
        saveBtn.onClick = [this] { owner.saveUserPreset(); };
        delBtn.onClick = [this] { owner.deleteUserPreset(); };
        presetButtonsRow.add (abBtn); presetButtonsRow.add (saveBtn); presetButtonsRow.add (renameBtn); presetButtonsRow.add (delBtn);
        presetViewport.setViewedComponent (&presetHolder, false);
        presetViewport.setScrollBarsShown (true, false);
        presetViewport.setScrollBarThickness (6);
        presetSec->addItem (catHolder, 38);
        presetSec->addItem (presetViewport, 300);
        presetSec->addItem (presetButtonsRow, 34);

        // ---- effects
        auto param = [this, &fx] (const char* name, FxParams::Param p, KnobRow& row) -> Knob*
        {
            auto* k = knobs.add (new Knob (u8 (name), 0, 100, fx.get (p), [] (double v) { return pct (v); }));
            k->slider.onDragStart = [this] { dragging = true; };
            k->slider.onDragEnd = [this] { dragging = false; owner.fxEdited (true); };
            k->onChange = [this, p] (double v) { owner.fx.set (p, (float) v); owner.fxEdited (! dragging); refreshHeaders(); };
            paramKnobs.push_back ({ k, p });
            row.add (*k);
            return k;
        };
        auto sec = [this] (const char* title, FxParams::Switch s, juce::Path icon) -> FxSection*
        {
            auto* x = addSection (vocalFx, u8 (title), true, std::move (icon));
            switchSecs.push_back ({ x, s });
            x->sw.onClick = [this, x, s] { owner.fx.setOn (s, x->sw.getToggleState()); owner.fxEdited (true); x->refreshHeader(); };
            return x;
        };
        auto addRow = [this] (FxSection* s) -> KnobRow& { auto* r = knobRows.add (new KnobRow()); s->addItem (*r, 96); return *r; };

        auto* tune = sec ("অটো-টিউন (সুর ঠিক করা)", FxParams::tuneOn, icons::wand());
        { auto& r = addRow (tune); param ("জোর", FxParams::tuneAmt, r); param ("গতি", FxParams::tuneSpd, r); }
        scaleRow.label = u8 ("স্কেল");
        tune->addItem (scaleRow, 34);
        tune->addItem (scaleInfo, 20);
        liveTuneRow.sw.onClick = [this] { owner.engine.setLiveTune (liveTuneRow.sw.getToggleState()); owner.props.setValue ("liveTune", liveTuneRow.sw.getToggleState()); };
        tune->addItem (liveTuneRow, 42);
        tune->summary = [this] { return pct (owner.fx.get (FxParams::tuneAmt)); };

        auto* tone = sec ("কণ্ঠের রং (EQ)", FxParams::toneOn, icons::music());
        { auto& r = addRow (tone); param ("মোটা ভাব", FxParams::bass, r); param ("মাঝের সুর", FxParams::mid, r); param ("উজ্জ্বলতা", FxParams::bright, r); }
        tone->summary = [this] { return u8 ("মোটা ") + pct (owner.fx.get (FxParams::bass)); };

        auto* body = sec ("ভরাট গলা (কম্প্রেসর)", FxParams::bodyOn, icons::mic());
        { auto& r = addRow (body); param ("ভরাট ভাব", FxParams::body, r); param ("উষ্ণতা", FxParams::warm, r); }
        body->summary = [this] { return pct (owner.fx.get (FxParams::body)); };

        auto* rev = sec ("রিভার্ব (হলঘরের আওয়াজ)", FxParams::revOn, icons::sparkle());
        { auto& r = addRow (rev); param ("পরিমাণ", FxParams::rev, r); param ("ঘরের আকার", FxParams::room, r); }
        rev->summary = [this] { return pct (owner.fx.get (FxParams::rev)); };

        auto* echo = sec ("ইকো (প্রতিধ্বনি)", FxParams::echoOn, icons::loop());
        { auto& r = addRow (echo); param ("পরিমাণ", FxParams::echo, r); param ("দূরত্ব", FxParams::etime, r); }
        echo->summary = [this] { return pct (owner.fx.get (FxParams::echo)); };

        auto* dbl = sec ("ডাবল ভয়েস (কোরাস)", FxParams::dblOn, icons::headphones());
        { auto& r = addRow (dbl); param ("পরিমাণ", FxParams::dbl, r); }
        dbl->summary = [this] { return pct (owner.fx.get (FxParams::dbl)); };

        auto* deess = sec ("ডি-এসার (স-শ এর হিস)", FxParams::deessOn, icons::wand());
        { auto& r = addRow (deess); param ("পরিমাণ", FxParams::deess, r); }
        deess->summary = [this] { return pct (owner.fx.get (FxParams::deess)); };

        // ---- microphone (shared by every voice track)
        auto& props = owner.props;
        micSec = addSection (vocalMic, u8 ("মাইক ইনপুট"), false, icons::mic());
        micMeterRow.addAndMakeVisible (micMeter);
        micSec->addItem (micMeterRow, 28);
        gainKnob = knobs.add (new Knob (u8 ("মাইকের জোর"), -12, 24, props.getDoubleValue ("gainDb", 0.0),
                                       [] (double v) { return (v > 0.05 ? "+" : "") + bn (juce::String (v, 1)) + " dB"; }));
        monKnob = knobs.add (new Knob (u8 ("লাইভ মাইকের ভলিউম"), 0, 100, props.getDoubleValue ("monitorVol", 80.0), [] (double v) { return pct (v); }));
        micKnobs.add (*gainKnob);
        micKnobs.add (*monKnob);
        micSec->addItem (micKnobs, 96);
        micSec->summary = [this] { auto v = gainKnob->slider.getValue(); return (v > 0.05 ? "+" : "") + bn (juce::String (v, 1)) + " dB"; };

        noiseSec = addSection (vocalMic, u8 ("AI নয়েজ রিমুভ"), true, icons::sparkle());
        noiseSec->sw.setToggleState (props.getBoolValue ("noiseOn", true), juce::dontSendNotification);
        noiseKnob = knobs.add (new Knob (u8 ("পরিমাণ"), 0, 100, props.getDoubleValue ("noiseAmt", 80.0), [] (double v) { return pct (v); }));
        noiseKnobs.add (*noiseKnob);
        noiseSec->addItem (noiseKnobs, 96);
        noiseLiveRow.sw.setToggleState (props.getBoolValue ("noiseMon", false), juce::dontSendNotification);
        noiseSec->addItem (noiseLiveRow, 42);
        noiseInfo.text = [this] { return owner.engine.noiseAvailable() ? u8 ("রেকর্ডে ফ্যান, এসি, রাস্তার শব্দ সরিয়ে দেয়।")
                                                                      : u8 ("এটা চালাতে সাউন্ড সেটিংসে ৪৮০০০ Hz বাছুন।"); };
        noiseSec->addItem (noiseInfo, 34);
        noiseSec->summary = [this] { return owner.engine.noiseAvailable() ? pct (noiseKnob->slider.getValue()) : u8 ("৪৮০০০ Hz দরকার"); };

        for (auto* k : { gainKnob, monKnob, noiseKnob }) k->onChange = [this] (double) { owner.applyEngineGlobals(); refreshHeaders(); };
        noiseSec->sw.onStateChange = [this] { owner.applyEngineGlobals(); };
        noiseLiveRow.sw.onClick = [this] { owner.applyEngineGlobals(); };

        liveTuneRow.sw.setToggleState (props.getBoolValue ("liveTune", false), juce::dontSendNotification);
        scaleInfo.text = [this]
        {
            const int m = owner.engine.getScaleMode();
            if (m != -2) return m == -1 ? u8 ("সব নোটে মেলাবে") : u8 ("নিজে বাছাই করা স্কেল");
            if (owner.project.state().musicTrack() == nullptr) return u8 ("মিউজিক দিলে তার স্কেল নিজে থেকে ধরবে");
            if (owner.engine.isDetectingKey()) return u8 ("গানের স্কেল খোঁজা হচ্ছে…");
            const int k = owner.engine.getSongKey();
            return k >= 0 ? u8 ("গানের স্কেল পাওয়া গেছে: ") + keyName (k) : u8 ("স্কেল বোঝা যায়নি — সব নোটে মেলাবে");
        };

        presetSec->setExpanded (true, false);
    }

    void buildMusic()
    {
        // ---- speed
        speedSec = addSection (musicMain, u8 ("গতি (স্পিড)"), false, icons::play());
        speedSec->summary = [this] { auto* t = owner.track(); return t != nullptr ? pct (t->speed * 100.0) : juce::String(); };
        const int speeds[] = { 80, 90, 100, 110, 120 };
        for (auto s : speeds)
        {
            auto* b = speedButtons.add (new TFButton (bn (s) + "%", TFButton::Style::segment));
            b->setFontSize (12.5f);
            b->onClick = [this, s] { speedSlider.slider.setValue (s); };
            speedPresetRow.add (*b);
        }
        speedSec->addItem (speedPresetRow, 32);
        speedSlider.slider.setRange (50.0, 150.0, 1.0);
        speedSlider.onChange = [this] (double) { updateSpeedButtons(); };
        speedSec->addItem (speedSlider, 50);
        speedApply.setRadius (9.0f);
        speedApply.onClick = [this] { owner.applySpeed (speedSlider.slider.getValue() / 100.0); };
        speedCancel.setRadius (9.0f);
        speedCancel.onClick = [this] { owner.cancelJob(); };
        speedActions.add (speedApply);
        speedActions.add (speedCancel);
        speedSec->addItem (speedActions, 36);
        speedSec->addItem (speedBar, 0);
        speedInfo.text = [] { return u8 ("সুর একই থাকে, শুধু গতি বদলায়। গতি ঠিক করে তারপর রেকর্ড করুন।"); };
        speedSec->addItem (speedInfo, 36);

        // ---- scale
        scaleSec = addSection (musicMain, u8 ("গানের স্কেল (স্মার্ট ম্যাচ)"), false, icons::music());
        scaleSec->summary = [this] { const int k = owner.engine.getEffectiveKey(); return k >= 0 ? keyName (k) : u8 ("সব নোট"); };
        scaleRow2.label = u8 ("স্কেল");
        scaleSec->addItem (scaleRow2, 34);
        scaleInfo2.text = scaleInfo.text;
        scaleSec->addItem (scaleInfo2, 34);

        // ---- AI vocal remover
        sepSec = addSection (musicMain, u8 ("AI দিয়ে আসল কণ্ঠ সরান"), false, icons::wand());
        sepSec->summary = [] { return u8 ("কারাওকে বানান"); };
        sepInfo.text = [] { return u8 ("গানে শিল্পীর কণ্ঠ থাকলে সরিয়ে শুধু মিউজিক রাখে।"); };
        sepSec->addItem (sepInfo, 36);
        sepBtn.setRadius (9.0f);
        sepBtn.onClick = [this] { owner.runSeparation(); };
        sepCancel.setRadius (9.0f);
        sepCancel.onClick = [this] { owner.cancelJob(); };
        sepActions.add (sepBtn);
        sepActions.add (sepCancel);
        sepSec->addItem (sepActions, 36);
        sepSec->addItem (sepBar, 0);

        // ---- manage
        manageSec = addSection (musicMain, u8 ("মিউজিক বদলান / সরান"), false, icons::folder());
        manageSec->summary = [this] { auto* t = owner.track(); return t != nullptr ? t->source.getFileExtension().trimCharactersAtStart (".").toUpperCase() : juce::String(); };
        fileInfo.text = [this] { auto* t = owner.track(); return t != nullptr ? u8 ("ফাইল: ") + t->source.getFileName() : juce::String(); };
        manageSec->addItem (fileInfo, 34);
        changeBtn.setRadius (9.0f);
        removeBtn.setRadius (9.0f);
        changeBtn.onClick = [this] { if (owner.onChangeMusic) owner.onChangeMusic(); };
        removeBtn.onClick = [this]
        {
            auto id = owner.trackId;
            auto& pr = owner.project;
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle (u8 ("মিউজিক সরাবেন?"))
                                              .withMessage (u8 ("মিউজিক ট্র্যাক সরিয়ে দেওয়া হবে, আপনার কণ্ঠ থাকবে। ভুল হলে \"আগের অবস্থা\" চাপুন।"))
                                              .withButton (u8 ("সরিয়ে দিন"), 1).withButton (u8 ("বাতিল"), 0)
                                              .withAssociatedComponent (this),
                                          [&pr, id] (int r) { if (r == 1) pr.removeTrack (id); });
        };
        manageActions.add (changeBtn);
        manageActions.add (removeBtn);
        manageSec->addItem (manageActions, 36);

        speedSec->setExpanded (true, false);

        // scale boxes (shared setting)
        for (auto* box : { &scaleRow.box, &scaleRow2.box })
        {
            box->setWantsKeyboardFocus (false);
            box->addItem (u8 ("গান অনুযায়ী (স্মার্ট)"), 1);
            box->addItem (u8 ("সব নোট (ক্রোম্যাটিক)"), 2);
            box->addSeparator();
            for (int k = 0; k < 24; ++k) box->addItem (keyName (k), 3 + k);
            box->onChange = [this, box]
            {
                const int id = box->getSelectedId();
                const int mode = id == 1 ? -2 : id == 2 ? -1 : id - 3;
                owner.engine.setScaleMode (mode);
                owner.props.setValue ("scaleMode", mode);
                syncScale();
                refreshHeaders();
            };
        }
        syncScale();
    }

    // ======================================================================
    struct ScaleRow : public juce::Component
    {
        juce::String label;
        juce::ComboBox box;
        ScaleRow() { addAndMakeVisible (box); }
        void resized() override { box.setBounds (getLocalBounds().withTrimmedLeft (64)); }
        void paint (juce::Graphics& g) override
        {
            g.setColour (col::inkSoft); g.setFont (font (14.0f));
            g.drawText (label, getLocalBounds().withWidth (60), juce::Justification::centredLeft);
        }
    };

    struct MeterRow : public juce::Component
    {
        void resized() override { for (auto* c : getChildren()) c->setBounds (getLocalBounds().withTrimmedLeft (64).withSizeKeepingCentre (getWidth() - 64, 8)); }
        void paint (juce::Graphics& g) override
        {
            g.setColour (col::inkSoft); g.setFont (font (14.0f));
            g.drawText (juce::String::fromUTF8 ("লেভেল"), getLocalBounds().withWidth (60), juce::Justification::centredLeft);
        }
    };

    struct Group
    {
        Group (const juce::String& t, const juce::String& s) : title (t), subtitle (s) {}
        juce::String title, subtitle;
        juce::Array<FxSection*> sections;
        juce::Rectangle<int> bounds;
    };

    FxSection* addSection (Group& g, const juce::String& title, bool sw, juce::Path icon)
    {
        auto* s = sections.add (new FxSection (title, sw, std::move (icon)));
        g.sections.add (s);
        addChildComponent (s);
        s->onLayoutChange = [this] { owner.layout(); };
        s->onExpand = [&g] (FxSection* opened) { for (auto* o : g.sections) if (o != opened && o->isExpanded()) o->setExpanded (false); };
        return s;
    }

    void setTrackValue (std::function<void (TrackInfo&)> f, bool undoableNow = false)
    {
        auto* t = owner.track();
        if (t == nullptr || updating) return;
        auto c = *t;
        f (c);
        owner.project.setTrack (c, true);
        juce::ignoreUnused (undoableNow);
    }

    void syncAB()
    {
        const bool b = owner.fx.bypass.load();
        abBtn.setSelected (b);
        abBtn.setText (b ? u8 ("ইফেক্ট ছাড়া") : u8 ("আগে/পরে"));
    }

    void syncScale()
    {
        const int m = owner.engine.getScaleMode();
        const int id = m == -2 ? 1 : m == -1 ? 2 : m + 3;
        for (auto* box : { &scaleRow.box, &scaleRow2.box }) box->setSelectedId (id, juce::dontSendNotification);
        scaleInfo.repaint();
        scaleInfo2.repaint();
    }

    void updateSpeedButtons()
    {
        const int v = (int) std::round (speedSlider.slider.getValue());
        const int speeds[] = { 80, 90, 100, 110, 120 };
        for (int i = 0; i < speedButtons.size(); ++i) speedButtons[i]->setSelected (speeds[i] == v);
        auto* t = owner.track();
        const bool same = t != nullptr && (int) std::round (t->speed * 100.0) == v;
        speedApply.setEnabled (! same && ! owner.busy.load());
        speedApply.setText (same ? u8 ("এই গতিই চলছে") : u8 ("এই গতি প্রয়োগ করুন"));
    }

    void refreshHeaders() { for (auto* s : sections) s->refreshHeader(); }

    /** Reads the selected track into the widgets (no notifications). */
    void refresh()
    {
        updating = true;
        auto* t = owner.track();
        const bool vocal = t != nullptr && t->kind == TrackKind::vocal;
        const bool music = t != nullptr && t->kind == TrackKind::music;
        for (auto* s : vocalPresets.sections) s->setVisible (vocal);
        for (auto* s : vocalFx.sections) s->setVisible (vocal);
        for (auto* s : vocalMic.sections) s->setVisible (vocal);
        for (auto* s : musicMain.sections) s->setVisible (music);
        for (auto* c : std::initializer_list<juce::Component*> { &meter, &trackKnobs, &muteBtn, &soloBtn }) c->setVisible (t != nullptr);
        if (t != nullptr)
        {
            volKnob.setValue (t->gain * 100.0);
            panKnob.setValue (t->pan * 100.0);
            muteBtn.setSelected (t->mute);
            muteBtn.setIcon (t->mute ? icons::speakerOff() : icons::speaker(), false, 16.0f);
            soloBtn.setSelected (t->solo);
        }
        if (vocal)
        {
            for (auto& pk : paramKnobs) pk.first->setValue (owner.fx.get (pk.second));
            for (auto& ss : switchSecs) ss.first->sw.setToggleState (owner.fx.isOn (ss.second), juce::dontSendNotification);
            syncAB();
        }
        if (music)
        {
            const bool b = owner.busy.load();
            if (! b) speedSlider.slider.setValue (t->speed * 100.0, juce::dontSendNotification);
            updateSpeedButtons();
            sepBtn.setEnabled (! b);
            speedCancel.setVisible (b && jobIsSpeed);
            sepCancel.setVisible (b && ! jobIsSpeed);
            speedSec->setItemHeight (speedBar, b && jobIsSpeed ? 30 : (jobIsSpeed && speedBar.status.isNotEmpty() ? 30 : 0));
            sepSec->setItemHeight (sepBar, ! jobIsSpeed && sepBar.status.isNotEmpty() ? 30 : 0);
        }
        syncScale();
        refreshHeaders();
        updating = false;
        repaint();
    }

    int layout (int w)
    {
        int y = 0;
        auto* t = owner.track();
        cardArea = { 0, 0, w, t != nullptr ? 176 : 150 };
        if (t != nullptr)
        {
            auto r = cardArea.reduced (16, 14);
            auto top = r.removeFromTop (30);
            soloBtn.setBounds (top.removeFromRight (34).withSizeKeepingCentre (34, 30));
            top.removeFromRight (6);
            muteBtn.setBounds (top.removeFromRight (34).withSizeKeepingCentre (34, 30));
            r.removeFromTop (10);
            meter.setBounds (r.removeFromTop (8));
            r.removeFromTop (8);
            trackKnobs.setBounds (r.removeFromTop (96));
        }
        y = cardArea.getBottom() + 14;

        auto placeGroup = [&] (Group& g)
        {
            if (g.sections.isEmpty() || ! g.sections.getFirst()->isVisible()) { g.bounds = {}; return; }
            const int top = y;
            int sy = y + 46;
            for (auto* s : g.sections)
            {
                s->setBounds (6, sy, w - 12, s->getDesiredHeight());
                sy += s->getDesiredHeight();
            }
            g.bounds = { 0, top, w, sy - top + 8 };
            y = g.bounds.getBottom() + 14;
        };
        for (auto* g : { &vocalPresets, &vocalFx, &vocalMic, &musicMain }) placeGroup (*g);

        // preset category well
        auto cw = catHolder.getLocalBounds().reduced (3);
        if (! catButtons.isEmpty())
        {
            const int bw = cw.getWidth() / catButtons.size();
            for (auto* b : catButtons) b->setBounds (cw.removeFromLeft (bw).reduced (1, 0));
        }
        int py = 0;
        for (auto* b : presetButtons) { b->setBounds (0, py, juce::jmax (10, presetViewport.getWidth() - 10), 50); py += 54; }
        presetHolder.setSize (juce::jmax (10, presetViewport.getWidth() - 8), juce::jmax (py, 1));
        return y;
    }

    void paint (juce::Graphics& g) override
    {
        auto card = [&] (juce::Rectangle<int> r)
        {
            auto f = r.toFloat().reduced (0.5f);
            g.setColour (col::panel);  g.fillRoundedRectangle (f, 18.0f);
            g.setColour (col::border); g.drawRoundedRectangle (f, 18.0f, 1.0f);
        };
        card (cardArea);
        auto* t = owner.track();
        if (t == nullptr)
        {
            auto r = cardArea.reduced (20, 18);
            g.setColour (col::ink); g.setFont (font (16.0f, Weight::semibold));
            g.drawText (u8 ("কোনো ট্র্যাক বাছাই করা নেই"), r.removeFromTop (28), juce::Justification::centredLeft);
            g.setColour (col::muted); g.setFont (font (13.5f));
            g.drawFittedText (u8 ("নিচের টাইমলাইনে কোনো ট্র্যাকের নামে চাপ দিন — তার সব সেটিংস এখানে দেখাবে।"), r, juce::Justification::topLeft, 4);
        }
        else
        {
            auto r = cardArea.reduced (16, 14).removeFromTop (30).withTrimmedRight (84);
            const bool vocal = t->kind == TrackKind::vocal;
            g.setColour (vocal ? col::accent : juce::Colour (0xff2E8C9A));
            g.fillEllipse (r.removeFromLeft (12).toFloat().withSizeKeepingCentre (10.0f, 10.0f));
            r.removeFromLeft (8);
            g.setColour (col::ink); g.setFont (font (16.5f, Weight::semibold));
            g.drawText (t->name, r, juce::Justification::centredLeft, true);
            g.setColour (col::muted); g.setFont (font (12.0f));
            g.drawText (vocal ? u8 ("কণ্ঠ ট্র্যাক") : u8 ("মিউজিক ট্র্যাক"), r, juce::Justification::centredRight, false);
        }
        for (auto* gr : { &vocalPresets, &vocalFx, &vocalMic, &musicMain })
        {
            if (gr->bounds.isEmpty()) continue;
            card (gr->bounds);
            g.setColour (col::ink);
            g.setFont (font (15.5f, Weight::semibold));
            auto hr = gr->bounds.reduced (18, 0).withHeight (46);
            g.drawText (gr->title, hr, juce::Justification::centredLeft);
            if (gr->subtitle.isNotEmpty())
            {
                g.setColour (col::dim); g.setFont (font (12.0f));
                g.drawText (gr->subtitle, hr, juce::Justification::centredRight);
            }
        }
    }

    Inspector& owner;
    bool updating = false, dragging = false, jobIsSpeed = true;
    juce::Rectangle<int> cardArea;

    // track card
    LevelMeter meter;
    Knob volKnob { u8 ("ভলিউম"), 0, 150, 100, [] (double v) { return pct (v); } },
         panKnob { u8 ("প্যান (বাম/ডান)"), -100, 100, 0, [] (double v) { return panText (v); } };
    KnobRow trackKnobs;
    TFButton muteBtn { u8 ("M") }, soloBtn { "S" };

    // groups
    Group vocalPresets { u8 ("প্রিসেট"), u8 ("এক চাপে পুরো সাউন্ড") },
          vocalFx { u8 ("ইফেক্ট চেইন"), u8 ("উপর থেকে নিচে কাজ করে") },
          vocalMic { u8 ("মাইক"), u8 ("সব কণ্ঠ ট্র্যাকে একই") },
          musicMain { u8 ("মিউজিক সেটিংস"), {} };
    juce::OwnedArray<FxSection> sections;
    FxSection *presetSec = nullptr, *micSec = nullptr, *noiseSec = nullptr,
              *speedSec = nullptr, *scaleSec = nullptr, *sepSec = nullptr, *manageSec = nullptr;

    // presets
    struct CatHolder : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.setColour (col::well); g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
            g.setColour (col::border); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 10.0f, 1.0f);
        }
        void resized() override { if (onResize) onResize(); }
        std::function<void()> onResize;
    } catHolder;
    juce::OwnedArray<TFButton> catButtons, presetButtons;
    juce::Viewport presetViewport;
    juce::Component presetHolder;
    juce::String currentCategory { "all" };
    TFButton abBtn { u8 ("আগে/পরে") }, saveBtn { u8 ("সেভ") }, renameBtn { u8 ("নাম বদল") }, delBtn { u8 ("মুছুন") };
    ButtonPair presetButtonsRow;

    // effects
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<KnobRow> knobRows;
    std::vector<std::pair<Knob*, FxParams::Param>> paramKnobs;
    std::vector<std::pair<FxSection*, FxParams::Switch>> switchSecs;
    ScaleRow scaleRow, scaleRow2;
    InfoText scaleInfo, scaleInfo2;
    SwitchRow liveTuneRow { u8 ("লাইভ মাইকেও অটো-টিউন"), u8 ("চালু করলে লাইভে ৪৩ ms দেরি বাড়ে") };

    // mic
    MeterRow micMeterRow;
    LevelMeter micMeter;
    Knob *gainKnob = nullptr, *monKnob = nullptr, *noiseKnob = nullptr;
    KnobRow micKnobs, noiseKnobs;
    SwitchRow noiseLiveRow { u8 ("লাইভ মাইকেও নয়েজ রিমুভ"), u8 ("চালু করলে লাইভে ১০ ms দেরি বাড়ে") };
    InfoText noiseInfo;

    // music
    juce::OwnedArray<TFButton> speedButtons;
    ButtonPair speedPresetRow, speedActions, sepActions, manageActions;
    LabeledSlider speedSlider { u8 ("গতি"), 50, 150, 100, [] (double v) { return pct (v); } };
    TFButton speedApply { u8 ("এই গতি প্রয়োগ করুন"), TFButton::Style::accent }, speedCancel { u8 ("বাতিল") };
    JobBar speedBar, sepBar;
    InfoText speedInfo, sepInfo, fileInfo;
    TFButton sepBtn { u8 ("কণ্ঠ সরিয়ে কারাওকে বানান"), TFButton::Style::accent }, sepCancel { u8 ("বাতিল") };
    TFButton changeBtn { u8 ("অন্য মিউজিক দিন") }, removeBtn { u8 ("মিউজিক সরান") };
};

}
