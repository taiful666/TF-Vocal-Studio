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
        if (e.mods.isPopupMenu()) owner.showTrackMenu (id);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { rename(); }

    void confirmDelete()
    {
        auto& ow = owner;
        auto tid = id;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle (u8 ("ট্র্যাক মুছে ফেলবেন?"))
                                          .withMessage (u8 ("এই কণ্ঠের ট্র্যাক সরিয়ে দেওয়া হবে। ভুল হলে \"আগের অবস্থা\" চাপলে ফিরে আসবে।"))
                                          .withButton (u8 ("মুছে ফেলুন"), 1).withButton (u8 ("বাতিল"), 0)
                                          .withAssociatedComponent (&ow),
                                      [&ow, tid] (int res) { if (res == 1) ow.project.removeTrack (tid); });
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
    cutBtn.setTooltip (u8 ("বাছাই করা অংশ বা টুকরো মুছে ফেলুন (Delete)"));
    redoPartBtn.setTooltip (u8 ("বাছাই করা অংশটুকু নতুন করে রেকর্ড করুন — আগে ২ সেকেন্ড বাজিয়ে শোনাবে"));
    loopBtn.setTooltip (u8 ("বাছাই করা অংশ বারবার বাজবে — অনুশীলনের জন্য"));
    exportPartBtn.setTooltip (u8 ("শুধু বাছাই করা অংশ মিক্স করে সেভ করুন"));
    addTrackBtn.setTooltip (u8 ("নতুন কণ্ঠের ট্র্যাক — যেমন দ্বিতীয় জনের জন্য"));
    undoBtn.setTooltip (u8 ("আগের অবস্থা (Ctrl+Z)"));
    redoBtn.setTooltip (u8 ("আবার করুন (Ctrl+Y)"));
    zoomOutBtn.setTooltip (u8 ("ছোট করে দেখুন"));
    zoomInBtn.setTooltip (u8 ("বড় করে দেখুন"));

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
    zoomInBtn.onClick = [this] { zoomSlider.setValue (juce::jmin (400.0, pxPerSec * 1.5)); };
    zoomOutBtn.onClick = [this] { zoomSlider.setValue (juce::jmax (4.0, pxPerSec / 1.5)); };
    zoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.setRange (4.0, 400.0, 0.0);
    zoomSlider.setSkewFactorFromMidPoint (40.0);
    zoomSlider.setValue (pxPerSec, juce::dontSendNotification);
    zoomSlider.setTitle (u8 ("জুম"));
    zoomSlider.setTooltip (u8 ("টেনে বড় / ছোট করে দেখুন (Ctrl + মাউস চাকাও চলে)"));
    zoomSlider.setWantsKeyboardFocus (false);
    zoomSlider.onValueChange = [this]
    {
        // zoom around the playhead (or the middle) so the view does not jump away
        const double anchor = engine.getPosition() >= scrollSec && engine.getPosition() <= scrollSec + visibleSeconds() ? engine.getPosition() : scrollSec + visibleSeconds() * 0.5;
        const double ax = (anchor - scrollSec) * pxPerSec;
        pxPerSec = zoomSlider.getValue();
        scrollSec = juce::jmax (0.0, anchor - ax / pxPerSec);
        resized();
        repaint();
    };
    addAndMakeVisible (zoomSlider);

    hScroll.setAutoHide (false);
    addAndMakeVisible (hScroll);
    vScroll.setAutoHide (true);
    addChildComponent (vScroll);

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
    if (engine.isRecording()) return;
    if (! selection.isEmpty() && selectionTrack.isNotEmpty())
    {
        project.deleteRange (selectionTrack, selection);
        if (toast) toast (u8 ("অংশটি মুছে ফেলা হয়েছে — ফেরত চাইলে \"আগের অবস্থা\" চাপুন"));
        clearSelection();
        return;
    }
    if (selectedClip.isNotEmpty())
    {
        project.deleteClip (selectedClipTrack, selectedClip);
        selectedClip = {};
        if (toast) toast (u8 ("টুকরোটি মুছে ফেলা হয়েছে — ফেরত চাইলে \"আগের অবস্থা\" চাপুন"));
        updateToolbar();
        repaint();
    }
}

