#include "FxPanel.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

static void paintCard (juce::Graphics& g, juce::Rectangle<int> r, float radius = 18.0f)
{
    auto f = r.toFloat().reduced (0.5f);
    g.setColour (col::panel);  g.fillRoundedRectangle (f, radius);
    g.setColour (col::border); g.drawRoundedRectangle (f, radius, 1.0f);
}

FxSection::Header::Header (FxSection& o) : juce::Button ("header"), owner (o)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setMouseClickGrabsKeyboardFocus (false);
}

//==============================================================================
// LevelMeter
//==============================================================================
void LevelMeter::setLevel (float p)
{
    auto db = juce::Decibels::gainToDecibels (p, -60.0f);
    auto v = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    shown = v > shown ? v : shown - 0.035f;   // fast attack, smooth release
    shown = juce::jmax (0.0f, shown);
    if (v >= hold) { hold = v; holdFrames = 30; }
    else if (--holdFrames < 0) hold = juce::jmax (0.0f, hold - 0.02f);
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (col::border);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    auto greenEnd = 0.75f, yellowEnd = 0.92f;   // -15 dB, -5 dB
    auto fill = [&] (float from, float to, juce::Colour c)
    {
        auto a = juce::jmin (shown, to);
        if (a <= from) return;
        g.setColour (c);
        g.fillRect (r.withX (r.getX() + r.getWidth() * from).withWidth (r.getWidth() * (a - from)));
    };
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip; clip.addRoundedRectangle (r, r.getHeight() * 0.5f);
        g.reduceClipRegion (clip);
        fill (0.0f, greenEnd, col::ok);
        fill (greenEnd, yellowEnd, col::warn);
        fill (yellowEnd, 1.0f, col::rec);
        if (hold > 0.02f)
        {
            g.setColour (hold > yellowEnd ? col::rec : col::ink.withAlpha (0.7f));
            g.fillRect (r.getX() + r.getWidth() * hold - 2.0f, r.getY(), 2.0f, r.getHeight());
        }
    }
}

//==============================================================================
// SwitchRow
//==============================================================================
SwitchRow::SwitchRow (const juce::String& label, const juce::String& n) : text (label), note (n)
{
    sw.setTitle (label);
    addAndMakeVisible (sw);
}

void SwitchRow::resized() { sw.setBounds (getLocalBounds().removeFromRight (48).withSizeKeepingCentre (44, 26)); }

void SwitchRow::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().withTrimmedRight (56).toFloat();
    if (note.isEmpty())
    {
        g.setColour (col::inkSoft); g.setFont (font (14.0f));
        g.drawText (text, r, juce::Justification::centredLeft, true);
        return;
    }
    g.setColour (col::inkSoft); g.setFont (font (14.0f));
    g.drawText (text, r.removeFromTop (r.getHeight() * 0.5f), juce::Justification::bottomLeft, true);
    g.setColour (col::muted); g.setFont (font (12.5f));
    g.drawText (note, r, juce::Justification::topLeft, true);
}

//==============================================================================
// FxSection
//==============================================================================
FxSection::FxSection (const juce::String& t, bool sw_) : title (t), hasSwitch (sw_)
{
    addAndMakeVisible (header);
    header.setTitle (t);
    header.onClick = [this] { setExpanded (! expanded); if (expanded && onExpand) onExpand (this); };
    if (hasSwitch)
    {
        sw.setTitle (t + juce::String::fromUTF8 (" চালু/বন্ধ"));
        sw.onStateChange = [this] { header.repaint(); };
        addAndMakeVisible (sw);
    }
    anim.onChange = [this] { if (onLayoutChange) onLayoutChange(); header.repaint(); };
}

void FxSection::addItem (juce::Component& c, int height)
{
    items.add ({ &c, height });
    addAndMakeVisible (c);
}

int FxSection::contentHeight() const
{
    int h = 6;
    for (auto& it : items) h += it.second + 10;
    return h + 8;
}

