#include "StudioView.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

static void paintCard (juce::Graphics& g, juce::Rectangle<int> r, float radius = 18.0f)
{
    auto f = r.toFloat().reduced (0.5f);
    g.setColour (col::panel);  g.fillRoundedRectangle (f, radius);
    g.setColour (col::border); g.drawRoundedRectangle (f, radius, 1.0f);
}

//==============================================================================
// LyricsView
//==============================================================================
LyricsView::LyricsView (AudioEngine& e) : engine (e)
{
    setOpaque (false);
    openLibrary.setRadius (12.0f);
    openLibrary.setIcon (icons::music(), false, 18.0f);
    openFile.setIcon (icons::folder(), false, 18.0f);
    addChildComponent (openLibrary);
    addChildComponent (openFile);
}

void LyricsView::setLyrics (Lyrics l)
{
    lyrics = std::move (l);
    scrollIdx = (float) lyrics.indexAt (engine.getPosition());
    repaint();
}

void LyricsView::resized()
{
    auto c = getLocalBounds().getCentre();
    openLibrary.setBounds (c.x - 230, c.y + 30, 236, 48);
    openFile.setBounds (c.x + 16, c.y + 30, 196, 48);
}

void LyricsView::tick()
{
    auto now = juce::Time::getMillisecondCounterHiRes();
    auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;

    const bool empty = ! engine.hasSong() && ! engine.isRecording();
    if (openLibrary.isVisible() != empty) { openLibrary.setVisible (empty); openFile.setVisible (empty); repaint(); }
    if (empty || ! isShowing()) return;

    float target;
    if (lyrics.timed)
        target = (float) juce::jmax (0, lyrics.indexAt (engine.getPosition()));   // before the first line: show it centred, not yet sung
    else if (! lyrics.isEmpty() && engine.getSongLength() > 0)
        target = (float) juce::jlimit (0.0, (double) lyrics.lines.size() - 1.0,
                                       engine.getPosition() / engine.getSongLength() * lyrics.lines.size() - 1.0);
    else
        target = 0.0f;

    const float before = scrollIdx;
    scrollIdx += (target - scrollIdx) * (1.0f - std::exp (-dt * 9.0f));
    if (std::abs (target - scrollIdx) < 0.0005f) scrollIdx = target;

    if (engine.isPlaying() || std::abs (before - scrollIdx) > 1e-5f)
        repaint();
}

void LyricsView::drawLine (juce::Graphics& g, const juce::String& text, float centreY, float size, float alpha,
                           double progress, bool current)
{
    auto maxW = (float) getWidth() - 40.0f;
    auto f = font (size, current ? Weight::semibold : Weight::regular);
    auto w = juce::GlyphArrangement::getStringWidth (f, text);
    if (w > maxW && w > 0) { size = juce::jmax (14.0f, size * maxW / w); f = font (size, current ? Weight::semibold : Weight::regular); w = juce::GlyphArrangement::getStringWidth (f, text); }

    auto lineH = size * 1.9f;
    juce::Rectangle<float> r (0.0f, centreY - lineH * 0.5f, (float) getWidth(), lineH);
    g.setFont (f);
    g.setColour ((current ? juce::Colour (0xffF2F3F6) : col::muted).withMultipliedAlpha (alpha));
    g.drawText (text, r, juce::Justification::centred, false);

    if (current && progress > 0.0)
    {
        auto left = r.getCentreX() - w * 0.5f;
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (juce::Rectangle<float> (left - 2.0f, r.getY(), (float) (w * progress) + 2.0f, r.getHeight()).toNearestInt());
        g.setColour (col::accent.withMultipliedAlpha (alpha));
        g.drawText (text, r, juce::Justification::centred, false);
    }
}

