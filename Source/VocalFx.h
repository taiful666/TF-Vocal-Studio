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
    enum Param { tuneAmt, tuneSpd, bass, mid, bright, body, warm, rev, room, echo, etime, dbl, deess, numParams };
    enum Switch { tuneOn, toneOn, bodyOn, revOn, echoOn, dblOn, deessOn, numSwitches };

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
    static constexpr int frameLen = 2048, fftOrder = 12, fftSize = 1 << fftOrder;   // 4096
    std::vector<float> inRing, outAcc, winAcc;
    juce::int64 now = 0;               // samples written so far (input timeline)
    int latency = 2048;

    // detection
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> fa, fb, frame, dfun;
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
/** Dattorro plate reverb (stereo out, mono in). */
class PlateReverb
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (const float* in, float* outL, float* outR, int n, float decay, float damping, float wet) noexcept;
private:
    struct Delay
    {
        std::vector<float> buf; int w = 0, mask = 0;
        void init (int maxLen) { int s = 1; while (s < maxLen + 2) s <<= 1; buf.assign ((size_t) s, 0.0f); mask = s - 1; w = 0; }
        inline void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }
        inline float tap (int d) const noexcept { return buf[(size_t) ((w - 1 - d) & mask)]; }
        inline float tapFrac (float d) const noexcept
        {
            const int i = (int) d; const float f = d - (float) i;
            return tap (i) + f * (tap (i + 1) - tap (i));
        }
    };
    float allpass (Delay& d, int len, float g, float x) noexcept
    {
        const float v = d.tap (len - 1);
        const float u = x + g * v;
        d.push (u);
        return v - g * u;
    }
    double sr = 48000.0, scale = 1.0;
    Delay pre, ap1, ap2, ap3, ap4, mapA, dlA, ap5, dlA2, mapB, dlB, ap6, dlB2;
    int lpre = 0, l1 = 0, l2 = 0, l3 = 0, l4 = 0, lmA = 0, ldA = 0, l5 = 0, ldA2 = 0, lmB = 0, ldB = 0, l6 = 0, ldB2 = 0;
    float bwState = 0.0f, dampA = 0.0f, dampB = 0.0f, lfo = 0.0f, lfoInc = 0.0f;
    int tapsL[7] {}, tapsR[7] {};
};

//==============================================================================
/** The complete vocal chain: auto-tune -> de-esser -> EQ -> compressor/warmth -> double voice, echo, reverb.
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

private:
    void updateFilters() noexcept;
    const FxParams& params;
    double sr = 48000.0;
    bool tuneInPath = false;
    AutoTune tune;
    PlateReverb reverb;
    Biquad deLp1, deLp2, deHp1, deHp2, low, midPk, high, warmLp;
    float deessEnv = 0.0f, compEnv = 0.0f, makeup = 1.0f;
    std::vector<float> work, delayLine, echoL, echoR, revL, revR, dblL, dblR, tuneOut;
    int dlMask = 0, dlW = 0, echoMask = 0, echoW = 0;
    float lfo1 = 0.0f, lfo2 = 0.0f;
    Biquad echoLpL, echoLpR, echoHpL, echoHpR;
    float lastBass = -1, lastMid = -1, lastBright = -1;
    juce::SmoothedValue<float> wetRev, wetEcho, wetDbl, drive, bypassMix;
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
