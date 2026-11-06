#include "Theme.h"
#include "BinaryData.h"

namespace tf
{
//==============================================================================
namespace
{
    struct Typefaces
    {
        juce::Typeface::Ptr regular, medium, semibold, bold;

        Typefaces()
        {
            regular  = juce::Typeface::createSystemTypefaceFor (TFData::HindSiliguriRegular_ttf,  TFData::HindSiliguriRegular_ttfSize);
            medium   = juce::Typeface::createSystemTypefaceFor (TFData::HindSiliguriMedium_ttf,   TFData::HindSiliguriMedium_ttfSize);
            semibold = juce::Typeface::createSystemTypefaceFor (TFData::HindSiliguriSemiBold_ttf, TFData::HindSiliguriSemiBold_ttfSize);
            bold     = juce::Typeface::createSystemTypefaceFor (TFData::HindSiliguriBold_ttf,     TFData::HindSiliguriBold_ttfSize);
        }

        static Typefaces& get() { static Typefaces t; return t; }
    };
}

juce::Font font (float size, Weight w)
{
    auto& t = Typefaces::get();
    juce::Typeface::Ptr tf = t.regular;
    switch (w)
    {
        case Weight::medium:   tf = t.medium;   break;
        case Weight::semibold: tf = t.semibold; break;
        case Weight::bold:     tf = t.bold;     break;
        case Weight::regular:  break;
    }
    return juce::Font (juce::FontOptions (tf).withPointHeight (size));
}

juce::String bn (const juce::String& s)
{
    static const juce::juce_wchar digits[] = { 0x09E6, 0x09E7, 0x09E8, 0x09E9, 0x09EA, 0x09EB, 0x09EC, 0x09ED, 0x09EE, 0x09EF };
    juce::String out;
    out.preallocateBytes (s.getNumBytesAsUTF8() * 3);
    for (auto c : s)
        out += (c >= '0' && c <= '9') ? juce::String::charToString (digits[c - '0']) : juce::String::charToString (c);
    return out;
}

juce::String bn (int v) { return bn (juce::String (v)); }

juce::String bnTime (double seconds)
{
    auto t = juce::jmax (0, (int) std::floor (seconds));
    return bn (juce::String::formatted ("%02d:%02d", t / 60, t % 60));
}

//==============================================================================
namespace icons
{
    static juce::Path svg (const char* d) { return juce::Drawable::parseSVGPath (d); }