void TimelineView::selectClip (const juce::String& trackId, const juce::String& clipId)
{
    selectedClipTrack = trackId;
    selectedClip = clipId;
    updateToolbar();
    repaint();
}

void TimelineView::splitAtPlayhead()
{
    if (engine.isRecording()) return;
    const double t = engine.getPosition();
    bool done = selectedTrack.isNotEmpty() && project.splitAt (selectedTrack, t);
    if (! done)
        for (auto& tr : project.state().tracks)
            done |= project.splitAt (tr.id, t);
    if (toast) toast (done ? u8 ("দুই ভাগ করা হয়েছে — প্রতিটা টুকরো আলাদা সরানো বা মোছা যায়")
                           : u8 ("দাগের জায়গায় কোনো টুকরো নেই — দাগটা অডিওর উপর আনুন"));
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
    cutBtn.setEnabled (hasSel || selectedClip.isNotEmpty());
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
    return { lanesArea.getX(), lanesArea.getY() + i * 72 - vOffset, lanesArea.getWidth(), 66 };
}

int TimelineView::laneIndexAt (int y) const
{
    if (y < lanesArea.getY() || y > lanesArea.getBottom()) return -1;
    const int i = (y - lanesArea.getY() + vOffset) / 72;
    return i < project.state().tracks.size() ? i : -1;
}

juce::Rectangle<float> TimelineView::clipRect (int lane, const ClipInfo& c) const
{
    auto l = laneArea (lane).toFloat();
    double start = c.start, len = c.length();
    if (drag == Drag::move && dragMoved && c.id == dragClip) start = juce::jmax (0.0, dragOrigStart + dragDelta);
    if (drag == Drag::trim && c.id == dragClip)
    {
        if (trimEdge < 0) { len -= trimTo - start; start = trimTo; }
        else len = trimTo - start;
    }
    const float x0 = timeToX (start), x1 = timeToX (start + len);
    return { x0, l.getY() + 4.0f, juce::jmax (2.0f, x1 - x0), l.getHeight() - 8.0f };
}

juce::Rectangle<float> TimelineView::closeRect (const juce::Rectangle<float>& r) const
{
    if (r.getWidth() < 34.0f) return {};
    return { r.getRight() - 20.0f, r.getY() + 3.0f, 16.0f, 16.0f };
}

juce::Rectangle<float> TimelineView::scissorsRect() const
{
    const float x = timeToX (engine.getPosition());
    if (x < lanesArea.getX() - 1 || x > lanesArea.getRight() + 1) return {};
    return { x - 11.0f, (float) rulerArea.getBottom() - 11.0f, 22.0f, 22.0f };
}

TimelineView::Hit TimelineView::hitTest (juce::Point<int> p) const
{
    Hit h;
    const int lane = laneIndexAt (p.y);
    if (lane < 0 || ! lanesArea.contains (p)) return h;
    const auto& tr = project.state().tracks.getReference (lane);
    h.lane = lane;
    h.trackId = tr.id;
    const auto pf = p.toFloat();
    // pieces touch (re-sung parts overlap by 10 ms): the piece under the mouse wins, and of its two
    // edges the nearer one; only outside every piece does a neighbour's edge count
    const ClipInfo* inside = nullptr;
    const ClipInfo* nearEdge = nullptr;
    int nearSide = 0;
    float nearDist = 7.0f;
    for (auto& c : tr.clips)
    {
        auto r = clipRect (lane, c);
        if (r.contains (pf) && (inside == nullptr || r.getWidth() < clipRect (lane, *inside).getWidth())) inside = &c;
        if (r.getWidth() > 14.0f && pf.y >= r.getY() && pf.y <= r.getBottom())
        {
            const float dl = std::abs (pf.x - r.getX()), dr = std::abs (pf.x - r.getRight());
            if (dl < nearDist) { nearDist = dl; nearEdge = &c; nearSide = -1; }
            if (dr < nearDist) { nearDist = dr; nearEdge = &c; nearSide = 1; }
        }
    }
    if (inside != nullptr)
    {
        h.clip = inside;
        auto r = clipRect (lane, *inside);
        if (closeRect (r).expanded (2.0f).contains (pf)) { h.close = true; return h; }
        if (r.getWidth() > 14.0f)
        {
            if (pf.x - r.getX() <= 6.0f) h.edge = -1;
            else if (r.getRight() - pf.x <= 6.0f) h.edge = 1;
        }
        return h;
    }
    if (nearEdge != nullptr) { h.clip = nearEdge; h.edge = nearSide; }
    return h;
}

