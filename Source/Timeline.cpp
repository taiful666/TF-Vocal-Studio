#include "Timeline.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
static const juce::Colour musicCol { 0xff2E8C9A }, vocalCol { 0xffF5A524 };

//==============================================================================
// PeakCache
//==============================================================================
PeakCache::PeakCache() {}
PeakCache::~PeakCache() { pool.removeAllJobs (true, 5000); }

float PeakCache::peak (const juce::File& f, double t0, double t1)
{
    std::shared_ptr<Entry> e;
    {
        const juce::ScopedLock sl (lock);
        auto key = f.getFullPathName();
        auto it = entries.find (key);
        if (it == entries.end())
        {
            e = std::make_shared<Entry>();
            entries[key] = e;
            pool.addJob ([this, e, f]
            {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::vector<float> d;
                if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (f) })
                {
                    const int chunk = juce::jmax (1, (int) (r->sampleRate / 100.0));
                    juce::AudioBuffer<float> buf ((int) juce::jmax (1u, r->numChannels), chunk * 100);
                    d.reserve ((size_t) (r->lengthInSamples / chunk + 1));
                    for (juce::int64 p = 0; p < r->lengthInSamples; p += chunk * 100)
                    {
                        const int n = (int) juce::jmin ((juce::int64) chunk * 100, r->lengthInSamples - p);
                        r->read (&buf, 0, n, p, true, true);
                        for (int k = 0; k < n; k += chunk)
                        {
                            float m = 0;
                            for (int ch = 0; ch < buf.getNumChannels(); ++ch) m = juce::jmax (m, buf.getMagnitude (ch, k, juce::jmin (chunk, n - k)));
                            d.push_back (m);
                        }
                    }
                }
                {
                    const juce::ScopedLock sl2 (lock);
                    e->data = std::move (d);
                    e->ready = true;
                }
                sendChangeMessage();
            });
            return -1.0f;
        }
        e = it->second;
        if (! e->ready) return -1.0f;
    }
    const int a = juce::jmax (0, (int) (t0 * 100.0)), b = juce::jmin ((int) e->data.size() - 1, (int) (t1 * 100.0));
    float m = 0.0f;
    for (int i = a; i <= b; ++i) m = juce::jmax (m, e->data[(size_t) i]);
    return m;
}

