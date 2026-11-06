// Sound-quality tests for v1.1: every claim about "smooth, clean, no crackle" is measured here.
// Included by EngineTest.cpp (uses its CHECK macro and helpers).
#pragma once
#include "AudioInput.h"
#include "MixTools.h"

namespace quality
{
using namespace tf;
static constexpr double SR = 48000.0;

/** A sung vowel: harmonics with a voice-like spectrum, gentle vibrato. */
static std::vector<float> sungVowel (double seconds, double hz, float amp, double vibCents = 15.0, unsigned seed = 1)
{
    const int n = (int) (seconds * SR);
    std::vector<float> x ((size_t) n);
    double ph = 0.0;
    juce::Random rnd ((juce::int64) seed);
    for (int i = 0; i < n; ++i)
    {
        const double f = hz * std::pow (2.0, vibCents / 1200.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * i / SR));
        ph += 2.0 * juce::MathConstants<double>::pi * f / SR;
        double v = 0.0;
        for (int h = 1; h <= 24 && h * f < 10000.0; ++h)
        {
            const double fh = h * f;
            const double formant = std::exp (-std::pow ((fh - 700.0) / 500.0, 2.0)) + 0.5 * std::exp (-std::pow ((fh - 1200.0) / 400.0, 2.0))
                                 + 0.25 * std::exp (-std::pow ((fh - 2600.0) / 600.0, 2.0)) + 0.05;
            v += formant / h * std::sin (h * ph);
        }
        x[(size_t) i] = (float) v;
    }
    float pk = 0; for (auto s : x) pk = juce::jmax (pk, std::abs (s));
    for (auto& s : x) s *= amp / pk;
    return x;
}

static double rmsOf (const std::vector<float>& v, size_t a, size_t b)
{
    double s = 0; b = juce::jmin (b, v.size());
    for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) juce::jmax ((size_t) 1, b - a));
}
static double db (double x) { return 20.0 * std::log10 (x + 1e-12); }

/** Best lag of b against a (b delayed by lag) and the correlation there. */
static int lagOf (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t len, int maxLag, double& corr)
{
    int best = 0; double bc = -2.0;
    for (int l = -maxLag; l <= maxLag; ++l)
    {
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = from; i < from + len; ++i)
        {
            const long j = (long) i + l;
            if (j < 0 || j >= (long) b.size()) continue;
            sab += a[i] * b[(size_t) j]; saa += a[i] * a[i]; sbb += b[(size_t) j] * b[(size_t) j];
        }
        const double c = sab / std::sqrt (saa * sbb + 1e-20);
        if (c > bc) { bc = c; best = l; }
    }
    corr = bc;
    return best;
}

/** Pitch (Hz) of a stretch of signal, by a fine autocorrelation search. */
static double pitchOf (const std::vector<float>& x, size_t from, size_t len, double lo, double hi)
{
    double bestR = -1, bestP = 0;
    for (double p = SR / hi; p <= SR / lo; p += 0.05)
    {
        double s = 0, e = 0;
        for (size_t i = from; i < from + len; ++i)
        {
            const double t = (double) i + p;
            const size_t k = (size_t) t; const double f = t - (double) k;
            if (k + 1 >= x.size()) break;
            const double y = x[k] + f * (x[k + 1] - x[k]);
            s += x[i] * y; e += x[i] * x[i];
        }
        const double r = s / (e + 1e-20);
        if (r > bestR) { bestR = r; bestP = p; }
    }
    return bestP > 0 ? SR / bestP : 0.0;
}

