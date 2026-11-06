#include "Lyrics.h"

namespace tf
{
juce::String Lyrics::readTextFile (const juce::File& f)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb) || mb.getSize() == 0)
        return {};

    auto* d = static_cast<const unsigned char*> (mb.getData());
    auto n = mb.getSize();

    // UTF-16 with BOM
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF)))
        return juce::String::createStringFromData (mb.getData(), (int) n);

    // UTF-8 (with or without BOM)
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF)
        return juce::String::fromUTF8 (reinterpret_cast<const char*> (d + 3), (int) n - 3);

    if (juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (d), (int) n))
        return juce::String::fromUTF8 (reinterpret_cast<const char*> (d), (int) n);

    return juce::String::createStringFromData (mb.getData(), (int) n);
}

static bool parseTimeTag (const juce::String& tag, double& seconds)
{
    // mm:ss, mm:ss.xx, mm:ss:xx, h:mm:ss.xx
    auto t = tag.trim().replaceCharacter (',', '.');
    auto parts = juce::StringArray::fromTokens (t, ":", "");
    if (parts.size() < 2 || parts.size() > 3)
        return false;

    for (auto& p : parts)
        if (p.isEmpty() || ! p.containsOnly ("0123456789."))
            return false;

    double total = 0.0;
    if (parts.size() == 2)
        total = parts[0].getIntValue() * 60.0 + parts[1].getDoubleValue();
    else // mm:ss:xx (hundredths) — the only 3-part form used in LRC files
        total = parts[0].getIntValue() * 60.0 + parts[1].getIntValue()
              + parts[2].getIntValue() / std::pow (10.0, (double) parts[2].length());

    seconds = total;
    return true;
}

Lyrics Lyrics::parse (const juce::String& text)
{
    Lyrics result;
    double offsetSec = 0.0;
    juce::Array<LyricLine> timedLines, plainLines;

    juce::StringArray rows;
    rows.addLines (text);

    for (auto row : rows)
    {
        row = row.trim();
        if (row.isEmpty())
            continue;

        juce::Array<double> times;
        bool isMeta = false;

        while (row.startsWithChar ('['))
        {
            auto close = row.indexOfChar (']');
            if (close < 0) break;
            auto tag = row.substring (1, close);
            double t = 0;
            if (parseTimeTag (tag, t))
                times.add (t);
            else if (tag.startsWithIgnoreCase ("offset:"))
            {
                offsetSec = tag.fromFirstOccurrenceOf (":", false, false).trim().getIntValue() / 1000.0;
                isMeta = true;
            }
            else if (tag.containsChar (':'))
                isMeta = true;   // [ar:], [ti:], [al:], [by:] ...
            else
                break;
            row = row.substring (close + 1).trim();
        }

        // strip enhanced-LRC word tags <mm:ss.xx>
        while (row.containsChar ('<'))
        {
            auto a = row.indexOfChar ('<'), b = row.indexOfChar (a, '>');
            if (b < 0) break;
            double t = 0;
            if (! parseTimeTag (row.substring (a + 1, b), t)) break;
            row = (row.substring (0, a) + row.substring (b + 1));
        }
        row = row.trim();

        if (! times.isEmpty())
        {
            for (auto t : times)
                timedLines.add ({ t, row });
        }
        else if (! isMeta)
        {
            plainLines.add ({ -1.0, row });
        }
    }

    if (! timedLines.isEmpty())
    {
        // LRC [offset:+500] means lyrics appear 0.5 s earlier
        for (auto& l : timedLines) l.time = juce::jmax (0.0, l.time - offsetSec);
        std::stable_sort (timedLines.begin(), timedLines.end(), [] (const LyricLine& a, const LyricLine& b) { return a.time < b.time; });

        // drop empty instrumental markers but keep their timing as a gap
        for (auto& l : timedLines)
            result.lines.add (l);
        result.timed = true;
    }
    else
    {
        result.lines = plainLines;
        result.timed = false;
    }
    return result;
}

Lyrics Lyrics::loadFromFile (const juce::File& f)
{
    if (! f.existsAsFile() || f.getSize() > 2 * 1024 * 1024)
        return {};
    return parse (readTextFile (f));
}

juce::File Lyrics::findFor (const juce::File& audioFile)
{
    for (auto ext : { ".lrc", ".LRC", ".txt", ".TXT" })
    {
        auto f = audioFile.withFileExtension (ext);
        if (f.existsAsFile())
            return f;
    }
    return {};
}

int Lyrics::indexAt (double t) const noexcept
{
    if (! timed || lines.isEmpty() || t < lines.getReference (0).time)
        return -1;
    int lo = 0, hi = lines.size() - 1;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;
        if (lines.getReference (mid).time <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

double Lyrics::progressAt (int i, double t, double songLength) const noexcept
{
    if (! timed || i < 0 || i >= lines.size())
        return 0.0;
    auto start = lines.getReference (i).time;
    auto end = (i + 1 < lines.size()) ? lines.getReference (i + 1).time
                                      : juce::jmax (start + 4.0, songLength);
    // sung text usually finishes a little before the next line starts
    auto singEnd = start + juce::jmax (0.6, (end - start) * 0.92);
    singEnd = juce::jmin (singEnd, start + 12.0);
    return juce::jlimit (0.0, 1.0, (t - start) / (singEnd - start));
}
}