void FxSection::setExpanded (bool e, bool animate)
{
    expanded = e;
    anim.setTarget (e ? 1.0f : 0.0f, ! animate);
}

int FxSection::getDesiredHeight() const
{
    return 48 + (int) std::round (anim.get() * (float) contentHeight());
}

void FxSection::resized()
{
    auto r = getLocalBounds();
    auto head = r.removeFromTop (48);
    if (hasSwitch) sw.setBounds (head.removeFromRight (52).withSizeKeepingCentre (40, 24));
    header.setBounds (head.withTrimmedRight (hasSwitch ? 0 : 4));

    int y = 48 + 6;
    for (auto& it : items)
    {
        it.first->setBounds (34, y, getWidth() - 34 - 14, it.second);
        y += it.second + 10;
    }
}

void FxSection::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff1E222A));
    g.fillRect (0, 0, getWidth(), 1);
}

void FxSection::Header::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f, 1.0f);
    if (over) { g.setColour (juce::Colour (0xff1A1D24)); g.fillRoundedRectangle (r, 10.0f); }

    auto chev = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ r.getX() + 18.0f, r.getCentreY() });
    auto rot = juce::AffineTransform::rotation (owner.anim.get() * juce::MathConstants<float>::halfPi, 12.0f, 12.0f);
    auto p = icons::chevronRight(); p.applyTransform (rot);
    drawIcon (g, p, chev, col::muted, false, 2.6f);

    const bool on = ! owner.hasSwitch || owner.sw.getToggleState();
    auto sum = owner.summary ? owner.summary() : juce::String();
    auto textR = r.withTrimmedLeft (34.0f).withTrimmedRight (8.0f);
    g.setFont (font (13.0f));
    auto sw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), sum);
    g.setColour (col::muted);
    g.drawText (on ? sum : u8 ("বন্ধ"), textR, juce::Justification::centredRight, false);
    g.setColour (on ? col::ink : col::dim);
    g.setFont (font (15.0f, Weight::medium));
    g.drawText (owner.title, textR.withTrimmedRight (sw + 10.0f), juce::Justification::centredLeft, true);

    if (hasKeyboardFocus (false)) { g.setColour (col::accent); g.drawRoundedRectangle (r, 10.0f, 2.0f); }
}

//==============================================================================
// FxPanel
//==============================================================================
static juce::String pct (double v) { return bn ((int) std::round (v)) + "%"; }