//==============================================================================
// TrackHeader
//==============================================================================
class TimelineView::TrackHeader : public juce::Component
{
public:
    TrackHeader (TimelineView& o, const TrackInfo& t) : owner (o), id (t.id)
    {
        mute.setIcon (icons::speaker(), false, 16.0f);
        mute.getProperties().set ("iconOnly", true);
        mute.setTooltip (u8 ("চুপ করুন (মিউট)"));
        solo.setTooltip (u8 ("শুধু এই ট্র্যাক শুনুন"));
        for (auto* b : { &mute, &solo }) { b->setRadius (7.0f); b->setFontSize (13.0f); addAndMakeVisible (b); }
        mute.onClick = [this] { auto ti = info(); if (ti == nullptr) return; auto c = *ti; c.mute = ! c.mute; owner.project.setTrack (c); };
        solo.onClick = [this] { auto ti = info(); if (ti == nullptr) return; auto c = *ti; c.solo = ! c.solo; owner.project.setTrack (c); };

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5, 0.0);
        volume.setDoubleClickReturnValue (true, t.kind == TrackKind::music ? 0.65 : 1.0);
        volume.setTitle (u8 ("ভলিউম"));
        volume.setWantsKeyboardFocus (false);
        volume.onDragStart = [this] { owner.project.beginGesture(); };
        volume.onDragEnd = [this] { owner.project.endGesture(); };
        volume.onValueChange = [this]
        {
            auto ti = info(); if (ti == nullptr) return;
            auto c = *ti; c.gain = (float) volume.getValue();
            owner.project.setTrack (c);
        };
        addAndMakeVisible (volume);
        update (t);
    }

    const TrackInfo* info() const
    {
        const int i = owner.project.state().indexOf (id);
        return i >= 0 ? &owner.project.state().tracks.getReference (i) : nullptr;
    }

    void update (const TrackInfo& t)
    {
        name = t.name;
        kind = t.kind;
        mute.setSelected (t.mute);
        mute.setIcon (t.mute ? icons::speakerOff() : icons::speaker(), false, 16.0f);
        solo.setSelected (t.solo);
        volume.setValue (t.gain, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        auto top = r.removeFromTop (26);
        solo.setBounds (top.removeFromRight (30));
        top.removeFromRight (6);
        mute.setBounds (top.removeFromRight (30));
        r.removeFromTop (6);
        volume.setBounds (r.removeFromTop (20).withTrimmedRight (52));
    }

    void paint (juce::Graphics& g) override
    {
        const bool sel = owner.selectedTrack == id;
        auto r = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        g.setColour (sel ? juce::Colour (0xff1E222B) : juce::Colour (0xff181B22));
        g.fillRoundedRectangle (r, 10.0f);
        if (sel) { g.setColour (col::accent.withAlpha (0.55f)); g.drawRoundedRectangle (r, 10.0f, 1.2f); }
        g.setColour (kind == TrackKind::music ? musicCol : vocalCol);
        g.fillRoundedRectangle (r.withWidth (4.0f).reduced (0.0f, 8.0f), 2.0f);

        auto txt = getLocalBounds().reduced (12, 8).removeFromTop (26).withTrimmedRight (74).toFloat();
        const bool rec = owner.engine.isRecording() && owner.engine.getRecordingTrack() == id;
        if (kind == TrackKind::vocal && (sel || rec))
        {
            g.setColour (rec ? col::rec : col::rec.withAlpha (0.65f));
            g.fillEllipse (txt.getX(), txt.getCentreY() - 4.5f, 9.0f, 9.0f);
            txt.removeFromLeft (16.0f);
        }
        g.setColour (col::ink);
        g.setFont (font (14.5f, Weight::semibold));
        g.drawText (name, txt, juce::Justification::centredLeft, true);

        auto volArea = getLocalBounds().reduced (12, 8).withTrimmedTop (32).removeFromTop (20);
        g.setColour (col::muted);
        g.setFont (font (12.0f));
        g.drawText (bn ((int) std::round (volume.getValue() * 100.0)) + "%", volArea.removeFromRight (50), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        focusMainWindow (this);
        owner.selectTrack (id);
        if (e.mods.isPopupMenu()) showMenu();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { rename(); }

    void showMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, u8 ("নাম বদলান"));
        if (kind == TrackKind::vocal) m.addItem (2, u8 ("ট্র্যাক মুছে ফেলুন"));
        juce::Component::SafePointer<TrackHeader> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1) safe->rename();
            if (r == 2)
            {
                auto& ow = safe->owner;
                auto tid = safe->id;
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                  .withTitle (u8 ("ট্র্যাক মুছে ফেলবেন?"))
                                                  .withMessage (u8 ("এই কণ্ঠের ট্র্যাক সরিয়ে দেওয়া হবে। ভুল হলে \"আগের অবস্থা\" চাপলে ফিরে আসবে।"))
                                                  .withButton (u8 ("মুছে ফেলুন"), 1).withButton (u8 ("বাতিল"), 0)
                                                  .withAssociatedComponent (&ow),
                                              [&ow, tid] (int res) { if (res == 1) ow.project.removeTrack (tid); });
            }
        });
    }

    void rename()
    {
        auto t = info(); if (t == nullptr) return;
        auto* w = new juce::AlertWindow (u8 ("ট্র্যাকের নাম"), {}, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", t->name);
        w->addButton (u8 ("ঠিক আছে"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (u8 ("বাতিল"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<TrackHeader> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r)
        {
            if (safe == nullptr || r != 1) return;
            auto n = w->getTextEditorContents ("name").trim();
            auto ti = safe->info();
            if (n.isNotEmpty() && ti != nullptr) { auto c = *ti; c.name = n; safe->owner.project.setTrack (c); }
        }), true);
    }

    TimelineView& owner;
    juce::String id, name;
    TrackKind kind = TrackKind::vocal;
    TFButton mute { u8 ("মিউট") }, solo { "S" };
    juce::Slider volume;
};

//==============================================================================
// TimelineView
//==============================================================================
TimelineView::TimelineView (Project& p, AudioEngine& e) : project (p), engine (e)
{
    project.addChangeListener (this);
    peaks.addChangeListener (this);

    selectTool.setIcon (icons::cursor(), false, 16.0f);
    moveTool.setIcon (icons::hand(), false, 16.0f);
    cutBtn.setIcon (icons::scissors(), false, 16.0f);
    redoPartBtn.setIcon (icons::mic(), false, 16.0f);
    loopBtn.setIcon (icons::loop(), false, 16.0f);
    exportPartBtn.setIcon (icons::download(), false, 16.0f);
    addTrackBtn.setIcon (icons::plus(), false, 16.0f);
    undoBtn.setIcon (icons::undo(), false, 16.0f);
    redoBtn.setIcon (icons::redo(), false, 16.0f);
    zoomOutBtn.setIcon (icons::zoomOut(), false, 16.0f);
    zoomInBtn.setIcon (icons::zoomIn(), false, 16.0f);
    for (auto* b : { &undoBtn, &redoBtn, &zoomOutBtn, &zoomInBtn }) b->getProperties().set ("iconOnly", true);

    selectTool.setTooltip (u8 ("টেনে অংশ বাছাই করুন, একবার চাপলে সেখানে যাবে"));
    moveTool.setTooltip (u8 ("ক্লিপ টেনে ডানে-বামে সরান"));
    cutBtn.setTooltip (u8 ("বাছাই করা অংশ মুছে ফেলুন (Delete)"));
    redoPartBtn.setTooltip (u8 ("বাছাই করা অংশটুকু নতুন করে রেকর্ড করুন — আগে ২ সেকেন্ড বাজিয়ে শোনাবে"));
    loopBtn.setTooltip (u8 ("বাছাই করা অংশ বারবার বাজবে — অনুশীলনের জন্য"));
    exportPartBtn.setTooltip (u8 ("শুধু বাছাই করা অংশ মিক্স করে সেভ করুন"));
    addTrackBtn.setTooltip (u8 ("নতুন কণ্ঠের ট্র্যাক — যেমন দ্বিতীয় জনের জন্য"));
    undoBtn.setTooltip (u8 ("আগের অবস্থা (Ctrl+Z)"));
    redoBtn.setTooltip (u8 ("আবার করুন (Ctrl+Y)"));
    zoomOutBtn.setTooltip (u8 ("ছোট করে দেখুন (Ctrl + মাউস চাকা)"));
    zoomInBtn.setTooltip (u8 ("বড় করে দেখুন (Ctrl + মাউস চাকা)"));

    for (auto* b : { &selectTool, &moveTool, &cutBtn, &redoPartBtn, &loopBtn, &exportPartBtn, &addTrackBtn, &undoBtn, &redoBtn, &zoomOutBtn, &zoomInBtn })
    {
        b->setRadius (9.0f);
        b->setFontSize (13.5f);
        addAndMakeVisible (b);
    }

    selectTool.onClick = [this] { moveMode = false; updateToolbar(); };
    moveTool.onClick = [this] { moveMode = true; clearSelection(); updateToolbar(); };
    cutBtn.onClick = [this] { deleteSelection(); };
    redoPartBtn.onClick = [this] { if (onRedoPart && ! selection.isEmpty()) onRedoPart (selectionTrack, selection); };
    loopBtn.onClick = [this]
    {
        const bool on = ! engine.isLooping();
        engine.setLoop (selection, on && ! selection.isEmpty());
        if (engine.isLooping()) { engine.setPosition (selection.getStart()); if (! engine.isPlaying()) engine.play(); }
        updateToolbar();
        repaint();
    };
    exportPartBtn.onClick = [this] { if (onExportPart && ! selection.isEmpty()) onExportPart (selection); };
    addTrackBtn.onClick = [this] { auto id = project.addVocalTrack(); selectTrack (id); if (toast) toast (u8 ("নতুন কণ্ঠ ট্র্যাক যোগ হয়েছে")); };
    undoBtn.onClick = [this] { project.undo(); };
    redoBtn.onClick = [this] { project.redo(); };
    zoomInBtn.onClick = [this] { pxPerSec = juce::jmin (400.0, pxPerSec * 1.5); resized(); repaint(); };
    zoomOutBtn.onClick = [this] { pxPerSec = juce::jmax (4.0, pxPerSec / 1.5); resized(); repaint(); };

    hScroll.setAutoHide (false);
    addAndMakeVisible (hScroll);

    rebuildHeaders();
    updateToolbar();
    startTimerHz (30);
}

TimelineView::~TimelineView()
{
    project.removeChangeListener (this);
    peaks.removeChangeListener (this);
}

int TimelineView::getIdealHeight() const
{
    return 48 + 26 + juce::jmax (1, project.state().tracks.size()) * 72 + 22;
}

void TimelineView::selectTrack (const juce::String& id)
{
    if (selectedTrack == id) return;
    selectedTrack = id;
    for (auto* h : headers) h->repaint();
    if (onTrackSelected) onTrackSelected (id);
    repaint();
}

void TimelineView::clearSelection()
{
    selection = {};
    selectionTrack = {};
    if (engine.isLooping()) engine.setLoop ({}, false);
    updateToolbar();
    repaint();
}

void TimelineView::deleteSelection()
{
    if (selection.isEmpty() || selectionTrack.isEmpty() || engine.isRecording()) return;
    project.deleteRange (selectionTrack, selection);
    if (toast) toast (u8 ("অংশটি মুছে ফেলা হয়েছে — ফেরত চাইলে \"আগের অবস্থা\" চাপুন"));
    clearSelection();
}

void TimelineView::zoomToFit()
{
    const double len = juce::jmax (30.0, project.state().length() + 5.0);
    pxPerSec = juce::jlimit (4.0, 400.0, (double) juce::jmax (100, lanesArea.getWidth()) / len);
    scrollSec = 0.0;
    resized();
    repaint();
}

void TimelineView::changeListenerCallback (juce::ChangeBroadcaster* src)
{
    if (src == &project)
    {
        rebuildHeaders();
        updateToolbar();
        if (project.state().indexOf (selectedTrack) < 0)
        {
            // keep a valid selection: prefer the first voice track
            juce::String pick;
            for (auto& t : project.state().tracks) if (t.kind == TrackKind::vocal) { pick = t.id; break; }
            if (pick.isEmpty() && ! project.state().tracks.isEmpty()) pick = project.state().tracks.getReference (0).id;
            selectedTrack = {};
            selectTrack (pick);
        }
    }
    repaint();
}

void TimelineView::rebuildHeaders()
{
    auto& s = project.state();
    bool same = headers.size() == s.tracks.size();
    for (int i = 0; same && i < headers.size(); ++i) same = headers[i]->id == s.tracks.getReference (i).id;
    if (! same)
    {
        headers.clear();
        for (auto& t : s.tracks) addAndMakeVisible (headers.add (new TrackHeader (*this, t)));
        if (auto* p = getParentComponent()) p->resized();
        resized();
    }
    else
    {
        for (int i = 0; i < headers.size(); ++i) headers[i]->update (s.tracks.getReference (i));
    }
}

void TimelineView::updateToolbar()
{
    const bool hasSel = ! selection.isEmpty();
    selectTool.setSelected (! moveMode);
    moveTool.setSelected (moveMode);
    cutBtn.setEnabled (hasSel);
    redoPartBtn.setEnabled (hasSel);
    loopBtn.setEnabled (hasSel);
    loopBtn.setSelected (engine.isLooping());
    exportPartBtn.setEnabled (hasSel);
    undoBtn.setEnabled (project.canUndo());
    redoBtn.setEnabled (project.canRedo());
}

double TimelineView::visibleSeconds() const { return (double) lanesArea.getWidth() / pxPerSec; }
double TimelineView::xToTime (float x) const { return scrollSec + (double) (x - (float) lanesArea.getX()) / pxPerSec; }
float TimelineView::timeToX (double t) const { return (float) lanesArea.getX() + (float) ((t - scrollSec) * pxPerSec); }

juce::Rectangle<int> TimelineView::laneArea (int i) const
{
    return { lanesArea.getX(), lanesArea.getY() + i * 72, lanesArea.getWidth(), 66 };
}

int TimelineView::laneIndexAt (int y) const
{
    if (y < lanesArea.getY()) return -1;
    const int i = (y - lanesArea.getY()) / 72;
    return i < project.state().tracks.size() ? i : -1;
}

const ClipInfo* TimelineView::clipAt (int lane, double t, juce::String& trackId) const
{
    auto& s = project.state();
    if (! juce::isPositiveAndBelow (lane, s.tracks.size())) return nullptr;
    auto& tr = s.tracks.getReference (lane);
    trackId = tr.id;
    for (auto& c : tr.clips) if (t >= c.start && t < c.end()) return &c;
    return nullptr;
}

void TimelineView::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    toolbarArea = r.removeFromTop (40);
    r.removeFromTop (8);
    auto bottom = r.removeFromBottom (12);
    headerColumn = r.withWidth (236);
    auto right = r.withTrimmedLeft (240);
    rulerArea = right.removeFromTop (26);
    lanesArea = right;
    hScroll.setBounds (bottom.withTrimmedLeft (240));

    for (int i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (headerColumn.getX(), lanesArea.getY() + i * 72, 236, 66);

    // toolbar: tools | edit actions | track + undo + zoom
    auto tb = toolbarArea;
    toolWell = tb.removeFromLeft (190).withSizeKeepingCentre (190, 38);
    auto tw = toolWell.reduced (3);
    selectTool.setBounds (tw.removeFromLeft (tw.getWidth() / 2));
    moveTool.setBounds (tw);
    tb.removeFromLeft (14);
    // narrow windows: the less used buttons show only their icon (the tooltip still explains them)
    const int full = 118 + 160 + 70 + 104 + 128 + 4 * 38 + 70;
    const bool compact = tb.getWidth() < full;
    for (auto* b : { &cutBtn, &loopBtn, &exportPartBtn, &addTrackBtn }) b->getProperties().set ("iconOnly", compact);
    auto place = [&tb] (juce::Component& c, int w) { c.setBounds (tb.removeFromLeft (w).withSizeKeepingCentre (w, 36)); tb.removeFromLeft (6); };
    place (cutBtn, compact ? 40 : 118);
    place (redoPartBtn, 160);
    place (loopBtn, compact ? 40 : 70);
    place (exportPartBtn, compact ? 40 : 104);
    auto placeR = [&tb] (juce::Component& c, int w) { c.setBounds (tb.removeFromRight (w).withSizeKeepingCentre (w, 36)); tb.removeFromRight (6); };
    placeR (zoomInBtn, 38);
    placeR (zoomOutBtn, 38);
    tb.removeFromRight (8);
    placeR (redoBtn, 38);
    placeR (undoBtn, 38);
    tb.removeFromRight (8);
    placeR (addTrackBtn, compact ? 40 : 128);

    const double len = juce::jmax (project.state().length() + 30.0, visibleSeconds());
    hScroll.setRangeLimits (0.0, len);
    hScroll.setCurrentRange (scrollSec, visibleSeconds(), juce::dontSendNotification);
}

void TimelineView::timerCallback()
{
    // follow the scroll bar
    const double sb = hScroll.getCurrentRangeStart();
    if (std::abs (sb - scrollSec) > 1e-6 && drag == Drag::none) { scrollSec = sb; repaint(); }

    const double p = engine.getPosition();
    if (std::abs (p - lastPlayhead) > 1e-4 || engine.isRecording())
    {
        lastPlayhead = p;
        if (engine.isPlaying()) ensurePlayheadVisible();
        repaint();
        for (auto* h : headers) if (engine.isRecording()) h->repaint();
    }
}

void TimelineView::ensurePlayheadVisible()
{
    const double p = engine.getPosition();
    if (p > scrollSec + visibleSeconds() * 0.92 || p < scrollSec)
    {
        scrollSec = juce::jmax (0.0, p - visibleSeconds() * 0.1);
        resized();
    }
}

void TimelineView::paint (juce::Graphics& g)
{
    auto card = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (col::panel);  g.fillRoundedRectangle (card, 18.0f);
    g.setColour (col::border); g.drawRoundedRectangle (card, 18.0f, 1.0f);

    g.setColour (col::well);
    g.fillRoundedRectangle (toolWell.toFloat(), 10.0f);

    // ruler
    g.setColour (juce::Colour (0xff181B22));
    g.fillRoundedRectangle (rulerArea.toFloat(), 6.0f);
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (rulerArea.withRight (lanesArea.getRight()));
        static const double steps[] = { 0.5, 1, 2, 5, 10, 15, 30, 60, 120 };
        double step = 120;
        for (auto s : steps) if (s * pxPerSec >= 70.0) { step = s; break; }
        g.setFont (font (11.5f));
        for (double t = std::floor (scrollSec / step) * step; t < scrollSec + visibleSeconds() + step; t += step)
        {
            const float x = timeToX (t);
            g.setColour (col::track);
            g.fillRect (x, (float) rulerArea.getBottom() - 8.0f, 1.0f, 8.0f);
            g.setColour (col::muted);
            g.drawText (bnTime (t) + (step < 1 ? "." + bn ((int) std::round (std::fmod (t, 1.0) * 10)) : juce::String()),
                        juce::Rectangle<float> (x + 4.0f, (float) rulerArea.getY(), 70.0f, (float) rulerArea.getHeight() - 4.0f),
                        juce::Justification::centredLeft, false);
        }
        if (! selection.isEmpty())
        {
            g.setColour ((engine.isLooping() ? col::ok : col::accent).withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (timeToX (selection.getStart()), (float) rulerArea.getY(),
                                                (float) (selection.getLength() * pxPerSec), (float) rulerArea.getHeight()));
        }
    }

    // lanes
    auto& s = project.state();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (lanesArea);
        for (int i = 0; i < s.tracks.size(); ++i)
        {
            auto& tr = s.tracks.getReference (i);
            auto lane = laneArea (i).toFloat();
            g.setColour (tr.id == selectedTrack ? juce::Colour (0xff171A21) : juce::Colour (0xff13161C));
            g.fillRect (lane);
            const auto base = tr.kind == TrackKind::music ? musicCol : vocalCol;
            for (auto& c : tr.clips)
            {
                double start = c.start;
                if (drag == Drag::move && dragMoved && tr.id == dragTrack && c.id == dragClip) start = juce::jmax (0.0, dragOrigStart + dragDelta);
                const float x0 = timeToX (start), x1 = timeToX (start + c.length());
                if (x1 < lanesArea.getX() || x0 > lanesArea.getRight()) continue;
                auto cr = juce::Rectangle<float> (x0, lane.getY() + 4.0f, juce::jmax (2.0f, x1 - x0), lane.getHeight() - 8.0f);
                const bool dim = tr.mute;
                g.setColour (base.withAlpha (dim ? 0.10f : 0.20f));
                g.fillRoundedRectangle (cr, 6.0f);
                g.setColour (base.withAlpha (dim ? 0.35f : 0.75f));
                g.drawRoundedRectangle (cr.reduced (0.5f), 6.0f, 1.0f);
                // waveform
                g.setColour (base.withAlpha (dim ? 0.35f : 0.9f));
                const float mid = cr.getCentreY(), half = cr.getHeight() * 0.42f;
                const float from = juce::jmax (cr.getX() + 1.0f, (float) lanesArea.getX()), to = juce::jmin (cr.getRight() - 1.0f, (float) lanesArea.getRight());
                for (float x = from; x < to; x += 2.0f)
                {
                    const double tl = scrollSec + (x - (float) lanesArea.getX()) / pxPerSec;
                    const double tf = c.in + (tl - start);
                    const float pk = peaks.peak (c.file, tf, tf + 2.0 / pxPerSec);
                    if (pk < 0.0f) break;
                    const float h = juce::jmax (1.0f, std::sqrt (juce::jmin (1.0f, pk)) * half);
                    g.fillRect (x, mid - h, 1.2f, h * 2.0f);
                }
            }
            // live recording on its track
            if (engine.isRecording() && engine.getRecordingTrack() == tr.id)
            {
                auto& lp = engine.getLivePeaks();
                g.setColour (col::rec.withAlpha (0.85f));
                const float mid = lane.getCentreY(), half = lane.getHeight() * 0.38f;
                for (float x = (float) lanesArea.getX(); x < timeToX (engine.getPosition()); x += 2.0f)
                {
                    const double tl = xToTime (x);
                    const float pk = lp.maxBetween (tl, tl + 2.0 / pxPerSec);
                    if (pk <= 0.0f) continue;
                    const float h = juce::jmax (1.0f, std::sqrt (juce::jmin (1.0f, pk)) * half);
                    g.fillRect (x, mid - h, 1.2f, h * 2.0f);
                }
            }
            // selection on this lane
            if (! selection.isEmpty() && tr.id == selectionTrack)
            {
                auto sr = juce::Rectangle<float> (timeToX (selection.getStart()), lane.getY(), (float) (selection.getLength() * pxPerSec), lane.getHeight());
                g.setColour (col::accent.withAlpha (0.16f));
                g.fillRect (sr);
                g.setColour (col::accent);
                g.fillRect (sr.withWidth (1.5f));
                g.fillRect (sr.withX (sr.getRight() - 1.5f).withWidth (1.5f));
            }
        }
        if (s.tracks.isEmpty())
        {
            g.setColour (col::muted);
            g.setFont (font (14.0f));
            g.drawText (u8 ("কোনো ট্র্যাক নেই"), lanesArea, juce::Justification::centred);
        }
    }
}