void TimelineView::updateVScroll()
{
    const int content = project.state().tracks.size() * 72;
    const int visible = juce::jmax (1, lanesArea.getHeight());
    vOffset = juce::jlimit (0, juce::jmax (0, content - visible), vOffset);
    vScroll.setVisible (content > visible + 2);
    vScroll.setRangeLimits (0.0, (double) juce::jmax (content, visible));
    vScroll.setCurrentRange ((double) vOffset, (double) visible, juce::dontSendNotification);
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
    vScroll.setBounds (lanesArea.getRight() - 8, lanesArea.getY(), 8, lanesArea.getHeight());
    updateVScroll();

    for (int i = 0; i < headers.size(); ++i)
    {
        auto hb = juce::Rectangle<int> (headerColumn.getX(), lanesArea.getY() + i * 72 - vOffset, 236, 66);
        headers[i]->setBounds (hb);
        headers[i]->setVisible (lanesArea.withX (headerColumn.getX()).withWidth (236).contains (hb));   // half-hidden rows are not drawn over the ruler
    }

    // toolbar: tools | edit actions | track + undo + zoom
    auto tb = toolbarArea;
    toolWell = tb.removeFromLeft (190).withSizeKeepingCentre (190, 38);
    auto tw = toolWell.reduced (3);
    selectTool.setBounds (tw.removeFromLeft (tw.getWidth() / 2));
    moveTool.setBounds (tw);
    tb.removeFromLeft (14);
    // narrow windows: the less used buttons show only their icon (the tooltip still explains them)
    const int full = 118 + 160 + 70 + 104 + 128 + 2 * 38 + 200 + 70;
    const bool compact = tb.getWidth() < full;
    for (auto* b : { &cutBtn, &loopBtn, &exportPartBtn, &addTrackBtn }) b->getProperties().set ("iconOnly", compact);
    auto place = [&tb] (juce::Component& c, int w) { c.setBounds (tb.removeFromLeft (w).withSizeKeepingCentre (w, 36)); tb.removeFromLeft (6); };
    place (cutBtn, compact ? 40 : 118);
    place (redoPartBtn, 160);
    place (loopBtn, compact ? 40 : 70);
    place (exportPartBtn, compact ? 40 : 104);
    auto placeR = [&tb] (juce::Component& c, int w) { c.setBounds (tb.removeFromRight (w).withSizeKeepingCentre (w, 36)); tb.removeFromRight (6); };
    placeR (zoomInBtn, 34);
    zoomSlider.setBounds (tb.removeFromRight (compact ? 90 : 130).withSizeKeepingCentre (compact ? 90 : 130, 24));
    tb.removeFromRight (2);
    placeR (zoomOutBtn, 34);
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
    // follow the scroll bars
    const double sb = hScroll.getCurrentRangeStart();
    if (std::abs (sb - scrollSec) > 1e-6 && drag == Drag::none) { scrollSec = sb; repaint(); }
    if (vScroll.isVisible() && std::abs (vScroll.getCurrentRangeStart() - vOffset) > 0.5 && drag == Drag::none) { vOffset = (int) vScroll.getCurrentRangeStart(); resized(); repaint(); }
    if (std::abs (zoomSlider.getValue() - pxPerSec) > 1e-6) zoomSlider.setValue (pxPerSec, juce::dontSendNotification);

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
                const auto cr = clipRect (i, c);
                if (cr.getRight() < lanesArea.getX() || cr.getX() > lanesArea.getRight()) continue;
                double start = c.start, inF = c.in;
                if (drag == Drag::move && dragMoved && c.id == dragClip) start = juce::jmax (0.0, dragOrigStart + dragDelta);
                if (drag == Drag::trim && c.id == dragClip && trimEdge < 0) { inF += trimTo - start; start = trimTo; }
                const bool dim = tr.mute;
                const bool sel = c.id == selectedClip && tr.id == selectedClipTrack;
                g.setColour (base.withAlpha (dim ? 0.10f : (sel ? 0.30f : 0.20f)));
                g.fillRoundedRectangle (cr, 6.0f);
                g.setColour (sel ? col::ink.withAlpha (0.9f) : base.withAlpha (dim ? 0.35f : 0.75f));
                g.drawRoundedRectangle (cr.reduced (0.5f), 6.0f, sel ? 2.0f : 1.0f);
                // waveform
                g.setColour (base.withAlpha (dim ? 0.35f : 0.9f));
                const float mid = cr.getCentreY(), half = cr.getHeight() * 0.42f;
                const float from = juce::jmax (cr.getX() + 1.0f, (float) lanesArea.getX()), to = juce::jmin (cr.getRight() - 1.0f, (float) lanesArea.getRight());
                for (float x = from; x < to; x += 2.0f)
                {
                    const double tl = scrollSec + (x - (float) lanesArea.getX()) / pxPerSec;
                    const double tf = inF + (tl - start);
                    const float pk = peaks.peak (c.file, tf, tf + 2.0 / pxPerSec);
                    if (pk < 0.0f) break;
                    const float h = juce::jmax (1.0f, std::sqrt (juce::jmin (1.0f, pk)) * half);
                    g.fillRect (x, mid - h, 1.2f, h * 2.0f);
                }
                // edge grips (drag to trim) and the small x (delete this piece)
                if (cr.getWidth() > 14.0f && ! engine.isRecording())
                {
                    g.setColour (base.withAlpha (dim ? 0.3f : 0.85f));
                    g.fillRoundedRectangle (cr.getX() + 2.0f, mid - 9.0f, 3.0f, 18.0f, 1.5f);
                    g.fillRoundedRectangle (cr.getRight() - 5.0f, mid - 9.0f, 3.0f, 18.0f, 1.5f);
                }
                if (auto x = closeRect (cr); ! x.isEmpty() && ! engine.isRecording())
                {
                    g.setColour (juce::Colour (0xff0F1115).withAlpha (0.85f));
                    g.fillEllipse (x);
                    g.setColour (col::inkSoft);
                    const auto k = x.reduced (5.0f);
                    g.drawLine (k.getX(), k.getY(), k.getRight(), k.getBottom(), 1.6f);
                    g.drawLine (k.getRight(), k.getY(), k.getX(), k.getBottom(), 1.6f);
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
        // the cut bar: click the scissors to split the audio here, drag it to move
        if (! engine.isRecording())
        {
            auto sr = scissorsRect();
            g.setColour (juce::Colour (0xff0F1115));
            g.fillEllipse (sr);
            g.setColour (col::accent);
            g.drawEllipse (sr.reduced (0.75f), 1.5f);
            drawIcon (g, icons::scissors(), sr.reduced (5.0f), col::ink, false, 1.6f);
        }
    }
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    if (scissorsRect().contains (e.position)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    const auto h = hitTest (e.getPosition());
    if (h.clip != nullptr && ! engine.isRecording())
    {
        if (h.close) setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else if (h.edge != 0) setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else setMouseCursor (moveMode ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::IBeamCursor);
        return;
    }
    setMouseCursor (rulerArea.contains (e.getPosition()) || lanesArea.contains (e.getPosition()) ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    focusMainWindow (this);
    dragMoved = false;
    drag = Drag::none;
    if (scissorsRect().contains (e.position))
    {
        drag = Drag::seek;   // a drag moves the cut bar; a click (mouseUp without moving) cuts
        dragClip = "scissors";
        return;
    }
    if (rulerArea.contains (e.getPosition()))
    {
        drag = Drag::seek;
        dragClip = {};
        engine.setPosition (juce::jmax (0.0, xToTime ((float) e.x)));
        return;
    }
    const int lane = laneIndexAt (e.y);
    if (lane < 0 || ! lanesArea.contains (e.getPosition())) return;
    auto& tr = project.state().tracks.getReference (lane);
    selectTrack (tr.id);
    const double t = juce::jmax (0.0, xToTime ((float) e.x));
    const auto h = hitTest (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (h.clip != nullptr) { selectClip (h.trackId, h.clip->id); showClipMenu (h, t); }
        else showTrackMenu (tr.id);
        return;
    }
    if (h.clip != nullptr && ! engine.isRecording())
    {
        if (h.close)
        {
            project.deleteClip (h.trackId, h.clip->id);
            if (selectedClip == h.clip->id) selectedClip = {};
            if (toast) toast (u8 ("টুকরোটি মুছে ফেলা হয়েছে — ফেরত চাইলে \"আগের অবস্থা\" চাপুন"));
            return;
        }
        selectClip (h.trackId, h.clip->id);
        if (h.edge != 0)
        {
            drag = Drag::trim;
            trimEdge = h.edge;
            dragTrack = h.trackId;
            dragClip = h.clip->id;
            trimTo = h.edge < 0 ? h.clip->start : h.clip->end();
            return;
        }
        if (moveMode)
        {
            drag = Drag::move;
            dragTrack = h.trackId;
            dragClip = h.clip->id;
            dragOrigStart = h.clip->start;
            dragAnchor = t;
            dragDelta = 0.0;
            return;
        }
    }
    else if (h.clip == nullptr) { selectedClip = {}; updateToolbar(); }
    drag = Drag::select;
    dragAnchor = t;
    selectionTrack = tr.id;
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    const double t = juce::jmax (0.0, xToTime ((float) e.x));
    if (e.getDistanceFromDragStart() > 3) dragMoved = true;
    if (drag == Drag::seek) { if (dragMoved || dragClip.isEmpty()) engine.setPosition (t); return; }
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
    if (drag == Drag::trim && dragMoved)
    {
        if (auto i = project.state().indexOf (dragTrack); i >= 0)
            for (auto& c : project.state().tracks.getReference (i).clips)
                if (c.id == dragClip)
                    trimTo = trimEdge < 0 ? juce::jlimit (juce::jmax (0.0, c.start - c.in), c.end() - 0.05, t)
                                          : juce::jmax (c.start + 0.05, t);
        repaint();
    }
}

void TimelineView::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::seek && dragClip == "scissors" && ! dragMoved) splitAtPlayhead();
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
    if (drag == Drag::trim && dragMoved)
        project.trimClip (dragTrack, dragClip, trimEdge < 0, trimTo);
    drag = Drag::none;
    dragClip = {};
    repaint();
}

