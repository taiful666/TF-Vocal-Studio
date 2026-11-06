#include "StudioView.h"
#include "VideoExport.h"

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
// OptionCard
//==============================================================================
OptionCard::OptionCard (const juce::String& t, const juce::String& s, juce::Path ic, bool p)
    : juce::Button (t), sub (s), icon (std::move (ic)), primary (p)
{
    setTitle (t);
    setTooltip (s);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
}

void OptionCard::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    if (down) r = r.reduced (1.5f);
    g.setColour (over ? col::raisedHi : col::raised);
    g.fillRoundedRectangle (r, 16.0f);
    g.setColour (primary ? col::accentEdge : (over ? col::track : col::border));
    g.drawRoundedRectangle (r, 16.0f, primary ? 1.5f : 1.0f);

    auto inner = r.reduced (18.0f, 16.0f);
    auto iconBox = inner.removeFromLeft (46.0f).withSizeKeepingCentre (46.0f, 46.0f);
    g.setColour (primary ? col::accentDeep : col::well);
    g.fillRoundedRectangle (iconBox, 12.0f);
    drawIcon (g, icon, iconBox.reduced (11.0f), primary ? col::accent : col::inkSoft, false, 1.9f);
    inner.removeFromLeft (14.0f);
    g.setColour (col::ink);
    g.setFont (font (16.5f, Weight::semibold));
    g.drawText (getButtonText(), inner.removeFromTop (inner.getHeight() * 0.5f), juce::Justification::bottomLeft, true);
    g.setColour (col::muted);
    g.setFont (font (13.0f));
    g.drawFittedText (sub, inner.toNearestInt().withTrimmedTop (2), juce::Justification::topLeft, 2, 1.0f);
    if (hasKeyboardFocus (false)) { g.setColour (col::accent); g.drawRoundedRectangle (r, 16.0f, 2.0f); }
}

//==============================================================================
// LyricsView (stage)
//==============================================================================
LyricsView::LyricsView (AudioEngine& e, Project& p, juce::PropertiesFile& pr)
    : addAudio (u8 ("অডিও / মিউজিক দিন"), u8 ("কারাওকে বা যেকোনো গান — মিউজিক ট্র্যাকে বসবে"), icons::music(), true),
      addVideo (u8 ("লিরিক্স ভিডিও দিন"), u8 ("ছবি এখানে চলবে, আওয়াজ মিউজিক ট্র্যাকে"), icons::play()),
      pasteLyrics (u8 ("লিরিক্স লিখুন / পেস্ট করুন"), u8 ("লেখা দিলে গান চলার সাথে নিজে থেকে স্ক্রল হবে"), icons::plus()),
      singFree (u8 ("মিউজিক ছাড়াই গান করুন"), u8 ("এখনই রেকর্ড শুরু — ২ সেকেন্ড হোক বা ১০ মিনিট"), icons::mic()),
      engine (e), project (p), props (pr)
{
    setOpaque (false);
    fontSize = (float) props.getDoubleValue ("lyricsFont", 34.0);
    speed = (float) props.getDoubleValue ("tpSpeed", 1.0);
    openLibrary.setIcon (icons::folder(), false, 16.0f);
    writeLyrics.setIcon (icons::plus(), false, 18.0f);
    backToProject.setIcon (icons::chevronRight(), false, 14.0f);
    newStart.setIcon (icons::plus(), false, 15.0f);
    for (auto* b : { &openLibrary, &writeLyrics, &backToProject, &newStart }) { b->setRadius (12.0f); addChildComponent (b); }
    for (auto* c : { &addAudio, &addVideo, &pasteLyrics, &singFree }) addChildComponent (c);
}

double LyricsView::songTime() const
{
    const auto pos = engine.getPosition();
    if (project.state().musicTrack() == nullptr) return pos;
    return project.state().musicTimeAt (pos);
}

void LyricsView::setLyrics (Lyrics l)
{
    lyrics = std::move (l);
    scrollIdx = (float) juce::jmax (0, lyrics.indexAt (songTime() - offset));
    manualOffset = 0.0f;
    layoutWidth = 0.0f;
    updateHome();
    repaint();
}

void LyricsView::setVideo (const juce::File& f)
{
    lastFrame = {};
    if (f.existsAsFile() && media::isVideo (f)) { video.open (f); mode = Mode::video; }
    else { video.close(); mode = Mode::lyrics; }
    updateHome();
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
    const double len = juce::jmax (60.0, engine.getLength() * 0.92);
    return speed * juce::jmax (6.0f, (float) (h / len));
}

void LyricsView::setForceHome (bool f)
{
    forceHome = f;
    updateHome();
    resized();
    repaint();
}

void LyricsView::setOffset (double seconds)
{
    offset = juce::jlimit (-30.0, 30.0, seconds);
    repaint();
}

void LyricsView::changeSpeed (float d)
{
    const float pos = (float) engine.getPosition();
    const float scroll = manualOffset + pos * pixelsPerSecond();
    speed = juce::jlimit (0.25f, 3.0f, speed + d);
    props.setValue ("tpSpeed", speed);
    manualOffset = scroll - pos * pixelsPerSecond();
    repaint();
}

void LyricsView::updateHome()
{
    const bool hasMusic = project.state().musicTrack() != nullptr;
    if (engine.isRecording()) forceHome = false;
    const bool h = forceHome || (! video.isOpen() && lyrics.isEmpty() && ! engine.isRecording() && ! hasMusic);
    const bool needWrite = ! h && hasMusic && lyrics.isEmpty() && ! (mode == Mode::video && video.isOpen());
    const bool back = h && forceHome && (hasMusic || ! lyrics.isEmpty() || project.state().length() > 0.0);
    if (h != home || writeLyrics.isVisible() != needWrite || backToProject.isVisible() != back)
    {
        home = h;
        for (auto* c : std::initializer_list<juce::Component*> { &addAudio, &addVideo, &pasteLyrics, &singFree, &openLibrary, &newStart }) c->setVisible (h);
        backToProject.setVisible (back);
        writeLyrics.setVisible (needWrite);
        resized();
        repaint();
    }
}

void LyricsView::resized()
{
    auto b = getLocalBounds().reduced (28, 20);
    const int gridW = juce::jmin (760, b.getWidth());
    const int cardH = 88, gap = 14;
    const int total = 74 + cardH * 2 + gap + 18 + 40;
    auto area = b.withSizeKeepingCentre (gridW, juce::jmin (b.getHeight(), total));
    homeTitle = area.removeFromTop (74);
    auto row1 = area.removeFromTop (cardH);
    area.removeFromTop (gap);
    auto row2 = area.removeFromTop (cardH);
    const int cw = (gridW - gap) / 2;
    addAudio.setBounds (row1.removeFromLeft (cw));
    addVideo.setBounds (row1.removeFromRight (cw));
    pasteLyrics.setBounds (row2.removeFromLeft (cw));
    singFree.setBounds (row2.removeFromRight (cw));
    area.removeFromTop (16);
    {
        auto row = area.removeFromTop (40);
        const int bw = 230, rowW = bw * (backToProject.isVisible() ? 3 : 2) + 12 * (backToProject.isVisible() ? 2 : 1);
        auto r = row.withSizeKeepingCentre (rowW, 40);
        if (backToProject.isVisible()) { backToProject.setBounds (r.removeFromLeft (bw)); r.removeFromLeft (12); }
        openLibrary.setBounds (r.removeFromLeft (bw));
        r.removeFromLeft (12);
        newStart.setBounds (r.removeFromLeft (bw));
    }

    auto c = getLocalBounds().getCentre();
    writeLyrics.setBounds (juce::Rectangle<int> (280, 50).withCentre ({ c.x, c.y + 50 }));
    layoutWidth = 0.0f;
}

