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
LyricsView::LyricsView (AudioEngine& e, juce::PropertiesFile& p) : engine (e), props (p)
{
    setOpaque (false);
    fontSize = (float) props.getDoubleValue ("lyricsFont", 34.0);
    speed = (float) props.getDoubleValue ("tpSpeed", 1.0);
    addMedia.setIcon (icons::plus(), false, 18.0f);
    openLibrary.setIcon (icons::music(), false, 18.0f);
    writeLyrics.setIcon (icons::plus(), false, 18.0f);
    for (auto* b : { &addMedia, &openLibrary, &writeLyrics }) { b->setRadius (12.0f); addChildComponent (b); }
}

void LyricsView::setLyrics (Lyrics l)
{
    lyrics = std::move (l);
    scrollIdx = (float) juce::jmax (0, lyrics.indexAt (engine.getPosition()));
    manualOffset = 0.0f;
    layoutWidth = 0.0f;   // force re-layout
    repaint();
}

void LyricsView::setVideo (const juce::File& f)
{
    lastFrame = {};
    if (f.existsAsFile() && media::isVideo (f)) { video.open (f); mode = Mode::video; }
    else { video.close(); mode = Mode::lyrics; }
    repaint();
}

void LyricsView::changeFontSize (float d)
{
    fontSize = juce::jlimit (18.0f, 76.0f, fontSize + d);
    props.setValue ("lyricsFont", fontSize);
    layoutWidth = 0.0f;
    repaint();
}

float LyricsView::pixelsPerSecond() const
{
    const float h = layout.getHeight();
    const double len = juce::jmax (30.0, engine.getSongLength() * 0.92);
    return speed * juce::jmax (6.0f, (float) (h / len));
}

void LyricsView::changeSpeed (float d)
{
    const float pos = (float) engine.getPosition();
    const float scroll = manualOffset + pos * pixelsPerSecond();
    speed = juce::jlimit (0.25f, 3.0f, speed + d);
    props.setValue ("tpSpeed", speed);
    manualOffset = scroll - pos * pixelsPerSecond();   // keep the text where it is
    repaint();
}

void LyricsView::resized()
{
    auto c = getLocalBounds().getCentre();
    addMedia.setBounds (c.x - 236, c.y + 30, 240, 50);
    openLibrary.setBounds (c.x + 16, c.y + 30, 220, 50);
    writeLyrics.setBounds (juce::Rectangle<int> (280, 50).withCentre ({ c.x, c.y + 60 }));
    layoutWidth = 0.0f;
}

void LyricsView::tick()
{
    auto now = juce::Time::getMillisecondCounterHiRes();
    auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;

    const bool empty = ! engine.hasSong() && ! engine.isRecording();
    const bool needWrite = ! empty && engine.hasSong() && lyrics.isEmpty() && ! (mode == Mode::video && video.isOpen());
    if (addMedia.isVisible() != empty) { addMedia.setVisible (empty); openLibrary.setVisible (empty); repaint(); }
    if (writeLyrics.isVisible() != needWrite) { writeLyrics.setVisible (needWrite); repaint(); }
    if (empty || ! isShowing()) return;

    if (mode == Mode::video && video.isOpen())
    {
        auto f = video.frameAt (engine.getPosition(), engine.isPlaying());
        if (f.isValid() && ! (f == lastFrame)) { lastFrame = f; repaint(); }
        return;
    }

    if (lyrics.timed)
    {
        const float target = (float) juce::jmax (0, lyrics.indexAt (engine.getPosition()));
        const float before = scrollIdx;
        scrollIdx += (target - scrollIdx) * (1.0f - std::exp (-dt * 9.0f));
        if (std::abs (target - scrollIdx) < 0.0005f) scrollIdx = target;
        if (engine.isPlaying() || std::abs (before - scrollIdx) > 1e-5f) repaint();
    }
    else if (engine.isPlaying())
        repaint();
}

void LyricsView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (! isTeleprompter() || mode == Mode::video) return;
    manualOffset -= w.deltaY * 180.0f;
    repaint();
}