    juce::Path play()         { return svg ("M7 4.5v15l13-7.5z"); }
    juce::Path pause()        { juce::Path p; p.addRoundedRectangle (6.0f, 4.0f, 4.0f, 16.0f, 1.2f); p.addRoundedRectangle (14.0f, 4.0f, 4.0f, 16.0f, 1.2f); return p; }
    juce::Path stop()         { juce::Path p; p.addRoundedRectangle (4.0f, 4.0f, 16.0f, 16.0f, 2.5f); return p; }
    juce::Path toStart()      { juce::Path p = svg ("M20 5v14l-10-7z"); p.addRectangle (6.0f, 5.0f, 2.5f, 14.0f); return p; }
    juce::Path download()     { return svg ("M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4 M7 10l5 5 5-5 M12 15V3"); }
    juce::Path gear()         { return svg ("M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6z M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"); }
    juce::Path chevronRight() { return svg ("M9 6l6 6-6 6"); }
    juce::Path check()        { return svg ("M20 6L9 17l-5-5"); }
    juce::Path plus()         { return svg ("M12 5v14 M5 12h14"); }
    juce::Path folder()       { return svg ("M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"); }
    juce::Path headphones()   { return svg ("M3 18v-6a9 9 0 0 1 18 0v6 M21 19a2 2 0 0 1-2 2h-1a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2h3z M3 19a2 2 0 0 0 2 2h1a2 2 0 0 0 2-2v-3a2 2 0 0 0-2-2H3z"); }
    juce::Path music()        { return svg ("M9 18V5l12-2v13 M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0z M21 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0z"); }
    juce::Path search()       { return svg ("M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14z M20 20l-3.5-3.5"); }
    juce::Path sparkle()      { return svg ("M12 3l1.8 4.7L18.5 9l-4.7 1.8L12 15.5l-1.8-4.7L5.5 9l4.7-1.3z"); }
}

void drawIcon (juce::Graphics& g, const juce::Path& p24, juce::Rectangle<float> area, juce::Colour c, bool filled, float strokeWidth)
{
    auto scale = juce::jmin (area.getWidth(), area.getHeight()) / 24.0f;
    auto t = juce::AffineTransform::scale (scale).translated (area.getCentreX() - 12.0f * scale, area.getCentreY() - 12.0f * scale);
    g.setColour (c);
    if (filled)
        g.fillPath (p24, t);
    else
        g.strokePath (p24, juce::PathStrokeType (strokeWidth * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
}

//==============================================================================
void Smoother::setTarget (float t, bool instant)
{
    target = t;
    if (instant) { value = t; stopTimer(); if (onChange) onChange(); return; }
    if (! isTimerRunning()) { lastMs = juce::Time::getMillisecondCounterHiRes(); startTimerHz (60); }
}

void Smoother::timerCallback()
{
    auto now = juce::Time::getMillisecondCounterHiRes();
    auto dt = (float) juce::jlimit (0.0, 0.1, (now - lastMs) / 1000.0);
    lastMs = now;
    value += (target - value) * (1.0f - std::exp (-dt * 14.0f));   // ease-out
    if (std::abs (target - value) < 0.002f) { value = target; stopTimer(); }
    if (onChange) onChange();
}

//==============================================================================
TFLookAndFeel::TFLookAndFeel()
{
    setColourScheme ({ col::bg, col::panel, col::raised, col::track, col::ink, col::accent,
                       col::onAccent, col::raisedHi, col::ink });

    setColour (juce::ResizableWindow::backgroundColourId, col::bg);
    setColour (juce::DocumentWindow::textColourId, col::ink);
    setColour (juce::Label::textColourId, col::ink);
    setColour (juce::TextButton::buttonColourId, col::raised);
    setColour (juce::TextButton::textColourOffId, col::ink);
    setColour (juce::TextButton::textColourOnId, col::ink);
    setColour (juce::ComboBox::backgroundColourId, col::raised);
    setColour (juce::ComboBox::textColourId, col::ink);
    setColour (juce::ComboBox::outlineColourId, col::border);
    setColour (juce::ComboBox::arrowColourId, col::muted);
    setColour (juce::PopupMenu::backgroundColourId, col::raised);
    setColour (juce::PopupMenu::textColourId, col::ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::raisedHi);
    setColour (juce::PopupMenu::highlightedTextColourId, col::accentHi);
    setColour (juce::TextEditor::backgroundColourId, col::raised);
    setColour (juce::TextEditor::textColourId, col::ink);
    setColour (juce::TextEditor::outlineColourId, col::border);
    setColour (juce::TextEditor::focusedOutlineColourId, col::accent);
    setColour (juce::TextEditor::highlightColourId, col::accent.withAlpha (0.35f));
    setColour (juce::CaretComponent::caretColourId, col::accent);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::thumbColourId, juce::Colours::white);
    setColour (juce::Slider::trackColourId, col::accent);
    setColour (juce::Slider::backgroundColourId, col::border);
    setColour (juce::ToggleButton::textColourId, col::ink);
    setColour (juce::ToggleButton::tickColourId, col::accent);
    setColour (juce::ScrollBar::thumbColourId, col::track);
    setColour (juce::AlertWindow::backgroundColourId, col::panel);
    setColour (juce::AlertWindow::textColourId, col::ink);
    setColour (juce::AlertWindow::outlineColourId, col::border);
    setColour (juce::DialogWindow::backgroundColourId, col::panel);
    setColour (juce::TreeView::backgroundColourId, col::panel);
    setColour (juce::ListBox::textColourId, col::ink);
}

juce::Typeface::Ptr TFLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName()
        || f.getTypefaceName() == juce::Font::getDefaultSerifFontName())
        return font (f.getHeight(), f.isBold() ? Weight::semibold : Weight::regular).getTypefacePtr();
    return LookAndFeel_V4::getTypefaceForFont (f);
}

void TFLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                      juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
        return;
    }

    auto cy = (float) y + (float) h * 0.5f;
    auto left = (float) x, right = (float) (x + w);
    juce::Rectangle<float> trackR (left, cy - 3.0f, right - left, 6.0f);
    g.setColour (col::border);
    g.fillRoundedRectangle (trackR, 3.0f);
    g.setColour (s.isEnabled() ? col::accent : col::track);
    g.fillRoundedRectangle (trackR.withRight (pos), 3.0f);

    auto hover = s.isMouseOverOrDragging();
    auto r = hover ? 9.0f : 8.0f;
    juce::Rectangle<float> knob (pos - r, cy - r, r * 2.0f, r * 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (knob.translated (0.0f, 1.0f));
    g.setColour (s.isEnabled() ? juce::Colours::white : col::muted);
    g.fillEllipse (knob);
    if (s.hasKeyboardFocus (false))
    {
        g.setColour (col::accent);
        g.drawEllipse (knob.expanded (3.0f), 2.0f);
    }
}

void TFLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    juce::ColourGradient grad (over ? juce::Colour (0xff262B35) : juce::Colour (0xff20242D), 0, r.getY(),
                               over ? juce::Colour (0xff1E222A) : juce::Colour (0xff191C23), 0, r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (over ? juce::Colour (0xff3A404C) : juce::Colour (0xff2C313C));
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
    if (down) { g.setColour (juce::Colours::black.withAlpha (0.15f)); g.fillRoundedRectangle (r, 10.0f); }
}

juce::Font TFLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return font (15.0f, Weight::semibold); }

void TFLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (font (15.0f, Weight::semibold));
    g.setColour (b.isEnabled() ? col::ink : col::dim);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (8, 2), juce::Justification::centred, 1);
}

void TFLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    // checkbox style used by system dialogs (device selector)
    auto box = juce::Rectangle<float> (4.0f, ((float) b.getHeight() - 18.0f) * 0.5f, 18.0f, 18.0f);
    g.setColour (b.getToggleState() ? col::accent : (over ? col::raisedHi : col::raised));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (b.getToggleState() ? col::accent : col::track);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);
    if (b.getToggleState())
        drawIcon (g, icons::check(), box.reduced (2.0f), col::onAccent, false, 3.0f);
    g.setColour (b.isEnabled() ? col::ink : col::dim);
    g.setFont (font (15.0f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (30), juce::Justification::centredLeft, 2);
}

void TFLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (col::raised);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (box.hasKeyboardFocus (true) ? col::accent : col::border);
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
    juce::Path arrow = juce::Drawable::parseSVGPath ("M6 9l6 6 6-6");
    drawIcon (g, arrow, { (float) w - 30.0f, 0.0f, 18.0f, (float) h }, col::muted, false, 2.2f);
}

void TFLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 1, box.getWidth() - 40, box.getHeight() - 2);
    label.setFont (font (15.0f));
}

void TFLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (col::raised);
    g.setColour (col::border);
    g.drawRect (0, 0, w, h);
}

void TFLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                   int thumbStart, int thumbSize, bool over, bool down)
{
    juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float> ((float) x + 3.0f, (float) thumbStart, (float) w - 6.0f, (float) thumbSize)
                                            : juce::Rectangle<float> ((float) thumbStart, (float) y + 3.0f, (float) thumbSize, (float) h - 6.0f);
    g.setColour ((over || down) ? juce::Colour (0xff4A5262) : col::track);
    g.fillRoundedRectangle (thumb, 3.0f);
}

void TFLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (col::raised);
    g.fillRoundedRectangle (0, 0, (float) w, (float) h, 12.0f);
}

void TFLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) ? col::accent : col::border);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 12.0f, e.hasKeyboardFocus (true) ? 1.5f : 1.0f);
}

//==============================================================================
TFButton::TFButton (const juce::String& name, Style s) : juce::Button (name), style (s)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);   // focus ring only for keyboard (Tab) navigation
    setButtonText (name);
    if (style == Style::segment || style == Style::listRow) radius = 9.0f;
}

void TFButton::buttonStateChanged() { startTimerHz (60); }

void TFButton::timerCallback()
{
    auto th = isOver() || isDown() ? 1.0f : 0.0f;
    auto tp = isDown() ? 1.0f : 0.0f;
    hover += (th - hover) * 0.35f;
    press += (tp - press) * 0.5f;
    if (std::abs (th - hover) < 0.01f && std::abs (tp - press) < 0.01f) { hover = th; press = tp; stopTimer(); }
    repaint();
}

