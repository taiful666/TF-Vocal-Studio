#include "RecordAids.h"
#include "VocalFx.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

//==============================================================================
void InputMeter::setLevel (float peak)
{
    const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
    shownDb = db > shownDb ? db : juce::jmax (db, shownDb - 1.5f);   // fast up, smooth down (30 fps)
    if (db >= holdDb) { holdDb = db; holdFrames = 45; }
    else if (--holdFrames < 0) holdDb = juce::jmax (-60.0f, holdDb - 1.0f);
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (db > -3.0f) lastLoud = now;
    // verdict from the recent peak: too loud if anything came near clipping in the last 2 s
    if (now - lastLoud < 2000.0) state = 1;
    else if (holdDb < -30.0f) state = -1;
    else state = 0;
    repaint();
}

void InputMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto label = r.removeFromTop (18.0f);
    g.setFont (font (12.5f, Weight::medium));
    g.setColour (col::muted);
    g.drawText (u8 ("মাইক লেভেল"), label, juce::Justification::centredLeft);
    const juce::String word = state > 0 ? u8 ("বেশি জোরে — মাইকের জোর কমান") : state < 0 ? u8 ("কম — কাছে এসে গাইন") : u8 ("ঠিক আছে");
    g.setColour (state > 0 ? col::rec : state < 0 ? col::warn : col::ok);
    g.drawText (word, label, juce::Justification::centredRight);

    auto bar = r.withSizeKeepingCentre (r.getWidth(), 12.0f);
    g.setColour (col::well);
    g.fillRoundedRectangle (bar, 6.0f);
    auto xOf = [&bar] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    // the good zone (-24 .. -6 dBFS peaks)
    g.setColour (col::ok.withAlpha (0.16f));
    g.fillRect (juce::Rectangle<float> (xOf (-24.0f), bar.getY(), xOf (-6.0f) - xOf (-24.0f), bar.getHeight()));
    const float x = xOf (shownDb);
    juce::ColourGradient grad (col::warn, bar.getX(), 0, col::rec, bar.getRight(), 0, false);
    grad.addColour ((xOf (-24.0f) - bar.getX()) / bar.getWidth(), col::ok);
    grad.addColour ((xOf (-6.0f) - bar.getX()) / bar.getWidth(), col::ok);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (bar.withRight (juce::jmax (bar.getX() + 2.0f, x)), 6.0f);
    g.setColour (col::ink.withAlpha (0.8f));
    g.fillRect (xOf (holdDb) - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f);
}

//==============================================================================
void PitchMeter::setPitch (float hz, int key)
{
    if (hz > 50.0f && hz < 1500.0f)
    {
        const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
        const int nearest = (int) std::round (midi);
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        // sargam relative to the song's key (Sa = the key's tonic), or plain note names
        static const char* sargam[] = { "সা", "ঋ", "রে", "জ্ঞ", "গা", "মা", "কড়ি মা", "পা", "দ", "ধা", "ণ", "নি" };
        note = juce::String (names[((nearest % 12) + 12) % 12]) + juce::String (nearest / 12 - 1);
        if (key >= 0) note << "  " << juce::String::fromUTF8 (sargam[(((nearest - key % 12) % 12) + 12) % 12]);
        cents = (midi - (float) nearest) * 100.0f;
        inScale = key < 0 || AutoTune::nearestScaleNote (midi, key, -1000) == nearest;
        voiced = true;
        silentFrames = 0;
    }
    else if (++silentFrames > 8) voiced = false;
    shownCents += (cents - shownCents) * 0.35f;
    repaint();
}

void PitchMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (col::well);
    g.fillRoundedRectangle (r, 10.0f);
    auto inner = r.reduced (10.0f, 6.0f);
    auto txt = inner.removeFromLeft (92.0f);
    const bool good = voiced && std::abs (shownCents) < 15.0f && inScale;
    g.setFont (font (16.0f, Weight::semibold));
    g.setColour (voiced ? (good ? col::ok : col::ink) : col::dim);
    g.drawText (voiced ? note : u8 ("সুর —"), txt, juce::Justification::centredLeft, true);
    // cents scale -50 .. +50
    auto sc = inner.withSizeKeepingCentre (inner.getWidth(), 6.0f);
    g.setColour (col::border);
    g.fillRoundedRectangle (sc, 3.0f);
    g.setColour (col::ok.withAlpha (0.35f));
    g.fillRect (sc.withSizeKeepingCentre (sc.getWidth() * 0.3f, sc.getHeight()));
    if (voiced)
    {
        const float x = sc.getCentreX() + sc.getWidth() * 0.5f * juce::jlimit (-1.0f, 1.0f, shownCents / 50.0f);
        g.setColour (good ? col::ok : (std::abs (shownCents) < 30.0f ? col::warn : col::rec));
        g.fillEllipse (x - 6.0f, sc.getCentreY() - 6.0f, 12.0f, 12.0f);
    }
}

//==============================================================================
void CountIn::start (std::function<void()> onGo)
{
    go = std::move (onGo);
    count = 3;
    counting = true;
    setVisible (true);
    engine.playBeep (880.0f, 0.08, 0.25f);
    tickStart = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
    repaint();
}

void CountIn::cancel()
{
    stopTimer();
    counting = false;
    go = nullptr;
    setVisible (false);
}

void CountIn::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (now - tickStart >= 1000.0)
    {
        tickStart += 1000.0;
        if (--count <= 0)
        {
            stopTimer();
            counting = false;
            setVisible (false);
            auto f = std::move (go);
            if (f) f();
            return;
        }
        engine.playBeep (count == 1 ? 1320.0f : 880.0f, 0.08, 0.25f);
    }
    repaint();
}

void CountIn::paint (juce::Graphics& g)
{
    if (! counting) return;
    g.fillAll (juce::Colour (0xcc0B0D11));
    const float t = (float) juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - tickStart) / 1000.0);
    const float size = 150.0f * (1.15f - 0.25f * t);
    g.setColour (col::accent.withAlpha (1.0f - 0.6f * t));
    g.setFont (font (size, Weight::bold));
    g.drawText (bn (count), getLocalBounds(), juce::Justification::centred);
    g.setColour (col::muted);
    g.setFont (font (16.0f));
    g.drawText (u8 ("রেকর্ড শুরু হচ্ছে… থামাতে আবার রেকর্ড চাপুন"), getLocalBounds().removeFromBottom (getHeight() / 4), juce::Justification::centred);
}
}