void LyricsView::mouseDown (const juce::MouseEvent&) { dragStartOffset = manualOffset; }

void LyricsView::mouseDrag (const juce::MouseEvent& e)
{
    if (! isTeleprompter() || mode == Mode::video) return;
    manualOffset = dragStartOffset - (float) e.getDistanceFromDragStartY();
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

void LyricsView::paintKaraoke (juce::Graphics& g, juce::Rectangle<float> b)
{
    const float cy = b.getCentreY();
    const int sungIdx = lyrics.indexAt (engine.getPosition());
    const int current = juce::jmax (0, sungIdx);
    const double progress = sungIdx >= 0 ? lyrics.progressAt (sungIdx, engine.getPosition(), engine.getSongLength()) : 0.0;
    const float bigSize = fontSize * 1.15f;
    const float smallSize = fontSize * 0.62f;
    const float spacing = juce::jmax (bigSize * 1.5f, b.getHeight() / 5.2f);

    const int first = (int) std::floor (scrollIdx) - 2;
    for (int k = juce::jmax (0, first); k <= juce::jmin (lyrics.lines.size() - 1, first + 6); ++k)
    {
        const float d = (float) k - scrollIdx;
        const float ad = std::abs (d);
        if (ad > 2.8f) continue;
        const float size = bigSize - juce::jmin (1.0f, ad) * (bigSize - smallSize);
        float alpha = ad < 1.0f ? 1.0f - 0.4f * ad : juce::jmax (0.0f, 0.6f - (ad - 1.0f) * 0.32f);
        if (d < 0) alpha *= 0.8f;
        const float y = cy + d * spacing + (d > 0 ? 1.0f : -1.0f) * (bigSize - smallSize) * 0.5f * juce::jmin (1.0f, ad);
        auto& line = lyrics.lines.getReference (k);
        drawLine (g, line.text.isEmpty() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xaa  \xe2\x99\xaa")) : line.text,
                  y, size, alpha, k == current ? progress : 0.0, k == current);
    }

    if (sungIdx < 0)
    {
        auto toFirst = lyrics.lines.getReference (0).time - engine.getPosition();
        if (toFirst > 0 && toFirst < 4.0 && engine.isPlaying())
        {
            g.setColour (col::accent);
            g.setFont (font (20.0f, Weight::semibold));
            g.drawText (bn ((int) std::ceil (toFirst)), b.withY (cy - 2.6f * spacing).withHeight (40), juce::Justification::centred);
        }
    }
}

void LyricsView::rebuildLayout()
{
    const float w = (float) getWidth() - 80.0f;
    if (std::abs (w - layoutWidth) < 0.5f && std::abs (fontSize - layoutFont) < 0.01f) return;
    juce::AttributedString as;
    as.setJustification (juce::Justification::centredTop);
    as.setLineSpacing (fontSize * 0.5f);
    as.setWordWrap (juce::AttributedString::WordWrap::byWord);
    juce::StringArray rows;
    for (auto& l : lyrics.lines) rows.add (l.text);
    as.append (rows.joinIntoString ("\n"), font (fontSize, Weight::semibold), col::ink);
    layout.createLayout (as, juce::jmax (100.0f, w));
    layoutWidth = w;
    layoutFont = fontSize;
}

void LyricsView::paintTeleprompter (juce::Graphics& g, juce::Rectangle<float> b)
{
    rebuildLayout();
    const float readY = b.getY() + b.getHeight() * 0.36f;
    // keep at least part of the text on screen however far it is dragged
    const float auto_ = (float) engine.getPosition() * pixelsPerSecond();
    manualOffset = juce::jlimit (-b.getHeight() * 0.3f - auto_, layout.getHeight() - fontSize - auto_, manualOffset);
    const float scroll = manualOffset + auto_;

    // reading band
    g.setColour (col::accent.withAlpha (0.07f));
    g.fillRoundedRectangle (b.getX() + 16.0f, readY - fontSize * 0.25f, b.getWidth() - 32.0f, fontSize * 1.55f, 12.0f);
    juce::Path marker;
    marker.addTriangle (b.getX() + 22.0f, readY + fontSize * 0.2f, b.getX() + 22.0f, readY + fontSize * 1.0f, b.getX() + 32.0f, readY + fontSize * 0.6f);
    g.setColour (col::accent);
    g.fillPath (marker);

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (b.toNearestInt());
        layout.draw (g, juce::Rectangle<float> (b.getX() + 40.0f, readY - scroll, layoutWidth, layout.getHeight()));
    }

    // soft fades at the top and bottom
    auto fade = [&g, &b] (float y0, float y1, bool down)
    {
        juce::ColourGradient gr (col::panel.withAlpha (down ? 1.0f : 0.0f), 0, y0, col::panel.withAlpha (down ? 0.0f : 1.0f), 0, y1, false);
        g.setGradientFill (gr);
        g.fillRect (juce::Rectangle<float> (b.getX(), y0, b.getWidth(), y1 - y0));
    };
    fade (b.getY(), b.getY() + 60.0f, true);
    fade (b.getBottom() - 70.0f, b.getBottom(), false);

    if (! engine.isPlaying())
    {
        g.setColour (col::dim);
        g.setFont (font (13.0f));
        g.drawText (u8 ("মাউস চাকা বা টেনে উপর-নিচ করুন  ·  গান চললে নিজে থেকে স্ক্রল হবে"), b.removeFromBottom (26), juce::Justification::centred);
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
        g.drawText (u8 ("গান বা ভিডিও দিয়ে গাওয়া শুরু করুন"), b.withY (b.getCentreY() - 56).withHeight (44), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (16.0f));
        g.drawText (u8 ("যেকোনো অডিও বা লিরিক্স-ভিডিও দিন — ফাইল এখানে টেনে এনেও ছাড়তে পারেন"), b.withY (b.getCentreY() - 14).withHeight (28), juce::Justification::centred);
        return;
    }

    if (mode == Mode::video && video.isOpen())
    {
        auto area = b.reduced (12.0f);
        if (lastFrame.isValid())
        {
            auto ar = (float) lastFrame.getWidth() / (float) lastFrame.getHeight();
            auto r = area.getAspectRatio() > ar ? area.withSizeKeepingCentre (area.getHeight() * ar, area.getHeight())
                                                : area.withSizeKeepingCentre (area.getWidth(), area.getWidth() / ar);
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip; clip.addRoundedRectangle (r, 12.0f);
            g.reduceClipRegion (clip);
            g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
            g.drawImage (lastFrame, r);
        }
        else
        {
            g.setColour (col::muted);
            g.setFont (font (16.0f));
            g.drawText (u8 ("ভিডিও লোড হচ্ছে…"), area, juce::Justification::centred);
        }
        return;
    }

    if (lyrics.isEmpty())
    {
        g.setColour (col::ink);
        g.setFont (font (24.0f, Weight::semibold));
        g.drawText (engine.hasSong() ? u8 ("এই গানের লিরিক্স যোগ করা হয়নি") : u8 ("মুক্ত রেকর্ডিং চলছে"),
                    b.withY (b.getCentreY() - 52).withHeight (40), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (16.0f));
        g.drawText (engine.hasSong() ? u8 ("লিরিক্স লিখে বা পেস্ট করে দিন — গান চলার সাথে স্ক্রল হবে")
                                     : u8 ("গান ছাড়াই আপনার কণ্ঠ রেকর্ড হচ্ছে"),
                    b.withY (b.getCentreY() - 10).withHeight (28), juce::Justification::centred);
        return;
    }

    if (lyrics.timed) paintKaraoke (g, b);
    else paintTeleprompter (g, b);
}