void LyricsView::tick()
{
    auto now = juce::Time::getMillisecondCounterHiRes();
    auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;

    updateHome();
    if (home || ! isShowing()) return;

    const double st = songTime();
    if (mode == Mode::video && video.isOpen())
    {
        auto f = video.frameAt (juce::jmax (0.0, st), engine.isPlaying() && st >= 0.0);
        if (f.isValid() && ! (f == lastFrame)) { lastFrame = f; repaint(); }
        return;
    }

    if (lyrics.timed)
    {
        const float target = (float) juce::jmax (0, lyrics.indexAt (st - offset));
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

void LyricsView::mouseDown (const juce::MouseEvent&) { focusMainWindow (this); dragStartOffset = manualOffset; }

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
    const double st = songTime() - offset;
    const float cy = b.getCentreY();
    const int sungIdx = lyrics.indexAt (st);
    const int current = juce::jmax (0, sungIdx);
    double songLen = 0.0;
    if (auto* m = project.state().musicTrack()) for (auto& c : m->clips) songLen = juce::jmax (songLen, c.out * m->speed);
    const double progress = sungIdx >= 0 ? lyrics.progressAt (sungIdx, st, songLen) : 0.0;
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
        auto toFirst = lyrics.lines.getReference (0).time - st;
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
    const float auto_ = (float) (engine.getPosition() - offset) * pixelsPerSecond();
    manualOffset = juce::jlimit (-b.getHeight() * 0.3f - auto_, layout.getHeight() - fontSize - auto_, manualOffset);
    const float scroll = manualOffset + auto_;

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

    auto fade = [&g, &b] (float y0, float y1, bool down)
    {
        juce::ColourGradient gr (col::panel.withAlpha (down ? 1.0f : 0.0f), 0, y0, col::panel.withAlpha (down ? 0.0f : 1.0f), 0, y1, false);
        g.setGradientFill (gr);
        g.fillRect (juce::Rectangle<float> (b.getX(), y0, b.getWidth(), y1 - y0));
    };
    fade (b.getY(), b.getY() + 60.0f, true);
    fade (b.getBottom() - 70.0f, b.getBottom(), false);

}

void LyricsView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    if (home)
    {
        auto t = homeTitle.toFloat();
        g.setColour (col::ink);
        g.setFont (font (25.0f, Weight::semibold));
        const bool hasVoice = project.state().length() > 0.0 || project.state().musicTrack() != nullptr;
        g.drawText (hasVoice ? u8 ("এই প্রজেক্টে কী যোগ করবেন?") : u8 ("কী দিয়ে শুরু করবেন?"), t.removeFromTop (40), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (14.5f));
        g.drawText (u8 ("যেকোনো ফাইল এখানে টেনে এনেও ছাড়তে পারেন — অডিও, ভিডিও বা লিরিক্স (.txt / .lrc)"), t.removeFromTop (26), juce::Justification::centred);
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
        const bool music = project.state().musicTrack() != nullptr;
        g.setColour (col::ink);
        g.setFont (font (24.0f, Weight::semibold));
        g.drawText (music ? u8 ("এই গানের লিরিক্স যোগ করা হয়নি") : u8 ("মিউজিক ছাড়া রেকর্ডিং চলছে"),
                    b.withY (b.getCentreY() - 52).withHeight (40), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (16.0f));
        g.drawText (music ? u8 ("লিরিক্স লিখে বা পেস্ট করে দিন — গান চলার সাথে স্ক্রল হবে")
                          : u8 ("থামাতে রেকর্ড বোতাম বা Space চাপুন"),
                    b.withY (b.getCentreY() - 10).withHeight (28), juce::Justification::centred);
        return;
    }

    if (lyrics.timed && project.state().musicTrack() != nullptr) paintKaraoke (g, b);
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
    editor.setTextToShowWhenEmpty (u8 ("এখানে লিরিক্স লিখুন বা পেস্ট করুন (Ctrl+V)…"), col::dim);
    editor.setTitle (u8 ("লিরিক্স"));
    addAndMakeVisible (editor);
    for (auto* b : { &save, &cancel, &fromFile }) { b->setRadius (10.0f); addAndMakeVisible (b); }
    fromFile.setIcon (icons::folder(), false, 17.0f);
    save.onClick = [this] { if (onSave) onSave (editor.getText()); };
    cancel.onClick = [this] { if (onCancel) onCancel(); };
    fromFile.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (u8 ("লিরিক্স ফাইল বাছুন"), juce::File(), "*.lrc;*.txt");
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
    g.drawText (u8 ("লিরিক্স লিখুন বা পেস্ট করুন"), r.removeFromTop (30), juce::Justification::centredLeft);
    g.setColour (col::muted);
    g.setFont (font (14.0f));
    g.drawText (u8 ("সাধারণ লেখা দিলে গান চলার সাথে ধীরে ধীরে স্ক্রল হবে। সময়সহ .lrc লিরিক্স দিলে প্রতিটি লাইন ঠিক সময়ে সোনালি হবে।"),
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
// StudioView
//==============================================================================
StudioView::StudioView (AudioEngine& e, juce::PropertiesFile& p) : engine (e), props (p)
{
    addAndMakeVisible (stage);
    addAndMakeVisible (timeline);
    addAndMakeVisible (inspector);

    toStartBtn.setIcon (icons::toStart(), true, 17.0f);
    stopBtn.setIcon (icons::stop(), true, 15.0f);
    playBtn.setIcon (icons::play(), true, 22.0f);
    saveBtn.setIcon (icons::download(), false, 18.0f);
    for (auto* b : { &toStartBtn, &stopBtn, &playBtn }) b->getProperties().set ("iconOnly", true);
    toStartBtn.setTooltip (u8 ("শুরুতে যান (Home)"));
    stopBtn.setTooltip (u8 ("থামান"));
    playBtn.setTooltip (u8 ("চালান / বিরতি (Space) — মিউজিক আর সব কণ্ঠ একসাথে বাজবে"));
    recBtn.setTooltip (u8 ("বাছাই করা কণ্ঠ ট্র্যাকে রেকর্ড (R) — যেখানে দাগ আছে সেখান থেকে"));
    playBtn.setRadius (16.0f);
    recBtn.setRadius (16.0f);
    recBtn.setFontSize (16.0f);
    for (auto* b : { &toStartBtn, &stopBtn, &playBtn, &recBtn, &saveBtn }) addAndMakeVisible (b);
    addAndMakeVisible (inMeter);
    inMeter.setTitle (u8 ("মাইক লেভেল"));
    inMeter.setTooltip (u8 ("গাওয়ার সময় দাগ সবুজ অংশে থাকলে ঠিক আছে। \"কম\" দেখালে মাইকের কাছে এসে গাইন বা মাইকের জোর বাড়ান, \"বেশি জোরে\" দেখালে একটু দূরে সরুন বা জোর কমান।"));
    addChildComponent (pitchMeter);
    pitchMeter.setTooltip (u8 ("যে সুরে গাইছেন — সবুজ মানে ঠিক সুরে। পাশে সরগম (গানের স্কেল অনুযায়ী)"));
    addChildComponent (countIn);
    addChildComponent (tapView);
    tapView.onSave = [this] (const juce::String& lrc) { saveLyrics (lrc); if (toast) toast (u8 ("লিরিক্স এখন গানের সাথে সাথে চলবে")); };
    tapView.onClose = [this] { focusMainWindow (this); };

    // stage toolbar
    addBtn.setIcon (icons::plus(), false, 16.0f);
    addBtn.setTooltip (u8 ("অডিও, লিরিক্স ভিডিও বা লিরিক্স যোগ করুন"));
    for (auto* b : { &viewVideo, &viewLyrics }) { b->setFontSize (14.0f); addChildComponent (b); }
    lyricsBtn.setIcon (icons::gear(), false, 16.0f);
    lyricsBtn.setTooltip (u8 ("লিরিক্সের লেখার আকার, স্ক্রলের গতি, সময় মেলানো, লিরিক্স বদলানো"));
    noiseBtn.setIcon (icons::sparkle(), false, 16.0f);
    noiseBtn.setTooltip (u8 ("বাছাই করা কণ্ঠের রেকর্ডিং থেকে এক চাপে নয়েজ সরান — ফ্যান, এসি, হিস। আগের মতো চাইলে Ctrl+Z"));
    for (auto* b : { &addBtn, &lyricsBtn, &noiseBtn })
    {
        b->setRadius (9.0f);
        b->setFontSize (14.0f);
        addChildComponent (b);
    }
    noiseBtn.setVisible (true);
    camBtn.setIcon (icons::camera(), false, 16.0f);
    camBtn.setRadius (9.0f);
    camBtn.setFontSize (14.0f);
    camBtn.setTooltip (u8 ("চালু থাকলে রেকর্ডের সময় ক্যামেরায় আপনাদের ভিডিও হবে — ভিডিও সেভ করলে মুখ আর লিরিক্স একসাথে"));
    camBtn.onClick = [this] { setCameraEnabled (! cameraOn); };
    addChildComponent (camBtn);
    cameraOn = props.getBoolValue ("cameraOn", false) && props.getValue ("camera").isNotEmpty();
    lastCameraName = props.getValue ("camera");
    camBtn.setSelected (cameraOn);
    addChildComponent (camPreview);
    if (cameraOn) restartCamera ({});
    noiseBtn.setRadius (12.0f);
    lyricsBtn.onClick = [this] { showLyricsPanel(); };
    noiseBtn.onClick = [this] { inspector.cleanVoice (Inspector::CleanKind::noise, recordTrack()); };
    addBtn.onClick = [this] { showAddMenu(); };

    toStartBtn.onClick = [this] { goToStart(); };
    stopBtn.onClick = [this] { engine.stop(); stage.resetScroll(); };
    playBtn.onClick = [this] { togglePlay(); };
    recBtn.onClick = [this] { toggleRecord(); };
    saveBtn.onClick = [this] { showExportPanel(); };

    // home options
    stage.addAudio.onClick = [this] { stage.setForceHome (false); chooseFile (false); };
    stage.addVideo.onClick = [this] { stage.setForceHome (false); chooseFile (true); };
    stage.pasteLyrics.onClick = [this] { stage.setForceHome (false); openLyricsEditor(); };
    stage.singFree.onClick = [this] { stage.setForceHome (false); singWithoutMusic(); };
    stage.openLibrary.onClick = [this] { stage.setForceHome (false); if (onOpenLibrary) onOpenLibrary(); };
    stage.backToProject.onClick = [this] { stage.setForceHome (false); updateToolbar(); };
    stage.newStart.onClick = [this] { resetProject(); };
    stage.writeLyrics.onClick = [this] { openLyricsEditor(); };

    lyricsEditor.onSave = [this] (const juce::String& t) { saveLyrics (t); };
    lyricsEditor.onCancel = [this] { lyricsEditor.setVisible (false); focusMainWindow (this); };
    addChildComponent (lyricsEditor);

    overlay.onCancel = [this] { importCancel = true; };
    addChildComponent (overlay);

    // timeline <-> inspector <-> engine
    timeline.onTrackSelected = [this] (const juce::String& id)
    {
        inspector.showTrack (id);
        if (auto i = project.state().indexOf (id); i >= 0 && project.state().tracks.getReference (i).kind == TrackKind::vocal)
            engine.setArmedTrack (id);
    };
    timeline.onRedoPart = [this] (const juce::String& id, juce::Range<double> r) { startRedoPart (id, r); };
    timeline.onExportPart = [this] (juce::Range<double> r) { exportRange (r, true); };
    timeline.toast = [this] (const juce::String& t) { if (toast) toast (t); };
    timeline.presetList = [this] { return inspector.presetNames(); };
    timeline.onTrackAction = [this] (TimelineView::Action a, const juce::String& id, const juce::String& v)
    {
        timeline.selectTrack (id);
        switch (a)
        {
            case TimelineView::Action::cleanNoise:  inspector.cleanVoice (Inspector::CleanKind::noise, id); break;
            case TimelineView::Action::cleanBleed:  inspector.cleanVoice (Inspector::CleanKind::bleed, id); break;
            case TimelineView::Action::balance:     inspector.autoBalance(); break;
            case TimelineView::Action::preset:      inspector.choosePreset (v); break;
            case TimelineView::Action::speed:       inspector.applySpeed (v.getIntValue() / 100.0); break;
            case TimelineView::Action::key:         inspector.applyKey (v.getIntValue()); break;
            case TimelineView::Action::changeMusic: showAddMenu(); break;
            case TimelineView::Action::exportTrack:
            {
                auto st = project.state();
                for (auto& t : st.tracks) { t.solo = t.id == id; }
                const auto i = st.indexOf (id);
                const auto suffix = i >= 0 ? " (" + st.tracks.getReference (i).name + ")" : juce::String();
                ExportJob j; j.state = st; j.format = ExportFormat::mp3; j.suffix = suffix;
                startExports ({ j });
                break;
            }
        }
    };
    inspector.toast = [this] (const juce::String& t) { if (toast) toast (t); };
    inspector.onChangeMusic = [this] { showAddMenu(); };
    inspector.onNoiseChanged = [this] { updateNoiseButton(); };
    updateNoiseButton();

    project.addChangeListener (this);
    engine.addChangeListener (this);

    // reopen the last project (or start a fresh one)
    const juce::File last (props.getValue ("lastProject"));
    if (! (last.existsAsFile() && project.open (last)))
        project.newProject (u8 ("নতুন প্রজেক্ট"));
    projectChanged();
    timeline.zoomToFit();

    updateTransportButtons();
    updateToolbar();
    timelineHeight = props.getIntValue ("timelineH", -1);
    startTimerHz (30);
}

StudioView::~StudioView()
{
    cam.stop();
    importCancel = true;
    jobs.removeAllJobs (true, 10000);
    toast = nullptr;   // the owner may already be half destroyed
    if (engine.isRecording()) engine.stopRecording();
    consumeRecording();
    project.removeChangeListener (this);
    engine.removeChangeListener (this);
    project.saveNow();
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

//==============================================================================
void StudioView::projectChanged()
{
    engine.setProject (project.state());
    if (project.getFile().existsAsFile()) props.setValue ("lastProject", project.getFile().getFullPathName());

    if (auto* m = project.state().musicTrack()) engine.detectKeyOf (m->baseAudio);
    else engine.detectKeyOf ({});

    // keep a valid record track selected
    auto sel = timeline.getSelectedTrack();
    if (project.state().indexOf (sel) < 0)
    {
        juce::String pick;
        for (auto& t : project.state().tracks) if (t.kind == TrackKind::vocal) { pick = t.id; break; }
        if (pick.isEmpty() && ! project.state().tracks.isEmpty()) pick = project.state().tracks.getReference (0).id;
        timeline.selectTrack (pick);
    }
    reloadStage();
    updateTransportButtons();
    resized();
    repaint();
}

juce::File StudioView::lyricsFile() const
{
    // lyrics live next to the music file; without music, next to the project
    if (auto* m = project.state().musicTrack())
    {
        auto f = Lyrics::findFor (m->source);
        if (f.existsAsFile()) return f;
    }
    return Lyrics::findFor (project.getFile());
}

void StudioView::reloadStage()
{
    juce::File src;
    if (auto* m = project.state().musicTrack()) src = m->source;
    const auto lf = lyricsFile();
    const auto key = lf.getFullPathName() + "|" + juce::String (lf.getLastModificationTime().toMilliseconds());
    if (src != stageSource)
    {
        stageSource = src;
        stage.setVideo (media::isVideo (src) ? src : juce::File());
    }
    if (key != stageLyricsKey)
    {
        stageLyricsKey = key;
        lyrics = Lyrics::loadFromFile (lf);
        stage.setLyrics (lyrics);
        stage.setOffset (props.getDoubleValue ("lyrOffset|" + lf.getFullPathName(), 0.0));
    }
    updateToolbar();
}

void StudioView::openProject (const juce::File& f)
{
    if (engine.isRecording() || importing.load()) return;
    engine.stop();
    if (! project.open (f)) { showError (u8 ("প্রজেক্টটি খোলা যাচ্ছে না।")); return; }
    timeline.zoomToFit();
}

void StudioView::projectFileDeleted (const juce::File& f)
{
    if (project.getFile() != f) return;
    engine.stop();
    consumeRecording();
    project.discard();
    project.newProject (u8 ("নতুন প্রজেক্ট"));
    timeline.zoomToFit();
}

void StudioView::showHome()
{
    lyricsEditor.setVisible (false);
    stage.setForceHome (true);
    updateToolbar();
}

bool StudioView::projectHasContent() const
{
    return project.state().length() > 0.0 || project.state().musicTrack() != nullptr || ! lyrics.isEmpty();
}

void StudioView::resetProject()
{
    if (importing.load()) return;
    if (engine.isRecording()) stopAndKeep();
    if (! projectHasContent()) { newProject(); return; }
    juce::Component::SafePointer<StudioView> safe (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (u8 ("নতুন করে শুরু করবেন?"))
                                      .withMessage (u8 ("এখনকার কাজ \"") + project.state().name + u8 ("\" নামে লাইব্রেরির \"আমার প্রজেক্ট\"-এ সেভ থাকবে। যেকোনো সময় সেখান থেকে খুলতে পারবেন।"))
                                      .withButton (u8 ("নতুন শুরু করুন"), 1).withButton (u8 ("বাতিল"), 0)
                                      .withAssociatedComponent (this),
                                  [safe] (int r) { if (safe != nullptr && r == 1) safe->newProject(); });
}

void StudioView::newProject()
{
    if (engine.isRecording() || importing.load()) return;
    engine.stop();
    consumeRecording();
    if (projectHasContent()) project.saveNow();
    project.newProject (u8 ("নতুন প্রজেক্ট"));
    stage.setForceHome (false);
    lyricsEditor.setVisible (false);
    timeline.zoomToFit();
    if (toast) toast (u8 ("নতুন প্রজেক্ট — আগেরটা লাইব্রেরিতে সেভ আছে"));
}

void StudioView::openSong (const juce::File& f)
{
    if (engine.isRecording() || importing.load()) return;
    bool hasVoice = false;
    for (auto& t : project.state().tracks) if (t.kind == TrackKind::vocal && ! t.clips.isEmpty()) hasVoice = true;
    if (hasVoice)
    {
        engine.stop();
        project.newProject (media::displayName (f));
    }
    importAndAdd (f, false);
}

//==============================================================================
// Adding media
//==============================================================================
void StudioView::importAndAdd (const juce::File& source, bool copyToLibrary)
{
    if (engine.isRecording()) { if (toast) toast (u8 ("আগে রেকর্ডিং থামান")); return; }
    if (importing.exchange (true)) return;
    importCancel = false;
    overlay.show (media::isVideo (source) ? u8 ("ভিডিও তৈরি করা হচ্ছে") : u8 ("মিউজিক তৈরি করা হচ্ছে"));
    overlay.setProgress (0.0f, u8 ("ফাইল পড়া হচ্ছে…"));
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
                    if (importCancel.load()) err = u8 ("বাতিল করা হয়েছে।");
                }
                if (err.isNotEmpty()) dest.deleteFile();
            }
            if (err.isEmpty())
            {
                auto lf = Lyrics::findFor (source);
                if (lf.existsAsFile()) lf.copyFileTo (dest.withFileExtension (lf.getFileExtension()));
                target = dest;
            }
        }

        // 2. videos and unusual formats: extract the audio once (cached)
        juce::File audio;
        if (err.isEmpty())
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            if (media::canReadDirectly (target, fm)) audio = target;
            else
            {
                audio = media::cacheFor (target);
                if (! audio.existsAsFile())
                    err = media::extractAudio (target, audio, 48000.0,
                                               [setP, copyToLibrary] (float pr) { setP ((copyToLibrary ? 0.3f : 0.0f) + (copyToLibrary ? 0.7f : 1.0f) * pr, u8 ("অডিও আলাদা করা হচ্ছে…")); },
                                               importCancel);
            }
        }

        juce::MessageManager::callAsync ([safe, target, audio, err]
        {
            if (safe == nullptr) return;
            safe->importing = false;
            safe->overlay.setVisible (false);
            if (err.isNotEmpty()) { if (err != u8 ("বাতিল করা হয়েছে।")) safe->showError (err); return; }
            safe->musicAdded (target, audio);
        });
    });
}