/** Energy far from the harmonics of f0 (roughness / "khas-khas") relative to the harmonic energy, dB. */
static double inharmonicDb (const std::vector<float>& x, size_t from, double f0)
{
    constexpr int order = 15, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> b ((size_t) N * 2, 0.0f);
    for (int i = 0; i < N && from + (size_t) i < x.size(); ++i)
        b[(size_t) i] = x[from + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
    fft.performFrequencyOnlyForwardTransform (b.data());
    double harm = 0, other = 0;
    for (int k = 1; k < N / 2; ++k)
    {
        const double f = k * SR / N;
        if (f < 100.0 || f > 9000.0) continue;
        const double h = f / f0, d = std::abs (h - std::round (h)) * f0;
        const double e = (double) b[(size_t) k] * b[(size_t) k];
        if (d < 25.0) harm += e; else if (d > 60.0) other += e;
    }
    return 10.0 * std::log10 ((other + 1e-20) / (harm + 1e-20));
}

static std::vector<float> runTune (FxParams& p, const std::vector<float>& in, int& lat)
{
    VocalChain ch (p);
    ch.prepare (SR, 512, true);
    std::vector<float> L (in.size()), R (in.size());
    for (size_t i = 0; i < in.size(); i += 256)
    {
        const int n = (int) juce::jmin ((size_t) 256, in.size() - i);
        ch.process (in.data() + i, L.data() + i, R.data() + i, n, false);
    }
    lat = ch.latencySamples (false);
    return L;
}

static void onlyTune (FxParams& p, float amount, float speed)
{
    for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, false);
    p.setOn (FxParams::tuneOn, true);
    p.set (FxParams::tuneAmt, amount);
    p.set (FxParams::tuneSpd, speed);
    p.scaleKey = -1;
}

//==============================================================================
/** A microphone on its own sound card, with its own (slightly fast or slow) clock. */
struct FakeMic : public juce::AudioIODevice, private juce::Thread
{
    double rateError; int block = 480; double hz = 441.0;
    juce::AudioIODeviceCallback* cb = nullptr;
    bool opened = false, started = false;
    double phase = 0.0;
    FakeMic (double ppm) : AudioIODevice ("FakeMic", "Fake"), Thread ("fake mic"), rateError (ppm * 1e-6) {}
    ~FakeMic() override { close(); }
    juce::StringArray getOutputChannelNames() override { return {}; }
    juce::StringArray getInputChannelNames() override  { return { "L", "R" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 480 }; }
    int getDefaultBufferSize() override { return 480; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { opened = true; return {}; }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* c) override { cb = c; c->audioDeviceAboutToStart (this); started = true; startRealtimeThread ({}); }
    void stop() override { if (! started) return; stopThread (2000); started = false; if (cb != nullptr) cb->audioDeviceStopped(); cb = nullptr; }
    bool isPlaying() override { return started; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return 0; }
    juce::BigInteger getActiveInputChannels() const override { return 3; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 240; }
    void run() override
    {
        juce::AudioBuffer<float> in (2, block);
        const double period = 1000.0 * block / (48000.0 * (1.0 + rateError));
        auto next = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            for (int i = 0; i < block; ++i)
            {
                const float v = 0.3f * (float) std::sin (phase);
                phase += 2.0 * juce::MathConstants<double>::pi * hz / 48000.0;
                in.setSample (0, i, v);
                in.setSample (1, i, 0.0f);   // a dead second channel (some USB mics) must not halve the level
            }
            const float* ins[] = { in.getReadPointer (0), in.getReadPointer (1) };
            cb->audioDeviceIOCallbackWithContext (ins, 2, nullptr, 0, block, {});
            next += period;
            const auto wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0) juce::Thread::sleep ((int) wait);
        }
    }
};