void LyricsView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    if (! engine.hasSong() && ! engine.isRecording())
    {
        drawIcon (g, icons::music(), juce::Rectangle<float> (48, 48).withCentre ({ b.getCentreX(), b.getCentreY() - 92 }), col::accent, false, 1.6f);
        g.setColour (col::ink);
        g.setFont (font (28.0f, Weight::semibold));
        g.drawText (u8 ("গান বাছাই করে গাওয়া শুরু করুন"), b.withY (b.getCentreY() - 56).withHeight (44), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (16.0f));
        g.drawText (u8 ("গানের ফাইল এখানে টেনে এনেও ছাড়তে পারেন"), b.withY (b.getCentreY() - 14).withHeight (28), juce::Justification::centred);
        return;
    }

    const float cy = b.getCentreY();

    if (lyrics.isEmpty())
    {
        g.setColour (col::ink);
        g.setFont (font (24.0f, Weight::semibold));
        g.drawText (engine.hasSong() ? u8 ("এই গানের লিরিক্স যোগ করা হয়নি") : u8 ("মুক্ত রেকর্ডিং চলছে"),
                    b.withY (cy - 40).withHeight (40), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (16.0f));
        g.drawText (engine.hasSong() ? u8 ("গানের ফাইলের পাশে একই নামের .lrc ফাইল রাখুন, বা এখানে টেনে আনুন")
                                     : u8 ("গান ছাড়াই আপনার কণ্ঠ রেকর্ড হচ্ছে"),
                    b.withY (cy + 4).withHeight (28), juce::Justification::centred);
        return;
    }

    const int sungIdx = lyrics.timed ? lyrics.indexAt (engine.getPosition()) : -1;
    const int current = lyrics.timed ? juce::jmax (0, sungIdx) : (int) std::round (scrollIdx);
    const double progress = sungIdx >= 0 ? lyrics.progressAt (sungIdx, engine.getPosition(), engine.getSongLength()) : 0.0;
    const float spacing = juce::jlimit (52.0f, 80.0f, b.getHeight() / 5.2f);
    const float bigSize = juce::jlimit (28.0f, 44.0f, b.getHeight() / 8.0f);

    const int first = (int) std::floor (scrollIdx) - 2;
    for (int k = juce::jmax (0, first); k <= juce::jmin (lyrics.lines.size() - 1, first + 6); ++k)
    {
        const float d = (float) k - scrollIdx;
        const float ad = std::abs (d);
        if (ad > 2.8f) continue;
        const float size = bigSize - juce::jmin (1.0f, ad) * (bigSize - 22.0f);
        float alpha = ad < 1.0f ? 1.0f - 0.4f * ad : juce::jmax (0.0f, 0.6f - (ad - 1.0f) * 0.32f);
        if (d < 0) alpha *= 0.8f;   // already sung lines a little dimmer
        const float y = cy + d * spacing + (d > 0 ? (bigSize - 22.0f) * 0.5f * juce::jmin (1.0f, ad) : -(bigSize - 22.0f) * 0.5f * juce::jmin (1.0f, ad));
        auto& line = lyrics.lines.getReference (k);
        drawLine (g, line.text.isEmpty() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xaa  \xe2\x99\xaa")) : line.text,
                  y, size, alpha, k == current ? progress : 0.0, k == current && lyrics.timed);
    }

    // Count-in before the first line
    if (lyrics.timed && sungIdx < 0)
    {
        auto toFirst = lyrics.lines.getReference (0).time - engine.getPosition();
        if (toFirst > 0 && toFirst < 4.0 && engine.isPlaying())
        {
            g.setColour (col::accent);
            g.setFont (font (18.0f, Weight::semibold));
            g.drawText (bn ((int) std::ceil (toFirst)), b.withY (cy - 3 * spacing).withHeight (40), juce::Justification::centred);
        }
    }

    if (! lyrics.timed)
    {
        g.setColour (col::dim);
        g.setFont (font (13.0f));
        g.drawText (u8 ("লিরিক্সে সময় মেলানো নেই — আন্দাজে স্ক্রল হচ্ছে"), b.removeFromBottom (24), juce::Justification::centred);
    }
}