void StudioView::musicAdded (const juce::File& source, const juce::File& audio)
{
    std::unique_ptr<juce::AudioFormatReader> r (engine.getFormatManager().createReaderFor (audio));
    if (r == nullptr || r->lengthInSamples <= 0) { showError (u8 ("এই ফাইলটি খোলা যাচ্ছে না।")); return; }
    const double dur = (double) r->lengthInSamples / r->sampleRate;
    r.reset();
    engine.stop();
    const auto id = project.addMusic (source, audio, dur);
    juce::ignoreUnused (id);
    props.setValue ("lastSong", source.getFullPathName());
    lyricsEditor.setVisible (false);
    timeline.zoomToFit();
    // select a voice track so pressing record just works
    for (auto& t : project.state().tracks) if (t.kind == TrackKind::vocal) { timeline.selectTrack (t.id); break; }
    if (toast) toast (media::isVideo (source) ? u8 ("ভিডিও যোগ হয়েছে — আওয়াজ মিউজিক ট্র্যাকে") : u8 ("মিউজিক যোগ হয়েছে"));
}

void StudioView::chooseFile (bool video)
{
    if (importing.load() || engine.isRecording()) return;
    chooser = std::make_unique<juce::FileChooser> (video ? u8 ("লিরিক্স ভিডিও বাছুন") : u8 ("গান, কারাওকে বা অডিও বাছুন"),
                                                   juce::File::getSpecialLocation (video ? juce::File::userMoviesDirectory : juce::File::userMusicDirectory),
                                                   media::allPatterns());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f.existsAsFile()) importAndAdd (f, true);
                          });
}