//==============================================================================
// LyricsEditor
//==============================================================================
LyricsEditor::LyricsEditor()
{
    editor.setMultiLine (true, true);
    editor.setReturnKeyStartsNewLine (true);
    editor.setScrollbarsShown (true);
    editor.setFont (font (19.0f));
    editor.setIndents (16, 14);
    editor.setTextToShowWhenEmpty (juce::String::fromUTF8 ("এখানে লিরিক্স লিখুন বা পেস্ট করুন (Ctrl+V)…"), col::dim);
    editor.setTitle (juce::String::fromUTF8 ("লিরিক্স"));
    addAndMakeVisible (editor);
    for (auto* b : { &save, &cancel, &fromFile }) { b->setRadius (10.0f); addAndMakeVisible (b); }
    fromFile.setIcon (icons::folder(), false, 17.0f);
    save.onClick = [this] { if (onSave) onSave (editor.getText()); };
    cancel.onClick = [this] { if (onCancel) onCancel(); };
    fromFile.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("লিরিক্স ফাইল বাছুন"), juce::File(), "*.lrc;*.txt");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { auto f = fc.getResult(); if (f.existsAsFile()) editor.setText (Lyrics::readTextFile (f)); });
    };
}

void LyricsEditor::open (const juce::String& text)
{
    editor.setText (text, false);
    setVisible (true);
    editor.grabKeyboardFocus();
}