static void testInputBridge (int& failures)
{
    std::cout << "\nMicrophone on its own sound card (clock drift, no crackle)\n";
    for (double ppm : { 300.0, -300.0 })
    {
        InputBridge bridge;
        auto err = bridge.openDevice (std::make_unique<FakeMic> (ppm), 48000.0, 480);
        TFCHECK (err.isEmpty(), "separate microphone opened");
        bridge.prepareOutput (48000.0);
        const int block = 128, seconds = 9;
        std::vector<float> out;
        out.reserve ((size_t) (48000 * seconds));
        std::vector<float> buf (block);
        auto next = juce::Time::getMillisecondCounterHiRes();
        const double period = 1000.0 * block / 48000.0;
        while (out.size() < (size_t) (48000 * seconds))
        {
            bridge.pull (buf.data(), block);
            out.insert (out.end(), buf.begin(), buf.end());
            next += period;
            const auto wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0) juce::Thread::sleep ((int) wait);
        }
        const int underrunsAfterStart = bridge.getUnderruns();
        // after the first second: a perfect, continuous sine — no gaps, clicks or repeated bits
        size_t start = 48000;
        while (start < out.size() && std::abs (out[start]) < 1e-6f) ++start;
        start += 48000;
        double worst = 0.0;
        // the sine obeys x[n+1] + x[n-1] = 2 cos(w) x[n]; any glitch breaks it
        const double c2 = 2.0 * std::cos (2.0 * juce::MathConstants<double>::pi * 441.0 * (1.0 + ppm * 1e-6) / 48000.0);
        for (size_t i = start + 1; i + 1 < out.size(); ++i)
            worst = juce::jmax (worst, std::abs ((double) out[i + 1] + out[i - 1] - c2 * out[i]));
        const double level = rmsOf (out, start, out.size()) * std::sqrt (2.0);
        TFCHECK (worst < 0.003, "continuous voice with " + juce::String ((int) ppm) + " ppm clock difference (worst jump " + juce::String (worst, 5) + ")");
        TFCHECK (std::abs (db (level / 0.3)) < 0.2, "full level, a silent second channel is ignored (" + juce::String (db (level / 0.3), 2) + " dB)");
        TFCHECK (underrunsAfterStart <= 1, "no gaps after start-up (" + juce::String (underrunsAfterStart) + ")");
        TFCHECK (std::abs (bridge.getDriftPpm() - ppm) < 120.0, "drift followed (" + juce::String (bridge.getDriftPpm(), 0) + " ppm)");
        bridge.close();
    }
}