void StudioView::showAddMenu()
{
    if (engine.isRecording()) return;
    const bool hasMusic = project.state().musicTrack() != nullptr;
    juce::PopupMenu m;
    m.addItem (1, hasMusic ? u8 ("মিউজিক বদলান (অডিও)…") : u8 ("অডিও / মিউজিক যোগ করুন…"));
    m.addItem (2, u8 ("লিরিক্স ভিডিও যোগ করুন…"));
    m.addItem (3, lyrics.isEmpty() ? u8 ("লিরিক্স লিখুন / পেস্ট করুন") : u8 ("লিরিক্স বদলান"));
    m.addSeparator();
    m.addItem (4, u8 ("লাইব্রেরি থেকে গান বাছুন"));
    m.addItem (5, u8 ("নতুন কণ্ঠ ট্র্যাক"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addBtn.isShowing() ? (juce::Component*) &addBtn : (juce::Component*) this),
                     [this] (int r)
    {
        if (r == 1) chooseFile (false);
        if (r == 2) chooseFile (true);
        if (r == 3) openLyricsEditor();
        if (r == 4 && onOpenLibrary) onOpenLibrary();
        if (r == 5) timeline.selectTrack (project.addVocalTrack());
    });
}

//==============================================================================
// Lyrics
//==============================================================================
void StudioView::openLyricsEditor()
{
    auto lf = lyricsFile();
    lyricsEditor.open (lf.existsAsFile() ? Lyrics::readTextFile (lf) : juce::String());
    resized();
}

void StudioView::saveLyrics (const juce::String& text)
{
    juce::File base;
    if (auto* m = project.state().musicTrack()) base = m->source;
    else
    {
        if (! project.getFile().existsAsFile()) project.saveNow();
        base = project.getFile();
    }
    if (base == juce::File()) return;
    auto parsed = Lyrics::parse (text);
    auto target = base.withFileExtension (parsed.timed ? ".lrc" : ".txt");
    auto other = base.withFileExtension (parsed.timed ? ".txt" : ".lrc");
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
    stageLyricsKey = {};
    reloadStage();
    stage.setMode (LyricsView::Mode::lyrics);
    lyricsEditor.setVisible (false);
    focusMainWindow (this);
    updateToolbar();
    resized();
    if (toast) toast (text.trim().isEmpty() ? u8 ("লিরিক্স সরানো হয়েছে") : u8 ("লিরিক্স সেভ হয়েছে"));
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
    if (song.existsAsFile()) { importAndAdd (song, true); return; }
    if (lyr.existsAsFile()) saveLyrics (Lyrics::readTextFile (lyr));
}

//==============================================================================
// Transport and recording
//==============================================================================
void StudioView::cancelCameraRecording()
{
    if (! cam.isRecording()) return;
    cam.stop();
    pendingCamera.deleteFile();
    pendingCamera = juce::File();
    restartCamera ({});
}

void StudioView::togglePlay()
{
    if (countIn.isCounting()) { countIn.cancel(); cancelCameraRecording(); return; }
    if (engine.isRecording()) { engine.stopRecording(); return; }
    if (! engine.hasContent()) { if (toast) toast (u8 ("আগে মিউজিক দিন বা রেকর্ড করুন")); return; }
    engine.togglePlay();
}

juce::String StudioView::recordTrack()
{
    auto sel = timeline.getSelectedTrack();
    if (auto i = project.state().indexOf (sel); i >= 0 && project.state().tracks.getReference (i).kind == TrackKind::vocal) return sel;
    for (auto& t : project.state().tracks) if (t.kind == TrackKind::vocal) { timeline.selectTrack (t.id); return t.id; }
    auto id = project.addVocalTrack();
    engine.setProject (project.state());
    timeline.selectTrack (id);
    return id;
}

void StudioView::toggleRecord()
{
    if (importing.load()) return;
    if (engine.isRecording()) { engine.stopRecording(); return; }
    if (countIn.isCounting()) { countIn.cancel(); cancelCameraRecording(); return; }
    lyricsEditor.setVisible (false);
    if (props.getBoolValue ("countIn", true))
    {
        startCameraRecording();
        countIn.toFront (false);
        juce::Component::SafePointer<StudioView> safe (this);
        countIn.start ([safe] { if (safe != nullptr) safe->startRecordingNow(); });
        return;
    }
    startRecordingNow();
}

juce::File StudioView::cameraPreviewFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TF Vocal Studio").getChildFile ("camera preview.jpg");
}

void StudioView::restartCamera (const juce::File& recordTo)
{
    if (! cameraOn) { cam.stop(); camPreview.setActive (false); return; }
    const auto device = props.getValue ("camera");
    auto input = cameraInputOverride ? cameraInputOverride
                                     : std::function<juce::StringArray (bool)> ([device] (bool fixed) { return camera::dshowInput (device, fixed); });
    auto e = cam.start (input, cameraPreviewFile(), recordTo);
    if (e.isNotEmpty() && toast) toast (e);
    camPreview.setActive (! stage.showingHome());
    camPreview.toFront (false);
    resized();
}

void StudioView::setCameraEnabled (bool on)
{
    if (engine.isRecording() || countIn.isCounting()) return;
    if (on && props.getValue ("camera").isEmpty() && ! cameraInputOverride)
    {
        if (toast) toast (u8 ("আগে \"অডিও সেটিংস → ডিভাইস\" থেকে ক্যামেরা বাছুন (ফোন হলে DroidCam / Iriun দিয়ে USB-তে যুক্ত করুন)"));
        if (onOpenSettings) onOpenSettings();
        return;
    }
    cameraOn = on;
    props.setValue ("cameraOn", on);
    camBtn.setSelected (on);
    restartCamera ({});
    if (toast) toast (on ? u8 ("ক্যামেরা চালু — নিজেদের দেখতে পাবেন; রেকর্ড চাপলে ভিডিওও রেকর্ড হবে") : u8 ("ক্যামেরা বন্ধ"));
}

void StudioView::startCameraRecording()
{
    // the same camera process now also records the video (started early, during the count-in if any)
    if (! cameraOn || cam.isRecording()) return;
    pendingCamera = AudioEngine::recordingsFolder().getChildFile ("camera " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".ts").getNonexistentSibling();
    restartCamera (pendingCamera);
    if (! cam.isRecording()) pendingCamera = juce::File();
}

void StudioView::startRecordingNow()
{
    if (engine.isRecording() || importing.load()) return;
    const auto id = recordTrack();
    engine.setArmedTrack (id);
    startCameraRecording();
    juce::String err;
    if (! engine.startRecording (id, {}, err))
    {
        cam.stop();
        pendingCamera.deleteFile();
        pendingCamera = juce::File();
        restartCamera ({});
        showError (err);
    }
}

void StudioView::startRedoPart (const juce::String& trackId, juce::Range<double> range)
{
    if (engine.isRecording() || importing.load()) return;
    auto i = project.state().indexOf (trackId);
    if (i < 0 || project.state().tracks.getReference (i).kind != TrackKind::vocal)
    {
        if (toast) toast (u8 ("কণ্ঠের ট্র্যাকে অংশ বাছাই করুন — মিউজিক ট্র্যাকে রেকর্ড হয় না"));
        return;
    }
    if (engine.isLooping()) engine.setLoop ({}, false);
    timeline.selectTrack (trackId);
    engine.setArmedTrack (trackId);
    juce::String err;
    if (! engine.startRecording (trackId, range, err)) { showError (err); return; }
    if (toast) toast (u8 ("আগের ২ সেকেন্ড শুনে নিন — তারপর বাছাই করা অংশটা আবার গেয়ে ফেলুন"));
}

void StudioView::singWithoutMusic()
{
    if (engine.isRecording()) return;
    if (toast) toast (u8 ("রেকর্ড শুরু — থামাতে Space বা রেকর্ড বোতাম চাপুন"));
    toggleRecord();
}

void StudioView::goToStart()
{
    if (! engine.isRecording()) { engine.setPosition (0.0); stage.resetScroll(); }
}