FxPanel::FxPanel (AudioEngine& e, juce::PropertiesFile& p) : engine (e), fx (e.getFx()), props (p)
{
    addAndMakeVisible (micMeter);
    addAndMakeVisible (outMeter);
    micMeter.setTitle (u8 ("মাইকের লেভেল"));
    outMeter.setTitle (u8 ("স্পিকারের লেভেল"));

    // ---- restore saved settings (first run: start from the ghazal preset)
    selectedPreset = props.getValue ("preset", "ghazal");
    if (props.containsKey ("fx")) fx.fromString (props.getValue ("fx"));
    else for (auto& b : builtInPresets()) if (b.id == selectedPreset) b.applyTo (fx);
    fx.liveTune = props.getBoolValue ("liveTune", false);

    // ---- smart match
    smartSw.setTitle (u8 ("স্মার্ট ম্যাচ"));
    smartSw.setToggleState (props.getBoolValue ("smart", true), juce::dontSendNotification);
    smartSw.onClick = [this] { props.setValue ("smart", smartSw.getToggleState()); applySmartMatch(); };
    addAndMakeVisible (smartSw);

    // ---- presets
    abBtn.setRadius (9.0f); abBtn.setFontSize (13.5f);
    saveBtn.setRadius (9.0f); saveBtn.setFontSize (13.5f);
    abBtn.setTooltip (u8 ("চাপ দিয়ে ধরে রাখার দরকার নেই — একবার চাপলে ইফেক্ট ছাড়া, আবার চাপলে ইফেক্টসহ"));
    abBtn.onClick = [this]
    {
        fx.bypass = ! fx.bypass.load();
        abBtn.setSelected (fx.bypass.load());
        abBtn.setText (fx.bypass.load() ? u8 ("ইফেক্ট ছাড়া") : u8 ("আগে / পরে"));
    };
    saveBtn.onClick = [this] { saveUserPreset(); };
    addAndMakeVisible (abBtn);
    addAndMakeVisible (saveBtn);
    presetViewport.setViewedComponent (&presetHolder, false);
    presetViewport.setScrollBarsShown (true, false);
    presetViewport.setScrollBarThickness (6);
    addAndMakeVisible (presetViewport);

    // ---- effects
    auto addSection = [this] (const char* title, int sw) -> FxSection*
    {
        auto* s = sections.add (new FxSection (u8 (title), sw != -1));   // -2 = own switch, not an FxParams switch
        if (sw >= 0)
        {
            const auto swId = (FxParams::Switch) sw;
            switchSections.push_back ({ s, swId });
            s->sw.onClick = [this, s, swId] { fx.setOn (swId, s->sw.getToggleState()); paramsChanged(); s->refreshHeader(); };
        }
        addAndMakeVisible (s);
        s->onLayoutChange = [this] { updateLayout(); };
        s->onExpand = [this] (FxSection* opened) { for (auto* o : sections) if (o != opened && o->isExpanded()) o->setExpanded (false); };
        return s;
    };
    auto addParam = [this] (FxSection* s, const char* label, FxParams::Param prm) -> LabeledSlider*
    {
        auto* ls = sliders.add (new LabeledSlider (u8 (label), 0, 100, fx.get (prm), [] (double v) { return pct (v); }));
        ls->onChange = [this, prm, s] (double v) { fx.set (prm, (float) v); s->refreshHeader(); paramsChanged(); };
        paramSliders.push_back ({ ls, prm });
        s->addItem (*ls, 48);
        return ls;
    };

    auto* tune = addSection ("সুর ঠিক করা (অটো-টিউন)", FxParams::tuneOn);
    addParam (tune, "জোর", FxParams::tuneAmt);
    addParam (tune, "কত দ্রুত সুরে বসাবে", FxParams::tuneSpd);
    liveTuneRow = switchRows.add (new SwitchRow (u8 ("লাইভ মাইকেও অটো-টিউন"), u8 ("চালু করলে লাইভে ৪৩ ms দেরি বাড়ে")));
    liveTuneRow->sw.setToggleState (fx.liveTune.load(), juce::dontSendNotification);
    liveTuneRow->sw.onClick = [this] { fx.liveTune = liveTuneRow->sw.getToggleState(); props.setValue ("liveTune", fx.liveTune.load()); };
    tune->addItem (*liveTuneRow, 44);
    tune->summary = [this] { return pct (fx.get (FxParams::tuneAmt)); };

    auto* tone = addSection ("কণ্ঠের রং (EQ)", FxParams::toneOn);
    addParam (tone, "মোটা ভাব (বেস)", FxParams::bass);
    addParam (tone, "মাঝের সুর", FxParams::mid);
    addParam (tone, "উজ্জ্বলতা", FxParams::bright);
    tone->summary = [this] { return u8 ("মোটা ") + pct (fx.get (FxParams::bass)); };

    auto* body = addSection ("ভরাট গলা", FxParams::bodyOn);
    addParam (body, "ভরাট ভাব", FxParams::body);
    addParam (body, "উষ্ণতা / খসখসে ভাব", FxParams::warm);
    body->summary = [this] { return pct (fx.get (FxParams::body)); };

    auto* rev = addSection ("রিভার্ব", FxParams::revOn);
    addParam (rev, "পরিমাণ", FxParams::rev);
    addParam (rev, "ঘরের আকার", FxParams::room);
    rev->summary = [this] { return pct (fx.get (FxParams::rev)); };

    auto* echo = addSection ("ইকো", FxParams::echoOn);
    addParam (echo, "পরিমাণ", FxParams::echo);
    addParam (echo, "দূরত্ব", FxParams::etime);
    echo->summary = [this] { return pct (fx.get (FxParams::echo)); };

    auto* dbl = addSection ("ডাবল ভয়েস (কোরাস)", FxParams::dblOn);
    addParam (dbl, "পরিমাণ", FxParams::dbl);
    dbl->summary = [this] { return pct (fx.get (FxParams::dbl)); };

    auto* deess = addSection ("স-শ এর হিস কমানো", FxParams::deessOn);
    addParam (deess, "পরিমাণ", FxParams::deess);
    deess->summary = [this] { return pct (fx.get (FxParams::deess)); };

    // engine-level settings
    noiseSection = addSection ("AI নয়েজ রিমুভ", -2);
    noiseSection->sw.setToggleState (props.getBoolValue ("noiseOn", true), juce::dontSendNotification);
    noiseAmt = sliders.add (new LabeledSlider (u8 ("পরিমাণ"), 0, 100, props.getDoubleValue ("noiseAmt", 80.0), [] (double v) { return pct (v); }));
    noiseSection->addItem (*noiseAmt, 48);
    noiseLiveRow = switchRows.add (new SwitchRow (u8 ("লাইভ মাইকেও নয়েজ রিমুভ"), u8 ("চালু করলে লাইভে ১০ ms দেরি বাড়ে")));
    noiseLiveRow->sw.setToggleState (props.getBoolValue ("noiseMon", false), juce::dontSendNotification);
    noiseSection->addItem (*noiseLiveRow, 44);
    noiseSection->summary = [this]
    {
        if (! engine.noiseAvailable()) return u8 ("৪৮০০০ Hz দরকার");
        return pct (noiseAmt->slider.getValue());
    };

    auto* gainSec = addSection ("মাইক গেইন", -1);
    gain = sliders.add (new LabeledSlider (u8 ("মাইকের জোর"), -12, 24, props.getDoubleValue ("gainDb", 0.0),
                                           [] (double v) { return (v > 0.05 ? "+" : "") + bn (juce::String (v, 1)) + " dB"; }));
    gainSec->addItem (*gain, 48);
    gainSec->summary = [this] { auto v = gain->slider.getValue(); return (v > 0.05 ? "+" : "") + bn (juce::String (v, 1)) + " dB"; };

    auto* mixSec = addSection ("ভলিউম মিক্স", -1);
    voice = sliders.add (new LabeledSlider (u8 ("কণ্ঠ"), 0, 100, props.getDoubleValue ("voice", 80.0), [] (double v) { return pct (v); }));
    music = sliders.add (new LabeledSlider (u8 ("মিউজিক"), 0, 100, props.getDoubleValue ("music", 65.0), [] (double v) { return pct (v); }));
    mixSec->addItem (*voice, 48);
    mixSec->addItem (*music, 48);
    mixSec->summary = [this] { return u8 ("কণ্ঠ ") + pct (voice->slider.getValue()); };


    auto applyEngine = [this]
    {
        engine.setNoiseEnabled (noiseSection->sw.getToggleState());
        engine.setNoiseAmount ((float) noiseAmt->slider.getValue() / 100.0f);
        engine.setNoiseInMonitor (noiseLiveRow->sw.getToggleState());
        engine.setInputGainDb ((float) gain->slider.getValue());
        engine.setVoiceVolume ((float) voice->slider.getValue() / 100.0f);
        engine.setMusicVolume ((float) music->slider.getValue() / 100.0f);
        props.setValue ("noiseOn", noiseSection->sw.getToggleState());
        props.setValue ("noiseAmt", noiseAmt->slider.getValue());
        props.setValue ("noiseMon", noiseLiveRow->sw.getToggleState());
        props.setValue ("gainDb", gain->slider.getValue());
        props.setValue ("voice", voice->slider.getValue());
        props.setValue ("music", music->slider.getValue());
        for (auto* s : sections) s->refreshHeader();
    };
    applyEngine();
    noiseLiveRow->sw.onClick = applyEngine;
    noiseSection->sw.onStateChange = applyEngine;
    for (auto* s : { noiseAmt, gain, voice, music })
        s->onChange = [applyEngine] (double) { applyEngine(); };

    sections[0]->setExpanded (true, false);
    refreshFromParams();
    rebuildPresetList();
    applySmartMatch();
    engine.addChangeListener (this);
    startTimer (1000);
}