//==============================================================================
// Right-click menus: everything for a clip or a track in one place
//==============================================================================
void TimelineView::showClipMenu (const Hit& h, double t)
{
    if (h.clip == nullptr || engine.isRecording()) return;
    const auto clip = *h.clip;
    const auto trackId = h.trackId;
    const int ti = project.state().indexOf (trackId);
    if (ti < 0) return;
    const auto& tr = project.state().tracks.getReference (ti);
    const bool vocal = tr.kind == TrackKind::vocal;

    juce::PopupMenu m;
    m.addItem (1, u8 ("এখানে কাটুন (দুই ভাগ)"), t > clip.start + 0.02 && t < clip.end() - 0.02);
    m.addItem (2, u8 ("এই টুকরো মুছুন"));
    if (vocal)
    {
        m.addItem (3, u8 ("এই টুকরোটা আবার গাই"));
        // takes that cover this piece
        juce::PopupMenu takes;
        std::vector<ClipInfo> list;
        for (auto& k : tr.takes)
            if (k.start < clip.end() - 0.05 && k.end() > clip.start + 0.05)
            {
                list.push_back (k);
                const bool current = k.file == clip.file;
                auto when = juce::Time ((juce::int64) k.recordedAt);
                takes.addItem (100 + (int) list.size() - 1, k.label + (k.recordedAt > 0 ? "  (" + when.formatted ("%H:%M") + ")" : juce::String()), ! current, current);
            }
        m.addSubMenu (u8 ("টেক বেছে নিন"), takes, list.size() > 1);
        m.addSeparator();
        m.addItem (4, u8 ("নয়েজ সরান (পুরো ট্র্যাক)"));
        m.addItem (5, u8 ("স্পিকারের মিউজিক সরান (পুরো ট্র্যাক)"));
        juce::Component::SafePointer<TimelineView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, clip, trackId, t, list] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& self = *safe;
            if (r == 1) { self.project.splitAt (trackId, t); }
            if (r == 2) { self.project.deleteClip (trackId, clip.id); self.selectedClip = {}; }
            if (r == 3 && self.onRedoPart) self.onRedoPart (trackId, { clip.start, clip.end() });
            if (r == 4 && self.onTrackAction) self.onTrackAction (Action::cleanNoise, trackId, {});
            if (r == 5 && self.onTrackAction) self.onTrackAction (Action::cleanBleed, trackId, {});
            if (r >= 100 && r - 100 < (int) list.size())
            {
                self.project.useTake (trackId, clip.id, list[(size_t) (r - 100)]);
                if (self.toast) self.toast (list[(size_t) (r - 100)].label + u8 (" বসানো হয়েছে — আগেরটা চাইলে Ctrl+Z"));
            }
        });
        return;
    }
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, clip, trackId, t] (int r)
    {
        if (safe == nullptr) return;
        if (r == 1) safe->project.splitAt (trackId, t);
        if (r == 2) { safe->project.deleteClip (trackId, clip.id); safe->selectedClip = {}; }
    });
}