void LyricsEditor::paint (juce::Graphics& g)
{
    paintCard (g, getLocalBounds());
    auto r = getLocalBounds().reduced (24, 18);
    g.setColour (col::ink);
    g.setFont (font (20.0f, Weight::semibold));
    g.drawText (juce::String::fromUTF8 ("লিরিক্স লিখুন বা পেস্ট করুন"), r.removeFromTop (30), juce::Justification::centredLeft);
    g.setColour (col::muted);
    g.setFont (font (14.0f));
    g.drawText (juce::String::fromUTF8 ("সাধারণ লেখা দিলে গান চলার সাথে ধীরে ধীরে স্ক্রল হবে। সময়সহ .lrc লিরিক্স দিলে প্রতিটি লাইন ঠিক সময়ে সোনালি হবে।"),
                r.removeFromTop (24), juce::Justification::centredLeft, true);
}

void LyricsEditor::resized()
{
    auto r = getLocalBounds().reduced (24, 18);
    r.removeFromTop (62);
    auto bottom = r.removeFromBottom (48);
    save.setBounds (bottom.removeFromRight (140));
    bottom.removeFromRight (10);
    cancel.setBounds (bottom.removeFromRight (110));
    fromFile.setBounds (bottom.removeFromLeft (180));
    r.removeFromBottom (12);
    editor.setBounds (r);
}

//==============================================================================
// ProgressOverlay
//==============================================================================
ProgressOverlay::ProgressOverlay()
{
    cancel.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible (cancel);
}

void ProgressOverlay::show (const juce::String& t)
{
    title = t; detail = {}; progress = 0.0f;
    setVisible (true);
    startTimerHz (30);
    repaint();
}

void ProgressOverlay::setProgress (float p, const juce::String& d)
{
    progress = p;
    if (d.isNotEmpty()) detail = d;
    repaint();
}

void ProgressOverlay::resized()
{
    cancel.setBounds (juce::Rectangle<int> (120, 44).withCentre ({ getWidth() / 2, getHeight() / 2 + 80 }));
}