//==============================================================================
// WaveformView
//==============================================================================
WaveformView::WaveformView (AudioEngine& e) : engine (e), thumb (512, e.getFormatManager(), cache)
{
    thumb.addChangeListener (this);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

WaveformView::~WaveformView() { thumb.removeChangeListener (this); }

void WaveformView::setSong (const juce::File& f)
{
    if (f.existsAsFile()) thumb.setSource (new juce::FileInputSource (f));
    else thumb.clear();
    repaint();
}

double WaveformView::timelineLength() const
{
    if (engine.hasSong()) return juce::jmax (1.0, engine.getSongLength());
    return juce::jmax (30.0, engine.getRecordedSeconds() + 10.0, engine.getVocalPeaks().lastSecond() + 5.0);
}

juce::Rectangle<float> WaveformView::rowArea (int row) const
{
    auto r = getLocalBounds().toFloat().reduced (18.0f, 14.0f);
    auto h = (r.getHeight() - 10.0f) * 0.5f;
    auto rr = juce::Rectangle<float> (r.getX(), r.getY() + (float) row * (h + 10.0f), r.getWidth(), h);
    return rr.withTrimmedLeft (106.0f);
}

void WaveformView::paint (juce::Graphics& g)
{
    const double len = timelineLength();
    const double pos = engine.getPosition();
    const auto peak = juce::jmax (0.05f, thumb.getApproximatePeak());

    for (int row = 0; row < 2; ++row)
    {
        auto area = rowArea (row);
        g.setColour (col::muted);
        g.setFont (font (14.0f));
        g.drawText (row == 0 ? u8 ("মিউজিক") : u8 ("আপনার কণ্ঠ"), area.withX (18.0f).withWidth (100.0f), juce::Justification::centredLeft);

        const float barW = 3.0f, gap = 2.0f;
        const int nb = juce::jmax (1, (int) (area.getWidth() / (barW + gap)));
        const float step = area.getWidth() / (float) nb;
        for (int i = 0; i < nb; ++i)
        {
            const double t0 = len * i / nb, t1 = len * (i + 1) / nb;
            float amp = 0.0f;
            if (row == 0)
            {
                if (engine.hasSong() && thumb.getTotalLength() > 0)
                {
                    float mn = 0, mx = 0;
                    thumb.getApproximateMinMax (t0, t1, 0, mn, mx);
                    amp = juce::jmax (std::abs (mn), std::abs (mx)) / peak;
                }
            }
            else
            {
                amp = juce::jmin (1.0f, std::sqrt (engine.getVocalPeaks().maxBetween (t0, t1)) * 1.15f);
            }
            const float h = juce::jmax (3.0f, amp * area.getHeight());
            const bool played = t0 < pos;
            juce::Colour c = row == 0 ? (played ? col::inkSoft : col::track)
                                      : (amp > 0.01f ? (played ? col::accent : col::accent.withAlpha (0.45f)) : juce::Colour (0xff2C313C));
            g.setColour (c);
            g.fillRoundedRectangle (area.getX() + i * step, area.getCentreY() - h * 0.5f, barW, h, 1.5f);
        }
    }

    // playhead
    auto a = rowArea (0);
    auto x = a.getX() + (float) (pos / len) * a.getWidth();
    auto bottom = rowArea (1).getBottom();
    g.setColour (engine.isRecording() ? col::rec : col::accent);
    g.fillRoundedRectangle (x - 1.0f, a.getY() - 4.0f, 2.0f, bottom - a.getY() + 8.0f, 1.0f);
}

void WaveformView::seekTo (float x)
{
    if (engine.isRecording() || ! engine.hasSong()) return;
    auto a = rowArea (0);
    engine.setPosition (juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth())) * timelineLength());
}

