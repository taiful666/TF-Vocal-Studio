#pragma once

#include <juce_core/juce_core.h>

namespace tf
{
struct LyricLine
{
    double time = -1.0;      // seconds; -1 for untimed lines
    juce::String text;
};

/** Song lyrics. Supports .lrc (timed, incl. [offset:]) and plain .txt (untimed). */
class Lyrics
{
public:
    juce::Array<LyricLine> lines;
    bool timed = false;

    bool isEmpty() const noexcept { return lines.isEmpty(); }

    static Lyrics parse (const juce::String& text);
    static Lyrics loadFromFile (const juce::File& f);

    /** Finds .lrc (preferred) or .txt with the same name as the audio file. */
    static juce::File findFor (const juce::File& audioFile);

    /** Index of the line active at time t (-1 = before the first line). */
    int indexAt (double t) const noexcept;

    /** Progress 0..1 through line i at time t. */
    double progressAt (int i, double t, double songLength) const noexcept;

    /** Reads a text file as UTF-8 (with BOM handling), UTF-16, or falls back to Windows-1252-safe decoding. */
    static juce::String readTextFile (const juce::File& f);
};
}
