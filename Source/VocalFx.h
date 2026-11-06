#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_data_structures/juce_data_structures.h>

namespace tf
{
//==============================================================================
/** All vocal-effect settings, shared (lock-free) between UI, live monitor and playback/export. */
struct FxParams
{
    enum Param { tuneAmt, tuneSpd, bass, mid, bright, body, warm, rev, room, echo, etime, dbl, deess, level, numParams };
    enum Switch { tuneOn, toneOn, bodyOn, revOn, echoOn, dblOn, deessOn, levelOn, numSwitches };

    std::array<std::atomic<float>, numParams> p;   // 0..100
    std::array<std::atomic<bool>, numSwitches> on;
    std::atomic<bool> bypass { false };             // A/B compare: hear the voice without effects
    std::atomic<bool> liveTune { false };           // auto-tune in the live monitor (adds ~43 ms)
    std::atomic<int> scaleKey { -1 };               // -1 chromatic, 0..11 major (C..B), 12..23 minor

    FxParams();
    float get (Param i) const noexcept { return p[(size_t) i].load (std::memory_order_relaxed); }
    bool isOn (Switch s) const noexcept { return on[(size_t) s].load (std::memory_order_relaxed); }
    void set (Param i, float v) noexcept { p[(size_t) i].store (juce::jlimit (0.0f, 100.0f, v)); }
    void setOn (Switch s, bool b) noexcept { on[(size_t) s].store (b); }

    juce::String toString() const;            // "k=v;..." for saving
    void fromString (const juce::String&);
};

//==============================================================================
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void reset() noexcept { z1 = z2 = 0; }
    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void setLowShelf (double sr, double f, double q, double gainDb) noexcept;
    void setHighShelf (double sr, double f, double q, double gainDb) noexcept;
    void setPeak (double sr, double f, double q, double gainDb) noexcept;
    void setHighPass (double sr, double f, double q) noexcept;
    void setLowPass (double sr, double f, double q) noexcept;
private:
    void norm (double b0, double b1, double b2, double a0, double a1, double a2) noexcept;
};

//==============================================================================
/** Pitch of a voice frame (YIN with FFT autocorrelation). Shared by the auto-tune and the live pitch display. */
class PitchDetector
{
public:
    static constexpr int frameLen = 2048;
    void prepare (double sampleRate);
    /** frame: the last frameLen samples. Returns the period in samples, or 0 if unvoiced. */
    float analyse (const float* frame) noexcept;
private:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    double sr = 48000.0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> fa, fb, dfun;
};

/** Live pitch of the microphone for the on-screen tuner (runs on the audio thread, cheap). */
class LivePitch
{
public:
    void prepare (double sampleRate);
    void push (const float* x, int n) noexcept;
    float hz() const noexcept { return hzNow.load (std::memory_order_relaxed); }
private:
    PitchDetector det;
    std::vector<float> ring, frame;
    int w = 0, since = 0, hop = 1024;
    std::atomic<float> hzNow { 0.0f };
    double sr = 48000.0;
};

//==============================================================================
/** Real-time pitch correction: FFT-based YIN detection + TD-PSOLA resynthesis (formant-preserving).
    Fixed latency of latencySamples(). With amount 0 it reproduces the input exactly (delayed). */
class AutoTune
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (const float* in, float* out, int n, float amount, float speed, int scaleKey) noexcept;
    int latencySamples() const noexcept { return latency; }
    float lastDetectedHz() const noexcept { return detectedHz; }

    static int nearestScaleNote (float midi, int scaleKey, int currentTarget) noexcept;

private:
    void detect() noexcept;
    void generateMarks() noexcept;

    double sr = 48000.0;
    static constexpr int ringSize = 1 << 14, ringMask = ringSize - 1;
    static constexpr int frameLen = PitchDetector::frameLen;
    std::vector<float> inRing, outAcc, winAcc;
    juce::int64 now = 0;               // samples written so far (input timeline)
    int latency = 2048;

    // detection
    PitchDetector detector;
    std::vector<float> frame;
    int hop = 256, sinceDetect = 0;
    float periodSamples = 0.0f;        // 0 = unvoiced
    float detectedHz = 0.0f;
    int targetNote = -1000;
    float shiftSemis = 0.0f;           // smoothed correction
    float amountNow = 0.0f, speedNow = 50.0f;
    int keyNow = -1;

    // PSOLA marks (absolute sample positions)
    juce::int64 nextAnalysis = 0;
    std::vector<juce::int64> marks; std::vector<float> markPeriod, markRatio;
    double nextSynthesis = 0.0;
    std::vector<float> hann;           // scratch window
};

//==============================================================================
/** Studio-quality pitch correction for playback and export: the same note decisions as AutoTune,
    but the voice is re-pitched by Rubber Band (R3, formants kept), which stays smooth and natural.
    Fixed latency. When off (or nothing to correct) the voice passes through untouched. */