void ProgressOverlay::paint (juce::Graphics& g)
{
    g.setColour (col::panel.withAlpha (0.96f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 18.0f);
    auto c = getLocalBounds().toFloat().getCentre();
    g.setColour (col::ink);
    g.setFont (font (21.0f, Weight::semibold));
    g.drawText (title, juce::Rectangle<float> ((float) getWidth(), 34).withCentre ({ c.x, c.y - 52 }), juce::Justification::centred);
    auto bar = juce::Rectangle<float> (juce::jmin (460.0f, (float) getWidth() - 80.0f), 10.0f).withCentre ({ c.x, c.y - 10 });
    g.setColour (col::border);
    g.fillRoundedRectangle (bar, 5.0f);
    g.setColour (col::accent);
    if (progress > 0.001f)
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress)), 5.0f);
    else
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (bar.toNearestInt());
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * 0.3f).withX (bar.getX() + (bar.getWidth() * 1.3f) * phase - bar.getWidth() * 0.3f), 5.0f);
    }
    g.setColour (col::muted);
    g.setFont (font (15.0f));
    auto txt = detail + (progress > 0.001f ? "  " + bn ((int) (progress * 100)) + "%" : juce::String());
    g.drawText (txt, juce::Rectangle<float> ((float) getWidth(), 28).withCentre ({ c.x, c.y + 24 }), juce::Justification::centred);
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
    listenBtn.setTooltip (u8 ("শেষ রেকর্ডিং গানের সাথে, ইফেক্টসহ শুনুন"));
    playBtn.setRadius (16.0f);
    recBtn.setRadius (16.0f);
    recBtn.setFontSize (16.0f);
    for (auto* b : { &toStartBtn, &stopBtn, &playBtn, &recBtn, &listenBtn, &saveBtn })
        addAndMakeVisible (b);

    // lyrics toolbar
    for (auto* b : { &viewVideo, &viewLyrics }) { b->setFontSize (14.0f); addChildComponent (b); }
    editLyricsBtn.setIcon (icons::plus(), false, 16.0f);
    editLyricsBtn.setTooltip (u8 ("লিরিক্স লিখুন, পেস্ট করুন বা বদলান"));
    changeSong.setIcon (icons::folder(), false, 16.0f);
    changeSong.setTooltip (u8 ("অন্য অডিও বা ভিডিও দিন"));
    fontDown.setTooltip (u8 ("লেখা ছোট করুন"));
    fontUp.setTooltip (u8 ("লেখা বড় করুন"));
    slower.setTooltip (u8 ("স্ক্রল ধীরে"));
    faster.setTooltip (u8 ("স্ক্রল দ্রুত"));
    for (auto* b : { &editLyricsBtn, &fontDown, &fontUp, &slower, &faster, &changeSong })
    {
        b->setRadius (9.0f);
        b->setFontSize (14.0f);
        addChildComponent (b);
    }
    for (auto* b : { &fontDown, &fontUp, &slower, &faster }) b->setFontSize (18.0f);
    viewVideo.onClick = [this] { lyricsView.setMode (LyricsView::Mode::video); updateToolbar(); };
    viewLyrics.onClick = [this] { lyricsView.setMode (LyricsView::Mode::lyrics); updateToolbar(); };
    editLyricsBtn.onClick = [this] { openLyricsEditor(); };
    fontDown.onClick = [this] { lyricsView.changeFontSize (-3.0f); };
    fontUp.onClick = [this] { lyricsView.changeFontSize (3.0f); };
    slower.onClick = [this] { lyricsView.changeSpeed (-0.1f); repaint (speedArea); };
    faster.onClick = [this] { lyricsView.changeSpeed (0.1f); repaint (speedArea); };
    changeSong.onClick = [this] { chooseFile(); };

    toStartBtn.onClick = [this] { goToStart(); };
    stopBtn.onClick = [this] { engine.stop(); lyricsView.resetScroll(); };
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
    lyricsView.addMedia.onClick = [this] { chooseFile(); };
    lyricsView.writeLyrics.onClick = [this] { openLyricsEditor(); };

    lyricsEditor.onSave = [this] (const juce::String& t) { saveLyrics (t); };
    lyricsEditor.onCancel = [this] { lyricsEditor.setVisible (false); };
    addChildComponent (lyricsEditor);

    overlay.onCancel = [this] { importCancel = true; };
    addChildComponent (overlay);

    rightViewport.setViewedComponent (&right, false);
    rightViewport.setScrollBarsShown (true, false);
    rightViewport.setScrollBarThickness (8);
    addAndMakeVisible (rightViewport);
    right.toast = [this] (const juce::String& t) { if (toast) toast (t); };

    engine.addChangeListener (this);
    updateTransportButtons();
    updateToolbar();
    startTimerHz (30);
}

StudioView::~StudioView()
{
    importCancel = true;
    jobs.removeAllJobs (true, 10000);
    engine.removeChangeListener (this);
}

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
    if (importing.load()) return;
    juce::String err;
    if (engine.loadSong (f, err)) { songLoaded (f); return; }
    if (err == "needs-import") { importAndLoad (f, false); return; }
    showError (err);
}

