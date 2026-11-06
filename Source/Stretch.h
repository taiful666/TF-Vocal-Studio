#pragma once

#include <juce_core/juce_core.h>

namespace tf
{
/** Renders a tempo-changed copy of an audio file (pitch unchanged), offline at high quality (Rubber Band R3).
    speed 1.25 = 25% faster. Returns an error text, or empty on success. */
juce::String renderTempo (const juce::File& in, const juce::File& out, double speed,
                          std::function<void (float)> progress, std::atomic<bool>& cancel);

/** Cached output file for (audio, speed). */
juce::File tempoCacheFor (const juce::File& audio, double speed);
}