class SmoothTune
{
public:
    SmoothTune();
    ~SmoothTune();
    void prepare (double sampleRate, int maxBlock);
    void reset();
    /** amount 0..1, speed 0..100, scaleKey as in FxParams. enabled=false: delayed dry voice. */
    void process (const float* in, float* out, int n, bool enabled, float amount, float speed, int scaleKey) noexcept;
    int latencySamples() const noexcept { return latency; }
    float currentShift() const noexcept { return shiftSemis; }
    int getUnderruns() const noexcept { return underruns; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    double sr = 48000.0;
    int latency = 0, underruns = 0;
    float shiftSemis = 0.0f;
};

//==============================================================================
/** Smooth hall reverb: 8-line feedback delay network with slowly moving delays (no metallic ring),
    pre-delay, dense input diffusion, and a filtered send (no boomy lows, no hissy highs). */
class SmoothReverb
{
public:
    void prepare (double sampleRate);
    void reset();
    /** Adds the reverb of in to outL/outR. rt60 in seconds, damping 0..1 (high frequencies die sooner). */
    void process (const float* in, float* outL, float* outR, int n, float rt60, float damping, float wet) noexcept;
private:
    struct Line { std::vector<float> buf; int mask = 0, w = 0; float len = 0, g = 0, lp = 0; };
    std::array<Line, 8> lines;
    std::array<std::vector<float>, 4> diff; std::array<int, 4> diffLen {}, diffW {};
    std::vector<float> pre; int preLen = 0, preW = 0, preMask = 0;
    Biquad sendHp, sendLp;
    double sr = 48000.0;
    float lastRt = -1.0f, lastDamp = -1.0f, dampCoef = 0.3f;
    float ph1 = 0.0f, ph2 = 0.0f, inc1 = 0.0f, inc2 = 0.0f, modDepth = 0.0f;
};

//==============================================================================
/** The complete vocal chain:
      (auto-tune) -> clean-up filter -> auto level (vocal rider) -> EQ -> compressor -> de-esser -> warmth
      -> double voice, echo, reverb.
    Mono in, stereo out. Used for the live monitor, playing back a take, and export (offline). */
class VocalChain
{
public:
    explicit VocalChain (const FxParams& p) : params (p) {}
    /** tuneAlwaysInPath: keep the auto-tune delay in the path even when off (constant latency for playback). */
    void prepare (double sampleRate, int maxBlock, bool tuneAlwaysInPath);
    void reset();
    void process (const float* in, float* outL, float* outR, int n, bool isLiveMonitor) noexcept;
    int latencySamples (bool isLiveMonitor) const noexcept;
    /** Gain the vocal rider applies right now (dB), for the tests and the meter. */
    float riderGainDb() const noexcept { return juce::Decibels::gainToDecibels (riderGain); }

private:
    void updateFilters() noexcept;
    const FxParams& params;
    double sr = 48000.0;
    bool tuneInPath = false;
    AutoTune tune;          // live monitor (low latency)
    SmoothTune smoothTune;  // playback and export (studio quality)
    SmoothReverb reverb;
    Biquad deLp1, deLp2, deHp1, deHp2, low, midPk, high, wLp1, wLp2, wHp1, wHp2;
    float deessEnv = 0.0f, deessFull = 0.0f, compDet = 0.0f, compGain = 1.0f, riderDet = 0.0f, riderGain = 1.0f, dcX = 0.0f, dcY = 0.0f;
    std::vector<float> work, delayLine, echoL, echoR, tuneOut, sendBuf;
    int dlMask = 0, dlW = 0, echoMask = 0, echoW = 0;
    float lfo1 = 0.0f, lfo2 = 0.0f;
    Biquad echoLpL, echoLpR, echoHpL, echoHpR;
    float lastBass = -1, lastMid = -1, lastBright = -1;
    juce::SmoothedValue<float> wetRev, wetEcho, wetDbl, warmMix;
};

//==============================================================================
/** Detects the musical key of a song (Krumhansl-Schmuckler on a chromagram). Returns -1 if unsure. */
int detectSongKey (juce::AudioFormatReader& reader, std::atomic<bool>* cancel = nullptr);
juce::String keyName (int key);   // e.g. "G মেজর"

//==============================================================================
struct Preset
{
    juce::String id, category, name, note;
    std::vector<std::pair<FxParams::Param, float>> values;
    std::vector<std::pair<FxParams::Switch, bool>> switches;
    void applyTo (FxParams& p) const;
};
const std::vector<Preset>& builtInPresets();

//==============================================================================
/** "My presets": saved in the app settings, so they survive closing the software. */
struct UserPreset { juce::String id, name, state; };
class UserPresets
{
public:
    explicit UserPresets (juce::PropertiesFile& p) : props (p) {}
    juce::Array<UserPreset> list() const;
    juce::String add (const juce::String& name, const juce::String& fxState);   // returns the new id
    bool rename (const juce::String& id, const juce::String& newName);
    bool remove (const juce::String& id);
    const UserPreset* find (const juce::String& id, juce::Array<UserPreset>& storage) const;
private:
    void store (const juce::Array<UserPreset>&);
    static juce::String clean (const juce::String& s) { return s.replaceCharacters ("\t\r\n", "   ").trim(); }
    juce::PropertiesFile& props;
};
}