void StudioView::importAndLoad (const juce::File& source, bool copyToLibrary)
{
    if (importing.exchange (true)) return;
    importCancel = false;
    overlay.show (media::isVideo (source) ? u8 ("ভিডিও তৈরি করা হচ্ছে") : u8 ("গান তৈরি করা হচ্ছে"));
    overlay.setProgress (0.0f, u8 ("ফাইল কপি করা হচ্ছে…"));
    resized();

    juce::Component::SafePointer<StudioView> safe (this);
    jobs.addJob ([this, safe, source, copyToLibrary]
    {
        juce::File target = source;
        juce::String err;
        auto setP = [safe] (float p, juce::String d) { juce::MessageManager::callAsync ([safe, p, d] { if (safe != nullptr) safe->overlay.setProgress (p, d); }); };

        // 1. keep a copy in the library so it is always there next time
        const auto songs = AudioEngine::songsFolder();
        if (copyToLibrary && ! source.isAChildOf (songs))
        {
            auto dest = songs.getChildFile (source.getFileName());
            if (! (dest.existsAsFile() && dest.getSize() == source.getSize()))
            {
                dest = dest.getNonexistentSibling();
                juce::FileInputStream in (source);
                juce::FileOutputStream out (dest);
                if (! in.openedOk() || ! out.openedOk()) err = u8 ("ফাইল কপি করা যায়নি।");
                else
                {
                    const auto total = juce::jmax ((juce::int64) 1, source.getSize());
                    juce::HeapBlock<char> buf (1 << 20);
                    juce::int64 done = 0;
                    while (! in.isExhausted() && ! importCancel.load())
                    {
                        const auto n = in.read (buf.get(), 1 << 20);
                        if (n <= 0) break;
                        out.write (buf.get(), (size_t) n);
                        done += n;
                        setP (0.3f * (float) done / (float) total, u8 ("ফাইল কপি করা হচ্ছে…"));
                    }
                    out.flush();
                    if (importCancel.load()) { err = u8 ("বাতিল করা হয়েছে।"); }
                }
                if (err.isNotEmpty()) dest.deleteFile();
            }
            if (err.isEmpty())
            {
                // bring lyrics with the same name along
                auto lf = Lyrics::findFor (source);
                if (lf.existsAsFile()) lf.copyFileTo (dest.withFileExtension (lf.getFileExtension()));
                target = dest;
            }
        }

        // 2. videos and unusual formats: extract the audio once (cached)
        if (err.isEmpty())
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            if (! media::canReadDirectly (target, fm))
            {
                auto cache = media::cacheFor (target);
                if (! cache.existsAsFile())
                    err = media::extractAudio (target, cache, 48000.0,
                                               [setP, copyToLibrary] (float pr) { setP ((copyToLibrary ? 0.3f : 0.0f) + (copyToLibrary ? 0.7f : 1.0f) * pr, u8 ("অডিও আলাদা করা হচ্ছে…")); },
                                               importCancel);
            }
        }

        juce::MessageManager::callAsync ([safe, target, err]
        {
            if (safe == nullptr) return;
            safe->importing = false;
            safe->overlay.setVisible (false);
            if (err.isNotEmpty()) { if (err != u8 ("বাতিল করা হয়েছে।")) safe->showError (err); return; }
            juce::String e2;
            if (safe->engine.loadSong (target, e2)) safe->songLoaded (target);
            else safe->showError (e2 == "needs-import" ? u8 ("এই ফাইলটি খোলা যাচ্ছে না।") : e2);
        });
    });
}

void StudioView::songLoaded (const juce::File& f)
{
    lyrics = Lyrics::loadFromFile (Lyrics::findFor (f));
    lyricsView.setLyrics (lyrics);
    lyricsView.setVideo (media::isVideo (f) ? f : juce::File());
    juce::AudioFormatManager& fm = engine.getFormatManager();
    waveform.setSong (AudioEngine::playableAudioFor (f, fm));
    props.setValue ("lastSong", f.getFullPathName());
    lyricsEditor.setVisible (false);
    updateToolbar();
    resized();
    repaint();
}

