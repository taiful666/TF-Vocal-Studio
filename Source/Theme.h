#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace tf
{
//==============================================================================
// Design tokens (match the approved interface design)
namespace col
{
    const juce::Colour bg          { 0xff0E1014 };
    const juce::Colour header      { 0xff12141A };
    const juce::Colour panel       { 0xff14171D };
    const juce::Colour border      { 0xff22262F };
    const juce::Colour raised      { 0xff1E222B };
    const juce::Colour raisedHi    { 0xff262B35 };
    const juce::Colour well        { 0xff0B0D11 };
    const juce::Colour ink         { 0xffEEF0F4 };
    const juce::Colour inkSoft     { 0xffC9CFD9 };
    const juce::Colour muted       { 0xffA3ABB9 };
    const juce::Colour dim         { 0xff808999 };
    const juce::Colour track       { 0xff3A404C };
    const juce::Colour accent      { 0xffF5A524 };
    const juce::Colour accentHi    { 0xffFFC35C };
    const juce::Colour accentDeep  { 0xff241D10 };
    const juce::Colour accentEdge  { 0xff8A6418 };
    const juce::Colour onAccent    { 0xff14110A };
    const juce::Colour rec         { 0xffE5484D };
    const juce::Colour recSoft     { 0xffFF8F92 };
    const juce::Colour recDeep     { 0xff1E1214 };
    const juce::Colour ok          { 0xff3FCF8E };
    const juce::Colour okText      { 0xff6FE3AE };
    const juce::Colour okDeep      { 0xff173226 };
    const juce::Colour warn        { 0xffF5C04A };
    const juce::Colour danger      { 0xffFF9C8A };
}

enum class Weight { regular, medium, semibold, bold };

/** Embedded Hind Siliguri (Bengali + Latin). size = CSS-style em size in px. */
juce::Font font (float size, Weight w = Weight::regular);

/** Western digits -> Bengali digits. */
juce::String bn (const juce::String& s);
juce::String bn (int v);
/** mm:ss in Bengali digits. */
juce::String bnTime (double seconds);

//==============================================================================
namespace icons
{
    juce::Path play();
    juce::Path pause();
    juce::Path stop();
    juce::Path toStart();
    juce::Path download();
    juce::Path gear();
    juce::Path chevronRight();
    juce::Path check();
    juce::Path plus();
    juce::Path folder();
    juce::Path headphones();
    juce::Path music();
    juce::Path search();
    juce::Path sparkle();
    juce::Path mic();
    juce::Path wand();
    juce::Path speaker();
    juce::Path speakerOff();
    juce::Path scissors();
    juce::Path undo();
    juce::Path redo();
    juce::Path loop();
    juce::Path hand();
    juce::Path cursor();
    juce::Path zoomIn();
    juce::Path zoomOut();
    juce::Path trash();
    juce::Path chevronDown();
    juce::Path redo2();
}

/** Draw a 24x24-viewBox icon path into area, stroked (line icons) or filled. */
void drawIcon (juce::Graphics&, const juce::Path& p24, juce::Rectangle<float> area,
               juce::Colour c, bool filled, float strokeWidth = 2.0f);

//==============================================================================
/** Small helper that animates a float towards a target (smooth hover/press/expand). */
class Smoother : private juce::Timer
{
public:
    std::function<void()> onChange;
    void setTarget (float t, bool instant = false);
    float get() const noexcept { return value; }
private:
    void timerCallback() override;
    float value = 0.0f, target = 0.0f;
    double lastMs = 0;
};

//==============================================================================
class TFLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TFLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override { return 8; }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (15.0f); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override { return font (15.0f); }
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    juce::Font getLabelFont (juce::Label&) override { return font (15.0f); }
    juce::Font getAlertWindowTitleFont() override { return font (19.0f, Weight::semibold); }
    juce::Font getAlertWindowMessageFont() override { return font (16.0f); }
    juce::Font getAlertWindowFont() override { return font (15.0f); }

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 10; }

    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
};

//==============================================================================
/** Professional button: gradient "tool" style, primary (white), record, accent, ghost, segment. */
class TFButton : public juce::Button, private juce::Timer
{
public:
    enum class Style { tool, primary, record, accent, ghost, segment, listRow };

    TFButton (const juce::String& name, Style s = Style::tool);

    void setIcon (juce::Path p, bool filled, float iconSize = 18.0f) { icon = std::move (p); iconFilled = filled; iconPx = iconSize; repaint(); }
    void setText (const juce::String& t) { setButtonText (t); repaint(); }
    void setSubText (const juce::String& t) { subText = t; repaint(); }
    void setStyle (Style s) { style = s; repaint(); }
    void setRadius (float r) { radius = r; }
    void setFontSize (float s) { fontPx = s; }
    void setSelected (bool s) { selected = s; repaint(); }
    bool isSelected() const { return selected; }

    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    void timerCallback() override;
    void buttonStateChanged() override;

    Style style;
    juce::Path icon;
    bool iconFilled = true, selected = false;
    float iconPx = 18.0f, radius = 12.0f, fontPx = 15.0f;
    juce::String subText;
    float hover = 0.0f, press = 0.0f;
};

//==============================================================================
/** Animated pill switch. */
class TFSwitch : public juce::ToggleButton, private juce::Timer
{
public:
    TFSwitch() { setClickingTogglesState (true); setWantsKeyboardFocus (true); setMouseClickGrabsKeyboardFocus (false); setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
private:
    using juce::ToggleButton::clicked;
    void clicked() override { startTimerHz (60); }
    void timerCallback() override;
    float knob = -1.0f;
};

//==============================================================================
/** Rounded card background. */
class Card : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (col::panel);  g.fillRoundedRectangle (r, 18.0f);
        g.setColour (col::border); g.drawRoundedRectangle (r, 18.0f, 1.0f);
    }
};

//==============================================================================
/** Professional rotary knob with its name under it and the value inside the ring. */
class Knob : public juce::Component
{
public:
    Knob (const juce::String& name, double min, double max, double def, std::function<juce::String (double)> fmt);
    juce::Slider slider;
    std::function<void (double)> onChange;
    void resized() override;
    void paint (juce::Graphics&) override;
    void setValue (double v) { slider.setValue (v, juce::dontSendNotification); repaint(); }
private:
    juce::String name;
    std::function<juce::String (double)> format;
};

//==============================================================================
/** Label + value + slider, as in the design's effect rows. */
class LabeledSlider : public juce::Component
{
public:
    LabeledSlider (const juce::String& label, double min, double max, double def,
                   std::function<juce::String (double)> formatter);
    juce::Slider slider;
    void resized() override;
    void paint (juce::Graphics&) override;
    std::function<void (double)> onChange;
private:
    juce::String text;
    std::function<juce::String (double)> fmt;
};

} // namespace tf