void WaveformView::mouseDown (const juce::MouseEvent& e) { seekTo ((float) e.x); }
void WaveformView::mouseDrag (const juce::MouseEvent& e) { seekTo ((float) e.x); }

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
// RightColumn
//==============================================================================
RightColumn::RightColumn (AudioEngine& e, juce::PropertiesFile& p) : engine (e), props (p)
{
    addAndMakeVisible (micMeter);
    addAndMakeVisible (outMeter);
    micMeter.setTitle (u8 ("মাইকের লেভেল"));
    outMeter.setTitle (u8 ("স্পিকারের লেভেল"));

    // restore settings
    noiseSec.sw.setToggleState (props.getBoolValue ("noiseOn", true), juce::dontSendNotification);
    noiseAmt.slider.setValue (props.getDoubleValue ("noiseAmt", 80.0), juce::dontSendNotification);
    noiseMonitor.sw.setToggleState (props.getBoolValue ("noiseMon", false), juce::dontSendNotification);
    gain.slider.setValue (props.getDoubleValue ("gainDb", 0.0), juce::dontSendNotification);
    voice.slider.setValue (props.getDoubleValue ("voice", 80.0), juce::dontSendNotification);
    music.slider.setValue (props.getDoubleValue ("music", 65.0), juce::dontSendNotification);
    monitor.sw.setToggleState (props.getBoolValue ("monitor", true), juce::dontSendNotification);

    auto apply = [this]
    {
        engine.setNoiseEnabled (noiseSec.sw.getToggleState());
        engine.setNoiseAmount ((float) noiseAmt.slider.getValue() / 100.0f);
        engine.setNoiseInMonitor (noiseMonitor.sw.getToggleState());
        engine.setInputGainDb ((float) gain.slider.getValue());
        engine.setVoiceVolume ((float) voice.slider.getValue() / 100.0f);
        engine.setMusicVolume ((float) music.slider.getValue() / 100.0f);
        engine.setMonitorEnabled (monitor.sw.getToggleState());
        props.setValue ("noiseOn", noiseSec.sw.getToggleState());
        props.setValue ("noiseAmt", noiseAmt.slider.getValue());
        props.setValue ("noiseMon", noiseMonitor.sw.getToggleState());
        props.setValue ("gainDb", gain.slider.getValue());
        props.setValue ("voice", voice.slider.getValue());
        props.setValue ("music", music.slider.getValue());
        props.setValue ("monitor", monitor.sw.getToggleState());
        for (auto* s : sections) s->refreshHeader();
    };
    apply();

    noiseSec.sw.onClick = apply;
    noiseMonitor.sw.onClick = apply;
    monitor.sw.onClick = apply;
    for (auto* s : { &noiseAmt, &gain, &voice, &music })
        s->onChange = [apply] (double) { apply(); };

    noiseSec.addItem (noiseAmt, 48);
    noiseSec.addItem (noiseMonitor, 44);
    gainSec.addItem (gain, 48);
    mixSec.addItem (voice, 48);
    mixSec.addItem (music, 48);
    mixSec.addItem (monitor, 36);

    noiseSec.summary = [this]
    {
        if (! engine.noiseAvailable()) return u8 ("৪৮০০০ Hz দরকার");
        return bn ((int) std::round (noiseAmt.slider.getValue())) + "%";
    };
    gainSec.summary = [this] { auto v = gain.slider.getValue(); return (v > 0.05 ? "+" : "") + bn (juce::String (v, 1)) + " dB"; };
    mixSec.summary = [this] { return u8 ("কণ্ঠ ") + bn ((int) std::round (voice.slider.getValue())) + "%"; };

    for (auto* s : sections)
    {
        addAndMakeVisible (s);
        s->onLayoutChange = [this] { updateLayout(); };
        s->onExpand = [this] (FxSection* opened) { for (auto* o : sections) if (o != opened && o->isExpanded()) o->setExpanded (false); };
    }
    noiseSec.setExpanded (true, false);
}

void RightColumn::refreshAvailability()
{
    noiseMonitor.setNote (engine.noiseAvailable() ? u8 ("চালু করলে ১০ ms দেরি বাড়ে")
                                                  : u8 ("সেটিংসে Sample rate ৪৮০০০ বাছাই করুন"));
    noiseSec.refreshHeader();
}

void RightColumn::updateLayout()
{
    const int w = getWidth();
    meterCard = { 0, 0, w, 116 };
    int y = meterCard.getBottom() + 14;
    int fxH = 52 + 8;
    for (auto* s : sections) fxH += s->getDesiredHeight();
    fxCard = { 0, y, w, fxH };

    int sy = fxCard.getY() + 52;
    for (auto* s : sections)
    {
        s->setBounds (fxCard.getX() + 8, sy, w - 16, s->getDesiredHeight());
        sy += s->getDesiredHeight();
    }

    micMeter.setBounds (20, 58, w - 40 - 56, 8);
    outMeter.setBounds (20, 86, w - 40 - 56, 8);

    const int h = fxCard.getBottom() + 4;
    if (getHeight() != h) setSize (w, h);
    repaint();
}

void RightColumn::resized() { updateLayout(); }

void RightColumn::paint (juce::Graphics& g)
{
    paintCard (g, meterCard);
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

    g.setColour (col::ink);
    g.setFont (font (17.0f, Weight::semibold));
    g.drawText (u8 ("ইফেক্ট"), fxCard.reduced (20, 0).withHeight (52), juce::Justification::centredLeft);
}