//==============================================================================
static void testVoiceQuality (int& failures)
{
    std::cout << "\nVoice quality (smooth, no harshness)\n";

    // 1. auto-tune on an in-tune voice: the voice comes out (almost) untouched and exactly in time
    //    (a steady note: with vibrato the correction itself moves the phase, which is not a delay)
    {
        auto v = sungVowel (4.0, 220.0, 0.3f, 0.0);
        FxParams p; onlyTune (p, 50.0f, 50.0f);
        int lat = 0;
        auto o = runTune (p, v, lat);
        std::vector<float> d (v.size(), 0.0f);
        for (size_t i = (size_t) lat; i < v.size(); ++i) d[i] = v[i - (size_t) lat];
        double c = 0;
        const int lag = lagOf (d, o, 96000, 48000, 40, c);
        TFCHECK (std::abs (lag) <= 1 && c > 0.97, "auto-tune keeps an in-tune voice natural and in time (lag " + juce::String (lag) + ", similarity " + juce::String (c, 3) + ")");
        const double rough = inharmonicDb (o, 96000, 220.0), roughIn = inharmonicDb (d, 96000, 220.0);
        TFCHECK (rough < -45.0, "no audible roughness added (" + juce::String (roughIn, 1) + " -> " + juce::String (rough, 1) + " dB between harmonics)");
    }
    // 2. a flat note is pulled into tune, smoothly (no "khas-khas" between the harmonics)
    {
        const double flat = 220.0 * std::pow (2.0, -40.0 / 1200.0);
        auto v = sungVowel (4.0, flat, 0.3f, 0.0);
        FxParams p; onlyTune (p, 100.0f, 80.0f);
        int lat = 0;
        auto o = runTune (p, v, lat);
        const double hz = pitchOf (o, (size_t) lat + 96000, 9600, 180.0, 260.0);
        const double cents = 1200.0 * std::log2 (hz / 220.0);
        TFCHECK (std::abs (cents) < 8.0, "a 40-cent flat note is corrected (now " + juce::String (cents, 1) + " cents)");
        const double rough = inharmonicDb (o, (size_t) lat + 96000, hz);
        TFCHECK (rough < -35.0, "corrected voice is clean, no roughness (" + juce::String (rough, 1) + " dB between harmonics)");
    }
    // 3. warmth: real harmonics on the body of the voice, no harsh aliasing from the "s" range
    {
        auto runTone = [&] (double hz, float amp)
        {
            const int n = 48000;
            std::vector<float> s ((size_t) n);
            for (int i = 0; i < n; ++i) s[(size_t) i] = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / SR);
            FxParams p;
            for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, false);
            p.set (FxParams::tuneAmt, 0.0f);
            p.setOn (FxParams::bodyOn, true); p.set (FxParams::body, 0.0f); p.set (FxParams::warm, 100.0f);
            int lat = 0;
            return runTune (p, s, lat);
        };
        auto level = [&] (const std::vector<float>& o, double hz)
        {
            const double w = 2 * juce::MathConstants<double>::pi * hz / SR, c = 2 * std::cos (w);
            double s1 = 0, s2 = 0;
            for (size_t i = 24000; i < 48000; ++i) { const double s0 = o[i] + c * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt (juce::jmax (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2.0 / 24000.0;
        };
        auto o1 = runTone (500.0, 0.4f);
        TFCHECK (db (level (o1, 1500.0) / level (o1, 500.0)) > -40.0, "warmth adds real harmonics (" + juce::String (db (level (o1, 1500.0) / level (o1, 500.0)), 0) + " dB)");
        auto o2 = runTone (5000.0, 0.5f);
        const double f = level (o2, 5000.0), alias = juce::jmax (level (o2, 13000.0), level (o2, 23000.0), level (o2, 9000.0));
        TFCHECK (db (alias / f) < -60.0, "warmth adds no aliasing hiss (" + juce::String (db (alias / f), 0) + " dB)");
    }
    // 4. live noise removal: the dry and cleaned voices are mixed in step (no hollow comb sound)
    {
        NoiseSuppressor ns;
        juce::Random r (9);
        std::vector<float> x (48000 * 2), y;
        for (auto& v : x) v = 0.1f * (r.nextFloat() * 2.0f - 1.0f);
        y = x;
        ns.process (y.data(), (int) y.size(), 0.0f);
        double c = 0;
        const int lag = lagOf (x, y, 4800, 24000, 1100, c);
        TFCHECK (lag == NoiseSuppressor::latency && c > 0.999, "noise removal blend is time-aligned (" + juce::String (lag) + " samples)");
    }
    // 5. vocal rider: soft and loud singing end up close together; pauses are left alone
    {
        std::vector<float> v;
        for (int k = 0; k < 6; ++k)
        {
            auto seg = sungVowel (1.0, 220.0, k % 2 == 0 ? 0.03f : 0.3f, 10.0, (unsigned) k);
            v.insert (v.end(), seg.begin(), seg.end());
        }
        FxParams p;
        for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, false);
        p.set (FxParams::tuneAmt, 0.0f);
        p.setOn (FxParams::levelOn, true); p.set (FxParams::level, 100.0f);
        int lat = 0;
        auto o = runTune (p, v, lat);
        const size_t L = (size_t) lat;
        const double inRange = db (rmsOf (v, 48000 * 3 + 12000, 48000 * 4) / rmsOf (v, 48000 * 2 + 12000, 48000 * 3));
        const double outRange = db (rmsOf (o, 48000 * 3 + 12000 + L, 48000 * 4 + L) / rmsOf (o, 48000 * 2 + 12000 + L, 48000 * 3 + L));
        TFCHECK (outRange < 8.0 && inRange > 18.0, "auto level evens soft/loud singing: " + juce::String (inRange, 1) + " dB -> " + juce::String (outRange, 1) + " dB");
    }
}

