#include "Inspector.h"
#include "MixTools.h"
#include "InspectorContent.h"
#include "Stretch.h"
#include "VocalFx.h"

namespace tf
{
//==============================================================================
// LevelMeter
//==============================================================================
void LevelMeter::setLevel (float p)
{
    auto db = juce::Decibels::gainToDecibels (p, -60.0f);
    auto v = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    shown = v > shown ? v : shown - 0.035f;
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
    const float greenEnd = 0.75f, yellowEnd = 0.92f;
    auto fill = [&] (float from, float to, juce::Colour c)
    {
        auto a = juce::jmin (shown, to);
        if (a <= from) return;
        g.setColour (c);
        g.fillRect (r.withX (r.getX() + r.getWidth() * from).withWidth (r.getWidth() * (a - from)));
    };
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

//==============================================================================
// SwitchRow, KnobRow
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
    g.setColour (col::inkSoft); g.setFont (font (14.0f));
    if (note.isEmpty()) { g.drawText (text, r, juce::Justification::centredLeft, true); return; }
    g.drawText (text, r.removeFromTop (r.getHeight() * 0.5f), juce::Justification::bottomLeft, true);
    g.setColour (col::muted); g.setFont (font (12.5f));
    g.drawText (note, r, juce::Justification::topLeft, true);
}

void KnobRow::resized()
{
    if (knobs.isEmpty()) return;
    const int cols = juce::jmax (3, knobs.size());
    const int w = getWidth() / cols;
    int x = knobs.size() < cols ? (getWidth() - w * knobs.size()) / 2 : 0;
    for (auto* k : knobs) { k->setBounds (x, 0, w, getHeight()); x += w; }
}

//==============================================================================
// FxSection
//==============================================================================
FxSection::Header::Header (FxSection& o) : juce::Button ("header"), owner (o)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setMouseClickGrabsKeyboardFocus (false);
}

FxSection::FxSection (const juce::String& t, bool sw_, juce::Path ic) : title (t), icon (std::move (ic)), hasSwitch (sw_)
{
    addAndMakeVisible (header);
    header.setTitle (t);
    header.onClick = [this] { setExpanded (! expanded); if (expanded && onExpand) onExpand (this); };
    if (hasSwitch)
    {
        sw.setTitle (t + u8 (" চালু/বন্ধ"));
        sw.onStateChange = [this] { header.repaint(); };
        addAndMakeVisible (sw);
    }
    anim.onChange = [this] { if (onLayoutChange) onLayoutChange(); header.repaint(); };
}

void FxSection::addItem (juce::Component& c, int height) { items.add ({ &c, height }); addAndMakeVisible (c); }

void FxSection::setItemHeight (juce::Component& c, int h)
{
    for (auto& it : items) if (it.first == &c && it.second != h) { it.second = h; if (onLayoutChange) onLayoutChange(); resized(); }
}

int FxSection::contentHeight() const
{
    int h = 4;
    for (auto& it : items) if (it.second > 0) h += it.second + 8;
    return h + 10;
}

void FxSection::setExpanded (bool e, bool animate)
{
    expanded = e;
    anim.setTarget (e ? 1.0f : 0.0f, ! animate);
}

int FxSection::getDesiredHeight() const { return 46 + (int) std::round (anim.get() * (float) contentHeight()); }

void FxSection::resized()
{
    auto r = getLocalBounds();
    auto head = r.removeFromTop (46);
    if (hasSwitch) sw.setBounds (head.removeFromRight (50).withSizeKeepingCentre (40, 24));
    header.setBounds (head.withTrimmedRight (hasSwitch ? 0 : 4));
    int y = 46 + 4;
    for (auto& it : items)
    {
        it.first->setVisible (it.second > 0);
        if (it.second <= 0) continue;
        it.first->setBounds (16, y, getWidth() - 28, it.second);
        y += it.second + 8;
    }
}