void StudioView::loadTake (const Take& t)
{
    auto before = engine.getSongFile();
    juce::String err;
    if (! engine.loadTakeForPlayback (t, err))
    {
        if (err == "needs-import" && t.backing.existsAsFile()) { importAndLoad (t.backing, false); return; }
        showError (err);
        return;
    }
    if (engine.getSongFile() != before) songLoaded (engine.getSongFile());
    repaint();
}

void StudioView::togglePlay()
{
    if (! engine.hasSong() && ! engine.isRecording()) { chooseFile(); return; }
    engine.togglePlay();
}

void StudioView::toggleRecord()
{
    if (importing.load()) return;
    if (engine.isRecording()) { engine.stopRecording(); if (toast) toast (u8 ("রেকর্ডিং সেভ হয়েছে")); return; }
    juce::String err;
    if (! engine.startRecording (err)) showError (err);
}

void StudioView::goToStart()
{
    if (! engine.isRecording()) { engine.setPosition (0.0); lyricsView.resetScroll(); }
}

void StudioView::chooseFile()
{
    if (importing.load()) return;
    chooser = std::make_unique<juce::FileChooser> (u8 ("গান, কারাওকে বা লিরিক্স-ভিডিও বাছুন"),
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   media::allPatterns());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f.existsAsFile()) importAndLoad (f, true);
                          });
}

void StudioView::openLyricsEditor()
{
    if (! engine.hasSong()) { if (toast) toast (u8 ("আগে একটি গান বা ভিডিও দিন")); return; }
    auto lf = Lyrics::findFor (engine.getSongFile());
    lyricsEditor.open (lf.existsAsFile() ? Lyrics::readTextFile (lf) : juce::String());
    resized();
}

void StudioView::saveLyrics (const juce::String& text)
{
    const auto song = engine.getSongFile();
    if (song == juce::File()) return;
    auto parsed = Lyrics::parse (text);
    auto target = song.withFileExtension (parsed.timed ? ".lrc" : ".txt");
    auto other = song.withFileExtension (parsed.timed ? ".txt" : ".lrc");
    if (text.trim().isEmpty())
    {
        target.moveToTrash();
        other.moveToTrash();
    }
    else
    {
        if (! target.replaceWithText (text, false, false, "\r\n")) { showError (u8 ("লিরিক্স সেভ করা যায়নি।")); return; }
        if (other.existsAsFile()) other.moveToTrash();
    }
    lyrics = parsed;
    lyricsView.setLyrics (lyrics);
    lyricsView.setMode (LyricsView::Mode::lyrics);
    lyricsEditor.setVisible (false);
    updateToolbar();
    resized();
    if (toast) toast (u8 ("লিরিক্স সেভ হয়েছে"));
}

bool StudioView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension (media::audioExtensions() + ";" + media::videoExtensions() + ";lrc;txt")) return true;
    return false;
}

void StudioView::filesDropped (const juce::StringArray& files, int, int)
{
    juce::File song, lyr;
    for (auto& s : files)
    {
        juce::File f (s);
        if (f.hasFileExtension ("lrc;txt")) lyr = f;
        else if (f.hasFileExtension (media::audioExtensions() + ";" + media::videoExtensions())) song = f;
    }
    if (song.existsAsFile()) { importAndLoad (song, true); return; }
    if (lyr.existsAsFile() && engine.hasSong()) saveLyrics (Lyrics::readTextFile (lyr));
}

void StudioView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateTransportButtons();
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
    changeSong.setEnabled (! engine.isRecording());
    resized();
}