void RightColumn::updateMeters()
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

//==============================================================================
// StudioView
//==============================================================================
StudioView::StudioView (AudioEngine& e, juce::PropertiesFile& p) : engine (e), props (p)
{
    addAndMakeVisible (lyricsView);
    addAndMakeVisible (waveform);

    toStartBtn.setIcon (icons::toStart(), true, 17.0f);
    stopBtn.setIcon (icons::stop(), true, 15.0f);
    playBtn.setIcon (icons::play(), true, 22.0f);
    listenBtn.setIcon (icons::headphones(), false, 19.0f);
    saveBtn.setIcon (icons::download(), false, 18.0f);
    for (auto* b : { &toStartBtn, &stopBtn, &playBtn, &listenBtn })
        b->getProperties().set ("iconOnly", true);
    toStartBtn.setTooltip (u8 ("শুরুতে যান (Home)"));
    stopBtn.setTooltip (u8 ("থামান"));
    playBtn.setTooltip (u8 ("চালান / বিরতি (Space)"));
    recBtn.setTooltip (u8 ("রেকর্ড (R)"));
    listenBtn.setTooltip (u8 ("শেষ রেকর্ডিং গানের সাথে শুনুন"));
    playBtn.setRadius (16.0f);
    recBtn.setRadius (16.0f);
    recBtn.setFontSize (16.0f);

    for (auto* b : { &toStartBtn, &stopBtn, &playBtn, &recBtn, &listenBtn, &saveBtn })
        addAndMakeVisible (b);

    toStartBtn.onClick = [this] { goToStart(); };
    stopBtn.onClick = [this] { engine.stop(); };
    playBtn.onClick = [this] { togglePlay(); };
    recBtn.onClick = [this] { toggleRecord(); };
    saveBtn.onClick = [this] { showSaveMenu(); };
    listenBtn.onClick = [this]
    {
        if (engine.hasTakePlayback()) { engine.clearTakePlayback(); return; }
        juce::String err;
        if (engine.loadTakeForPlayback (engine.getLastTake(), err)) engine.play();
        else showError (err);
    };

    lyricsView.openLibrary.onClick = [this] { if (onOpenLibrary) onOpenLibrary(); };
    lyricsView.openFile.onClick = [this] { chooseFile(); };

    rightViewport.setViewedComponent (&right, false);
    rightViewport.setScrollBarsShown (true, false);
    rightViewport.setScrollBarThickness (8);
    addAndMakeVisible (rightViewport);

    engine.addChangeListener (this);
    right.refreshAvailability();
    updateTransportButtons();
    startTimerHz (30);
}

StudioView::~StudioView() { engine.removeChangeListener (this); }

void StudioView::showError (const juce::String& msg)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (u8 ("একটু সমস্যা হয়েছে"))
                                      .withMessage (msg)
                                      .withButton (u8 ("ঠিক আছে"), 1)
                                      .withAssociatedComponent (this),
                                  nullptr);
}

void StudioView::loadSong (const juce::File& f)
{
    juce::String err;
    if (! engine.loadSong (f, err)) { showError (err); return; }
    lyrics = Lyrics::loadFromFile (Lyrics::findFor (f));
    lyricsView.setLyrics (lyrics);
    waveform.setSong (f);
    props.setValue ("lastSong", f.getFullPathName());
    resized();
    repaint();
}

void StudioView::loadTake (const Take& t)
{
    auto before = engine.getSongFile();
    juce::String err;
    if (! engine.loadTakeForPlayback (t, err)) { showError (err); return; }
    if (engine.getSongFile() != before)
    {
        lyrics = Lyrics::loadFromFile (Lyrics::findFor (engine.getSongFile()));
        lyricsView.setLyrics (lyrics);
        waveform.setSong (engine.getSongFile());
    }
    repaint();
}

void StudioView::togglePlay()
{
    if (! engine.hasSong() && ! engine.isRecording()) { if (onOpenLibrary) onOpenLibrary(); return; }
    engine.togglePlay();
}

void StudioView::toggleRecord()
{
    if (engine.isRecording()) { engine.stopRecording(); if (toast) toast (u8 ("রেকর্ডিং সেভ হয়েছে")); return; }
    juce::String err;
    if (! engine.startRecording (err)) showError (err);
}