void FxSection::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff1E222A));
    g.fillRect (12, 0, getWidth() - 24, 1);
    if (anim.get() > 0.01f)
    {
        g.setColour (juce::Colour (0xff111318).withAlpha (anim.get()));
        g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, 46.0f, (float) getWidth() - 12.0f, (float) getHeight() - 50.0f), 10.0f);
    }
}

void FxSection::Header::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f, 2.0f);
    if (over) { g.setColour (juce::Colour (0xff1A1D24)); g.fillRoundedRectangle (r, 10.0f); }

    auto chev = juce::Rectangle<float> (13.0f, 13.0f).withCentre ({ r.getX() + 16.0f, r.getCentreY() });
    auto rot = juce::AffineTransform::rotation (owner.anim.get() * juce::MathConstants<float>::halfPi, 12.0f, 12.0f);
    auto p = icons::chevronRight(); p.applyTransform (rot);
    drawIcon (g, p, chev, owner.expanded ? col::accent : col::muted, false, 2.6f);

    const bool on = ! owner.hasSwitch || owner.sw.getToggleState();
    auto textR = r.withTrimmedLeft (30.0f).withTrimmedRight (8.0f);
    if (! owner.icon.isEmpty())
    {
        drawIcon (g, owner.icon, textR.removeFromLeft (18.0f).withSizeKeepingCentre (17.0f, 17.0f), on ? col::accent : col::dim, false, 1.8f);
        textR.removeFromLeft (8.0f);
    }
    auto sum = owner.summary ? owner.summary() : juce::String();
    g.setFont (font (12.5f));
    const auto sumW = juce::jmin (textR.getWidth() * 0.5f, juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), sum) + 4.0f);
    g.setColour (on ? col::muted : col::dim);
    g.drawText (on ? sum : u8 ("বন্ধ"), textR, juce::Justification::centredRight, true);
    g.setColour (on ? col::ink : col::dim);
    g.setFont (font (14.5f, Weight::medium));
    g.drawText (owner.title, textR.withTrimmedRight (sumW + 8.0f), juce::Justification::centredLeft, true);

    if (hasKeyboardFocus (false)) { g.setColour (col::accent); g.drawRoundedRectangle (r, 10.0f, 2.0f); }
}

//==============================================================================
// Inspector
//==============================================================================
Inspector::Inspector (Project& p, AudioEngine& e, juce::PropertiesFile& pr) : project (p), engine (e), props (pr)
{
    // first run defaults for the shared input settings
    engine.setScaleMode (props.getIntValue ("scaleMode", -2));
    engine.setLiveTune (props.getBoolValue ("liveTune", false));

    content = std::make_unique<Content> (*this);
    content->catHolder.onResize = [this] { layout(); };
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);

    applyEngineGlobals();
    rebuildPresetList();
    project.addChangeListener (this);
    engine.addChangeListener (this);
    startTimerHz (5);
}

Inspector::~Inspector()
{
    cancel = true;
    jobs.removeAllJobs (true, 20000);
    if (fxPending) commitFx();
    project.removeChangeListener (this);
    engine.removeChangeListener (this);
}

const TrackInfo* Inspector::track() const
{
    const int i = project.state().indexOf (trackId);
    return i >= 0 ? &project.state().tracks.getReference (i) : nullptr;
}

void Inspector::showTrack (const juce::String& id)
{
    if (fxPending) commitFx();
    trackId = id;
    if (auto* t = track()) kind = t->kind;
    loadFxFromTrack();
    rebuildPresetList();
    refresh();
    viewport.setViewPosition (0, 0);
}

void Inspector::loadFxFromTrack()
{
    auto* t = track();
    if (t == nullptr || t->kind != TrackKind::vocal) return;
    if (t->fx.isNotEmpty()) fx.fromString (t->fx);
    else for (auto& b : builtInPresets()) if (b.id == "ghazal") b.applyTo (fx);
    committedFx = fx.toString();
    if (auto* ef = engine.getTrackFx (trackId)) fx.bypass = ef->bypass.load();
}