void StudioView::toggleLoop()
{
    auto sel = timeline.getSelection();
    if (sel.isEmpty()) { if (toast) toast (u8 ("আগে টাইমলাইনে টেনে একটা অংশ বাছাই করুন")); return; }
    engine.setLoop (sel, ! engine.isLooping());
    if (engine.isLooping()) { engine.setPosition (sel.getStart()); if (! engine.isPlaying()) engine.play(); }
    timeline.repaint();
}

void StudioView::changeListenerCallback (juce::ChangeBroadcaster* src)
{
    if (src == &project) { projectChanged(); return; }

    consumeRecording();
    updateTransportButtons();
    repaint();
}

void StudioView::consumeRecording()
{
    // a finished recording goes onto its track
    auto res = engine.takeLastRecording();
    if (res.wav == juce::File()) return;
    juce::File camFile;
    double camStart = 0.0;
    if (cam.isRecording())
    {
        const double wall = cam.stop();
        if (res.ok && ! res.punch && pendingCamera.existsAsFile() && pendingCamera.getSize() > 10000)
        {
            // the singer is seen at wall-clock time W singing the music heard at W: the music reached
            // the speaker about half the round trip after the playhead, plus the camera's own delay
            const double audioStartWall = (double) res.startedAt.toMilliseconds() / 1000.0;
            const double firstSample = res.start - (res.in - (double) engine.getRoundTripLatencySamples() / juce::jmax (1.0, engine.getSampleRate()));
            camStart = firstSample + (wall - audioStartWall) - engine.getRoundTripLatencyMs() / 2000.0
                       - 0.06 + props.getDoubleValue ("camOffsetMs", 0.0) / 1000.0;
            camFile = pendingCamera;
        }
        else pendingCamera.deleteFile();
        pendingCamera = juce::File();
        restartCamera ({});   // back to the live picture
    }
    if (res.ok)
    {
        project.insertRecording (res.trackId, res.wav, res.start, res.in, res.out, res.punch, res.speaker, camFile, camStart);
        if (toast) toast (u8 ("রেকর্ড হয়েছে — চালান চাপলে মিউজিকের সাথে শুনবেন"));
    }
    else if (toast) toast (u8 ("রেকর্ডিং খুব ছোট ছিল, রাখা হয়নি"));
}

void StudioView::stopAndKeep()
{
    engine.stop();
    consumeRecording();
    project.saveNow();
}

void StudioView::updateNoiseButton()
{
    noiseBtn.setText (u8 ("নয়েজ সরান"));
    noiseBtn.setEnabled (! engine.isRecording() && ! inspector.isBusy());
}

//==============================================================================
/** Small panel for the lyrics display: size, scroll speed, timing, edit. */
class LyricsPanel : public juce::Component
{
public:
    struct StepRow : public juce::Component
    {
        StepRow (const juce::String& l, std::function<juce::String()> v, std::function<void (int)> step) : label (l), value (std::move (v)), onStep (std::move (step))
        {
            for (auto* b : { &minus, &plus }) { b->setRadius (9.0f); b->setFontSize (18.0f); addAndMakeVisible (b); }
            minus.onClick = [this] { onStep (-1); repaint(); };
            plus.onClick  = [this] { onStep (+1); repaint(); };
        }
        void resized() override
        {
            auto r = getLocalBounds();
            plus.setBounds (r.removeFromRight (38).withSizeKeepingCentre (38, 34));
            r.removeFromRight (6);
            valueArea = r.removeFromRight (84);
            r.removeFromRight (6);
            minus.setBounds (r.removeFromRight (38).withSizeKeepingCentre (38, 34));
        }
        void paint (juce::Graphics& g) override
        {
            g.setColour (col::inkSoft); g.setFont (font (14.5f));
            g.drawText (label, getLocalBounds().withRight (minus.getX() - 6), juce::Justification::centredLeft, true);
            g.setColour (col::ink); g.setFont (font (14.5f, Weight::semibold));
            g.drawText (value(), valueArea, juce::Justification::centred);
        }
        juce::String label;
        std::function<juce::String()> value;
        std::function<void (int)> onStep;
        TFButton minus { juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) }, plus { "+" };
        juce::Rectangle<int> valueArea;
    };

    LyricsPanel (LyricsView& v, std::function<void (double)> offsetChanged, std::function<void()> edit, std::function<void()> tap, bool canTap)
        : stage (v),
          size (u8 ("লেখার আকার"), [&v] { return bn ((int) std::round (v.getFontSize())); }, [&v] (int d) { v.changeFontSize (3.0f * (float) d); }),
          speed (u8 ("স্ক্রলের গতি"), [&v] { return bn (juce::String (v.getSpeed(), 1)) + "x"; }, [&v] (int d) { v.changeSpeed (0.1f * (float) d); }),
          sync (u8 ("সময় মেলানো"), [&v] { const double o = v.getOffset(); return (o > 0.001 ? "+" : "") + bn (juce::String (o, 2)) + u8 (" সে"); },
                [&v, offsetChanged] (int d) { v.setOffset (v.getOffset() + 0.25 * d); if (offsetChanged) offsetChanged (v.getOffset()); })
    {
        addAndMakeVisible (size);
        addAndMakeVisible (speed);
        addAndMakeVisible (sync);
        speed.setVisible (v.isTeleprompter());
        editBtn.setRadius (10.0f);
        editBtn.setIcon (icons::plus(), false, 16.0f);
        editBtn.onClick = [this, edit]
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            if (edit) juce::MessageManager::callAsync (edit);
        };
        addAndMakeVisible (editBtn);
        tapBtn.setRadius (10.0f);
        tapBtn.setIcon (icons::music(), false, 16.0f);
        tapBtn.setEnabled (canTap);
        tapBtn.setTooltip (canTap ? u8 ("গান বাজবে — প্রতিটা লাইন শুরু হওয়ার সময় একবার চাপুন (Enter)। এতে লিরিক্স গানের সাথে সাথে চলবে।")
                                  : u8 ("আগে মিউজিক আর লিরিক্স দুটোই দিন"));
        tapBtn.onClick = [this, tap]
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            if (tap) tap();
        };
        addAndMakeVisible (tapBtn);
        setSize (360, 20 + 46 * (speed.isVisible() ? 3 : 2) + 26 + 44 + 8 + 44 + 18);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (18, 14);
        size.setBounds (r.removeFromTop (46));
        if (speed.isVisible()) speed.setBounds (r.removeFromTop (46));
        sync.setBounds (r.removeFromTop (46));
        hint = r.removeFromTop (26);
        editBtn.setBounds (r.removeFromTop (44));
        r.removeFromTop (8);
        tapBtn.setBounds (r.removeFromTop (44));
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (col::muted); g.setFont (font (12.5f));
        g.drawText (u8 ("লেখা আগে এলে + চাপুন, পরে এলে − চাপুন"), hint, juce::Justification::centredLeft, true);
    }
    LyricsView& stage;
    StepRow size, speed, sync;
    TFButton editBtn { u8 ("লিরিক্স লিখুন / বদলান") }, tapBtn { u8 ("ট্যাপ করে সময় মেলান") };
    juce::Rectangle<int> hint;
};

void StudioView::startTapTiming()
{
    if (lyrics.isEmpty() || project.state().musicTrack() == nullptr || engine.isRecording()) return;
    juce::StringArray rows;
    for (auto& l : lyrics.lines) rows.add (l.text);
    lyricsEditor.setVisible (false);
    tapView.toFront (false);
    tapView.start (rows, [this] (double t) { const double s = project.state().musicTimeAt (t); return s >= 0.0 ? s : t; });
    tapView.grabKeyboardFocus();
    focusMainWindow (this);
}

void StudioView::showLyricsPanel()
{
    juce::Component::SafePointer<StudioView> safe (this);
    auto panel = std::make_unique<LyricsPanel> (stage,
        [safe] (double o) { if (safe != nullptr) safe->props.setValue ("lyrOffset|" + safe->lyricsFile().getFullPathName(), o); },
        [safe] { if (safe != nullptr) safe->openLyricsEditor(); },
        [safe] { if (safe != nullptr) safe->startTapTiming(); },
        ! lyrics.isEmpty() && project.state().musicTrack() != nullptr);
    juce::CallOutBox::launchAsynchronously (std::move (panel), getLocalArea (&lyricsBtn, lyricsBtn.getLocalBounds()), this);
}

void StudioView::updateTransportButtons()
{
    const bool playing = engine.isPlaying();
    const bool rec = engine.isRecording();
    playBtn.setIcon (playing && ! rec ? icons::pause() : icons::play(), true, 22.0f);
    playBtn.setTitle (playing ? u8 ("বিরতি") : u8 ("চালান"));
    recBtn.setSelected (rec);
    recBtn.setText (rec ? u8 ("থামান") : u8 ("রেকর্ড"));
    saveBtn.setEnabled (! rec && engine.hasContent() && exportProgress < 0.0f && ! engine.isExporting());
    toStartBtn.setEnabled (! rec);
    addBtn.setEnabled (! rec);
    camBtn.setEnabled (! rec);
}

void StudioView::updateToolbar()
{
    const bool home = stage.showingHome();
    const bool video = stage.hasVideo();
    addBtn.setVisible (! home);
    viewVideo.setVisible (! home && video);
    viewLyrics.setVisible (! home && video);
    viewVideo.setSelected (video && stage.getMode() == LyricsView::Mode::video);
    viewLyrics.setSelected (! viewVideo.isSelected());
    lyricsBtn.setVisible (! home);
    resized();
    repaint();
}