FxPanel::~FxPanel()
{
    engine.removeChangeListener (this);
    if (dirty) props.setValue ("fx", fx.toString());
}

void FxPanel::timerCallback()
{
    if (dirty) { props.setValue ("fx", fx.toString()); props.setValue ("preset", selectedPreset); dirty = false; }
}

void FxPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    applySmartMatch();
    if (noiseSection != nullptr) noiseSection->refreshHeader();
}

void FxPanel::paramsChanged() { dirty = true; }

void FxPanel::applySmartMatch()
{
    const int key = engine.getSongKey();
    if (smartSw.getToggleState())
    {
        fx.scaleKey = key;
        if (! engine.hasSong()) keyText = u8 ("গান চালালে তার স্কেল নিজে থেকে ধরবে");
        else if (engine.isDetectingKey()) keyText = u8 ("গানের স্কেল খোঁজা হচ্ছে…");
        else keyText = key >= 0 ? u8 ("গানের স্কেল: ") + keyName (key) : u8 ("স্কেল বোঝা যায়নি — সব নোটে মেলাবে");
    }
    else
    {
        fx.scaleKey = -1;
        keyText = u8 ("বন্ধ — সব নোটে মেলাবে");
    }
    repaint (smartCard);
}

void FxPanel::refreshFromParams()
{
    for (auto& ps : paramSliders) ps.first->slider.setValue (fx.get (ps.second), juce::dontSendNotification);
    for (auto& ss : switchSections) ss.first->sw.setToggleState (fx.isOn (ss.second), juce::dontSendNotification);
    for (auto* s : sliders) s->repaint();
    for (auto* s : sections) s->refreshHeader();
}