void TFButton::paintButton (juce::Graphics& g, bool, bool)
{
    auto bounds = getLocalBounds().toFloat();
    // press feedback: subtle scale
    auto scale = 1.0f - 0.03f * press;
    g.addTransform (juce::AffineTransform::scale (scale, scale, bounds.getCentreX(), bounds.getCentreY()));

    auto r = bounds.reduced (0.75f);
    juce::Colour fg = col::ink;
    const bool enabled = isEnabled();

    switch (style)
    {
        case Style::tool:
        {
            juce::ColourGradient grad (juce::Colour (0xff20242D).interpolatedWith (juce::Colour (0xff272C36), hover), 0, r.getY(),
                                       juce::Colour (0xff191C23).interpolatedWith (juce::Colour (0xff1F232B), hover), 0, r.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (r, radius);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawLine (r.getX() + radius, r.getY() + 1.0f, r.getRight() - radius, r.getY() + 1.0f, 1.0f);
            g.setColour ((selected ? col::accent : juce::Colour (0xff2C313C).interpolatedWith (juce::Colour (0xff3E4552), hover)));
            g.drawRoundedRectangle (r, radius, selected ? 1.5f : 1.0f);
            fg = selected ? col::accentHi : col::inkSoft.interpolatedWith (juce::Colours::white, hover);
            break;
        }
        case Style::primary:
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.fillRoundedRectangle (r.translated (0, 2.0f), radius);
            g.setColour (col::ink.interpolatedWith (juce::Colours::white, hover));
            g.fillRoundedRectangle (r, radius);
            fg = col::bg;
            break;
        case Style::record:
        {
            auto fill = selected ? col::rec : col::recDeep.interpolatedWith (juce::Colour (0xff2A1416), hover);
            g.setColour (fill);
            g.fillRoundedRectangle (r, radius);
            g.setColour (col::rec);
            g.drawRoundedRectangle (r.reduced (0.75f), radius, 1.5f);
            fg = selected ? juce::Colours::white : col::recSoft;
            break;
        }
        case Style::accent:
            g.setColour (col::accent.interpolatedWith (col::accentHi, hover * 0.6f));
            g.fillRoundedRectangle (r, radius);
            fg = col::onAccent;
            break;
        case Style::ghost:
            g.setColour (col::raisedHi.withAlpha (hover));
            g.fillRoundedRectangle (r, radius);
            fg = col::inkSoft.interpolatedWith (juce::Colours::white, hover);
            break;
        case Style::segment:
            if (selected)
            {
                g.setColour (juce::Colour (0xff232731));
                g.fillRoundedRectangle (r, radius);
                g.setColour (juce::Colours::white.withAlpha (0.07f));
                g.drawLine (r.getX() + radius, r.getY() + 1.0f, r.getRight() - radius, r.getY() + 1.0f, 1.0f);
            }
            fg = selected ? col::accentHi : col::muted.interpolatedWith (col::ink, hover);
            break;
        case Style::listRow:
            g.setColour (selected ? col::accentDeep : col::raisedHi.withAlpha (0.5f * hover));
            g.fillRoundedRectangle (r, radius);
            if (selected) { g.setColour (col::accentEdge); g.drawRoundedRectangle (r, radius, 1.0f); }
            fg = selected ? col::accentHi : col::ink;
            break;
    }

    if (! enabled) fg = col::dim;

    // content: icon + text (+ optional subtext for listRow)
    auto content = getLocalBounds().toFloat().reduced (style == Style::listRow ? 12.0f : 10.0f, 0.0f);
    auto text = getButtonText();

    if (style == Style::record)
    {
        juce::Rectangle<float> dot (0, 0, 18.0f, 18.0f);
        auto textW = juce::GlyphArrangement::getStringWidth (font (fontPx, Weight::bold), text);
        auto total = 18.0f + 10.0f + textW;
        dot.setCentre (content.getCentreX() - total * 0.5f + 9.0f, content.getCentreY());
        g.setColour (selected ? juce::Colours::white.withAlpha (0.35f) : col::rec.withAlpha (0.2f));
        g.fillEllipse (dot.expanded (3.0f));
        g.setColour (col::rec);
        g.fillEllipse (dot);
        if (selected) { g.setColour (juce::Colours::white); g.fillEllipse (dot.reduced (5.0f)); }
        g.setColour (fg);
        g.setFont (font (fontPx, Weight::bold));
        g.drawText (text, juce::Rectangle<float> (dot.getRight() + 10.0f, content.getY(), textW + 4.0f, content.getHeight()), juce::Justification::centredLeft, false);
    }
    else if (style == Style::listRow)
    {
        auto iconArea = content.removeFromRight (selected ? 22.0f : 0.0f);
        if (selected) drawIcon (g, icons::check(), iconArea.withSizeKeepingCentre (18, 18), col::accent, false, 2.6f);
        auto top = content.withTrimmedTop (subText.isEmpty() ? 0.0f : 6.0f).removeFromTop (subText.isEmpty() ? content.getHeight() : 22.0f);
        g.setColour (fg);
        g.setFont (font (15.0f, Weight::semibold));
        g.drawText (text, top, subText.isEmpty() ? juce::Justification::centredLeft : juce::Justification::bottomLeft, true);
        if (subText.isNotEmpty())
        {
            g.setColour (col::muted);
            g.setFont (font (13.0f));
            g.drawText (subText, content.withTrimmedTop (28.0f), juce::Justification::topLeft, true);
        }
    }
    else
    {
        g.setFont (font (fontPx, style == Style::segment || style == Style::accent ? Weight::semibold : Weight::medium));
        auto hasIcon = ! icon.isEmpty();
        auto hasText = text.isNotEmpty() && ! getProperties().getWithDefault ("iconOnly", false);
        if (hasIcon && ! hasText)
        {
            drawIcon (g, icon, content.withSizeKeepingCentre (iconPx, iconPx), fg, iconFilled, 2.0f);
        }
        else if (hasIcon)
        {
            auto tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text);
            auto total = iconPx + 8.0f + tw;
            auto x = content.getCentreX() - total * 0.5f;
            drawIcon (g, icon, { x, content.getCentreY() - iconPx * 0.5f, iconPx, iconPx }, fg, iconFilled, 2.0f);
            g.setColour (fg);
            g.drawText (text, juce::Rectangle<float> (x + iconPx + 8.0f, content.getY(), tw + 4.0f, content.getHeight()), juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour (fg);
            g.drawFittedText (text, content.toNearestInt(), juce::Justification::centred, 1);
        }
    }

    if (hasKeyboardFocus (false) && isShowing() && juce::Component::getCurrentlyFocusedComponent() == this)
    {
        g.setColour (col::accent);
        g.drawRoundedRectangle (bounds.reduced (1.0f), radius, 2.0f);
    }
}

//==============================================================================
void TFSwitch::timerCallback()
{
    auto target = getToggleState() ? 1.0f : 0.0f;
    knob += (target - knob) * 0.3f;
    if (std::abs (target - knob) < 0.01f) { knob = target; stopTimer(); }
    repaint();
}

void TFSwitch::paintButton (juce::Graphics& g, bool over, bool)
{
    if (knob < 0.0f) knob = getToggleState() ? 1.0f : 0.0f;
    if (! isTimerRunning() && std::abs (knob - (getToggleState() ? 1.0f : 0.0f)) > 0.01f) knob = getToggleState() ? 1.0f : 0.0f;

    auto r = getLocalBounds().toFloat().withSizeKeepingCentre (juce::jmin ((float) getWidth(), 44.0f), juce::jmin ((float) getHeight(), 26.0f));
    auto onCol = isEnabled() ? col::ok : col::ok.withAlpha (0.4f);
    g.setColour (col::track.interpolatedWith (onCol, knob).brighter (over ? 0.08f : 0.0f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    auto d = r.getHeight() - 6.0f;
    auto x = r.getX() + 3.0f + (r.getWidth() - 6.0f - d) * knob;
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.fillEllipse (x, r.getY() + 4.0f, d, d);
    g.setColour (juce::Colours::white);
    g.fillEllipse (x, r.getY() + 3.0f, d, d);
    if (hasKeyboardFocus (false))
    {
        g.setColour (col::accent);
        g.drawRoundedRectangle (r.expanded (2.0f), r.getHeight() * 0.5f + 2.0f, 2.0f);
    }
}

//==============================================================================
LabeledSlider::LabeledSlider (const juce::String& label, double min, double max, double def,
                              std::function<juce::String (double)> formatter)
    : text (label), fmt (std::move (formatter))
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange (min, max, 0.0);
    slider.setValue (def, juce::dontSendNotification);
    slider.setDoubleClickReturnValue (true, def);
    slider.setTitle (label);
    slider.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    slider.onValueChange = [this] { repaint(); if (onChange) onChange (slider.getValue()); };
    addAndMakeVisible (slider);
}

void LabeledSlider::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (24).withHeight (24));
}

void LabeledSlider::paint (juce::Graphics& g)
{
    auto top = getLocalBounds().removeFromTop (22).toFloat();
    g.setFont (font (14.0f));
    g.setColour (col::inkSoft);
    g.drawText (text, top, juce::Justification::centredLeft, true);
    g.setColour (col::muted);
    g.drawText (fmt ? fmt (slider.getValue()) : juce::String(), top, juce::Justification::centredRight, false);
}

} // namespace tf