void StudioView::timerCallback()
{
    const float micPeak = engine.popInputPeak();
    inspector.updateMeters (micPeak);
    inMeter.setLevel (micPeak);
    juce::ignoreUnused (engine.popOutputPeak(), engine.popOutputPeakR());
    if (pitchMeter.isVisible()) pitchMeter.setPitch (engine.getLivePitchHz(), engine.getEffectiveKey());
    repaint (timeArea);

    // a "sing this part again" take ends by itself at the end of the part
    if (engine.isRecording() && engine.punchFinished()) engine.stopRecording();

    const bool playing = engine.isPlaying(), rec = engine.isRecording();
    if (playing != wasPlaying || rec != wasRecording)
    {
        wasPlaying = playing;
        wasRecording = rec;
        updateTransportButtons();
    }
    if (stage.showingHome() == addBtn.isVisible()) updateToolbar();

    // a camera chosen (or removed) in the settings switches the camera on (or off) right away
    if (++cameraPoll >= 15)
    {
        cameraPoll = 0;
        const auto name = props.getValue ("camera");
        if (name != lastCameraName && ! engine.isRecording() && ! countIn.isCounting())
        {
            lastCameraName = name;
            if (name.isNotEmpty()) { cameraOn = false; setCameraEnabled (true); }
            else if (cameraOn) setCameraEnabled (false);
        }
    }
}

//==============================================================================
// Export
//==============================================================================
//==============================================================================
/** The save window: what to save, how it should sound/look, then where (a normal "Save as" window). */
class ExportPanel : public juce::Component
{
public:
    struct Format { int id; juce::String title, sub; };
    ExportPanel (bool hasMusic, bool hasSelection, bool hasCamera, juce::PropertiesFile& p)
        : props (p),
          fadeIn (u8 ("শুরুতে ধীরে আসা (ফেড-ইন)"), 0, 5, p.getDoubleValue ("exFadeIn", 0.0), [] (double v) { return v < 0.05 ? u8 ("নেই") : bn (juce::String (v, 1)) + u8 (" সে."); }),
          fadeOut (u8 ("শেষে ধীরে মিলিয়ে যাওয়া (ফেড-আউট)"), 0, 8, p.getDoubleValue ("exFadeOut", 2.0), [] (double v) { return v < 0.05 ? u8 ("নেই") : bn (juce::String (v, 1)) + u8 (" সে."); })
    {
        formats = { { 1, u8 ("ভিডিও MP4 — আড়াআড়ি"), u8 ("YouTube, Facebook (১৬:৯)") },
                    { 9, u8 ("ভিডিও MP4 — খাড়া"), u8 ("Reels, Shorts, TikTok (৯:১৬)") },
                    { 2, u8 ("পুরো গান MP3"), u8 ("মিউজিক + কণ্ঠ") },
                    { 3, u8 ("শুধু কণ্ঠ MP3"), u8 ("ইফেক্টসহ") },
                    { 4, u8 ("শুধু মিউজিক MP3"), {} },
                    { 5, u8 ("কণ্ঠ আর মিউজিক আলাদা"), u8 ("দুটি MP3 ফাইল") },
                    { 6, u8 ("পুরো গান WAV"), u8 ("সবচেয়ে ভালো মান") } };
        for (auto& f : formats)
        {
            auto* b = buttons.add (new TFButton (f.title, TFButton::Style::segment));
            b->setFontSize (14.0f);
            b->setTooltip (f.sub);
            b->setEnabled (hasMusic || (f.id != 4 && f.id != 5));
            b->onClick = [this, id = f.id] { choose (id); };
            addAndMakeVisible (b);
        }
        for (auto* s : { &fadeIn, &fadeOut }) { s->slider.setRange (s == &fadeIn ? 0.0 : 0.0, s == &fadeIn ? 5.0 : 8.0, 0.5); addAndMakeVisible (s); }
        loud.sw.setToggleState (p.getBoolValue ("exLoud", true), juce::dontSendNotification);
        cam.sw.setToggleState (hasCamera && p.getBoolValue ("exCamera", true), juce::dontSendNotification);
        cam.sw.setEnabled (hasCamera);
        if (! hasCamera) cam.setNote (u8 ("ক্যামেরা চালু করে রেকর্ড করলে এখানে আসবে"));
        part.sw.setEnabled (hasSelection);
        if (! hasSelection) part.setNote (u8 ("টাইমলাইনে টেনে অংশ বাছাই করলে চালু হবে"));
        for (auto* r : { &loud, &cam, &part }) addAndMakeVisible (r);
        save.setRadius (10.0f);
        save.onClick = [this] { if (onSave) onSave (chosen, settings(), part.sw.getToggleState()); };
        cancel.onClick = [this] { if (onCancel) onCancel(); };
        folder.setIcon (icons::folder(), false, 16.0f);
        folder.onClick = [this] { if (onFolder) onFolder(); };
        for (auto* b : { &save, &cancel, &folder }) addAndMakeVisible (b);
        choose (p.getIntValue ("exFormat", 1));
        setSize (640, 640);
    }

    ExportSettings settings() const
    {
        ExportSettings s;
        s.fadeIn = fadeIn.slider.getValue();
        s.fadeOut = fadeOut.slider.getValue();
        s.normalise = loud.sw.getToggleState();
        s.vertical = chosen == 9;
        s.camera = cam.sw.getToggleState();
        props.setValue ("exFadeIn", s.fadeIn);
        props.setValue ("exFadeOut", s.fadeOut);
        props.setValue ("exLoud", s.normalise);
        props.setValue ("exCamera", s.camera);
        props.setValue ("exFormat", chosen);
        return s;
    }

    void choose (int id)
    {
        bool found = false;
        for (size_t i = 0; i < formats.size(); ++i) if (formats[i].id == id && buttons[(int) i]->isEnabled()) found = true;
        chosen = found ? id : 2;
        for (size_t i = 0; i < formats.size(); ++i) buttons[(int) i]->setSelected (formats[i].id == chosen);
        const bool video = chosen == 1 || chosen == 9;
        cam.setVisible (video);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        auto r = getLocalBounds().reduced (24, 18);
        g.setColour (col::ink); g.setFont (font (19.0f, Weight::semibold));
        g.drawText (u8 ("কী সেভ করবেন?"), r.removeFromTop (30), juce::Justification::centredLeft);
        g.setColour (col::muted); g.setFont (font (13.5f));
        g.drawText (u8 ("ভলিউম, মিউট আর ইফেক্ট যেমন সেট করা, তেমনই থাকবে। সেভ চাপলে কোথায় রাখবেন বাছাই করবেন।"), r.removeFromTop (22), juce::Justification::centredLeft, true);
        for (size_t i = 0; i < formats.size(); ++i)
        {
            auto b = buttons[(int) i]->getBounds();
            auto card = b.withBottom (b.getBottom() + 18).toFloat().expanded (2.0f);
            g.setColour (col::panel);
            g.fillRoundedRectangle (card, 12.0f);
            g.setColour (formats[i].id == chosen ? col::accent.withAlpha (0.8f) : col::border);
            g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, formats[i].id == chosen ? 1.5f : 1.0f);
            g.setColour (col::dim); g.setFont (font (12.0f));
            g.drawText (formats[i].sub, b.withY (b.getBottom() + 1).withHeight (16), juce::Justification::centred, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24, 18);
        r.removeFromTop (60);
        const int cols = 2, w = (r.getWidth() - 12) / cols;
        for (int i = 0; i < buttons.size(); ++i)
        {
            const int row = i / cols, c = i % cols;
            buttons[i]->setBounds (r.getX() + c * (w + 12), r.getY() + row * 64, w, 42);
        }
        r.removeFromTop (((buttons.size() + 1) / cols) * 64 + 6);
        fadeIn.setBounds (r.removeFromTop (52));
        fadeOut.setBounds (r.removeFromTop (52));
        loud.setBounds (r.removeFromTop (44));
        part.setBounds (r.removeFromTop (44));
        cam.setBounds (r.removeFromTop (44));
        auto bottom = getLocalBounds().reduced (24, 18).removeFromBottom (44);
        save.setBounds (bottom.removeFromRight (170));
        bottom.removeFromRight (10);
        cancel.setBounds (bottom.removeFromRight (100));
        folder.setBounds (bottom.removeFromLeft (200));
    }

    std::function<void (int, ExportSettings, bool)> onSave;
    std::function<void()> onCancel, onFolder;
    juce::PropertiesFile& props;
    std::vector<Format> formats;
    juce::OwnedArray<TFButton> buttons;
    int chosen = 1;
    LabeledSlider fadeIn, fadeOut;
    SwitchRow loud { u8 ("সোশ্যাল মিডিয়ার জোর (−১৪ LUFS)"), u8 ("YouTube/Facebook-এর মতো সমান জোর, কোথাও ফাটবে না") },
              part { u8 ("শুধু বাছাই করা অংশ"), {} },
              cam  { u8 ("ক্যামেরার ভিডিও দেখান"), u8 ("আপনাদের মুখ, নিচে লিরিক্স") };
    TFButton save { u8 ("সেভ করুন…"), TFButton::Style::accent }, cancel { u8 ("বাতিল") }, folder { u8 ("সেভ করা ফাইলের ফোল্ডার") };
};