juce::Array<FxPanel::PresetItem> FxPanel::allPresets() const
{
    juce::Array<PresetItem> list;
    for (auto& p : builtInPresets())
        list.add ({ p.id, p.name, p.note, p.category, {}, false });
    for (auto line : juce::StringArray::fromLines (props.getValue ("userPresets")))
    {
        auto parts = juce::StringArray::fromTokens (line, "\t", "");
        if (parts.size() == 3) list.add ({ parts[0], parts[1], u8 ("আপনার সেভ করা"), "user", parts[2], true });
    }
    return list;
}

void FxPanel::rebuildPresetList()
{
    // categories
    struct Cat { const char* id; const char* label; };
    std::vector<Cat> cats { { "all", "সব" }, { "song", "গান" }, { "band", "ব্যান্ড" }, { "rel", "ধর্মীয়" }, { "talk", "কথা" } };
    const auto presets = allPresets();
    bool hasUser = false;
    for (auto& p : presets) hasUser |= p.user;
    if (hasUser) cats.push_back ({ "user", "নিজের" });

    for (auto* b : catButtons) removeChildComponent (b);
    catButtons.clear();
    for (auto& c : cats)
    {
        auto* b = catButtons.add (new TFButton (u8 (c.label), TFButton::Style::segment));
        b->setFontSize (13.5f);
        b->setSelected (currentCategory == c.id);
        const juce::String id (c.id);
        juce::Component::SafePointer<FxPanel> safe (this);
        b->onClick = [safe, id] { juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr) { safe->currentCategory = id; safe->rebuildPresetList(); } }); };
        addAndMakeVisible (b);
    }

    presetHolder.removeAllChildren();
    presetButtons.clear();
    for (auto& p : presets)
    {
        if (currentCategory != "all" && p.category != currentCategory) continue;
        auto* b = presetButtons.add (new TFButton (p.name, TFButton::Style::listRow));
        b->setSubText (p.note);
        b->setSelected (p.id == selectedPreset);
        const auto id = p.id;
        juce::Component::SafePointer<FxPanel> safe (this);
        b->onClick = [safe, id] { juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr) safe->selectPreset (id); }); };
        if (p.user)
            b->getProperties().set ("user", true);
        presetHolder.addAndMakeVisible (b);
    }
    updateLayout();
}

