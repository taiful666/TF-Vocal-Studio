#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "VocalFx.h"

namespace tf
{
//==============================================================================
/** "Smart mix", part 1: while someone sings, the music steps back a little (−2.5 dB) and opens a
    small gap in the 2–4 kHz range where the words live, then comes back when the singing stops.
    Used identically for playback and export. */
class MusicDucker
{
public:
    void prepare (double sampleRate);
    void reset();
    /** voice: the summed voices (mono, before reverb is fine). amount 0..1 (0 = off). */
    void process (float* L, float* R, const float* voice, int n, float amount) noexcept;
    float activity() const noexcept { return act; }
private:
    double sr = 48000.0;
    float env = 0.0f, act = 0.0f, lastDip = 99.0f;
    int sinceCoef = 0;
    Biquad pkL, pkR;
};

//==============================================================================
/** "Smart mix", part 2: the master bus — a gentle glue compressor (makes voice and music sound like
    one song) and a transparent peak limiter. Same code for playback and export. */
class MasterBus
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* L, float* R, int n, bool glue) noexcept;
    static constexpr int lookahead = 64;   // limiter look-ahead (samples at 48 kHz): fixed output delay
private:
    double sr = 48000.0;
    float det = 0.0f, gr = 1.0f, lim = 1.0f;
    std::vector<float> dL, dR, pk;
    int w = 0, la = lookahead;
    float attG = 0, relG = 0, attD = 0, limRel = 0;
};

//==============================================================================
/** Offline tools that work on a recording (run on a background thread). */
namespace mixtools
{
    using Progress = std::function<void (float)>;

    /** Integrated loudness (EBU R128 / ITU BS.1770, gated) of a stereo or mono buffer, in LUFS. */
    double integratedLufs (const juce::AudioBuffer<float>& b, int numSamples, double sampleRate);
    /** Loudness of only the parts where `active` is set (one flag per 100 ms block). */
    double gatedLoudness (const juce::AudioBuffer<float>& b, int numSamples, double sampleRate, const std::vector<bool>* active);

    /** Fade in/out (equal-power curve). */
    void applyFades (juce::AudioBuffer<float>& b, int numSamples, double sampleRate, double fadeInSec, double fadeOutSec);

    /** Spectral noise reduction like Audacity's: learns the steady background noise (fan, AC, hiss)
        from the quietest moments and turns down only what sounds like it. reductionDb 6..24. */
    juce::String reduceNoise (const juce::File& in, const juce::File& out, float reductionDb, Progress, std::atomic<bool>& cancel,
                              double* noiseFloorDbOut = nullptr);
    void reduceNoise (std::vector<float>& x, double sampleRate, float reductionDb, Progress, std::atomic<bool>& cancel,
                      double* noiseFloorDbOut = nullptr);

    /** Removes the music that leaked from the speakers into the microphone. Uses what the speakers
        actually played (recorded alongside the voice) and an adaptive echo canceller.
        Returns the error text, or empty on success. erleDb: how much the leaked music was reduced. */
    juce::String removeBleed (const juce::File& mic, const juce::File& speakerRef, const juce::File& out,
                              Progress, std::atomic<bool>& cancel, double* erleDb = nullptr, int* delayOut = nullptr);
    /** In-memory version (also used by the tests). Returns false if no leak was found (x unchanged). */
    bool removeBleed (std::vector<float>& mic, const std::vector<float>& ref, double sampleRate,
                      Progress, std::atomic<bool>& cancel, double* erleDb = nullptr, int* delayOut = nullptr);

    /** Finds the delay (in samples) of a click played through the speaker and heard by the mic.
        Returns -1 if no clear click was found. */
    int findClickDelay (const std::vector<float>& mic, const std::vector<float>& played, int maxLag, double* confidence = nullptr);

    std::vector<float> readMono (const juce::File& f, double* sampleRate = nullptr);
    bool writeMono (const juce::File& f, const std::vector<float>& x, double sampleRate);
}
}