void TimelineView::showTrackMenu (const juce::String& trackId)
{
    const int ti = project.state().indexOf (trackId);
    if (ti < 0 || engine.isRecording()) return;
    const auto tr = project.state().tracks.getReference (ti);
    juce::PopupMenu m;
    m.addSectionHeader (tr.name);
    if (tr.kind == TrackKind::vocal)
    {
        juce::PopupMenu presets;
        juce::StringArray ids;
        if (presetList)
        {
            auto pl = presetList();
            for (int i = 0; i < pl.size(); ++i) { ids.add (pl.getAllKeys()[i]); presets.addItem (200 + i, pl.getAllValues()[i], true, pl.getAllKeys()[i] == tr.presetId); }
        }
        m.addSubMenu (u8 ("প্রিসেট"), presets);
        m.addItem (1, u8 ("নয়েজ সরান"), ! tr.clips.isEmpty());
        m.addItem (2, u8 ("স্পিকারের মিউজিক সরান"), ! tr.clips.isEmpty());
        m.addItem (3, u8 ("কণ্ঠ-মিউজিক ব্যালান্স করুন"), ! tr.clips.isEmpty() && project.state().musicTrack() != nullptr);
        m.addItem (4, u8 ("শুধু এই কণ্ঠ সেভ করুন…"), ! tr.clips.isEmpty());
        m.addSeparator();
        m.addItem (10, tr.mute ? u8 ("আবার শুনুন (মিউট বন্ধ)") : u8 ("চুপ করুন (মিউট)"));
        m.addItem (11, tr.solo ? u8 ("সোলো বন্ধ") : u8 ("শুধু এই ট্র্যাক শুনুন (সোলো)"));
        m.addItem (12, u8 ("নাম বদলান"));
        m.addItem (13, u8 ("ট্র্যাক মুছে ফেলুন"));
        juce::Component::SafePointer<TimelineView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, tr, ids] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& self = *safe;
            auto act = [&self, &tr] (Action a, const juce::String& v = {}) { if (self.onTrackAction) self.onTrackAction (a, tr.id, v); };
            if (r == 1) act (Action::cleanNoise);
            if (r == 2) act (Action::cleanBleed);
            if (r == 3) act (Action::balance);
            if (r == 4) act (Action::exportTrack);
            if (r >= 200 && r - 200 < ids.size()) act (Action::preset, ids[r - 200]);
            if (r == 10) { auto c = tr; c.mute = ! c.mute; self.project.setTrack (c); }
            if (r == 11) { auto c = tr; c.solo = ! c.solo; self.project.setTrack (c); }
            if (r == 12) for (auto* h : self.headers) if (h->id == tr.id) h->rename();
            if (r == 13) for (auto* h : self.headers) if (h->id == tr.id) h->confirmDelete();
        });
        return;
    }
    juce::PopupMenu speed, key;
    for (int v : { 80, 85, 90, 95, 100, 105, 110, 115, 120 }) speed.addItem (300 + v, bn (v) + "%", true, (int) std::round (tr.speed * 100) == v);
    for (int k = -4; k <= 4; ++k) key.addItem (500 + k, k == 0 ? u8 ("আসল সুর") : (k > 0 ? "+" : u8 ("−")) + bn (std::abs (k)) + u8 (" ধাপ"), true, tr.semitones == k);
    m.addSubMenu (u8 ("গতি"), speed);
    m.addSubMenu (u8 ("সুর উঁচু / নিচু"), key);
    m.addItem (4, u8 ("শুধু মিউজিক সেভ করুন…"));
    m.addSeparator();
    m.addItem (10, tr.mute ? u8 ("আবার শুনুন (মিউট বন্ধ)") : u8 ("চুপ করুন (মিউট)"));
    m.addItem (11, tr.solo ? u8 ("সোলো বন্ধ") : u8 ("শুধু এই ট্র্যাক শুনুন (সোলো)"));
    m.addItem (5, u8 ("অন্য মিউজিক দিন…"));
    m.addItem (13, u8 ("মিউজিক সরান"));
    juce::Component::SafePointer<TimelineView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, tr] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& self = *safe;
        auto act = [&self, &tr] (Action a, const juce::String& v = {}) { if (self.onTrackAction) self.onTrackAction (a, tr.id, v); };
        if (r >= 380 && r <= 420) act (Action::speed, juce::String (r - 300));
        if (r >= 496 && r <= 504) act (Action::key, juce::String (r - 500));
        if (r == 4) act (Action::exportTrack);
        if (r == 5) act (Action::changeMusic);
        if (r == 10) { auto c = tr; c.mute = ! c.mute; self.project.setTrack (c); }
        if (r == 11) { auto c = tr; c.solo = ! c.solo; self.project.setTrack (c); }
        if (r == 13) self.project.removeTrack (tr.id);
    });
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const double anchor = xToTime ((float) e.x);
        pxPerSec = juce::jlimit (4.0, 400.0, pxPerSec * (w.deltaY > 0 ? 1.25 : 0.8));
        scrollSec = juce::jmax (0.0, anchor - (double) (e.x - lanesArea.getX()) / pxPerSec);
    }
    else if (vScroll.isVisible() && (e.mods.isShiftDown() || headerColumn.contains (e.getPosition())))
    {
        vOffset = juce::jmax (0, vOffset - (int) (w.deltaY * 200.0f));
        updateVScroll();
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