void Inspector::refresh()
{
    content->refresh();
    layout();
}

void Inspector::changeListenerCallback (juce::ChangeBroadcaster* src)
{
    if (src == &project)
    {
        auto* t = track();
        if (t != nullptr && t->kind == TrackKind::vocal && ! fxPending && t->fx.isNotEmpty() && t->fx != committedFx)
        {
            loadFxFromTrack();   // undo / redo changed the effects
            rebuildPresetList();
        }
        refresh();
    }
    else
    {
        content->syncScale();
        content->refreshHeaders();
    }
}

void Inspector::fxEdited (bool commitNow)
{
    if (auto* ef = engine.getTrackFx (trackId)) ef->fromString (fx.toString());
    fxPending = true;
    pendingTicks = 0;
    if (commitNow) commitFx();
}

void Inspector::commitFx()
{
    fxPending = false;
    auto* t = track();
    if (t == nullptr || t->kind != TrackKind::vocal) return;
    const auto s = fx.toString();
    if (s == t->fx) return;
    auto c = *t;
    c.fx = s;
    committedFx = s;
    project.setTrack (c, true);
}

void Inspector::applyEngineGlobals()
{
    auto& c = *content;
    engine.setInputGainDb ((float) c.gainKnob->slider.getValue());
    engine.setMonitorVolume ((float) c.monKnob->slider.getValue() / 100.0f);
    engine.setNoiseEnabled (c.noiseSec->sw.getToggleState());
    engine.setNoiseAmount ((float) c.noiseKnob->slider.getValue() / 100.0f);
    engine.setNoiseInMonitor (c.noiseSec->sw.getToggleState());
    props.setValue ("gainDb", c.gainKnob->slider.getValue());
    props.setValue ("monitorVol", c.monKnob->slider.getValue());
    props.setValue ("noiseLive", c.noiseSec->sw.getToggleState());
    props.setValue ("noiseAmt", c.noiseKnob->slider.getValue());
    c.refreshHeaders();
    if (onNoiseChanged) onNoiseChanged();
}

void Inspector::setNoiseOn (bool on)
{
    content->noiseSec->sw.setToggleState (on, juce::sendNotification);   // applies through onStateChange
    content->noiseSec->refreshHeader();
}

bool Inspector::isNoiseOn() const { return content->noiseSec->sw.getToggleState(); }

void Inspector::timerCallback()
{
    // knob edits by mouse wheel / keyboard: one undo step after a short pause
    if (fxPending && ! content->dragging && ++pendingTicks >= 3) commitFx();
}

void Inspector::updateMeters()
{
    if (track() != nullptr) content->meter.setLevel (engine.popTrackPeak (trackId));
    if (content->micMeter.isShowing()) content->micMeter.setLevel (engine.popInputPeak());
}

void Inspector::layout()
{
    const int w = viewport.getWidth() - (viewport.isVerticalScrollBarShown() ? 10 : 0);
    if (w <= 0) return;
    const int h = content->layout (w);
    if (content->getWidth() != w || content->getHeight() != h) content->setSize (w, h);
    content->repaint();
}

void Inspector::resized()
{
    viewport.setBounds (getLocalBounds());
    layout();
}

void Inspector::paint (juce::Graphics&) {}

//==============================================================================
// Presets
//==============================================================================
juce::Array<Inspector::PresetItem> Inspector::allPresets() const
{
    juce::Array<PresetItem> list;
    for (auto& p : builtInPresets()) list.add ({ p.id, p.name, p.note, p.category, {}, false });
    for (auto& u : UserPresets (props).list())
        list.add ({ u.id, u.name, u8 ("আমার প্রিসেট"), "user", u.state, true });
    return list;
}

