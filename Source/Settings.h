#pragma once

#include "Theme.h"
#include "AudioEngine.h"

namespace tf
{
/** Applies the saved audio preferences (sync offset, pre-roll) to the engine at start-up. */
void applySavedAudioSettings (AudioEngine&, juce::PropertiesFile&);

/** Opens the audio settings window: device, recording & sync, and a status check. */
void showAudioSettings (juce::Component* centreAround, AudioEngine&, juce::PropertiesFile&);
}