void FxPanel::selectPreset (const juce::String& id)
{
    for (auto& p : allPresets())
    {
        if (p.id != id) continue;
        if (p.user) fx.fromString (p.state);
        else for (auto& b : builtInPresets()) if (b.id == id) b.applyTo (fx);
        selectedPreset = id;
        refreshFromParams();
        paramsChanged();
        for (auto* b : presetButtons) b->setSelected (false);
        rebuildPresetList();
        if (toast) toast (p.name + u8 (" প্রিসেট চালু হয়েছে"));
        return;
    }
}

void FxPanel::saveUserPreset()
{
    auto* w = new juce::AlertWindow (u8 ("নিজের প্রিসেট সেভ করুন"), u8 ("এই সেটিংসের একটা নাম দিন:"), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", {}, {});
    w->addButton (u8 ("সেভ করুন"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (u8 ("বাতিল"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<FxPanel> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r)
    {
        if (safe == nullptr || r != 1) return;
        auto name = w->getTextEditorContents ("name").trim().replaceCharacter ('\t', ' ').replaceCharacter ('\n', ' ');
        if (name.isEmpty()) return;
        auto id = "user-" + juce::String (juce::Time::currentTimeMillis());
        auto lines = juce::StringArray::fromLines (safe->props.getValue ("userPresets"));
        lines.removeEmptyStrings();
        lines.add (id + "\t" + name + "\t" + safe->fx.toString());
        safe->props.setValue ("userPresets", lines.joinIntoString ("\n"));
        safe->selectedPreset = id;
        safe->currentCategory = "user";
        safe->rebuildPresetList();
        if (safe->toast) safe->toast (u8 ("\"") + name + u8 ("\" প্রিসেট সেভ হয়েছে"));
    }), true);
}

void FxPanel::updateLayout()
{
    const int w = getWidth();
    if (w <= 0) return;
    meterCard = { 0, 0, w, 116 };
    smartCard = { 0, meterCard.getBottom() + 14, w, 74 };
    presetCard = { 0, smartCard.getBottom() + 14, w, 372 };
    int fxH = 52 + 8;
    for (auto* s : sections) fxH += s->getDesiredHeight();
    fxCard = { 0, presetCard.getBottom() + 14, w, fxH };

    micMeter.setBounds (20, 58, w - 40 - 56, 8);
    outMeter.setBounds (20, 86, w - 40 - 56, 8);
    smartSw.setBounds (smartCard.getRight() - 64, smartCard.getCentreY() - 13, 44, 26);

    auto pc = presetCard.reduced (16, 14);
    auto top = pc.removeFromTop (34);
    saveBtn.setBounds (top.removeFromRight (96));
    top.removeFromRight (6);
    abBtn.setBounds (top.removeFromRight (112));
    pc.removeFromTop (12);
    catWell = pc.removeFromTop (40);
    if (! catButtons.isEmpty())
    {
        auto cw = catWell.reduced (3);
        const int bw = cw.getWidth() / catButtons.size();
        for (auto* b : catButtons) b->setBounds (cw.removeFromLeft (bw).reduced (1, 0));
    }
    pc.removeFromTop (10);
    presetViewport.setBounds (pc);
    int y = 0;
    for (auto* b : presetButtons) { b->setBounds (0, y, pc.getWidth() - 10, 52); y += 56; }
    presetHolder.setSize (pc.getWidth() - 8, juce::jmax (y, 1));
    for (auto* b : presetButtons)
        if (b->isSelected() && ! presetViewport.getViewArea().contains (b->getBounds()))
            presetViewport.setViewPosition (0, juce::jmax (0, b->getY() - 56));

    int sy = fxCard.getY() + 52;
    for (auto* s : sections)
    {
        s->setBounds (fxCard.getX() + 8, sy, w - 16, s->getDesiredHeight());
        sy += s->getDesiredHeight();
    }

    const int h = fxCard.getBottom() + 4;
    if (getHeight() != h) setSize (w, h);
    repaint();
}

void FxPanel::resized() { updateLayout(); }

void FxPanel::paint (juce::Graphics& g)
{
    paintCard (g, meterCard);
    paintCard (g, smartCard);
    paintCard (g, presetCard);
    paintCard (g, fxCard);

    g.setColour (col::ink);
    g.setFont (font (17.0f, Weight::semibold));
    g.drawText (u8 ("মাইকের লেভেল"), meterCard.reduced (20, 0).withY (14).withHeight (28), juce::Justification::centredLeft);
    g.setColour (micStatusColour);
    g.setFont (font (14.0f));
    g.drawText (micStatus, meterCard.reduced (20, 0).withY (14).withHeight (28), juce::Justification::centredRight);
    g.setColour (col::muted);
    g.setFont (font (13.0f));
    g.drawText (u8 ("মাইক"), juce::Rectangle<int> (getWidth() - 70, 50, 50, 24), juce::Justification::centredRight);
    g.drawText (u8 ("স্পিকার"), juce::Rectangle<int> (getWidth() - 70, 78, 50, 24), juce::Justification::centredRight);

    // smart match
    auto sc = smartCard.reduced (18, 12).withTrimmedRight (70);
    drawIcon (g, icons::sparkle(), juce::Rectangle<float> (22, 22).withCentre ({ (float) sc.getX() + 11.0f, (float) sc.getCentreY() }), col::accent, false, 1.9f);
    sc.removeFromLeft (34);
    g.setColour (col::ink);
    g.setFont (font (15.5f, Weight::semibold));
    g.drawText (u8 ("স্মার্ট ম্যাচ"), sc.removeFromTop (sc.getHeight() / 2), juce::Justification::bottomLeft);
    g.setColour (col::muted);
    g.setFont (font (13.0f));
    g.drawText (keyText, sc, juce::Justification::topLeft, true);

    g.setColour (col::ink);
    g.setFont (font (17.0f, Weight::semibold));
    g.drawText (u8 ("কণ্ঠের প্রিসেট"), presetCard.reduced (18, 14).withHeight (34), juce::Justification::centredLeft);
    g.setColour (col::well);
    g.fillRoundedRectangle (catWell.toFloat(), 10.0f);
    g.setColour (col::border);
    g.drawRoundedRectangle (catWell.toFloat().reduced (0.5f), 10.0f, 1.0f);

    g.setColour (col::ink);
    g.setFont (font (17.0f, Weight::semibold));
    g.drawText (u8 ("ইফেক্ট"), fxCard.reduced (20, 0).withHeight (52), juce::Justification::centredLeft);
}

void FxPanel::updateMeters()
{
    auto in = engine.popInputPeak();
    auto out = engine.popOutputPeak();
    micMeter.setLevel (in);
    outMeter.setLevel (out);

    loudFrames = in > 0.89f ? 60 : juce::jmax (0, loudFrames - 1);
    silentFrames = in < 0.002f ? silentFrames + 1 : 0;

    juce::String s; juce::Colour c;
    if (! engine.isDeviceOpen())       { s = u8 ("ডিভাইস বন্ধ");            c = col::danger; }
    else if (loudFrames > 0)            { s = u8 ("খুব জোরে — গেইন কমান");   c = col::danger; }
    else if (silentFrames > 90)         { s = u8 ("মাইকে আওয়াজ নেই");        c = col::warn; }
    else                                { s = u8 ("মাইক ঠিক আছে");           c = col::okText; }
    if (s != micStatus) { micStatus = s; micStatusColour = c; repaint (meterCard); }
}
}