juce::String Inspector::presetSummary() const
{
    auto* t = track();
    if (t == nullptr) return {};
    for (auto& p : allPresets())
    {
        if (p.id != t->presetId) continue;
        FxParams ref;
        if (p.user) ref.fromString (p.state); else for (auto& b : builtInPresets()) if (b.id == p.id) b.applyTo (ref);
        return p.name + (ref.toString() != fx.toString() ? u8 (" (বদলানো)") : juce::String());
    }
    return u8 ("নিজের মতো");
}

void Inspector::rebuildPresetList()
{
    auto& c = *content;
    struct Cat { const char* id; const char* label; };
    std::vector<Cat> cats { { "all", "সব" }, { "song", "গান" }, { "band", "ব্যান্ড" }, { "rel", "ধর্মীয়" }, { "talk", "কথা" } };
    const auto presets = allPresets();
    bool hasUser = false;
    for (auto& p : presets) hasUser |= p.user;
    if (hasUser) cats.push_back ({ "user", "আমার" });
    if (! hasUser && c.currentCategory == "user") c.currentCategory = "all";

    c.catHolder.removeAllChildren();
    c.catButtons.clear();
    for (auto& ct : cats)
    {
        auto* b = c.catButtons.add (new TFButton (u8 (ct.label), TFButton::Style::segment));
        b->setFontSize (13.0f);
        b->setSelected (c.currentCategory == ct.id);
        const juce::String id (ct.id);
        juce::Component::SafePointer<Inspector> safe (this);
        b->onClick = [safe, id] { juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr) { safe->content->currentCategory = id; safe->rebuildPresetList(); } }); };
        c.catHolder.addAndMakeVisible (b);
    }

    auto* t = track();
    const juce::String sel = t != nullptr ? t->presetId : juce::String();
    bool selIsUser = false;
    c.presetHolder.removeAllChildren();
    c.presetButtons.clear();
    for (auto& p : presets)
    {
        if (p.id == sel) selIsUser = p.user;
        if (c.currentCategory != "all" && p.category != c.currentCategory) continue;
        auto* b = c.presetButtons.add (new TFButton (p.name, TFButton::Style::listRow));
        b->setSubText (p.note);
        b->setSelected (p.id == sel);
        const auto id = p.id;
        juce::Component::SafePointer<Inspector> safe (this);
        b->onClick = [safe, id] { juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr) safe->selectPreset (id); }); };
        c.presetHolder.addAndMakeVisible (b);
    }
    c.delBtn.setEnabled (selIsUser);
    c.renameBtn.setEnabled (selIsUser);
    layout();
    for (auto* b : c.presetButtons)
        if (b->isSelected() && ! c.presetViewport.getViewArea().contains (b->getBounds()))
            c.presetViewport.setViewPosition (0, juce::jmax (0, b->getY() - 54));
}

void Inspector::selectPreset (const juce::String& id)
{
    auto* t = track();
    if (t == nullptr || t->kind != TrackKind::vocal) return;
    for (auto& p : allPresets())
    {
        if (p.id != id) continue;
        FxParams n;
        if (p.user) n.fromString (p.state);
        else for (auto& b : builtInPresets()) if (b.id == id) b.applyTo (n);
        fx.fromString (n.toString());
        if (auto* ef = engine.getTrackFx (trackId)) ef->fromString (fx.toString());
        auto c = *t;
        c.fx = fx.toString();
        c.presetId = id;
        committedFx = c.fx;
        fxPending = false;
        project.setTrack (c, true);
        rebuildPresetList();
        refresh();
        if (toast) toast (p.name + u8 (" প্রিসেট চালু হয়েছে"));
        return;
    }
}