void StudioView::updateToolbar()
{
    const bool song = engine.hasSong();
    const bool video = lyricsView.hasVideo();
    const bool showingLyrics = ! video || lyricsView.getMode() == LyricsView::Mode::lyrics;
    viewVideo.setVisible (song && video);
    viewLyrics.setVisible (song && video);
    viewVideo.setSelected (video && lyricsView.getMode() == LyricsView::Mode::video);
    viewLyrics.setSelected (! viewVideo.isSelected());
    editLyricsBtn.setVisible (song);
    editLyricsBtn.setText (lyricsView.hasLyrics() ? u8 ("লিরিক্স বদলান") : u8 ("লিরিক্স লিখুন"));
    fontDown.setVisible (song && showingLyrics && lyricsView.hasLyrics());
    fontUp.setVisible (fontDown.isVisible());
    slower.setVisible (song && showingLyrics && lyricsView.isTeleprompter());
    faster.setVisible (slower.isVisible());
    changeSong.setVisible (song);
    resized();
    repaint();
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
    m.addItem (1, u8 ("গান ও কণ্ঠ ইফেক্টসহ মিক্স করে WAV সেভ করুন"));
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

    if (engine.hasSong())
    {
        auto head = headArea;
        g.setColour (col::ink);
        g.setFont (font (21.0f, Weight::semibold));
        g.drawText (engine.getSongFile().getFileNameWithoutExtension(), head.removeFromTop (30), juce::Justification::centredLeft, true);
        juce::String sub = media::isVideo (engine.getSongFile()) ? u8 ("ভিডিও") : u8 ("অডিও");
        sub << "  ·  " << (lyrics.isEmpty() ? u8 ("লিরিক্স নেই") : (lyrics.timed ? u8 ("লিরিক্স: সময় মেলানো") : u8 ("লিরিক্স: লেখা")));
        sub << "  ·  " << bnTime (engine.getSongLength());
        if (engine.hasTakePlayback()) sub << "  ·  " << u8 ("শেষ রেকর্ডিং শোনানো হচ্ছে");
        g.setColour (col::muted);
        g.setFont (font (14.0f));
        g.drawText (sub, head, juce::Justification::centredLeft, true);
    }

    if (viewVideo.isVisible())
    {
        g.setColour (col::well);
        g.fillRoundedRectangle (viewWell.toFloat(), 10.0f);
        g.setColour (col::border);
        g.drawRoundedRectangle (viewWell.toFloat().reduced (0.5f), 10.0f, 1.0f);
    }
    if (slower.isVisible())
    {
        g.setColour (col::muted);
        g.setFont (font (13.5f));
        g.drawText (u8 ("গতি ") + bn (juce::String (lyricsView.getSpeed(), 1)) + "x", speedArea, juce::Justification::centred);
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
    auto rightW = juce::jlimit (350, 440, (int) (r.getWidth() * 0.3f));
    auto rightArea = r.removeFromRight (rightW);
    r.removeFromRight (20);

    rightViewport.setBounds (rightArea);
    right.setSize (rightArea.getWidth() - 10, right.getHeight());
    right.updateLayout();

    transportCard = r.removeFromBottom (84);
    r.removeFromBottom (14);
    waveCard = r.removeFromBottom (100);
    r.removeFromBottom (14);
    lyricsCard = r;

    // header row inside the lyrics card: title on the left, tools on the right
    auto head = lyricsCard.reduced (24, 0).withTrimmedTop (18).withHeight (54);
    auto tools = head;
    auto place = [&tools] (juce::Component& c, int w)
    {
        if (! c.isVisible()) return;
        c.setBounds (tools.removeFromRight (w).withSizeKeepingCentre (w, 38));
        tools.removeFromRight (8);
    };
    place (changeSong, 128);
    place (editLyricsBtn, 136);
    if (slower.isVisible())
    {
        place (faster, 38);
        speedArea = tools.removeFromRight (64).withSizeKeepingCentre (64, 38);
        place (slower, 38);
        tools.removeFromRight (4);
    }
    place (fontUp, 42);
    place (fontDown, 42);
    if (viewVideo.isVisible())
    {
        viewWell = tools.removeFromRight (180).withSizeKeepingCentre (180, 40);
        auto vw = viewWell.reduced (3);
        viewVideo.setBounds (vw.removeFromLeft (vw.getWidth() / 2));
        viewLyrics.setBounds (vw);
        tools.removeFromRight (8);
    }
    headArea = head.withRight (juce::jmax (head.getX() + 120, tools.getRight()));

    lyricsView.setBounds (lyricsCard.reduced (2).withTrimmedTop (engine.hasSong() ? 78 : 0));
    lyricsEditor.setBounds (lyricsCard);
    overlay.setBounds (lyricsCard);
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