void StudioView::showExportPanel()
{
    if (! engine.hasContent()) { if (toast) toast (u8 ("আগে কিছু রেকর্ড করুন")); return; }
    if (engine.isExporting() || exportProgress >= 0.0f) return;
    const auto sel = timeline.getSelection();
    double cs = 0.0;
    const bool hasCam = cameraFor (project.state(), {}, cs) != juce::File();
    auto panel = std::make_unique<ExportPanel> (project.state().musicTrack() != nullptr, ! sel.isEmpty(), hasCam, props);
    auto* raw = panel.get();
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (panel.release());
    o.dialogTitle = u8 ("সেভ / এক্সপোর্ট");
    o.dialogBackgroundColour = col::bg;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    if (isShowing() && juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()) != nullptr)
        o.componentToCentreAround = this;
    auto* win = o.launchAsync();
    juce::Component::SafePointer<juce::DialogWindow> safeWin (win);
    juce::Component::SafePointer<StudioView> safe (this);
    raw->onCancel = [safeWin] { if (safeWin != nullptr) safeWin->exitModalState (0); if (safeWin != nullptr) delete safeWin.getComponent(); };
    raw->onFolder = [] { auto f = AudioEngine::appFolder().getChildFile ("Exports"); f.createDirectory(); f.revealToUser(); };
    raw->onSave = [safe, safeWin, sel] (int choice, ExportSettings st, bool partOnly)
    {
        if (safeWin != nullptr) delete safeWin.getComponent();
        if (safe == nullptr) return;
        auto& self = *safe;
        if (partOnly && choice == 2) choice = 7;
        const bool video = choice == 1 || choice == 9;
        const juce::String ext = video ? ".mp4" : choice == 6 ? ".wav" : ".mp3";
        const juce::String suffix = choice == 3 ? u8 (" (শুধু কণ্ঠ)") : choice == 4 ? u8 (" (শুধু মিউজিক)") : (partOnly ? u8 (" (অংশ)") : juce::String());
        auto lastDir = juce::File (self.props.getValue ("exportDir"));
        if (! lastDir.isDirectory()) { lastDir = AudioEngine::appFolder().getChildFile ("Exports"); lastDir.createDirectory(); }
        auto name = juce::File::createLegalFileName ((self.project.state().name.isNotEmpty() ? self.project.state().name : juce::String ("TF Vocal")) + suffix);
        self.chooser = std::make_unique<juce::FileChooser> (u8 ("কোথায় সেভ করবেন?"), lastDir.getChildFile (name + ext), "*" + ext);
        self.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                                   [safe, choice, st, sel, partOnly, ext] (const juce::FileChooser& fc)
        {
            if (safe == nullptr) return;
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (! f.hasFileExtension (ext)) f = f.withFileExtension (ext);
            safe->props.setValue ("exportDir", f.getParentDirectory().getFullPathName());
            safe->exportChoice (choice, partOnly ? sel : juce::Range<double>(), st, f);
        });
    };
}

juce::File StudioView::cameraFor (const ProjectState& st, juce::Range<double> range, double& cameraStart) const
{
    // the camera recording that covers most of the exported time
    juce::File best;
    double bestCover = 0.0;
    const double a = range.isEmpty() ? 0.0 : range.getStart(), b = range.isEmpty() ? st.length() : range.getEnd();
    for (auto& t : st.tracks)
        for (auto& c : t.clips)
        {
            if (c.camera == juce::File() || ! c.camera.existsAsFile()) continue;
            const double cover = juce::jmin (b, c.end()) - juce::jmax (a, c.start);
            if (cover > bestCover) { bestCover = cover; best = c.camera; cameraStart = c.cameraStart; }
        }
    return best;
}

void StudioView::exportChoice (int choice, juce::Range<double> sel, ExportSettings settings, juce::File dest)
{
    const auto& st = project.state();
    std::vector<ExportJob> jobsList;
    auto job = [&settings] (ProjectState s, juce::Range<double> r, ExportFormat f, const juce::String& suffix, juce::File d)
    {
        ExportJob j; j.state = std::move (s); j.range = r; j.format = f; j.suffix = suffix; j.dest = d; j.settings = settings;
        return j;
    };
    auto sibling = [&dest] (const juce::String& add) { return dest == juce::File() ? juce::File() : dest.getSiblingFile (dest.getFileNameWithoutExtension() + add + dest.getFileExtension()); };
    switch (choice)
    {
        case 1: settings.vertical = false; jobsList.push_back (job (st, sel, ExportFormat::mp4, {}, dest)); break;
        case 9: settings.vertical = true;  jobsList.push_back (job (st, sel, ExportFormat::mp4, u8 (" (খাড়া)"), dest)); break;
        case 2: jobsList.push_back (job (st, {}, ExportFormat::mp3, {}, dest)); break;
        case 3: jobsList.push_back (job (withOnly (st, TrackKind::vocal), sel, ExportFormat::mp3, u8 (" (শুধু কণ্ঠ)"), dest)); break;
        case 4: jobsList.push_back (job (withOnly (st, TrackKind::music), sel, ExportFormat::mp3, u8 (" (শুধু মিউজিক)"), dest)); break;
        case 5: jobsList.push_back (job (withOnly (st, TrackKind::vocal), sel, ExportFormat::mp3, u8 (" (শুধু কণ্ঠ)"), sibling (u8 (" (শুধু কণ্ঠ)"))));
                jobsList.push_back (job (withOnly (st, TrackKind::music), sel, ExportFormat::mp3, u8 (" (শুধু মিউজিক)"), sibling (u8 (" (শুধু মিউজিক)")))); break;
        case 6: jobsList.push_back (job (st, sel, ExportFormat::wav, {}, dest)); break;
        case 7: jobsList.push_back (job (st, sel, ExportFormat::mp3, u8 (" (অংশ)"), dest)); break;
        default: return;
    }
    startExports (std::move (jobsList));
}

void StudioView::exportRange (juce::Range<double> range, bool mp3)
{
    ExportJob j;
    j.state = project.state(); j.range = range; j.format = mp3 ? ExportFormat::mp3 : ExportFormat::wav;
    j.suffix = range.isEmpty() ? juce::String() : u8 (" (অংশ)");
    startExports ({ j });
}

void StudioView::startExports (std::vector<ExportJob> list)
{
    if (engine.isExporting() || exportProgress >= 0.0f || engine.isRecording() || list.empty()) return;
    exportQueue = std::move (list);
    exportedFiles.clear();
    exportErrors.clear();
    exportProgress = 0.0f;
    updateTransportButtons();
    resized();
    runNextExport();
}

void StudioView::exportFinished()
{
    exportProgress = -1.0f;
    saveBtn.setText (u8 ("সেভ / এক্সপোর্ট"));
    updateTransportButtons();
    resized();
    if (exportErrors.isEmpty())
    {
        if (toast) toast (exportedFiles.size() > 1 ? bn (exportedFiles.size()) + u8 ("টি ফাইল সেভ হয়েছে") : u8 ("সেভ হয়েছে — ফোল্ডার খোলা হলো"));
        if (revealAfterExport && ! exportedFiles.isEmpty()) juce::File (exportedFiles[exportedFiles.size() - 1]).revealToUser();
    }
    else showError (exportErrors.joinIntoString ("\n"));
    if (onExportsDone) onExportsDone();
}

void StudioView::runNextExport()
{
    if (exportQueue.empty()) { exportFinished(); return; }
    auto job = exportQueue.front();
    exportQueue.erase (exportQueue.begin());

    auto folder = AudioEngine::appFolder().getChildFile ("Exports");
    folder.createDirectory();
    auto name = juce::File::createLegalFileName ((project.state().name.isNotEmpty() ? project.state().name : juce::String ("TF Vocal")) + job.suffix);
    // the mix is rendered to a temporary WAV first (unless WAV is what was asked for)
    juce::File wav;
    if (job.format == ExportFormat::wav && job.dest != juce::File()) wav = job.dest;
    else if (job.dest != juce::File()) wav = job.dest.getSiblingFile (job.dest.getFileNameWithoutExtension() + " (tmp).wav").getNonexistentSibling();
    else wav = folder.getChildFile (name + " - " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".wav").getNonexistentSibling();
    const auto finalOut = job.dest != juce::File() ? job.dest : wav.withFileExtension (job.format == ExportFormat::mp4 ? ".mp4" : ".mp3");
    const auto label = job.format == ExportFormat::mp4 ? u8 ("ভিডিও") : u8 ("সেভ");
    juce::Component::SafePointer<StudioView> safe (this);

    // what the video needs, taken now (on the message thread)
    videoexport::Options vo;
    vo.state = job.state;
    vo.lyrics = lyrics;
    vo.lyricsOffset = stage.getOffset();
    vo.scrollSpeed = stage.getSpeed();
    vo.title = project.state().name;
    vo.rangeStart = job.range.isEmpty() ? 0.0 : job.range.getStart();
    vo.useSourceVideo = ! stage.hasVideo() || stage.getMode() == LyricsView::Mode::video;
    if (job.settings.vertical) { vo.width = 720; vo.height = 1280; }
    if (job.settings.camera) vo.camera = cameraFor (job.state, job.range, vo.cameraStart);
    ExportOptions eo;
    eo.fadeIn = job.settings.fadeIn;
    eo.fadeOut = job.settings.fadeOut;
    eo.normalise = job.settings.normalise;

    engine.exportMix (job.state, wav, job.range,
        [safe, label] (float p) { if (safe != nullptr) { safe->exportProgress = p; safe->saveBtn.setText (label + u8 (" হচ্ছে… ") + bn ((int) (p * (label == u8 ("ভিডিও") ? 30 : 100))) + "%"); } },
        [safe, job, wav, vo, finalOut] (bool ok, juce::String msg) mutable
        {
            if (safe == nullptr) return;
            if (! ok) { safe->exportErrors.add (msg); safe->runNextExport(); return; }
            if (job.format == ExportFormat::wav) { safe->exportedFiles.add (wav.getFullPathName()); safe->runNextExport(); return; }

            safe->saveBtn.setText (job.format == ExportFormat::mp4 ? u8 ("ভিডিও তৈরি হচ্ছে…") : u8 ("MP3 তৈরি হচ্ছে…"));
            safe->jobs.addJob ([safe, job, wav, vo, finalOut]() mutable
            {
                juce::String err;
                juce::File out = finalOut;
                if (job.format == ExportFormat::mp3)
                {
                    err = media::encodeMp3 (wav, out);
                }
                else
                {
                    juce::AudioFormatManager fm; fm.registerBasicFormats();
                    if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (wav) }) vo.length = (double) r->lengthInSamples / r->sampleRate;
                    vo.mixWav = wav;
                    vo.out = out;
                    std::atomic<bool> never { false };
                    err = videoexport::render (vo, [safe] (float p)
                    {
                        juce::MessageManager::callAsync ([safe, p] { if (safe != nullptr) safe->saveBtn.setText (u8 ("ভিডিও তৈরি হচ্ছে… ") + bn (30 + (int) (p * 70)) + "%"); });
                    }, safe != nullptr ? safe->importCancel : never);
                }
                if (err.isEmpty()) wav.deleteFile();
                juce::MessageManager::callAsync ([safe, err, out, wav]
                {
                    if (safe == nullptr) return;
                    if (err.isEmpty()) safe->exportedFiles.add (out.getFullPathName());
                    else safe->exportErrors.add (err + u8 ("\nWAV ফাইলটি রাখা হয়েছে: ") + wav.getFullPathName());
                    safe->runNextExport();
                });
            });
        }, eo);
}