void Inspector::saveUserPreset()
{
    auto* w = new juce::AlertWindow (u8 ("নিজের প্রিসেট সেভ করুন"), u8 ("এই সেটিংসের একটা নাম দিন:"), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", {}, {});
    w->addButton (u8 ("সেভ করুন"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (u8 ("বাতিল"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<Inspector> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r)
    {
        if (safe == nullptr || r != 1) return;
        auto name = w->getTextEditorContents ("name").trim().replaceCharacter ('\t', ' ').replaceCharacter ('\n', ' ');
        if (name.isEmpty()) return;
        auto id = UserPresets (safe->props).add (name, safe->fx.toString());
        if (auto* t = safe->track())
        {
            auto c = *t;
            c.presetId = id;
            safe->project.setTrack (c, false);
        }
        safe->content->currentCategory = "user";
        safe->rebuildPresetList();
        if (safe->toast) safe->toast (u8 ("\"") + name + u8 ("\" প্রিসেট সেভ হয়েছে"));
    }), true);
}

void Inspector::renameUserPreset()
{
    auto* t = track();
    if (t == nullptr || ! t->presetId.startsWith ("user-")) return;
    const auto id = t->presetId;
    juce::Array<UserPreset> store;
    auto* p = UserPresets (props).find (id, store);
    if (p == nullptr) return;
    auto* w = new juce::AlertWindow (u8 ("প্রিসেটের নাম বদলান"), {}, juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", p->name);
    w->addButton (u8 ("ঠিক আছে"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (u8 ("বাতিল"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<Inspector> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, id] (int r)
    {
        if (safe == nullptr || r != 1) return;
        if (UserPresets (safe->props).rename (id, w->getTextEditorContents ("name"))) safe->rebuildPresetList();
    }), true);
}

void Inspector::deleteUserPreset()
{
    auto* t = track();
    if (t == nullptr || ! t->presetId.startsWith ("user-")) return;
    const auto id = t->presetId;
    juce::Component::SafePointer<Inspector> safe (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (u8 ("প্রিসেট মুছবেন?"))
                                      .withMessage (u8 ("আপনার সেভ করা এই প্রিসেট তালিকা থেকে মুছে যাবে। এখনকার সাউন্ড বদলাবে না।"))
                                      .withButton (u8 ("মুছে ফেলুন"), 1).withButton (u8 ("বাতিল"), 0)
                                      .withAssociatedComponent (this),
                                  [safe, id] (int r)
    {
        if (safe == nullptr || r != 1) return;
        UserPresets (safe->props).remove (id);
        safe->rebuildPresetList();
    });
}

//==============================================================================
// Music jobs: tempo and AI vocal removal
//==============================================================================
void Inspector::cancelJob() { cancel = true; }

void Inspector::applySpeed (double speed)
{
    auto* t = track();
    if (t == nullptr) return;
    renderMusic (speed, t->semitones, true);
}

void Inspector::applyKey (int semitones)
{
    auto* t = track();
    if (t == nullptr) return;
    renderMusic (t->speed, juce::jlimit (-6, 6, semitones), false);
}

void Inspector::renderMusic (double speed, int semis, bool isSpeed)
{
    auto* t = track();
    if (t == nullptr || t->kind != TrackKind::music || busy.exchange (true)) return;
    if (engine.isRecording()) { busy = false; return; }
    const auto id = t->id;
    const auto base = t->baseAudio;
    const double oldSpeed = t->speed;
    cancel = false;
    content->jobIsSpeed = isSpeed;
    auto& bar = isSpeed ? content->speedBar : content->keyBar;
    bar.progress = 0.0f;
    bar.status = isSpeed ? u8 ("গতি বদলানো হচ্ছে…") : u8 ("সুর বদলানো হচ্ছে…");
    refresh();

    auto done = [this, id, speed, semis, isSpeed, oldSpeed] (const juce::File& out)
    {
        if (isSpeed || std::abs (speed - oldSpeed) > 0.0005) project.replaceMusicAudio (id, out, speed);
        if (semis != (track() != nullptr ? track()->semitones : 0) || ! isSpeed) project.setMusicKey (id, out, semis);
    };

    if (std::abs (speed - 1.0) < 0.001 && semis == 0)
    {
        done (base);
        busy = false;
        bar.status = {};
        refresh();
        if (toast) toast (isSpeed ? u8 ("আসল গতিতে ফিরে এসেছে") : u8 ("আসল সুরে ফিরে এসেছে"));
        return;
    }

    juce::Component::SafePointer<Inspector> safe (this);
    jobs.addJob ([this, safe, base, speed, semis, isSpeed, done]
    {
        const auto out = tempoCacheFor (base, speed, semis);
        juce::String err;
        if (! out.existsAsFile())
            err = renderTempo (base, out, speed, semis, [safe, isSpeed] (float p)
            {
                juce::MessageManager::callAsync ([safe, p, isSpeed] { if (safe != nullptr) { auto& b = isSpeed ? safe->content->speedBar : safe->content->keyBar; b.progress = p; b.repaint(); } });
            }, cancel);
        const bool cancelled = cancel.load();
        juce::MessageManager::callAsync ([safe, out, speed, semis, err, cancelled, isSpeed, done]
        {
            if (safe == nullptr) return;
            safe->busy = false;
            auto& b = isSpeed ? safe->content->speedBar : safe->content->keyBar;
            if (cancelled) b.status = {};
            else if (err.isNotEmpty()) b.status = err;
            else
            {
                b.status = {};
                done (out);
                if (safe->toast)
                    safe->toast (isSpeed ? u8 ("গতি এখন ") + pct (speed * 100.0) + u8 (" — সুর একই আছে")
                                         : (semis == 0 ? u8 ("আসল সুরে ফিরে এসেছে")
                                                       : u8 ("সুর ") + (semis > 0 ? u8 ("উঁচু ") : u8 ("নিচু ")) + bn (std::abs (semis)) + u8 (" ধাপ — গতি একই আছে")));
            }
            safe->refresh();
        });
    });
}

//==============================================================================
// Recording clean-up (one click) and smart mix
//==============================================================================
void Inspector::cleanVoice (CleanKind kind, const juce::String& forTrack)
{
    const auto id = forTrack.isNotEmpty() ? forTrack : trackId;
    const int idx = project.state().indexOf (id);
    if (idx < 0) return;
    const auto& tr = project.state().tracks.getReference (idx);
    if (tr.kind != TrackKind::vocal) { if (toast) toast (u8 ("কণ্ঠের ট্র্যাক বাছাই করুন")); return; }
    if (tr.clips.isEmpty()) { if (toast) toast (u8 ("এই ট্র্যাকে এখনো কোনো রেকর্ডিং নেই")); return; }
    if (engine.isRecording() || busy.exchange (true)) return;

    // the files to treat (each once), with their speaker recordings
    std::vector<std::pair<juce::File, juce::File>> files;
    const juce::String tag = kind == CleanKind::noise ? "_clean" : "_noleak";
    for (auto& c : tr.clips)
    {
        if (c.file.getFileNameWithoutExtension().contains (tag)) continue;   // already done
        bool seen = false;
        for (auto& f : files) seen |= f.first == c.file;
        if (! seen) files.push_back ({ c.file, c.speaker });
    }
    if (files.empty())
    {
        busy = false;
        if (toast) toast (kind == CleanKind::noise ? u8 ("এই কণ্ঠ আগেই পরিষ্কার করা হয়েছে") : u8 ("স্পিকারের মিউজিক আগেই সরানো হয়েছে"));
        return;
    }
    cancel = false;
    auto& bar = content->cleanBar;
    bar.progress = 0.0f;
    bar.status = kind == CleanKind::noise ? u8 ("নয়েজ সরানো হচ্ছে…") : u8 ("স্পিকারের মিউজিক সরানো হচ্ছে…");
    refresh();
    const float strength = (float) content->cleanKnob->slider.getValue();

    juce::Component::SafePointer<Inspector> safe (this);
    jobs.addJob ([this, safe, files, kind, id, tag, strength]
    {
        std::map<juce::String, juce::File> done;
        juce::StringArray errors;
        int skipped = 0;
        double erleSum = 0.0; int erleN = 0;
        for (size_t i = 0; i < files.size() && ! cancel.load(); ++i)
        {
            const auto& src = files[i].first;
            auto out = src.getSiblingFile (src.getFileNameWithoutExtension() + tag + ".wav").getNonexistentSibling();
            auto prog = [safe, i, n = files.size()] (float p)
            {
                juce::MessageManager::callAsync ([safe, p, i, n] { if (safe != nullptr) { safe->content->cleanBar.progress = ((float) i + p) / (float) n; safe->content->cleanBar.repaint(); } });
            };
            juce::String err;
            if (kind == CleanKind::noise)
                err = mixtools::reduceNoise (src, out, 6.0f + strength * 0.12f, prog, cancel);   // 6..18 dB
            else
            {
                if (! files[i].second.existsAsFile()) { ++skipped; continue; }
                double erle = 0.0;
                err = mixtools::removeBleed (src, files[i].second, out, prog, cancel, &erle);
                if (err == "NOLEAK") { ++skipped; continue; }
                if (err.isEmpty()) { erleSum += erle; ++erleN; }
            }
            if (err.isEmpty()) done[src.getFullPathName()] = out;
            else errors.add (err);
        }
        const bool cancelled = cancel.load();
        juce::MessageManager::callAsync ([safe, done, errors, cancelled, kind, id, skipped, erleN]
        {
            if (safe == nullptr) return;
            safe->busy = false;
            auto& b = safe->content->cleanBar;
            b.status = {};
            if (! cancelled && ! done.empty()) safe->project.replaceClipFiles (id, done);
            if (cancelled) {}
            else if (! errors.isEmpty()) b.status = errors[0];
            else if (done.empty() && kind == CleanKind::bleed)
                b.status = u8 ("স্পিকারের মিউজিক মাইকে আসেনি (বা পুরোনো রেকর্ডিং) — কিছু বদলানোর দরকার নেই।");
            else if (safe->toast)
                safe->toast (kind == CleanKind::noise ? u8 ("নয়েজ সরানো হয়েছে — আগের মতো চাইলে Ctrl+Z")
                                                      : u8 ("স্পিকার থেকে আসা মিউজিক সরানো হয়েছে — আগের মতো চাইলে Ctrl+Z"));
            juce::ignoreUnused (skipped, erleN);
            safe->refresh();
        });
    });
}

juce::StringPairArray Inspector::presetNames() const
{
    juce::StringPairArray r;
    for (auto& p : allPresets()) r.set (p.id, p.name);
    return r;
}

void Inspector::autoBalance()
{
    if (busy.exchange (true)) return;
    if (project.state().musicTrack() == nullptr) { busy = false; if (toast) toast (u8 ("মিউজিক থাকলে তবেই ব্যালান্স করা যায়")); return; }
    auto st = project.state();
    const int key = engine.getEffectiveKey();
    content->mixBar.status = u8 ("কণ্ঠ আর মিউজিক মাপা হচ্ছে…");
    content->mixBar.progress = 0.3f;
    refresh();
    juce::Component::SafePointer<Inspector> safe (this);
    jobs.addJob ([safe, st, key]
    {
        auto gains = AudioEngine::computeBalance (st, key);
        juce::MessageManager::callAsync ([safe, gains]
        {
            if (safe == nullptr) return;
            safe->busy = false;
            safe->content->mixBar.status = {};
            if (gains.empty()) { if (safe->toast) safe->toast (u8 ("ব্যালান্স করার মতো কণ্ঠ পাওয়া যায়নি")); safe->refresh(); return; }
            safe->project.setTrackGains (gains);
            if (safe->toast) safe->toast (u8 ("কণ্ঠ আর মিউজিকের ভলিউম মিলিয়ে দেওয়া হয়েছে"));
            safe->refresh();
        });
    });
}
}
