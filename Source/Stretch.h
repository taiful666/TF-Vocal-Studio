#pragma once

#include <juce_core/juce_core.h>

namespace tf
{
/** Renders a tempo- and/or key-changed copy of an audio file, offline at high quality (Rubber Band R3).
    speed 1.25 = 25% faster (pitch unchanged); semitones +2 = two steps higher (speed unchanged).
    Returns an error text, or empty on success. */
juce::String renderTempo (const juce::File& in, const juce::File& out, double speed, int semitones,
                          std::function<void (float)> progress, std::atomic<bool>& cancel);

/** Cached output file for (audio, speed, semitones). */
juce::File tempoCacheFor (const juce::File& audio, double speed, int semitones = 0);
}