void TimelineView::paintOverChildren (juce::Graphics& g)
{
    // playhead over ruler and lanes
    const float x = timeToX (engine.getPosition());
    if (x >= lanesArea.getX() - 1 && x <= lanesArea.getRight() + 1)
    {
        g.setColour (engine.isRecording() ? col::rec : col::accent);
        const float top = (float) rulerArea.getY(), bottom = (float) laneArea (juce::jmax (0, project.state().tracks.size() - 1)).getBottom();
        g.fillRect (x - 1.0f, top, 2.0f, bottom - top);
        juce::Path tri;
        tri.addTriangle (x - 6.0f, top, x + 6.0f, top, x, top + 8.0f);
        g.fillPath (tri);
    }
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    if (moveMode && laneIndexAt (e.y) >= 0)
    {
        juce::String tid;
        setMouseCursor (clipAt (laneIndexAt (e.y), xToTime ((float) e.x), tid) != nullptr ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    else
        setMouseCursor (rulerArea.contains (e.getPosition()) || lanesArea.contains (e.getPosition()) ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    focusMainWindow (this);
    dragMoved = false;
    drag = Drag::none;
    if (rulerArea.contains (e.getPosition()))
    {
        drag = Drag::seek;
        engine.setPosition (juce::jmax (0.0, xToTime ((float) e.x)));
        return;
    }
    const int lane = laneIndexAt (e.y);
    if (lane < 0 || ! lanesArea.contains (e.getPosition())) return;
    auto& tr = project.state().tracks.getReference (lane);
    selectTrack (tr.id);
    const double t = juce::jmax (0.0, xToTime ((float) e.x));

    if (moveMode)
    {
        juce::String tid;
        if (auto* c = clipAt (lane, t, tid))
        {
            if (engine.isRecording()) return;
            drag = Drag::move;
            dragTrack = tid;
            dragClip = c->id;
            dragOrigStart = c->start;
            dragAnchor = t;
            dragDelta = 0.0;
        }
        return;
    }
    drag = Drag::select;
    dragAnchor = t;
    selectionTrack = tr.id;
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    const double t = juce::jmax (0.0, xToTime ((float) e.x));
    if (e.getDistanceFromDragStart() > 3) dragMoved = true;
    if (drag == Drag::seek) { engine.setPosition (t); return; }
    if (drag == Drag::select && dragMoved)
    {
        selection = { juce::jmin (dragAnchor, t), juce::jmax (dragAnchor, t) };
        if (engine.isLooping()) engine.setLoop (selection, true);
        updateToolbar();
        repaint();
    }
    if (drag == Drag::move && dragMoved)
    {
        dragDelta = t - dragAnchor;
        repaint();
    }
}

void TimelineView::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::select && ! dragMoved)
    {
        // a simple click: move the playhead here
        selection = {};
        if (engine.isLooping()) engine.setLoop ({}, false);
        engine.setPosition (juce::jmax (0.0, xToTime ((float) e.x)));
        updateToolbar();
    }
    if (drag == Drag::move && dragMoved)
        project.moveClip (dragTrack, dragClip, juce::jmax (0.0, dragOrigStart + dragDelta));
    drag = Drag::none;
    repaint();
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const double anchor = xToTime ((float) e.x);
        pxPerSec = juce::jlimit (4.0, 400.0, pxPerSec * (w.deltaY > 0 ? 1.25 : 0.8));
        scrollSec = juce::jmax (0.0, anchor - (double) (e.x - lanesArea.getX()) / pxPerSec);
    }
    else
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        scrollSec = juce::jmax (0.0, scrollSec - d * visibleSeconds() * 0.25);
    }
    resized();
    repaint();
}
}