//==============================================================================
void StudioView::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (splitterArea.contains (e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void StudioView::mouseDown (const juce::MouseEvent& e)
{
    draggingSplitter = splitterArea.contains (e.getPosition());
    dragStartHeight = timeline.getHeight();
}

void StudioView::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingSplitter) return;
    timelineHeight = dragStartHeight - e.getDistanceFromDragStartY();
    resized();
    repaint();
}

void StudioView::mouseUp (const juce::MouseEvent&)
{
    if (draggingSplitter) props.setValue ("timelineH", timeline.getHeight());
    draggingSplitter = false;
}

void StudioView::mouseDoubleClick (const juce::MouseEvent& e)
{
    // double-click the divider: back to the automatic height
    if (splitterArea.contains (e.getPosition())) { timelineHeight = -1; props.removeValue ("timelineH"); resized(); repaint(); }
}

void StudioView::paint (juce::Graphics& g)
{
    paintCard (g, stageCard);
    paintCard (g, transportCard);
    {
        // the divider grip between the stage and the timeline
        auto grip = splitterArea.withSizeKeepingCentre (56, 4).toFloat();
        g.setColour (draggingSplitter ? col::accent : col::border.brighter (0.25f));
        g.fillRoundedRectangle (grip, 2.0f);
    }

    if (! stage.showingHome())
    {
        auto head = headArea;
        g.setColour (col::ink);
        g.setFont (font (20.0f, Weight::semibold));
        g.drawText (project.state().name, head.removeFromTop (30), juce::Justification::centredLeft, true);
        juce::String sub;
        if (auto* m = project.state().musicTrack())
        {
            sub << (media::isVideo (m->source) ? u8 ("ভিডিও") : u8 ("অডিও"));
            if (std::abs (m->speed - 1.0) > 0.001) sub << u8 (" · গতি ") << bn ((int) std::round (m->speed * 100)) << "%";
        }
        else sub << u8 ("মিউজিক ছাড়া");
        sub << "  ·  " << (lyrics.isEmpty() ? u8 ("লিরিক্স নেই") : (lyrics.timed ? u8 ("লিরিক্স: সময় মেলানো") : u8 ("লিরিক্স: লেখা")));
        if (engine.isLooping()) sub << "  ·  " << u8 ("লুপ চলছে");
        g.setColour (col::muted);
        g.setFont (font (13.5f));
        g.drawText (sub, head, juce::Justification::centredLeft, true);
    }

    if (viewVideo.isVisible())
    {
        g.setColour (col::well);
        g.fillRoundedRectangle (viewWell.toFloat(), 10.0f);
        g.setColour (col::border);
        g.drawRoundedRectangle (viewWell.toFloat().reduced (0.5f), 10.0f, 1.0f);
    }

    // time
    g.setColour (col::ink);
    g.setFont (font (23.0f, Weight::bold));
    auto t = bnTime (engine.getPosition());
    auto tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t);
    g.drawText (t, timeArea.withWidth ((int) tw + 4), juce::Justification::centredLeft, false);
    g.setColour (engine.isRecording() ? col::recSoft : col::dim);
    g.setFont (font (16.0f, Weight::medium));
    auto total = engine.isRecording() ? (u8 (" ● ") + bnTime (engine.getRecordedSeconds())) : (" / " + bnTime (engine.getLength()));
    g.drawText (total, timeArea.withTrimmedLeft ((int) tw + 4), juce::Justification::centredLeft, false);

}

void StudioView::resized()
{
    auto r = getLocalBounds().reduced (20, 16);
    auto rightW = juce::jlimit (340, 420, (int) (r.getWidth() * 0.27f));
    inspector.setBounds (r.removeFromRight (rightW));
    r.removeFromRight (16);

    transportCard = r.removeFromBottom (76);
    r.removeFromBottom (12);
    // the timeline height: as the tracks need, or as the user dragged the divider
    const int maxH = juce::jmax (180, r.getHeight() - 220);
    const int tlH = timelineHeight > 0 ? juce::jlimit (170, maxH, timelineHeight)
                                       : juce::jlimit (200, juce::jmax (200, (int) (r.getHeight() * 0.55f)), timeline.getIdealHeight());
    timeline.setBounds (r.removeFromBottom (tlH));
    splitterArea = r.removeFromBottom (12);
    stageCard = r;

    // header row inside the stage card: title on the left, tools on the right
    const bool home = stage.showingHome();
    auto head = stageCard.reduced (22, 0).withTrimmedTop (14).withHeight (50);
    auto tools = head;
    auto place = [&tools] (juce::Component& c, int w)
    {
        if (! c.isVisible()) return;
        c.setBounds (tools.removeFromRight (w).withSizeKeepingCentre (w, 38));
        tools.removeFromRight (8);
    };
    place (addBtn, 120);
    place (lyricsBtn, 120);
    camBtn.setVisible (! home);
    place (camBtn, 116);
    pitchMeter.setVisible (! home);
    place (pitchMeter, 230);
    if (viewVideo.isVisible())
    {
        viewWell = tools.removeFromRight (180).withSizeKeepingCentre (180, 40);
        auto vw = viewWell.reduced (3);
        viewVideo.setBounds (vw.removeFromLeft (vw.getWidth() / 2));
        viewLyrics.setBounds (vw);
        tools.removeFromRight (8);
    }
    headArea = head.withRight (juce::jmax (head.getX() + 120, tools.getRight()));

    stage.setBounds (stageCard.reduced (2).withTrimmedTop (home ? 0 : 66));
    lyricsEditor.setBounds (stageCard);
    overlay.setBounds (stageCard);
    countIn.setBounds (stageCard.reduced (2));
    {
        const int w = juce::jlimit (180, 280, (int) (stageCard.getWidth() * 0.22f)), h = w * 9 / 16;
        camPreview.setBounds (stageCard.getRight() - w - 16, stageCard.getBottom() - h - 16, w, h);
        const bool show = cameraOn && cam.isRunning() && ! home;
        if (show != camPreview.isVisible()) camPreview.setActive (show);
    }
    tapView.setBounds (stageCard.reduced (2));

    auto tr = transportCard.reduced (16, 10);
    timeArea = tr.removeFromLeft (180);
    saveBtn.setBounds (tr.removeFromRight (exportProgress >= 0 ? 190 : 170).withSizeKeepingCentre (exportProgress >= 0 ? 190 : 170, 44));
    tr.removeFromRight (14);
    noiseBtn.setBounds (tr.removeFromRight (128).withSizeKeepingCentre (128, 44));
    tr.removeFromRight (14);
    const int imW = juce::jlimit (150, 250, tr.getWidth() - 340);
    inMeter.setBounds (tr.removeFromRight (imW).withSizeKeepingCentre (imW, 36));
    tr.removeFromRight (10);

    const int groupW = 44 + 10 + 44 + 10 + 56 + 10 + 150;
    auto gr = tr.withSizeKeepingCentre (juce::jmin (groupW, tr.getWidth()), 56);
    toStartBtn.setBounds (gr.removeFromLeft (44).withSizeKeepingCentre (44, 44)); gr.removeFromLeft (10);
    stopBtn.setBounds (gr.removeFromLeft (44).withSizeKeepingCentre (44, 44));    gr.removeFromLeft (10);
    playBtn.setBounds (gr.removeFromLeft (56));                                    gr.removeFromLeft (10);
    recBtn.setBounds (gr.removeFromLeft (150));
}
}