void StudioView::goToStart()
{
    if (! engine.isRecording()) engine.setPosition (0.0);
}

void StudioView::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> (u8 ("কারাওকে বা গানের ফাইল বাছুন"),
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.wma;*.aif;*.aiff");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f.existsAsFile()) loadSong (f);
                          });
}

bool StudioView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("mp3;wav;m4a;aac;flac;ogg;wma;aif;aiff;lrc;txt")) return true;
    return false;
}

void StudioView::filesDropped (const juce::StringArray& files, int, int)
{
    juce::File audio, lyr;
    for (auto& s : files)
    {
        juce::File f (s);
        if (f.hasFileExtension ("lrc;txt")) lyr = f;
        else if (f.hasFileExtension ("mp3;wav;m4a;aac;flac;ogg;wma;aif;aiff")) audio = f;
    }
    if (audio.existsAsFile()) loadSong (audio);
    if (lyr.existsAsFile() && engine.hasSong())
    {
        auto target = engine.getSongFile().withFileExtension (lyr.getFileExtension().toLowerCase());
        if (target != lyr) lyr.copyFileTo (target);
        lyrics = Lyrics::loadFromFile (lyr);
        lyricsView.setLyrics (lyrics);
        if (toast) toast (u8 ("লিরিক্স যোগ হয়েছে"));
        repaint();
    }
}

void StudioView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateTransportButtons();
    right.refreshAvailability();
    repaint();
}

void StudioView::updateTransportButtons()
{
    const bool playing = engine.isPlaying();
    playBtn.setIcon (playing ? icons::pause() : icons::play(), true, 22.0f);
    playBtn.setTitle (playing ? u8 ("বিরতি") : u8 ("চালান"));
    recBtn.setSelected (engine.isRecording());
    recBtn.setText (engine.isRecording() ? u8 ("রেকর্ড হচ্ছে") : u8 ("রেকর্ড"));
    const bool hasTake = engine.getLastTake().isValid() && ! engine.isRecording();
    listenBtn.setVisible (hasTake);
    listenBtn.setSelected (engine.hasTakePlayback());
    saveBtn.setEnabled (hasTake && exportProgress < 0.0f);
    toStartBtn.setEnabled (! engine.isRecording() && engine.hasSong());
    resized();
}

void StudioView::timerCallback()
{
    right.updateMeters();
    repaint (timeArea);

    const bool playing = engine.isPlaying();
    if (playing != wasPlaying)
    {
        wasPlaying = playing;
        if (! playing)
        {
            if (engine.isRecording() && engine.hasSong()) { engine.stopRecording(); if (toast) toast (u8 ("গান শেষ — রেকর্ডিং সেভ হয়েছে")); }
            else if (engine.hasTakePlayback()) engine.pause();
        }
        updateTransportButtons();
    }
}

