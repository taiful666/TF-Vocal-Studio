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
// OptionCard
//==============================================================================
OptionCard::OptionCard (const juce::String& t, const juce::String& s, juce::Path ic, bool p)
    : juce::Button (t), sub (s), icon (std::move (ic)), primary (p)
{
    setTitle (t);
    setTooltip (s);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (true);
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
    for (auto* b : { &openLibrary, &writeLyrics }) { b->setRadius (12.0f); addChildComponent (b); }
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
    scrollIdx = (float) juce::jmax (0, lyrics.indexAt (songTime()));
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
    const bool h = ! video.isOpen() && lyrics.isEmpty() && ! engine.isRecording() && ! hasMusic;
    const bool needWrite = hasMusic && lyrics.isEmpty() && ! (mode == Mode::video && video.isOpen());
    if (h != home || writeLyrics.isVisible() != needWrite)
    {
        home = h;
        for (auto* c : std::initializer_list<juce::Component*> { &addAudio, &addVideo, &pasteLyrics, &singFree, &openLibrary }) c->setVisible (h);
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
    openLibrary.setBounds (area.removeFromTop (40).withSizeKeepingCentre (240, 40));

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
        const float target = (float) juce::jmax (0, lyrics.indexAt (st));
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
    const double st = songTime();
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
    const float auto_ = (float) engine.getPosition() * pixelsPerSecond();
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

    if (! engine.isPlaying())
    {
        g.setColour (col::dim);
        g.setFont (font (13.0f));
        g.drawText (u8 ("মাউস চাকা বা টেনে উপর-নিচ করুন  ·  চালালে নিজে থেকে স্ক্রল হবে"), b.removeFromBottom (26), juce::Justification::centred);
    }
}

void LyricsView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    if (home)
    {
        auto t = homeTitle.toFloat();
        g.setColour (col::ink);
        g.setFont (font (25.0f, Weight::semibold));
        const bool hasVoice = project.state().length() > 0.0;
        g.drawText (hasVoice ? u8 ("আরও কিছু যোগ করবেন?") : u8 ("কী দিয়ে শুরু করবেন?"), t.removeFromTop (40), juce::Justification::centred);
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
    addAndMakeVisible (masterL);
    addAndMakeVisible (masterR);
    masterL.setTitle (u8 ("মাস্টার লেভেল বাম"));
    masterR.setTitle (u8 ("মাস্টার লেভেল ডান"));

    // stage toolbar
    addBtn.setIcon (icons::plus(), false, 16.0f);
    addBtn.setTooltip (u8 ("অডিও, লিরিক্স ভিডিও বা লিরিক্স যোগ করুন"));
    for (auto* b : { &viewVideo, &viewLyrics }) { b->setFontSize (14.0f); addChildComponent (b); }
    editLyricsBtn.setIcon (icons::plus(), false, 16.0f);
    editLyricsBtn.setTooltip (u8 ("লিরিক্স লিখুন, পেস্ট করুন বা বদলান"));
    fontDown.setTooltip (u8 ("লেখা ছোট করুন"));
    fontUp.setTooltip (u8 ("লেখা বড় করুন"));
    slower.setTooltip (u8 ("স্ক্রল ধীরে"));
    faster.setTooltip (u8 ("স্ক্রল দ্রুত"));
    for (auto* b : { &addBtn, &editLyricsBtn, &fontDown, &fontUp, &slower, &faster })
    {
        b->setRadius (9.0f);
        b->setFontSize (14.0f);
        addChildComponent (b);
    }
    for (auto* b : { &fontDown, &fontUp, &slower, &faster }) b->setFontSize (18.0f);
    viewVideo.onClick = [this] { stage.setMode (LyricsView::Mode::video); updateToolbar(); };
    viewLyrics.onClick = [this] { stage.setMode (LyricsView::Mode::lyrics); updateToolbar(); };
    editLyricsBtn.onClick = [this] { openLyricsEditor(); };
    fontDown.onClick = [this] { stage.changeFontSize (-3.0f); };
    fontUp.onClick = [this] { stage.changeFontSize (3.0f); };
    slower.onClick = [this] { stage.changeSpeed (-0.1f); repaint (speedArea); };
    faster.onClick = [this] { stage.changeSpeed (0.1f); repaint (speedArea); };
    addBtn.onClick = [this] { showAddMenu(); };

    toStartBtn.onClick = [this] { goToStart(); };
    stopBtn.onClick = [this] { engine.stop(); stage.resetScroll(); };
    playBtn.onClick = [this] { togglePlay(); };
    recBtn.onClick = [this] { toggleRecord(); };
    saveBtn.onClick = [this] { showSaveMenu(); };

    // home options
    stage.addAudio.onClick = [this] { chooseFile (false); };
    stage.addVideo.onClick = [this] { chooseFile (true); };
    stage.pasteLyrics.onClick = [this] { openLyricsEditor(); };
    stage.singFree.onClick = [this] { singWithoutMusic(); };
    stage.openLibrary.onClick = [this] { if (onOpenLibrary) onOpenLibrary(); };
    stage.writeLyrics.onClick = [this] { openLyricsEditor(); };

    lyricsEditor.onSave = [this] (const juce::String& t) { saveLyrics (t); };
    lyricsEditor.onCancel = [this] { lyricsEditor.setVisible (false); };
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
    timeline.onExportPart = [this] (juce::Range<double> r) { exportRange (r, false); };
    timeline.toast = [this] (const juce::String& t) { if (toast) toast (t); };
    inspector.toast = [this] (const juce::String& t) { if (toast) toast (t); };
    inspector.onChangeMusic = [this] { showAddMenu(); };

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
    startTimerHz (30);
}

StudioView::~StudioView()
{
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

void StudioView::newProject()
{
    if (engine.isRecording() || importing.load()) return;
    engine.stop();
    project.newProject (u8 ("নতুন প্রজেক্ট"));
    timeline.zoomToFit();
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
void StudioView::togglePlay()
{
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
    lyricsEditor.setVisible (false);
    const auto id = recordTrack();
    engine.setArmedTrack (id);
    juce::String err;
    if (! engine.startRecording (id, {}, err)) showError (err);
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
    if (res.ok)
    {
        project.insertRecording (res.trackId, res.wav, res.start, res.in, res.out);
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
}

void StudioView::updateToolbar()
{
    const bool home = stage.showingHome();
    const bool video = stage.hasVideo();
    const bool showingLyrics = ! video || stage.getMode() == LyricsView::Mode::lyrics;
    addBtn.setVisible (! home);
    viewVideo.setVisible (! home && video);
    viewLyrics.setVisible (! home && video);
    viewVideo.setSelected (video && stage.getMode() == LyricsView::Mode::video);
    viewLyrics.setSelected (! viewVideo.isSelected());
    editLyricsBtn.setVisible (! home);
    editLyricsBtn.setText (stage.hasLyrics() ? u8 ("লিরিক্স বদলান") : u8 ("লিরিক্স লিখুন"));
    fontDown.setVisible (! home && showingLyrics && stage.hasLyrics());
    fontUp.setVisible (fontDown.isVisible());
    slower.setVisible (! home && showingLyrics && stage.isTeleprompter());
    faster.setVisible (slower.isVisible());
    resized();
    repaint();
}

void StudioView::timerCallback()
{
    inspector.updateMeters();
    masterL.setLevel (engine.popOutputPeak());
    masterR.setLevel (engine.popOutputPeakR());
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
}

//==============================================================================
// Export
//==============================================================================
void StudioView::showSaveMenu()
{
    if (! engine.hasContent()) { if (toast) toast (u8 ("আগে কিছু রেকর্ড করুন")); return; }
    const auto sel = timeline.getSelection();
    juce::PopupMenu m;
    m.addSectionHeader (u8 ("মিউজিক + সব কণ্ঠ, ইফেক্টসহ"));
    m.addItem (1, u8 ("পুরোটা MP3 হিসেবে সেভ করুন (ফেসবুক/ইউটিউবের জন্য)"));
    m.addItem (2, u8 ("পুরোটা WAV হিসেবে সেভ করুন (সবচেয়ে ভালো মান)"));
    m.addItem (3, u8 ("শুধু বাছাই করা অংশ MP3"), ! sel.isEmpty());
    m.addSeparator();
    m.addItem (4, u8 ("সেভ করা ফাইলের ফোল্ডার খুলুন"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (saveBtn).withMinimumWidth (saveBtn.getWidth()),
                     [this, sel] (int r)
    {
        if (r == 1) exportRange ({}, true);
        if (r == 2) exportRange ({}, false);
        if (r == 3) exportRange (sel, true);
        if (r == 4) { auto f = AudioEngine::appFolder().getChildFile ("Exports"); f.createDirectory(); f.revealToUser(); }
    });
}

void StudioView::exportRange (juce::Range<double> range, bool mp3)
{
    if (engine.isExporting() || exportProgress >= 0.0f || engine.isRecording()) return;
    auto folder = AudioEngine::appFolder().getChildFile ("Exports");
    folder.createDirectory();
    auto name = juce::File::createLegalFileName (project.state().name.isNotEmpty() ? project.state().name : juce::String ("TF Vocal"));
    if (! range.isEmpty()) name << u8 (" (অংশ)");
    auto wav = folder.getChildFile (name + " - " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".wav").getNonexistentSibling();
    exportProgress = 0.0f;
    updateTransportButtons();
    juce::Component::SafePointer<StudioView> safe (this);
    engine.exportMix (project.state(), wav, range,
        [safe] (float p) { if (safe != nullptr) { safe->exportProgress = p; safe->saveBtn.setText (u8 ("সেভ হচ্ছে… ") + bn ((int) (p * 100)) + "%"); } },
        [safe, mp3, wav] (bool ok, juce::String msg)
        {
            if (safe == nullptr) return;
            auto finish = [safe] (bool ok2, const juce::String& m2)
            {
                if (safe == nullptr) return;
                safe->exportProgress = -1.0f;
                safe->saveBtn.setText (u8 ("সেভ / এক্সপোর্ট"));
                safe->updateTransportButtons();
                if (ok2) { if (safe->toast) safe->toast (u8 ("সেভ হয়েছে — ফোল্ডার খোলা হলো")); juce::File (m2).revealToUser(); }
                else safe->showError (m2);
            };
            if (! ok || ! mp3) { finish (ok, msg); return; }
            safe->saveBtn.setText (u8 ("MP3 তৈরি হচ্ছে…"));
            safe->jobs.addJob ([safe, wav, finish]
            {
                auto mp3File = wav.withFileExtension (".mp3");
                auto err = media::encodeMp3 (wav, mp3File);
                if (err.isEmpty()) wav.deleteFile();
                juce::MessageManager::callAsync ([finish, err, mp3File, wav] { if (err.isEmpty()) finish (true, mp3File.getFullPathName()); else finish (false, err + u8 ("\nWAV ফাইলটি রাখা হয়েছে: ") + wav.getFullPathName()); });
            });
        });
}

//==============================================================================
void StudioView::paint (juce::Graphics& g)
{
    paintCard (g, stageCard);
    paintCard (g, transportCard);

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
    if (slower.isVisible())
    {
        g.setColour (col::muted);
        g.setFont (font (13.5f));
        g.drawText (u8 ("স্ক্রল ") + bn (juce::String (stage.getSpeed(), 1)) + "x", speedArea, juce::Justification::centred);
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

    g.setColour (col::dim);
    g.setFont (font (11.5f));
    g.drawText ("L", meterArea.withHeight (meterArea.getHeight() / 2), juce::Justification::centredLeft);
    g.drawText ("R", meterArea.withTrimmedTop (meterArea.getHeight() / 2), juce::Justification::centredLeft);
}

void StudioView::resized()
{
    auto r = getLocalBounds().reduced (20, 16);
    auto rightW = juce::jlimit (340, 420, (int) (r.getWidth() * 0.27f));
    inspector.setBounds (r.removeFromRight (rightW));
    r.removeFromRight (16);

    transportCard = r.removeFromBottom (76);
    r.removeFromBottom (12);
    const int tlH = juce::jlimit (200, juce::jmax (200, (int) (r.getHeight() * 0.55f)), timeline.getIdealHeight());
    timeline.setBounds (r.removeFromBottom (tlH));
    r.removeFromBottom (12);
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
    place (editLyricsBtn, 136);
    if (slower.isVisible())
    {
        place (faster, 38);
        speedArea = tools.removeFromRight (70).withSizeKeepingCentre (70, 38);
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

    stage.setBounds (stageCard.reduced (2).withTrimmedTop (home ? 0 : 66));
    lyricsEditor.setBounds (stageCard);
    overlay.setBounds (stageCard);

    auto tr = transportCard.reduced (16, 10);
    timeArea = tr.removeFromLeft (200);
    saveBtn.setBounds (tr.removeFromRight (exportProgress >= 0 ? 190 : 176).withSizeKeepingCentre (exportProgress >= 0 ? 190 : 176, 44));
    tr.removeFromRight (16);
    meterArea = tr.removeFromRight (150).withSizeKeepingCentre (150, 26);
    masterL.setBounds (meterArea.withTrimmedLeft (14).withHeight (8));
    masterR.setBounds (meterArea.withTrimmedLeft (14).withTrimmedTop (18).withHeight (8));

    const int groupW = 44 + 10 + 44 + 10 + 56 + 10 + 150;
    auto gr = tr.withSizeKeepingCentre (juce::jmin (groupW, tr.getWidth()), 56);
    toStartBtn.setBounds (gr.removeFromLeft (44).withSizeKeepingCentre (44, 44)); gr.removeFromLeft (10);
    stopBtn.setBounds (gr.removeFromLeft (44).withSizeKeepingCentre (44, 44));    gr.removeFromLeft (10);
    playBtn.setBounds (gr.removeFromLeft (56));                                    gr.removeFromLeft (10);
    recBtn.setBounds (gr.removeFromLeft (150));
}
}