//==============================================================================
static void testMixTools (int& failures)
{
    std::cout << "\nOne-click clean-up, smart mix, mastering\n";
    juce::Random rnd (5);
    // phrases with pauses + fan noise
    std::vector<float> voice;
    for (int k = 0; k < 8; ++k)
    {
        auto seg = sungVowel (0.8, 196.0 + 20.0 * k, 0.25f, 15.0, (unsigned) k);
        voice.insert (voice.end(), seg.begin(), seg.end());
        voice.insert (voice.end(), (size_t) 24000, 0.0f);
    }
    std::vector<float> noise (voice.size());
    float p0 = 0, p1 = 0;
    for (auto& s : noise) { const float w = rnd.nextFloat() * 2.0f - 1.0f; p0 = 0.99f * p0 + 0.1f * w; p1 = 0.6f * p1 + 0.4f * w; s = 0.01f * (p0 + p1); }

    // 1. noise reduction
    {
        auto x = voice;
        for (size_t i = 0; i < x.size(); ++i) x[i] += noise[i];
        auto y = x;
        std::atomic<bool> c { false };
        mixtools::reduceNoise (y, SR, 12.0f, nullptr, c);
        const size_t gapA = 38400 + 4800, gapB = 38400 + 19200;   // inside the first pause
        const double before = rmsOf (x, gapA, gapB), after = rmsOf (y, gapA, gapB);
        const double vIn = rmsOf (x, 4800, 33600), vOut = rmsOf (y, 4800, 33600);
        TFCHECK (db (after / before) < -9.0, "noise removal: fan/AC noise " + juce::String (db (after / before), 1) + " dB in the pauses");
        TFCHECK (std::abs (db (vOut / vIn)) < 1.0, "noise removal keeps the voice (" + juce::String (db (vOut / vIn), 2) + " dB)");
        double c2 = 0; lagOf (voice, y, 4800, 24000, 0, c2);
        TFCHECK (c2 > 0.99, "cleaned voice matches the clean original (" + juce::String (c2, 4) + ")");
    }
    // 2. music leaking from the speakers into the mic is removed
    {
        const size_t n = voice.size();
        std::vector<float> music (n);
        double a = 0, b = 0;
        for (size_t i = 0; i < n; ++i)
        {
            a += 2.0 * juce::MathConstants<double>::pi * 110.0 / SR; b += 2.0 * juce::MathConstants<double>::pi * 329.6 / SR;
            music[i] = 0.2f * (float) (std::sin (a) + 0.5 * std::sin (b) + 0.3 * std::sin (3.0 * b)) + 0.05f * (rnd.nextFloat() * 2.0f - 1.0f);
        }
        // room: 30 ms away, a few reflections and a short decaying tail
        std::vector<float> h (4800, 0.0f);
        h[0] = 0.35f; h[480] = 0.12f; h[1300] = -0.08f;
        for (size_t i = 200; i < h.size(); ++i) h[i] += 0.02f * (rnd.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-(double) i / 900.0);
        const int delay = 1440;
        std::vector<float> mic (n, 0.0f);
        for (size_t i = 0; i < n; ++i)
        {
            double e = 0;
            for (size_t k = 0; k < h.size(); ++k) if (i >= k + delay) e += h[k] * music[i - k - delay];
            mic[i] = voice[i] + (float) e + 0.0005f * (rnd.nextFloat() - 0.5f);
        }
        auto out = mic;
        std::atomic<bool> c { false };
        double erle = 0; int found = 0;
        const bool ok = mixtools::removeBleed (out, music, SR, nullptr, c, &erle, &found);
        TFCHECK (ok && std::abs (found - delay) < 64, "speaker delay found (" + juce::String (found) + " samples)");
        // music alone (a pause late in the take): how much is left?
        const size_t g0 = n - 24000 + 2400, g1 = n - 1200;
        const double leakIn = rmsOf (mic, g0, g1), leakOut = rmsOf (out, g0, g1);
        TFCHECK (db (leakOut / leakIn) < -15.0, "leaked music removed: " + juce::String (db (leakOut / leakIn), 1) + " dB");
        double cv = 0;
        const size_t ph0 = 6 * (38400 + 24000) + 4800;
        lagOf (voice, out, ph0, 24000, 0, cv);
        double cm = 0; lagOf (voice, mic, ph0, 24000, 0, cm);
        TFCHECK (cv > 0.97 && cv > cm, "the voice stays clean while singing over the music (" + juce::String (cm, 3) + " -> " + juce::String (cv, 3) + ")");
    }
    // 3. loudness meter (EBU R128): a -20 dBFS 997 Hz tone on both speakers reads -20.0 LUFS
    {
        juce::AudioBuffer<float> b (2, 48000 * 5);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / SR);
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        const double l = mixtools::integratedLufs (b, b.getNumSamples(), SR);
        TFCHECK (std::abs (l + 20.0) < 0.2, "loudness meter is accurate (" + juce::String (l, 2) + " LUFS, expected -20.0)");
    }
    // 4. smart mix: music steps back ~2.5 dB while singing and comes back after
    {
        const int n = 48000 * 4;
        std::vector<float> L ((size_t) n), R ((size_t) n), v ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i) L[(size_t) i] = R[(size_t) i] = 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / SR);
        auto sv = sungVowel (1.5, 260.0, 0.2f);
        std::copy (sv.begin(), sv.end(), v.begin() + 48000);
        auto L0 = L;
        MusicDucker d; d.prepare (SR);
        for (int i = 0; i < n; i += 256) d.process (L.data() + i, R.data() + i, v.data() + i, juce::jmin (256, n - i), 1.0f);
        const double during = db (rmsOf (L, 48000 + 24000, 48000 + 60000) / rmsOf (L0, 48000 + 24000, 48000 + 60000));
        const double after = db (rmsOf (L, 48000 * 3 + 24000, 48000 * 4) / rmsOf (L0, 48000 * 3 + 24000, 48000 * 4));
        TFCHECK (during < -1.8 && during > -3.5, "smart mix: music gives way while singing (" + juce::String (during, 1) + " dB)");
        TFCHECK (std::abs (after) < 0.3, "smart mix: music comes back after (" + juce::String (after, 2) + " dB)");
    }
    // 5. master bus: never over -1 dBFS, glue is gentle
    {
        const int n = 48000 * 3;
        std::vector<float> L ((size_t) n), R ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float v = (float) (1.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 150.0 * i / SR) * (0.6 + 0.4 * std::sin (i / 9000.0)));
            L[(size_t) i] = v; R[(size_t) i] = 0.8f * v;
        }
        MasterBus m; m.prepare (SR);
        for (int i = 0; i < n; i += 512) m.process (L.data() + i, R.data() + i, juce::jmin (512, n - i), true);
        float pk = 0; for (int i = 0; i < n; ++i) pk = juce::jmax (pk, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]));
        TFCHECK (pk <= 0.9f, "master never goes over -1 dBFS (peak " + juce::String (juce::Decibels::gainToDecibels (pk), 2) + " dBFS)");
    }
    // 6. fades
    {
        juce::AudioBuffer<float> b (2, 48000 * 4);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (ch, i, 0.5f);
        mixtools::applyFades (b, b.getNumSamples(), SR, 2.0, 2.0);
        TFCHECK (std::abs (b.getSample (0, 0)) < 1e-4f && std::abs (b.getSample (1, b.getNumSamples() - 1)) < 1e-3f && std::abs (b.getSample (0, 96000) - 0.5f) < 1e-3f,
                 "fade in / fade out shape");
    }
    // 7. sync measurement finds a click delay
    {
        std::vector<float> played (48000 * 2, 0.0f), heard (48000 * 2, 0.0f);
        for (int b = 0; b < 4; ++b) for (int i = 0; i < 576; ++i) played[(size_t) (7200 + 21600 * b + i)] = 0.5f * (rnd.nextFloat() * 2.0f - 1.0f) * std::sin (juce::MathConstants<float>::pi * i / 576.0f);
        for (size_t i = 0; i < heard.size(); ++i) heard[i] = (i >= 2345 ? 0.2f * played[i - 2345] : 0.0f) + 0.01f * (rnd.nextFloat() - 0.5f);
        const int d = mixtools::findClickDelay (heard, played, 28800);
        TFCHECK (d == 2345, "automatic sync measurement finds the delay (" + juce::String (d) + " samples, expected 2345)");
    }
}
}