void StudioView::showSaveMenu()
{
    const auto take = engine.getLastTake();
    if (! take.isValid()) { if (toast) toast (u8 ("আগে একটি গান রেকর্ড করুন")); return; }

    juce::PopupMenu m;
    m.addSectionHeader (u8 ("শেষ রেকর্ডিং"));
    m.addItem (1, u8 ("গান ও কণ্ঠ মিক্স করে WAV সেভ করুন"));
    m.addItem (2, u8 ("রেকর্ডিং ফোল্ডার খুলুন"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (saveBtn).withMinimumWidth (saveBtn.getWidth()),
                     [this, take] (int r)
    {
        if (r == 2) { take.folder.revealToUser(); return; }
        if (r != 1) return;

        auto name = take.backing.existsAsFile() ? take.backing.getFileNameWithoutExtension() : juce::String ("TF Vocal");
        auto dest = take.folder.getChildFile (juce::File::createLegalFileName (name + " - TF Vocal Studio mix.wav"));
        exportProgress = 0.0f;
        saveBtn.setEnabled (false);
        juce::Component::SafePointer<StudioView> safe (this);
        engine.exportMix (take, dest,
            [safe] (float p) { if (safe != nullptr) { safe->exportProgress = p; safe->saveBtn.setText (juce::String::fromUTF8 ("সেভ হচ্ছে… ") + bn ((int) (p * 100)) + "%"); } },
            [safe] (bool ok, juce::String msg)
            {
                if (safe == nullptr) return;
                safe->exportProgress = -1.0f;
                safe->saveBtn.setText (juce::String::fromUTF8 ("সেভ / এক্সপোর্ট"));
                safe->updateTransportButtons();
                if (ok) { if (safe->toast) safe->toast (juce::String::fromUTF8 ("মিক্স সেভ হয়েছে")); juce::File (msg).revealToUser(); }
                else safe->showError (msg);
            });
    });
}

void StudioView::paint (juce::Graphics& g)
{
    paintCard (g, lyricsCard);
    paintCard (g, waveCard);
    paintCard (g, transportCard);

    // song header
    auto head = lyricsCard.reduced (30, 0).withTrimmedTop (20).withHeight (54);
    if (engine.hasSong())
    {
        g.setColour (col::ink);
        g.setFont (font (21.0f, Weight::semibold));
        g.drawText (engine.getSongFile().getFileNameWithoutExtension(), head.removeFromTop (30), juce::Justification::centredLeft, true);
        juce::String sub = lyrics.isEmpty() ? u8 ("লিরিক্স নেই") : (lyrics.timed ? u8 ("লিরিক্স: সময় মেলানো") : u8 ("লিরিক্স: শুধু লেখা"));
        sub << "  ·  " << bnTime (engine.getSongLength());
        if (engine.hasTakePlayback()) sub << "  ·  " << u8 ("শেষ রেকর্ডিং শোনানো হচ্ছে");
        g.setColour (col::muted);
        g.setFont (font (14.0f));
        g.drawText (sub, head, juce::Justification::centredLeft, true);
    }

    // time
    g.setColour (col::ink);
    g.setFont (font (23.0f, Weight::bold));
    auto t = bnTime (engine.getPosition());
    auto tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t);
    g.drawText (t, timeArea.withWidth ((int) tw + 4), juce::Justification::centredLeft, false);
    g.setColour (engine.isRecording() ? col::recSoft : col::dim);
    g.setFont (font (16.0f, Weight::medium));
    auto total = engine.isRecording() ? (u8 (" ● ") + bnTime (engine.getRecordedSeconds())) : (" / " + bnTime (engine.getSongLength()));
    g.drawText (total, timeArea.withTrimmedLeft ((int) tw + 4), juce::Justification::centredLeft, false);
}

void StudioView::resized()
{
    auto r = getLocalBounds().reduced (28, 20);
    auto rightW = juce::jlimit (340, 430, (int) (r.getWidth() * 0.3f));
    auto rightArea = r.removeFromRight (rightW);
    r.removeFromRight (20);

    rightViewport.setBounds (rightArea);
    right.setSize (rightArea.getWidth() - (right.getHeight() > rightArea.getHeight() ? 10 : 0), right.getHeight());
    right.updateLayout();

    transportCard = r.removeFromBottom (84);
    r.removeFromBottom (14);
    waveCard = r.removeFromBottom (100);
    r.removeFromBottom (14);
    lyricsCard = r;

    lyricsView.setBounds (lyricsCard.reduced (2).withTrimmedTop (engine.hasSong() ? 76 : 0));
    waveform.setBounds (waveCard);

    auto tr = transportCard.reduced (16, 13);
    timeArea = tr.removeFromLeft (220);
    saveBtn.setBounds (tr.removeFromRight (exportProgress >= 0 ? 190 : 176).withSizeKeepingCentre (exportProgress >= 0 ? 190 : 176, 46));

    const int groupW = 46 + 10 + 46 + 10 + 58 + 10 + 150 + (listenBtn.isVisible() ? 10 + 46 : 0);
    auto g = tr.withSizeKeepingCentre (juce::jmin (groupW, tr.getWidth()), 58);
    toStartBtn.setBounds (g.removeFromLeft (46).withSizeKeepingCentre (46, 46)); g.removeFromLeft (10);
    stopBtn.setBounds (g.removeFromLeft (46).withSizeKeepingCentre (46, 46));    g.removeFromLeft (10);
    playBtn.setBounds (g.removeFromLeft (58));                                    g.removeFromLeft (10);
    recBtn.setBounds (g.removeFromLeft (150));
    if (listenBtn.isVisible()) { g.removeFromLeft (10); listenBtn.setBounds (g.removeFromLeft (46).withSizeKeepingCentre (46, 46)); }
}
}
