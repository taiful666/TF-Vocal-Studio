// Automated test for the TF Vocal Studio audio engine using a simulated sound card.
// The fake device feeds the speaker output back into the microphone with a known delay
// (like a singer singing exactly in time with what they hear), so we can verify that
// recordings line up perfectly with the song after latency compensation.

#include "AudioEngine.h"
#include "Lyrics.h"
#include "VocalFx.h"
#include "Media.h"
#include "Project.h"
#include "Stretch.h"
#include "VideoExport.h"
#include <juce_events/juce_events.h>
#include <random>

using namespace tf;

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) std::cout << "  PASS  " << msg << "\n"; else { std::cout << "  FAIL  " << msg << "\n"; ++failures; } } while (0)

#include "FakeDevice.h"
#define TFCHECK(cond, msg) do { if (cond) std::cout << "  PASS  " << msg << "\n"; else { std::cout << "  FAIL  " << msg << "\n"; ++failures; } } while (0)
#include "QualityTests.h"

//==============================================================================
static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

static std::vector<float> readMono (const juce::File& f, juce::int64 from = 0, juce::int64 n = -1)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) return {};
    if (n < 0) n = r->lengthInSamples - from;
    juce::AudioBuffer<float> b (1, (int) n);
    r->read (&b, 0, (int) n, from, true, false);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + n);
}

static int bestLag (const std::vector<float>& a, const std::vector<float>& b, int maxLag, double& corrOut)
{
    // returns lag L maximising sum a[i] * b[i + L]
    int best = 0; double bestC = -1e30;
    const int n = (int) juce::jmin (a.size(), b.size()) - 2 * maxLag;
    double ea = 0, eb = 0;
    for (int i = maxLag; i < maxLag + n; ++i) { ea += a[(size_t) i] * a[(size_t) i]; eb += b[(size_t) i] * b[(size_t) i]; }
    for (int L = -maxLag; L <= maxLag; ++L)
    {
        double c = 0;
        for (int i = maxLag; i < maxLag + n; ++i) c += a[(size_t) i] * b[(size_t) (i + L)];
        if (c > bestC) { bestC = c; best = L; }
    }
    corrOut = bestC / std::sqrt (ea * eb + 1e-12);
    return best;
}

static double rms (const std::vector<float>& v) { double s = 0; for (auto x : v) s += x * x; return std::sqrt (s / juce::jmax ((size_t) 1, v.size())); }


//==============================================================================
static float estimateHz (const std::vector<float>& x, double sr)
{
    // count rising zero crossings with interpolation
    double first = -1, last = -1; int count = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0 && x[i] >= 0)
        {
            const double t = (double) (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t; else ++count;
            last = t;
        }
    return count > 0 ? (float) (count * sr / (last - first)) : 0.0f;
}

//==============================================================================
// Every effect, measured one by one
//==============================================================================
namespace fxtest
{
    constexpr double sr = 48000.0;
    static std::vector<float> tone (double hz, double seconds, float amp)
    {
        std::vector<float> v ((size_t) (sr * seconds));
        for (size_t i = 0; i < v.size(); ++i) v[i] = amp * (float) std::sin (2 * juce::MathConstants<double>::pi * hz * (double) i / sr);
        return v;
    }
    static std::vector<float> vowel (double seconds, float amp)
    {
        std::vector<float> v ((size_t) (sr * seconds));
        double ph = 0;
        for (size_t i = 0; i < v.size(); ++i)
        {
            ph += 2 * juce::MathConstants<double>::pi * 220.0 / sr;
            v[i] = amp * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph) + 0.3 * std::sin (3 * ph) + 0.15 * std::sin (5 * ph));
        }
        return v;
    }
    struct Out { std::vector<float> L, R; int latency = 0; };
    static Out run (const FxParams& p, const std::vector<float>& in, bool live = false, bool tuneInPath = true)
    {
        VocalChain ch (p);
        ch.prepare (sr, 512, tuneInPath);
        Out o; o.L.resize (in.size()); o.R.resize (in.size());
        for (size_t i = 0; i < in.size(); i += 256)
        {
            const int n = (int) juce::jmin ((size_t) 256, in.size() - i);
            ch.process (in.data() + i, o.L.data() + i, o.R.data() + i, n, live);
        }
        o.latency = ch.latencySamples (live);
        return o;
    }
    static void makeNeutral (FxParams& p)
    {
        for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, false);
        for (int i = 0; i < FxParams::numParams; ++i) p.set ((FxParams::Param) i, 50.0f);
        p.set (FxParams::tuneAmt, 0.0f);
        p.bypass = false; p.liveTune = false;
    }
    static double rmsRange (const std::vector<float>& v, size_t a, size_t b)
    {
        double s = 0; b = juce::jmin (b, v.size());
        for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i];
        return std::sqrt (s / (double) juce::jmax ((size_t) 1, b - a));
    }
    static double dB (double x) { return 20.0 * std::log10 (x + 1e-12); }
    /** level of one frequency (Goertzel) over a range */
    static double levelAt (const std::vector<float>& v, double hz, size_t a, size_t b)
    {
        const double w = 2 * juce::MathConstants<double>::pi * hz / sr, c = 2 * std::cos (w);
        double s1 = 0, s2 = 0;
        for (size_t i = a; i < b; ++i) { const double s0 = v[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        const double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        return std::sqrt (juce::jmax (0.0, pw)) * 2.0 / (double) (b - a);
    }
}

static void testEveryEffect()
{
    using namespace fxtest;
    std::cout << "\nEvery effect, one by one\n";
    const auto v = vowel (3.0, 0.12f);

    // bypass and "everything off" are exactly the dry voice (only delayed)
    {
        FxParams p; makeNeutral (p);
        p.bypass = true;
        auto o = run (p, v);
        double e = 0, r = 0;
        for (size_t i = 48000; i < v.size(); ++i) { const double d = o.L[i] - v[i - (size_t) o.latency]; e += d * d; r += (double) v[i] * v[i]; }
        CHECK (10 * std::log10 (r / (e + 1e-15)) > 60.0, "A/B (effects off) = the dry voice exactly");
        p.bypass = false;
        auto o2 = run (p, v);
        e = 0;
        for (size_t i = 48000; i < v.size(); ++i) { const double d = o2.L[i] - v[i - (size_t) o2.latency]; e += d * d; }
        CHECK (10 * std::log10 (r / (e + 1e-15)) > 40.0, "all effects switched off = clean voice");
    }

    // EQ: bass, middle, brightness move their own band by the expected amount
    {
        struct Band { FxParams::Param prm; double hz; double expectDb; const char* name; };
        for (auto b : { Band { FxParams::bass, 80.0, 9.0, "bass +100: 80 Hz louder" }, Band { FxParams::mid, 1800.0, 6.0, "middle +100: 1.8 kHz louder" },
                        Band { FxParams::bright, 12000.0, 8.0, "brightness +100: 12 kHz louder" } })
        {
            auto t = tone (b.hz, 1.0, 0.1f);
            FxParams p; makeNeutral (p); p.setOn (FxParams::toneOn, true);
            auto flat = run (p, t);
            p.set (b.prm, 100.0f);
            auto up = run (p, t);
            p.set (b.prm, 0.0f);
            auto down = run (p, t);
            const double gUp = dB (levelAt (up.L, b.hz, 24000, 48000) / levelAt (flat.L, b.hz, 24000, 48000));
            const double gDn = dB (levelAt (down.L, b.hz, 24000, 48000) / levelAt (flat.L, b.hz, 24000, 48000));
            CHECK (std::abs (gUp - b.expectDb) < 1.5 && std::abs (gDn + b.expectDb) < 1.5,
                   juce::String (b.name) + " (+" + juce::String (gUp, 1) + " dB / " + juce::String (gDn, 1) + " dB at 0)");
        }
        // the other bands stay put
        auto t = tone (1000.0, 1.0, 0.1f);
        FxParams p; makeNeutral (p); p.setOn (FxParams::toneOn, true);
        auto flat = run (p, t);
        p.set (FxParams::bass, 100.0f);
        auto up = run (p, t);
        const double g = dB (levelAt (up.L, 1000.0, 24000, 48000) / levelAt (flat.L, 1000.0, 24000, 48000));
        CHECK (std::abs (g) < 1.5, "bass knob leaves 1 kHz alone (" + juce::String (g, 1) + " dB)");
    }

    // compressor ("full voice"): loud and soft parts end up closer together, never clipping
    {
        std::vector<float> in;
        for (int k = 0; k < 6; ++k) { auto seg = vowel (0.5, k % 2 == 0 ? 0.25f : 0.012f); in.insert (in.end(), seg.begin(), seg.end()); }
        FxParams p; makeNeutral (p); p.setOn (FxParams::bodyOn, true); p.set (FxParams::body, 80.0f); p.set (FxParams::warm, 0.0f);
        auto o = run (p, in);
        const size_t L = (size_t) o.latency, seg = 24000;
        const double inRange = dB (rmsRange (in, seg * 2 + 4000, seg * 3) / rmsRange (in, seg * 3 + 4000, seg * 4));
        const double outRange = dB (rmsRange (o.L, seg * 2 + 4000 + L, seg * 3 + L) / rmsRange (o.L, seg * 3 + 4000 + L, seg * 4 + L));
        float peak = 0; for (auto x : o.L) peak = juce::jmax (peak, std::abs (x));
        CHECK (outRange < inRange - 8.0, "full voice evens out loud/soft: " + juce::String (inRange, 1) + " dB -> " + juce::String (outRange, 1) + " dB");
        CHECK (peak < 1.0f, "full voice never clips (peak " + juce::String (peak, 2) + ")");
    }

    // warmth adds gentle harmonics
    {
        auto t = tone (300.0, 1.0, 0.2f);
        FxParams p; makeNeutral (p); p.setOn (FxParams::bodyOn, true); p.set (FxParams::body, 0.0f); p.set (FxParams::warm, 0.0f);
        auto clean = run (p, t);
        p.set (FxParams::warm, 100.0f);
        auto warm = run (p, t);
        auto h3 = [] (const std::vector<float>& x) { return levelAt (x, 900.0, 24000, 48000) / levelAt (x, 300.0, 24000, 48000); };
        CHECK (h3 (warm.L) > 10.0 * h3 (clean.L) && h3 (warm.L) > 0.003, "warmth adds harmonics (3rd: " + juce::String (dB (h3 (clean.L)), 0) + " -> " + juce::String (dB (h3 (warm.L)), 0) + " dB)");
    }

    // de-esser: an "s" gets softer, the vowel does not
    {
        std::vector<float> in = vowel (2.0, 0.08f);
        juce::Random rnd (5);
        float hp = 0, last = 0;
        for (size_t i = 48000; i < 72000; ++i) { const float w = rnd.nextFloat() * 2 - 1; hp = 0.6f * (hp + w - last); last = w; in[i] += 0.35f * hp; }
        FxParams p; makeNeutral (p); p.setOn (FxParams::deessOn, false);
        auto off = run (p, in);
        p.setOn (FxParams::deessOn, true); p.set (FxParams::deess, 100.0f);
        auto on = run (p, in);
        auto hi = [] (const std::vector<float>& x, size_t a, size_t b)
        {
            juce::dsp::IIR::Filter<float> f; f.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 6000.0f);
            std::vector<float> y (x.begin() + (long) a, x.begin() + (long) b); for (auto& s : y) s = f.processSample (s);
            return rmsRange (y, 2000, y.size());
        };
        const size_t L = (size_t) on.latency;
        const double sCut = dB (hi (on.L, 50000 + L, 70000 + L) / hi (off.L, 50000 + L, 70000 + L));
        const double vowelChange = dB (rmsRange (on.L, 10000 + L, 40000 + L) / rmsRange (off.L, 10000 + L, 40000 + L));
        CHECK (sCut < -4.0, "de-esser softens the \"s\" by " + juce::String (-sCut, 1) + " dB");
        CHECK (std::abs (vowelChange) < 0.5, "de-esser leaves the vowel alone (" + juce::String (vowelChange, 2) + " dB)");
    }

    // double voice: a second, slightly later copy, spread left/right
    {
        FxParams p; makeNeutral (p); p.setOn (FxParams::dblOn, true); p.set (FxParams::dbl, 100.0f);
        auto o = run (p, v);
        double d = 0, e = 0;
        for (size_t i = 48000; i < v.size(); ++i) { d += std::abs (o.L[i] - o.R[i]); e += std::abs (o.L[i]); }
        FxParams p0; makeNeutral (p0);
        auto dry = run (p0, v);
        std::vector<float> wet (v.size());
        for (size_t i = 0; i < v.size(); ++i) wet[i] = o.L[i] - dry.L[i];
        const double copy = dB (rmsRange (wet, 48000, v.size()) / rmsRange (dry.L, 48000, v.size()));
        CHECK (d / e > 0.1 && copy > -12.0, "double voice adds a spread copy (L/R differ " + juce::String (100.0 * d / e, 0) + "%, copy at " + juce::String (copy, 1) + " dB)");
    }

    // echo: repeats come back at the chosen distance, ping-pong left/right
    for (float etime : { 0.0f, 100.0f })
    {
        std::vector<float> in ((size_t) (sr * 2.0), 0.0f);
        for (int i = 0; i < 480; ++i) in[(size_t) (4800 + i)] = 0.5f * (float) std::sin (2 * juce::MathConstants<double>::pi * 1000.0 * i / sr) * (float) std::sin (juce::MathConstants<double>::pi * i / 480.0);
        FxParams p; makeNeutral (p); p.setOn (FxParams::echoOn, true); p.set (FxParams::echo, 100.0f); p.set (FxParams::etime, etime);
        auto o = run (p, in);
        const int expect = (int) (sr * (0.09 + etime / 100.0 * 0.46));
        const size_t base = (size_t) (4800 + o.latency);
        // the first repeat appears on the left channel at the echo delay
        size_t best = 0; float bestV = 0;
        for (size_t i = base + 2400; i < juce::jmin (o.L.size(), base + (size_t) expect + 6000); ++i) if (std::abs (o.L[i]) > bestV) { bestV = std::abs (o.L[i]); best = i; }
        const double ms = ((double) best - (double) base - 240.0) / sr * 1000.0;
        CHECK (std::abs (ms - expect / sr * 1000.0) < 15.0 && bestV > 0.02f,
               "echo distance " + juce::String ((int) etime) + "%: repeat after " + juce::String (ms, 0) + " ms (expected " + juce::String (expect / sr * 1000.0, 0) + ")");
        float r = 0; for (size_t i = base + (size_t) expect + 2000; i < juce::jmin (o.R.size(), base + (size_t) expect * 2 + 6000); ++i) r = juce::jmax (r, std::abs (o.R[i]));
        CHECK (r > 0.005f, "echo bounces to the right side");
    }

    // reverb: a tail after the sound; bigger room = longer tail; off = no tail
    {
        std::vector<float> in ((size_t) (sr * 3.0), 0.0f);
        for (int i = 0; i < 2400; ++i) in[(size_t) (4800 + i)] = 0.4f * (float) std::sin (2 * juce::MathConstants<double>::pi * 440.0 * i / sr);
        auto tail = [&] (float room, bool on)
        {
            FxParams p; makeNeutral (p); p.setOn (FxParams::revOn, on); p.set (FxParams::rev, 100.0f); p.set (FxParams::room, room);
            auto o = run (p, in);
            const size_t end = (size_t) (7200 + o.latency);
            return std::make_pair (rmsRange (o.L, end + 4800, end + 19200), rmsRange (o.L, end + 48000, end + 72000));
        };
        auto small = tail (10.0f, true), big = tail (90.0f, true), off = tail (90.0f, false);
        CHECK (small.first > 0.003, "reverb leaves a tail after the voice (" + juce::String (dB (small.first), 0) + " dBFS)");
        CHECK (big.second > 3.0 * small.second, "bigger room = longer tail (1 s later: " + juce::String (dB (small.second), 0) + " vs " + juce::String (dB (big.second), 0) + " dBFS)");
        CHECK (off.first < 1e-5, "reverb off = no tail");
    }

    // live monitor: small delay unless live auto-tune is chosen
    {
        FxParams p; makeNeutral (p);
        VocalChain ch (p); ch.prepare (sr, 512, false);
        const int lat = ch.latencySamples (true);
        p.setOn (FxParams::tuneOn, true); p.set (FxParams::tuneAmt, 60.0f); p.liveTune = true;
        const int latTune = ch.latencySamples (true);
        CHECK (lat == 0, "live mic without auto-tune: no extra delay");
        CHECK (latTune > 0 && latTune < 2500, "live auto-tune adds " + juce::String (latTune / 48.0, 0) + " ms (only when chosen)");
    }

    // every preset: works in record (playback) and live mode, stays clean and in level
    {
        std::vector<float> in;
        for (int k = 0; k < 4; ++k) { auto seg = vowel (0.5, k % 2 == 0 ? 0.1f : 0.03f); in.insert (in.end(), seg.begin(), seg.end()); }
        for (auto& pr : builtInPresets())
        {
            FxParams p; pr.applyTo (p);
            p.scaleKey = 9;
            for (bool live : { false, true })
            {
                auto o = run (p, in, live, ! live);
                bool finite = true; float peak = 0;
                for (size_t i = 0; i < in.size(); ++i) { finite &= std::isfinite (o.L[i]) && std::isfinite (o.R[i]); peak = juce::jmax (peak, std::abs (o.L[i]), std::abs (o.R[i])); }
                const double lvl = dB (rmsRange (o.L, 24000, in.size()) / rmsRange (in, 24000, in.size()));
                CHECK (finite && peak < 1.0f && lvl > -6.0 && lvl < 12.0,
                       "preset " + pr.name + (live ? " (live)" : " (record)") + ": level " + juce::String (lvl, 1) + " dB, peak " + juce::String (peak, 2));
            }
            // the preset actually changes the sound
            FxParams pn; makeNeutral (pn); auto dry = run (pn, in);
            auto o = run (p, in);
            double d = 0, r = 0;
            for (size_t i = 24000; i < in.size(); ++i) { const double x = o.L[i] - dry.L[i]; d += x * x; r += (double) dry.L[i] * dry.L[i]; }
            CHECK (pr.id == "natural" || d > 0.01 * r, "preset " + pr.name + " changes the sound");
        }
    }
}

static void testEffects()
{
    const double sr = 48000.0;
    std::cout << "\nEffects engine\n";
    CHECK (builtInPresets().size() == 22, "22 built-in presets");

    // auto-tune transparency at amount 0 (sung vowel-like signal: harmonics + vibrato)
    {
        AutoTune at; at.prepare (sr);
        const int N = 48000 * 2;
        std::vector<float> in ((size_t) N), out ((size_t) N);
        double ph = 0;
        for (int i = 0; i < N; ++i)
        {
            ph += 2 * juce::MathConstants<double>::pi * (220.0 * std::pow (2.0, 0.15 * std::sin (i / sr * 2 * juce::MathConstants<double>::pi * 5.0) / 12.0)) / sr;
            in[(size_t) i] = (float) (0.3 * std::sin (ph) + 0.15 * std::sin (2 * ph) + 0.08 * std::sin (3 * ph));
        }
        for (int i = 0; i < N; i += 128) at.process (in.data() + i, out.data() + i, 128, 0.0f, 50.0f, -1);
        const int L = at.latencySamples();
        double err = 0, ref = 0;
        for (int i = 24000; i < N; ++i) { const double d = out[(size_t) i] - in[(size_t) (i - L)]; err += d * d; ref += in[(size_t) (i - L)] * in[(size_t) (i - L)]; }
        const auto snr = 10.0 * std::log10 (ref / (err + 1e-12));
        CHECK (snr > 25.0, "auto-tune at 0% is transparent (SNR " + juce::String (snr, 1) + " dB)");
    }

    // auto-tune pulls a sharp note onto the right pitch
    for (float cents : { 40.0f, -35.0f })
    {
        AutoTune at; at.prepare (sr);
        const int N = 48000 * 3;
        std::vector<float> in ((size_t) N), out ((size_t) N);
        const double f = 440.0 * std::pow (2.0, cents / 1200.0);
        double ph = 0;
        for (int i = 0; i < N; ++i) { ph += 2 * juce::MathConstants<double>::pi * f / sr; in[(size_t) i] = (float) (0.3 * std::sin (ph) + 0.1 * std::sin (2 * ph)); }
        for (int i = 0; i < N; i += 256) at.process (in.data() + i, out.data() + i, juce::jmin (256, N - i), 1.0f, 100.0f, -1);
        std::vector<float> tail (out.begin() + 72000, out.end());
        // remove 2nd harmonic influence by a crude low-pass before zero-crossing count
        float s = 0; for (auto& v : tail) { s += 0.08f * (v - s); v = s; }
        const auto hz = estimateHz (tail, sr);
        const auto errCents = 1200.0 * std::log2 (hz / 440.0);
        CHECK (std::abs (errCents) < 8.0, "auto-tune corrects " + juce::String (cents, 0) + " cents -> " + juce::String (errCents, 1) + " cents off A4");
    }

    // full chain: every effect on, stable, no NaN, sensible level, CPU cost
    {
        FxParams p;
        for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, true);
        builtInPresets()[11].applyTo (p);   // band rock
        p.setOn (FxParams::dblOn, true);
        VocalChain ch (p);
        ch.prepare (sr, 512, false);   // as used for playback: the pitch correction runs ahead of time on another thread
        const int N = 48000 * 10;
        std::vector<float> in ((size_t) N), L ((size_t) N), R ((size_t) N);
        juce::Random rnd (3);
        double ph = 0;
        for (int i = 0; i < N; ++i)
        {
            ph += 2 * juce::MathConstants<double>::pi * 196.0 / sr;
            const float env = (i / 24000) % 2 == 0 ? 1.0f : 0.0f;
            in[(size_t) i] = env * (float) (0.25 * std::sin (ph) + 0.1 * std::sin (2 * ph) + 0.05 * std::sin (3 * ph)) + 0.002f * (rnd.nextFloat() - 0.5f);
        }
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < N; i += 128) ch.process (in.data() + i, L.data() + i, R.data() + i, 128, false);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        bool finite = true; float peak = 0;
        for (int i = 0; i < N; ++i) { finite &= std::isfinite (L[(size_t) i]) && std::isfinite (R[(size_t) i]); peak = juce::jmax (peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i])); }
        CHECK (finite, "chain output is finite (no NaN/inf)");
        CHECK (peak > 0.1f && peak < 2.0f, "chain level sensible (peak " + juce::String (peak, 2) + ")");
        std::cout << "        full chain CPU (audio thread): " << juce::String (100.0 * ms / 10000.0, 2) << "% of one core\n";
        CHECK (ms < 1000.0, "full effects chain runs far faster than real time");
        // the studio pitch correction (background thread) must also keep well ahead of real time
        {
            SmoothTune st; st.prepare (sr, 1024);
            std::vector<float> o2 ((size_t) N);
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < N; i += 1024) st.process (in.data() + i, o2.data() + i, juce::jmin (1024, N - i), true, 0.6f, 50.0f, -1);
            const auto ms2 = juce::Time::getMillisecondCounterHiRes() - t1;
            std::cout << "        pitch correction CPU (read-ahead thread): " << juce::String (100.0 * ms2 / 10000.0, 2) << "% of one core\n";
            CHECK (ms2 < 5000.0 && st.getUnderruns() == 0, "pitch correction keeps ahead of real time, no gaps");
        }
        // stereo spread from doubler/echo/reverb
        double diff = 0; for (int i = 0; i < N; ++i) diff += std::abs (L[(size_t) i] - R[(size_t) i]);
        CHECK (diff > 1.0, "effects produce a stereo image");
    }

    // key detection on a synthetic C major progression
    {
        juce::WavAudioFormat wav;
        juce::MemoryBlock mb;
        {
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::MemoryOutputStream> (mb, false);
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (1).withBitsPerSample (16));
            juce::AudioBuffer<float> b (1, 44100 * 16);
            const int chords[4][3] = { { 60, 64, 67 }, { 65, 69, 72 }, { 67, 71, 74 }, { 60, 64, 67 } };
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const auto& c = chords[(i / (44100 * 2)) % 4];
                float v = 0;
                for (int k = 0; k < 3; ++k) { const double f = 440.0 * std::pow (2.0, (c[k] - 69) / 12.0); v += 0.1f * (float) std::sin (2 * juce::MathConstants<double>::pi * f * i / 44100.0); }
                const double bass = 440.0 * std::pow (2.0, (c[0] - 12 - 69) / 12.0);
                v += 0.12f * (float) std::sin (2 * juce::MathConstants<double>::pi * bass * i / 44100.0);
                b.setSample (0, i, v);
            }
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (new juce::MemoryInputStream (mb, false), true));
        const int key = detectSongKey (*r);
        CHECK (key == 0, "key detection finds C major (got " + keyName (key) + ")");
    }
}



//==============================================================================
// Every effect, one by one: does it do what its name promises?
//==============================================================================
struct ChainOut { std::vector<float> L, R; int lat = 0; };

static ChainOut runChain (FxParams& p, const std::vector<float>& in, bool live = false)
{
    VocalChain ch (p);
    ch.prepare (48000.0, 512, ! live);
    ChainOut o;
    o.L.assign (in.size(), 0.0f); o.R.assign (in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += 256)
    {
        const int n = (int) juce::jmin ((size_t) 256, in.size() - i);
        ch.process (in.data() + i, o.L.data() + i, o.R.data() + i, n, live);
    }
    o.lat = ch.latencySamples (live);
    return o;
}

static void allOff (FxParams& p)
{
    for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, false);
    p.set (FxParams::tuneAmt, 0.0f);
}

static std::vector<float> sine (double hz, float amp, int n, double sr = 48000.0)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (2 * juce::MathConstants<double>::pi * hz * i / sr);
    return v;
}

/** Level of one frequency (Goertzel), in dB, over [from, to). */
static double toneDb (const std::vector<float>& x, double hz, size_t from, size_t to, double sr = 48000.0)
{
    const double w = 2 * juce::MathConstants<double>::pi * hz / sr, c = 2 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < juce::jmin (to, x.size()); ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
    const double n = (double) (juce::jmin (to, x.size()) - from);
    return 10.0 * std::log10 (pw / (n * n) + 1e-20);
}

static double energyDb (const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0;
    for (size_t i = from; i < juce::jmin (to, x.size()); ++i) e += (double) x[i] * x[i];
    return 10.0 * std::log10 (e / (double) juce::jmax ((size_t) 1, juce::jmin (to, x.size()) - from) + 1e-20);
}

static void testEachEffect()
{
    std::cout << "\nEvery effect checked on its own\n";
    const int N = 48000 * 2;
    const size_t a = 24000, b = (size_t) N;

    // EQ
    {
        auto eq = [&] (FxParams::Param prm, float v, double hz)
        {
            FxParams p; allOff (p); p.setOn (FxParams::toneOn, true); p.set (prm, v);
            auto o = runChain (p, sine (hz, 0.1f, N));
            return toneDb (o.L, hz, a, b);
        };
        const double bUp = eq (FxParams::bass, 100, 90) - eq (FxParams::bass, 50, 90), bDn = eq (FxParams::bass, 0, 90) - eq (FxParams::bass, 50, 90);
        CHECK (bUp > 6.0 && bDn < -6.0, "EQ 'মোটা ভাব' (bass): +" + juce::String (bUp, 1) + " dB / " + juce::String (bDn, 1) + " dB at 90 Hz");
        const double mUp = eq (FxParams::mid, 100, 1800) - eq (FxParams::mid, 50, 1800);
        CHECK (mUp > 4.0, "EQ 'মাঝের সুর' (mid): +" + juce::String (mUp, 1) + " dB at 1.8 kHz");
        const double hUp = eq (FxParams::bright, 100, 11000) - eq (FxParams::bright, 50, 11000);
        CHECK (hUp > 5.0, "EQ 'উজ্জ্বলতা' (bright): +" + juce::String (hUp, 1) + " dB at 11 kHz");
        const double flat = eq (FxParams::bass, 50, 1000) - toneDb (sine (1000, 0.1f, N), 1000, a, b);
        CHECK (std::abs (flat) < 0.5, "EQ at 50% is flat (" + juce::String (flat, 2) + " dB)");
    }

    // full voice (compressor): loud and soft parts come closer together
    {
        std::vector<float> in ((size_t) N * 2);
        for (size_t i = 0; i < in.size(); ++i) in[i] = ((i / 24000) % 2 == 0 ? 0.5f : 0.05f) * (float) std::sin (2 * juce::MathConstants<double>::pi * 200.0 * (double) i / 48000.0);
        auto loudSoft = [&] (float body)
        {
            FxParams p; allOff (p); p.setOn (FxParams::bodyOn, true); p.set (FxParams::body, body); p.set (FxParams::warm, 0.0f);
            auto o = runChain (p, in);
            return energyDb (o.L, 48000 + 6000, 72000) - energyDb (o.L, 72000 + 6000, 96000);   // loud block minus soft block
        };
        const double dryGap = 20.0, gap = loudSoft (100.0f);
        CHECK (gap < dryGap - 6.0, "'ভরাট গলা' (compressor) evens the voice: 20 dB gap -> " + juce::String (gap, 1) + " dB");
    }

    // warmth adds harmonics (the thick, grainy band voice)
    {
        auto h3 = [&] (float warm)
        {
            FxParams p; allOff (p); p.setOn (FxParams::bodyOn, true); p.set (FxParams::body, 0.0f); p.set (FxParams::warm, warm);
            auto o = runChain (p, sine (200, 0.4f, N));
            return toneDb (o.L, 600, a, b) - toneDb (o.L, 200, a, b);
        };
        const double clean = h3 (0.0f), warm = h3 (100.0f);
        CHECK (warm > clean + 15.0, "'উষ্ণতা' adds harmonics (3rd harmonic " + juce::String (clean, 0) + " -> " + juce::String (warm, 0) + " dB)");
    }

    // reverb: a tail after the voice stops, longer for a bigger room
    {
        std::vector<float> in ((size_t) N * 2, 0.0f);
        for (int i = 0; i < 4800; ++i) in[(size_t) i] = 0.3f * (float) std::sin (2 * juce::MathConstants<double>::pi * 400.0 * i / 48000.0);
        auto tail = [&] (bool on, float room, size_t from, size_t to)
        {
            FxParams p; allOff (p); p.setOn (FxParams::revOn, on); p.set (FxParams::rev, 80.0f); p.set (FxParams::room, room);
            auto o = runChain (p, in);
            return energyDb (o.L, from + (size_t) o.lat, juce::jmin (o.L.size(), to + (size_t) o.lat));
        };
        const double off = tail (false, 50, 9600, 48000), on = tail (true, 50, 9600, 48000);
        CHECK (on > off + 40.0 && on > -60.0, "'রিভার্ব' leaves a hall tail (" + juce::String (on, 0) + " dB vs off " + juce::String (off, 0) + " dB)");
        const double small = tail (true, 0, 48000, 96000), big = tail (true, 100, 48000, 96000);
        CHECK (big > small + 10.0, "'ঘরের আকার' makes the tail longer (1-2 s: " + juce::String (small, 0) + " -> " + juce::String (big, 0) + " dB)");
    }

    // echo: a repeat at the chosen distance
    {
        std::vector<float> in ((size_t) N * 2, 0.0f);
        for (int i = 0; i < 2400; ++i) in[(size_t) i] = 0.3f * (float) std::sin (2 * juce::MathConstants<double>::pi * 800.0 * i / 48000.0);
        auto echoAt = [&] (float etime)
        {
            FxParams p; allOff (p); p.setOn (FxParams::echoOn, true); p.set (FxParams::echo, 100.0f); p.set (FxParams::etime, etime);
            auto o = runChain (p, in);
            // find where the first repeat starts (after the dry burst)
            // the dry burst ends 0.05 s after the fixed effect delay; the first repeat comes after that
            for (size_t i = (size_t) o.lat + 2500; i < o.L.size(); ++i) if (std::abs (o.L[i]) > 0.01f) return (double) ((int) i - o.lat) / 48000.0;
            return -1.0;
        };
        const double near = echoAt (0.0f), far = echoAt (100.0f);
        CHECK (near > 0.08 && near < 0.12 && far > 0.5 && far < 0.6,
               "'ইকো' repeats at the set distance (" + juce::String (near, 2) + " s at 0%, " + juce::String (far, 2) + " s at 100%)");
    }

    // double voice: a second, slightly different voice on each side
    {
        auto side = [&] (bool on)
        {
            FxParams p; allOff (p); p.setOn (FxParams::dblOn, on); p.set (FxParams::dbl, 100.0f);
            auto o = runChain (p, sine (300, 0.2f, N));
            std::vector<float> d (o.L.size());
            for (size_t i = 0; i < d.size(); ++i) d[i] = o.L[i] - o.R[i];
            return energyDb (d, a, b);
        };
        const double off = side (false), on = side (true);
        CHECK (on > -40.0 && off < -100.0, "'ডাবল ভয়েস' widens left/right (" + juce::String (on, 0) + " dB vs off " + juce::String (off, 0) + ")");
    }

    // de-esser: hissing 's' turned down, the voice itself untouched
    {
        auto lvl = [&] (bool on, double hz)
        {
            FxParams p; allOff (p); p.setOn (FxParams::deessOn, on); p.set (FxParams::deess, 100.0f);
            auto o = runChain (p, sine (hz, 0.2f, N));
            return toneDb (o.L, hz, a, b);
        };
        const double s = lvl (true, 8000) - lvl (false, 8000), v = lvl (true, 300) - lvl (false, 300);
        CHECK (s < -6.0 && std::abs (v) < 0.5, "'ডি-এসার' cuts 's' hiss " + juce::String (s, 1) + " dB, voice " + juce::String (v, 2) + " dB");
    }

    // auto-tune follows the song's scale: a C# sung in C major goes to C or D
    {
        FxParams p; allOff (p); p.setOn (FxParams::tuneOn, true); p.set (FxParams::tuneAmt, 100.0f); p.set (FxParams::tuneSpd, 100.0f);
        p.scaleKey = 0;
        const double f = 440.0 * std::pow (2.0, (61.2 - 69) / 12.0);   // C#4, a little sharp
        std::vector<float> in ((size_t) N * 2);
        double ph = 0;
        for (auto& v : in) { ph += 2 * juce::MathConstants<double>::pi * f / 48000.0; v = (float) (0.3 * std::sin (ph) + 0.1 * std::sin (2 * ph)); }
        auto o = runChain (p, in);
        std::vector<float> tail (o.L.begin() + 72000, o.L.end());
        float sm = 0; for (auto& v : tail) { sm += 0.08f * (v - sm); v = sm; }
        const double midi = 69 + 12 * std::log2 (estimateHz (tail, 48000.0) / 440.0);
        const double toC = std::abs (midi - 60), toD = std::abs (midi - 62);
        CHECK (juce::jmin (toC, toD) < 0.15, "auto-tune uses the song's scale: C# in C major -> " + juce::String (midi, 2) + " (C=60, D=62)");
    }

    // A/B button: effects off = the voice exactly, only delayed by the fixed latency
    {
        FxParams p; builtInPresets()[4].applyTo (p);
        p.bypass = true;
        auto in = sine (250, 0.2f, N);
        auto o = runChain (p, in);
        double err = 0, ref = 0;
        for (size_t i = 24000; i < in.size(); ++i) { const double d = o.L[i] - in[i - (size_t) o.lat]; err += d * d; ref += (double) in[i] * in[i]; }
        const double snr = 10.0 * std::log10 (ref / (err + 1e-20));
        CHECK (snr > 40.0, "'আগে/পরে' gives the plain voice (match " + juce::String (snr, 0) + " dB)");
    }

    // every preset: works, never shifts the voice in time, no runaway level
    {
        std::vector<float> in ((size_t) N * 2);
        juce::Random rnd (5);
        double ph = 0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const double t = (double) i / 48000.0;
            ph += 2 * juce::MathConstants<double>::pi * (196.0 * std::pow (2.0, 0.2 * std::sin (t * 3.0) / 12.0)) / 48000.0;
            const float env = (float) (0.5 + 0.5 * std::sin (t * 2.1));
            in[i] = env * (float) (0.22 * std::sin (ph) + 0.09 * std::sin (2 * ph) + 0.04 * std::sin (3 * ph)) + 0.01f * (rnd.nextFloat() - 0.5f);
        }
        int lat0 = -1;
        juce::StringArray bad;
        for (auto& pr : builtInPresets())
        {
            FxParams p; pr.applyTo (p); p.scaleKey = -1;
            auto o = runChain (p, in);
            bool finite = true; float peak = 0;
            for (size_t i = 0; i < in.size(); ++i) { finite &= std::isfinite (o.L[i]) && std::isfinite (o.R[i]); peak = juce::jmax (peak, std::abs (o.L[i]), std::abs (o.R[i])); }
            if (lat0 < 0) lat0 = o.lat;
            // timing: a noisy "breath" has no repeating period, so the match is exact
            std::vector<float> noise (48000);
            for (auto& v : noise) v = 0.1f * (rnd.nextFloat() * 2.0f - 1.0f);
            FxParams pt; pr.applyTo (pt); pt.scaleKey = -1;
            auto on = runChain (pt, noise);
            std::vector<float> x (noise.begin() + 12000, noise.begin() + 36000), y (on.L.begin() + 12000 + on.lat, on.L.begin() + 36000 + on.lat);
            double c = 0; const int lag = bestLag (x, y, 300, c);
            const double lv = energyDb (o.L, 24000, in.size()) - energyDb (in, 24000, in.size());
            std::cout << "        " << pr.id.paddedRight (' ', 9) << " peak " << juce::String (peak, 2) << "  level " << juce::String (lv, 1) << " dB  lag " << lag << "  corr " << juce::String (c, 2) << "\n";
            if (! finite || peak < 0.05f || peak > 1.6f || o.lat != lat0 || std::abs (lag) > 8 || std::abs (lv) > 15.0)
                bad.add (pr.id);
        }
        CHECK (bad.isEmpty(), "all 22 presets: sound, in time (within 0.2 ms), sensible level" + (bad.isEmpty() ? juce::String() : " — problems: " + bad.joinIntoString (", ")));
    }

    // live microphone: low delay unless "live auto-tune" is chosen
    {
        FxParams p; builtInPresets()[4].applyTo (p);
        auto o = runChain (p, sine (220, 0.2f, N), true);
        CHECK (o.lat == 0, "live mic through the effects has no extra delay");
        p.liveTune = true;
        auto o2 = runChain (p, sine (220, 0.2f, N), true);
        CHECK (o2.lat > 0 && o2.lat <= 2100, "live auto-tune adds only its fixed delay (" + juce::String (o2.lat * 1000.0 / 48000.0, 0) + " ms)");
    }
}

//==============================================================================
static void testMedia (const juce::File& tmp, const juce::File& song)
{
    std::cout << "\nVideo / media import\n";
    if (! media::ffmpeg().existsAsFile()) { std::cout << "  SKIP  ffmpeg not found next to the test\n"; return; }

    // make a short test video (colour bars + tone) with ffmpeg itself
    auto vid = tmp.getChildFile ("lyrics video.mp4");
    juce::ChildProcess cp;
    cp.start (juce::StringArray { media::ffmpeg().getFullPathName(), "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=8",
                                  "-f", "lavfi", "-i", "sine=frequency=440:duration=8", "-shortest", "-pix_fmt", "yuv420p", vid.getFullPathName() });
    cp.waitForProcessToFinish (60000);
    CHECK (vid.existsAsFile(), "test video created");

    auto info = media::probe (vid);
    CHECK (info.hasVideo && info.hasAudio && info.width == 640 && std::abs (info.duration - 8.0) < 0.3, "video probed (640x360, ~8 s)");

    std::atomic<bool> cancel { false };
    float lastP = 0;
    auto wav = tmp.getChildFile ("vid.wav");
    auto err = media::extractAudio (vid, wav, 48000.0, [&] (float p) { lastP = p; }, cancel);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (wav));
    CHECK (err.isEmpty() && r != nullptr && std::abs ((double) r->lengthInSamples / r->sampleRate - 8.0) < 0.2 && std::abs (lastP - 1.0f) < 1e-6f,
           "audio extracted from video at 48 kHz with progress");

    VideoFrames vf;
    vf.open (vid);
    juce::Image img;
    for (int i = 0; i < 100 && ! img.isValid(); ++i) { img = vf.frameAt (3.0, false); juce::Thread::sleep (30); }
    CHECK (img.isValid() && img.getWidth() > 0, "video frame decoded at 3.0 s (" + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight()) + ")");
    // play forward: frames must keep coming in step with the clock
    int got = 0; juce::Image prev = img;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < 1500)
    {
        auto f = vf.frameAt (3.0 + (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, true);
        if (f.isValid() && ! (f == prev)) { ++got; prev = f; }
        juce::Thread::sleep (16);
    }
    CHECK (got >= 18, "video plays smoothly in sync (" + juce::String (got) + " new frames in 1.5 s)");
    vf.close();

    // MP4 export from a lyrics video: the song's own picture, in step with the music clip
    {
        ProjectState ps;
        TrackInfo t; t.id = "m"; t.kind = TrackKind::music; t.source = vid; t.baseAudio = wav;
        ClipInfo c; c.id = "c"; c.file = wav; c.start = 1.0; c.in = 0.0; c.out = 6.0;
        t.clips.add (c);
        ps.tracks.add (t);
        videoexport::Options vo;
        vo.state = ps; vo.mixWav = wav; vo.out = tmp.getChildFile ("from video.mp4"); vo.title = "v"; vo.rangeStart = 0.0; vo.length = 7.0;
        auto e = videoexport::render (vo, nullptr, cancel);
        auto pi = media::probe (vo.out);
        CHECK (e.isEmpty() && pi.hasVideo && pi.hasAudio && std::abs (pi.duration - 7.0) < 0.2, "MP4 from the lyrics video (" + juce::String (pi.duration, 2) + " s)" + (e.isEmpty() ? juce::String() : " " + e));
    }

}

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto testStart = juce::Time::getCurrentTime() - juce::RelativeTime::seconds (2);
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tfvs_test");
    tmp.deleteRecursively(); tmp.createDirectory();

    // ---- 1. Lyrics parser
    std::cout << "Lyrics parser\n";
    {
        auto l = Lyrics::parse (juce::String::fromUTF8 ("[ti:Test]\n[offset:500]\n[00:10.50]প্রথম লাইন\n[00:05.00][00:20.00]কোরাস\n<00:21.00>\n[01:02:50]শেষ"));
        CHECK (l.timed && l.lines.size() == 4, "timed lines parsed (incl. repeated tag)");
        CHECK (std::abs (l.lines[0].time - 4.5) < 1e-6, "[offset:500] applied and sorted");
        CHECK (std::abs (l.lines[3].time - 62.0) < 1e-6, "mm:ss:xx format");
        CHECK (l.indexAt (0.0) == -1 && l.indexAt (4.6) == 0 && l.indexAt (10.0) == 1 && l.indexAt (100.0) == 3, "indexAt");
        auto p = Lyrics::parse ("line one\nline two");
        CHECK (! p.timed && p.lines.size() == 2, "plain text lyrics");
    }

    testEffects();
    testEveryEffect();
    testEachEffect();
    quality::testVoiceQuality (failures);
    quality::testMixTools (failures);
    quality::testInputBridge (failures);
    if (juce::SystemStats::getEnvironmentVariable ("TF_QUICK", {}).isNotEmpty())
    {
        std::cout << (failures == 0 ? "\nQUICK: ALL PASSED\n" : "\nQUICK: FAILURES\n");
        return failures == 0 ? 0 : 1;
    }

    std::cout << "\nAI noise removal (offline)\n";
    {
        NoiseSuppressor ns;
        juce::Random rnd (3);
        std::vector<float> x (48000 * 4);
        for (auto& v : x) v = 0.05f * (rnd.nextFloat() * 2.0f - 1.0f);
        auto y = x;
        for (size_t i = 0; i < y.size(); i += 128) ns.process (y.data() + i, (int) juce::jmin ((size_t) 128, y.size() - i), 1.0f);
        std::vector<float> a (x.begin() + 48000, x.begin() + 96000), b (y.begin() + 48000, y.begin() + 96000);
        auto db = juce::Decibels::gainToDecibels ((float) (rms (b) / rms (a)));
        CHECK (db < -15.0f, "white hiss reduced by " + juce::String (-db, 1) + " dB (offline)");
        double sumDb = 0;
        for (int seed = 10; seed < 16; ++seed)
        {
            std::mt19937 g ((unsigned) seed); std::uniform_real_distribution<float> u (-1.0f, 1.0f);
            NoiseSuppressor n2; std::vector<float> xx (48000 * 6);
            float p0 = 0, p1 = 0, p2 = 0;
            for (auto& v : xx) { const float w = u (g); p0 = 0.99765f * p0 + w * 0.0990460f; p1 = 0.96300f * p1 + w * 0.2965164f; p2 = 0.57000f * p2 + w * 1.0526913f; v = 0.012f * (p0 + p1 + p2 + w * 0.1848f); }
            auto yy = xx; for (size_t i = 0; i < yy.size(); i += 128) n2.process (yy.data() + i, 128, 1.0f);
            std::vector<float> a2 (xx.begin() + 144000, xx.end()), b2 (yy.begin() + 144000, yy.end());
            sumDb += juce::Decibels::gainToDecibels ((float) (rms (b2) / rms (a2)));
        }
        CHECK (sumDb / 6.0 < -12.0, "fan-like noise reduced by " + juce::String (-sumDb / 6.0, 1) + " dB on average (offline, 6 noises)");
    }

    // ---- 2. test song: 48 kHz stereo, rich content for correlation
    auto song = tmp.getChildFile ("song.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (song);
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (2).withBitsPerSample (24));
        juce::AudioBuffer<float> b (2, 48000 * 20);
        juce::Random rnd (7);
        double phase = 0.0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            auto t = i / 48000.0;
            phase += 2 * juce::MathConstants<double>::pi * (330.0 + 60.0 * std::sin (t * 1.3)) / 48000.0;   // voice-range melody with vibrato
            float v = 0.2f * (float) std::sin (phase)
                    + 0.08f * (rnd.nextFloat() * 2 - 1);
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    AudioEngine engine;
    auto* type = new FakeType();
    engine.getDeviceManager().addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (type));
    engine.initialise (nullptr);
    engine.getDeviceManager().setCurrentAudioDeviceType ("FakeType", true);
    pump (300);

    std::cout << "\nDevice\n";
    CHECK (engine.isDeviceOpen(), "simulated device open");
    CHECK (engine.noiseAvailable(), "48 kHz -> AI noise removal available");
    CHECK (engine.getRoundTripLatencySamples() == FakeDevice::loopDelay + MasterBus::lookahead, "round-trip latency read from device (+ master look-ahead)");

    auto* dev = dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
    dev->positionProbe = [&engine] { return engine.getPosition(); };
    juce::String err;
    engine.setMonitorEnabled (false);   // no live-mic feedback in the loopback test

    // ---- project: music track + one voice track, neutral voice effects for exact comparisons
    std::cout << "\nProject\n";
    Project project;
    project.newProject ("engine test");
    CHECK (project.state().smartMix > 0.99f && project.state().glue, "new projects start with the smart mix and master glue on");
    project.setMix (0.0f, false);   // the mixing-arithmetic checks below need the plain sum
    const auto musicId = project.addMusic (song, song, 20.0);
    juce::String voiceId;
    for (auto& t : project.state().tracks) if (t.kind == TrackKind::vocal) voiceId = t.id;
    {
        auto* m = project.state().musicTrack();
        CHECK (m != nullptr && project.state().tracks.size() == 2 && project.state().tracks.getReference (0).id == musicId, "music track first, one voice track");
        CHECK (std::abs (project.state().length() - 20.0) < 1e-6, "project length = music length");
        FxParams neutral;
        for (int i = 0; i < FxParams::numSwitches; ++i) neutral.setOn ((FxParams::Switch) i, false);
        neutral.set (FxParams::tuneAmt, 0.0f);
        auto v = project.state().tracks.getReference (project.state().indexOf (voiceId));
        v.fx = neutral.toString();
        v.gain = 1.0f;
        project.setTrack (v, false);
        auto mt = *m; mt.gain = 1.0f; project.setTrack (mt, false);
    }
    engine.setProject (project.state());
    engine.setArmedTrack (voiceId);
    pump (200);
    CHECK (std::abs (engine.getLength() - 20.0) < 1e-6 && engine.hasContent(), "engine loaded the project");

    auto finishRecording = [&]() -> AudioEngine::RecordingResult
    {
        engine.stopRecording();
        pump (200);
        auto r = engine.takeLastRecording();
        if (r.ok) { project.insertRecording (r.trackId, r.wav, r.start, r.in, r.out, r.punch); engine.setProject (project.state()); }
        return r;
    };

    // ---- recording into the voice track WITHOUT noise removal: must line up exactly with the music
    std::cout << "\nRecording alignment (noise removal off)\n";
    engine.setNoiseEnabled (false);
    engine.setPosition (2.0);
    pump (200);
    CHECK (engine.startRecording (voiceId, {}, err), "recording into the voice track starts");
    pump (4000);
    auto rec1 = finishRecording();
    CHECK (rec1.ok && rec1.trackId == voiceId, "recording result for the voice track");
    CHECK (std::abs (rec1.start - 2.0) < 0.01, "clip starts where recording started (" + juce::String (rec1.start, 3) + " s)");
    CHECK (std::abs (rec1.in * 48000.0 - (FakeDevice::loopDelay + MasterBus::lookahead)) < 0.5, "latency head skipped (" + juce::String (rec1.in * 48000.0, 1) + " samples)");
    CHECK (std::abs ((rec1.out - rec1.in) - 4.0) < 0.3, "clip length ~4 s");
    {
        auto vocal = readMono (rec1.wav, (juce::int64) std::llround (rec1.in * 48000), 48000 * 2);
        auto ref = readMono (song, (juce::int64) std::llround (rec1.start * 48000), 48000 * 2);
        double c = 0;
        auto lag = bestLag (vocal, ref, 600, c);
        CHECK (std::abs (lag) <= 1, "recorded vocal aligned with the music (lag " + juce::String (lag) + " samples, corr " + juce::String (c, 3) + ")");
    }
    {
        auto& v = project.state().tracks.getReference (project.state().indexOf (voiceId));
        CHECK (v.clips.size() == 1, "clip placed on the voice track");
    }

    // ---- THE KEY CHECK: after recording, playback plays the voice (aligned) together with the music
    std::cout << "\nPlayback after recording: voice + music together\n";
    auto capturePlayback = [&] (double from, int ms) -> std::pair<std::vector<float>, double>
    {
        engine.setPosition (from);
        pump (300);
        dev->captured.clear(); dev->captureStartPos = -1.0;
        dev->capture = true;
        engine.play();
        pump (ms);
        dev->capture = false;
        engine.pause();
        pump (100);
        // what the speaker plays is delayed by the master limiter's look-ahead (a fixed, compensated delay)
        auto c = dev->captured;
        c.erase (c.begin(), c.begin() + (long) juce::jmin ((size_t) MasterBus::lookahead, c.size()));
        return { c, dev->captureStartPos };
    };
    {
        // voice alone (music muted)
        auto m = *project.state().musicTrack(); m.mute = true; project.setTrack (m, false); engine.setProject (project.state());
        auto [out, pos] = capturePlayback (2.5, 2500);
        auto ref = readMono (song, (juce::int64) std::llround (pos * 48000), (juce::int64) out.size());
        double c = 0;
        const int n = (int) juce::jmin ((size_t) 48000 * 2, out.size());
        std::vector<float> o2 (out.begin(), out.begin() + n), r2 (ref.begin(), ref.begin() + n);
        auto lag = bestLag (o2, r2, 600, c);
        const double level = rms (o2) / juce::jmax (1e-9, rms (r2));
        CHECK (level > 0.5, "recorded voice is heard on playback (level " + juce::String (level, 2) + "x)");
        CHECK (std::abs (lag) <= 2 && c > 0.8, "voice plays in time on playback (lag " + juce::String (lag) + ", corr " + juce::String (c, 3) + ")");

        // voice + music
        m.mute = false; project.setTrack (m, false); engine.setProject (project.state());
        auto [out2, pos2] = capturePlayback (2.5, 2500);
        auto ref2 = readMono (song, (juce::int64) std::llround (pos2 * 48000), (juce::int64) out2.size());
        const int n2 = (int) juce::jmin ((size_t) 48000 * 2, out2.size());
        std::vector<float> o3 (out2.begin(), out2.begin() + n2), r3 (ref2.begin(), ref2.begin() + n2);
        const double both = rms (o3) / juce::jmax (1e-9, rms (r3));
        CHECK (both > 1.6, "music + voice play together (level " + juce::String (both, 2) + "x of music alone)");

        // mute the voice: only the music is left
        auto v = project.state().tracks.getReference (project.state().indexOf (voiceId)); v.mute = true; project.setTrack (v, false); engine.setProject (project.state());
        auto [out3, pos3] = capturePlayback (2.5, 1500);
        auto ref3 = readMono (song, (juce::int64) std::llround (pos3 * 48000), (juce::int64) out3.size());
        const double only = rms (out3) / juce::jmax (1e-9, rms (ref3));
        CHECK (only > 0.8 && only < 1.25, "voice mute works (level " + juce::String (only, 2) + "x)");
        v.mute = false; project.setTrack (v, false); engine.setProject (project.state());
    }

    // ---- sing a part again (punch-in): only the chosen part is replaced
    std::cout << "\nSing a part again (punch-in)\n";
    {
        engine.setPrerollSeconds (1.0);
        CHECK (engine.startRecording (voiceId, { 3.0, 4.0 }, err), "punch recording starts (1 s cue before)");
        for (int i = 0; i < 60 && ! engine.punchFinished(); ++i) pump (100);
        CHECK (engine.punchFinished(), "punch ends by itself at the end of the part");
        auto r = finishRecording();
        CHECK (r.ok && std::abs (r.start - 3.0) < 0.01 && std::abs ((r.out - r.in) - 1.0) < 0.01, "new clip covers exactly 3.0-4.0 s");
        auto& v = project.state().tracks.getReference (project.state().indexOf (voiceId));
        juce::String layout;
        for (auto& c : v.clips) layout << juce::String (c.start, 2) << "-" << juce::String (c.end(), 2) << " ";
        CHECK (v.clips.size() == 3, "old take split around the new part: " + layout);
        auto vocal = readMono (r.wav, (juce::int64) std::llround (r.in * 48000), 48000 / 2);
        auto ref = readMono (song, (juce::int64) std::llround (r.start * 48000), 48000 / 2);
        double c = 0; auto lag = bestLag (vocal, ref, 600, c);
        CHECK (std::abs (lag) <= 1, "punch-in aligned (lag " + juce::String (lag) + ")");
        engine.setPrerollSeconds (2.0);

        // the new part overlaps the old take by the fade time on both sides (cross-fade, not a gap)
        double overlapL = 1e9, overlapR = 1e9;
        for (auto& c1 : v.clips) for (auto& c2 : v.clips)
            if (c1.id != c2.id && c2.file == r.wav && c1.file != r.wav)
            {
                if (c1.start < c2.start) overlapL = c1.end() - c2.start; else overlapR = c2.end() - c1.start;
            }
        CHECK (std::abs (overlapL - clipFadeSeconds) < 1e-6 && std::abs (overlapR - clipFadeSeconds) < 1e-6,
               "joins cross-fade over " + juce::String (overlapL * 1000.0, 1) + " / " + juce::String (overlapR * 1000.0, 1) + " ms");

    }

    // ---- the joins of a re-sung part, heard: a steady sung note re-sung in the middle must stay steady
    std::cout << "\nPunch-in joins (click test)\n";
    {
        auto writeTone = [&] (const juce::File& f, double phase0)
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (1).withBitsPerSample (24));
            juce::AudioBuffer<float> bb (1, 48000 * 5);
            for (int i = 0; i < bb.getNumSamples(); ++i) bb.setSample (0, i, 0.25f * (float) std::sin (phase0 + 2 * juce::MathConstants<double>::pi * 220.0 * i / 48000.0));
            w->writeFromAudioSampleBuffer (bb, 0, bb.getNumSamples());
        };
        auto oldTake = tmp.getChildFile ("old take.wav"), newTake = tmp.getChildFile ("new take.wav");
        writeTone (oldTake, 0.0);
        writeTone (newTake, 0.0);   // the singer holds the same note in the new take
        Project pj;
        pj.newProject ("join test");
        pj.setMix (0.0f, false);
        juce::String vid;
        for (auto& t : pj.state().tracks) vid = t.id;
        { auto t = pj.state().tracks.getReference (0); FxParams n; allOff (n); t.fx = n.toString(); pj.setTrack (t, false); }
        pj.insertRecording (vid, oldTake, 0.0, 0.0, 4.0);
        pj.insertRecording (vid, newTake, 1.5, 1.5, 2.5, true);   // re-sung 1.5-2.5 s
        auto f = tmp.getChildFile ("join.wav");
        bool d = false, okk = false;
        engine.exportMix (pj.state(), f, { 1.0, 3.0 }, nullptr, [&] (bool o, juce::String) { okk = o; d = true; });
        for (int i = 0; i < 300 && ! d; ++i) pump (100);
        auto x = readMono (f);
        // a click = a jump in the waveform; the largest sample-to-sample step must stay that of the clean note
        double maxStep = 0, minLevel = 1e9;
        for (size_t i = 4800; i + 4800 < x.size(); ++i) maxStep = juce::jmax (maxStep, (double) std::abs (x[i] - x[i - 1]));
        for (size_t k = 4800; k + 480 + 4800 < x.size(); k += 48) minLevel = juce::jmin (minLevel, rms (std::vector<float> (x.begin() + (long) k, x.begin() + (long) k + 480)));
        const double cleanStep = 0.25 * 2 * juce::MathConstants<double>::pi * 220.0 / 48000.0;
        CHECK (okk && maxStep < cleanStep * 1.15, "no click at the joins (largest step " + juce::String (maxStep, 4) + ", clean note " + juce::String (cleanStep, 4) + ")");
        CHECK (minLevel > 0.25 / std::sqrt (2.0) * 0.9, "no dip at the joins (lowest 10 ms level " + juce::String (minLevel / (0.25 / std::sqrt (2.0)), 2) + "x)");
        pj.discard();
    }

    // ---- separate volume bars and mutes: every track muted = true silence
    std::cout << "\nVolume and mute per layer\n";
    {
        auto st = project.state();
        for (auto& t : st.tracks) t.mute = true;
        engine.setProject (st);
        auto [out, pos] = capturePlayback (2.5, 800);
        float peak = 0; for (size_t i = 4800; i < out.size(); ++i) peak = juce::jmax (peak, std::abs (out[i]));
        CHECK (peak < 1e-5f, "all layers muted = silence (peak " + juce::String (peak, 7) + ")");
        // music at 50%: half the level
        st = project.state();
        for (auto& t : st.tracks) { if (t.kind == TrackKind::vocal) t.mute = true; else t.gain = 0.5f; }
        engine.setProject (st);
        auto [o2, p2] = capturePlayback (2.5, 1200);
        auto ref = readMono (song, (juce::int64) std::llround (p2 * 48000), (juce::int64) o2.size());
        const double lv = rms (std::vector<float> (o2.begin() + 4800, o2.end())) / juce::jmax (1e-9, rms (std::vector<float> (ref.begin() + 4800, ref.end())));
        CHECK (std::abs (lv - 0.5) < 0.05, "music volume 50% -> level " + juce::String (lv, 2) + "x");
        engine.setProject (project.state());
    }

    // ---- editing: cut, move, undo, save/load
    std::cout << "\nEditing\n";
    {
        const auto before = projectToXml (project.state());
        project.deleteRange (voiceId, { 2.5, 3.5 });
        auto cnt = project.state().tracks.getReference (project.state().indexOf (voiceId)).clips.size();
        CHECK (cnt == 3, "cut a part out of the voice track");
        project.undo();
        CHECK (projectToXml (project.state()) == before, "undo restores the exact state");
        project.redo(); project.undo();
        auto& v = project.state().tracks.getReference (project.state().indexOf (voiceId));
        const auto clipId = v.clips.getReference (0).id;
        project.moveClip (voiceId, clipId, 1.5);
        bool moved = false;
        for (auto& c : project.state().tracks.getReference (project.state().indexOf (voiceId)).clips) if (c.id == clipId) moved = std::abs (c.start - 1.5) < 1e-9;
        CHECK (moved, "voice clip moved earlier");
        project.undo();
        ProjectState back;
        CHECK (projectFromXml (projectToXml (project.state()), back) && projectToXml (back) == projectToXml (project.state()), "project saves and loads identically");
        engine.setProject (project.state());
    }

    // ---- AI noise removal on a free recording (no music)
    std::cout << "\nAI noise removal (singing without music)\n";
    {
        engine.stop();
        Project free;
        free.newProject ("free");
        juce::String fv;
        for (auto& t : free.state().tracks) fv = t.id;
        engine.setProject (free.state());
        dev->extraNoise = 0.05f;
        engine.setNoiseEnabled (true);
        engine.setNoiseAmount (1.0f);
        engine.setPosition (0.0);
        pump (3000);   // in the app noise removal is on from the start, so it has always settled
        CHECK (engine.startRecording (fv, {}, err), "free recording (no music) starts");
        pump (3000);
        engine.stopRecording();
        pump (200);
        dev->extraNoise = 0.0f;
        auto r = engine.takeLastRecording();
        CHECK (r.ok && std::abs (r.start) < 1e-6, "free recording placed at 0 s");
        CHECK (std::abs (r.in * 48000.0 - (FakeDevice::loopDelay + MasterBus::lookahead)) < 0.5, "latency compensated (the recording itself is never processed)");
        // one-click noise removal works on the recording afterwards (offline, Audacity-style)
        {
            auto cleaned = r.wav.getSiblingFile ("vocal_clean.wav");
            std::atomic<bool> nc { false };
            auto e2 = mixtools::reduceNoise (r.wav, cleaned, 12.0f, nullptr, nc);
            auto v = readMono (cleaned, 48000);
            auto raw = readMono (r.wav, 48000);
            auto db = juce::Decibels::gainToDecibels ((float) (rms (v) / juce::jmax (1e-9, rms (raw))));
            CHECK (e2.isEmpty() && db < -8.0f, "noise removal on the recording: " + juce::String (-db, 1) + " dB less noise");
        }
        engine.setNoiseEnabled (false);

        // sung without music: it plays back, with effects, and a cut part is silent
        // (the hiss itself is the "voice" here)
        free.insertRecording (r.trackId, r.wav, r.start, r.in, r.out);
        dev->extraNoise = 0.0f;
        auto v2 = free.state().tracks.getReference (0);
        { FxParams fp; for (auto& pr : builtInPresets()) if (pr.id == "ghazal") pr.applyTo (fp); v2.fx = fp.toString(); }
        free.setTrack (v2, false);
        engine.setProject (free.state());
        auto [o1, p1] = capturePlayback (0.3, 1500);
        const double heard = rms (o1);
        CHECK (heard > 0.002, "voice recorded without music plays back (level " + juce::String (heard, 4) + ")");
        free.deleteRange (v2.id, { 1.0, 2.0 });
        engine.setProject (free.state());
        auto [o3, p3] = capturePlayback (0.5, 1600);
        const auto cutFrom = (size_t) std::llround ((1.6 - p3) * 48000.0), cutTo = (size_t) std::llround ((1.95 - p3) * 48000.0);
        const auto keepTo = (size_t) std::llround ((0.95 - p3) * 48000.0);
        const double inCut = rms (std::vector<float> (o3.begin() + (long) cutFrom, o3.begin() + (long) juce::jmin (cutTo, o3.size())));
        const double before = rms (std::vector<float> (o3.begin(), o3.begin() + (long) keepTo));
        CHECK (inCut < before * 0.2, "cut part is silent apart from the reverb tail (" + juce::String (juce::Decibels::gainToDecibels ((float) (inCut / juce::jmax (1e-9, before))), 0) + " dB)");
        engine.setProject (project.state());
    }

    // ---- export
    std::cout << "\nExport\n";
    {
        auto dest = tmp.getChildFile ("mix.wav");
        bool done = false, ok = false;
        engine.exportMix (project.state(), dest, {}, nullptr, [&] (bool o, juce::String) { ok = o; done = true; });
        for (int i = 0; i < 300 && ! done; ++i) pump (100);
        CHECK (done && ok && dest.existsAsFile(), "whole project exported");
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (dest));
        CHECK (r != nullptr && r->numChannels == 2 && std::abs (r->sampleRate - 48000.0) < 0.5, "stereo 48 kHz WAV");
        if (r != nullptr)
        {
            CHECK (std::abs (r->lengthInSamples / 48000.0 - 22.0) < 0.01, "length = project + 2 s tail");
            juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, b.getNumSamples(), 0, true, true);
            auto mag = b.getMagnitude (0, b.getNumSamples());
            CHECK (mag <= 0.9f, "safety limiter ceiling respected (peak " + juce::String (mag, 3) + ")");
        }

        // only the music / only the voice, as set (volumes, mutes, effects)
        auto exportTo = [&] (const ProjectState& ps, const juce::File& f, juce::Range<double> rng)
        {
            bool d = false, okk = false;
            engine.exportMix (ps, f, rng, nullptr, [&] (bool o, juce::String) { okk = o; d = true; });
            for (int i = 0; i < 300 && ! d; ++i) pump (100);
            return okk;
        };
        {
            auto musicOnly = tmp.getChildFile ("music-only.wav"), voiceOnly = tmp.getChildFile ("voice-only.wav");
            CHECK (exportTo (withOnly (project.state(), TrackKind::music), musicOnly, { 6.5, 8.5 }), "only-music file exported");
            CHECK (exportTo (withOnly (project.state(), TrackKind::vocal), voiceOnly, { 6.5, 8.5 }), "only-voice file exported");
            auto mo = readMono (musicOnly), vo = readMono (voiceOnly), ref = readMono (song, (juce::int64) (6.5 * 48000), 96000);
            double c = 0; auto lag = bestLag (std::vector<float> (mo.begin(), mo.begin() + 48000), std::vector<float> (ref.begin(), ref.begin() + 48000), 600, c);
            const double lv = rms (mo) / rms (ref);
            CHECK (std::abs (lag) <= 1 && c > 0.99 && std::abs (lv - 1.0) < 0.05, "only-music file = the music at its volume (corr " + juce::String (c, 3) + ", level " + juce::String (lv, 2) + ")");
            // the voice takes end at 6 s, so after that the voice-only file must be silent
            CHECK (rms (vo) < 0.02 * rms (ref), "only-voice file has no music in it (" + juce::String (juce::Decibels::gainToDecibels ((float) (rms (vo) / rms (ref))), 0) + " dB)");
            auto vo2f = tmp.getChildFile ("voice-only2.wav");
            exportTo (withOnly (project.state(), TrackKind::vocal), vo2f, { 4.2, 5.8 });
            auto vo2 = readMono (vo2f), ref2 = readMono (song, (juce::int64) (4.2 * 48000), 48000);
            double c2 = 0; auto lag2 = bestLag (std::vector<float> (vo2.begin(), vo2.begin() + 48000), ref2, 600, c2);
            CHECK (std::abs (lag2) <= 1 && c2 > 0.8, "only-voice file = the recorded voice, in time (corr " + juce::String (c2, 3) + ")");
        }

        // a selected part, voice only, through the full effect chain delay
        auto m = *project.state().musicTrack(); m.mute = true; project.setTrack (m, false);
        auto v = project.state().tracks.getReference (project.state().indexOf (voiceId));
        FxParams fx; fx.fromString (v.fx); fx.setOn (FxParams::tuneOn, true); v.fx = fx.toString(); project.setTrack (v, false);
        auto part = tmp.getChildFile ("part.wav");
        done = false; ok = false;
        engine.exportMix (project.state(), part, { 4.2, 5.8 }, nullptr, [&] (bool o, juce::String) { ok = o; done = true; });
        for (int i = 0; i < 300 && ! done; ++i) pump (100);
        std::unique_ptr<juce::AudioFormatReader> rp (fm.createReaderFor (part));
        CHECK (ok && rp != nullptr && std::abs (rp->lengthInSamples / 48000.0 - 1.6) < 0.01, "selected part exported (1.6 s)");
        auto mm = readMono (part, 0, 48000);
        auto ref = readMono (song, (juce::int64) std::llround (4.2 * 48000), 48000);
        double c = 0; auto lag = bestLag (mm, ref, 600, c);
        CHECK (std::abs (lag) <= 1, "voice in export aligned after auto-tune delay (lag " + juce::String (lag) + ", corr " + juce::String (c, 3) + ")");
        m.mute = false; project.setTrack (m, false);
        engine.setProject (project.state());
    }

    // ---- "my presets": save, rename, delete, still there after restarting
    std::cout << "\nMy presets\n";
    {
        auto f = tmp.getChildFile ("settings.xml");
        juce::PropertiesFile::Options o; o.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::String id;
        {
            juce::PropertiesFile pf (f, o);
            UserPresets up (pf);
            FxParams mine; builtInPresets()[11].applyTo (mine); mine.set (FxParams::rev, 77.0f);
            id = up.add (juce::String::fromUTF8 ("আমার গলা"), mine.toString());
            up.add ("second", FxParams().toString());
            CHECK (up.rename (id, juce::String::fromUTF8 ("আমার ভারী গলা")), "renamed");
        }
        juce::PropertiesFile again (f, o);   // as after closing and opening the software
        UserPresets up2 (again);
        juce::Array<UserPreset> store;
        auto* p = up2.find (id, store);
        FxParams back; if (p != nullptr) back.fromString (p->state);
        CHECK (p != nullptr && p->name == juce::String::fromUTF8 ("আমার ভারী গলা") && std::abs (back.get (FxParams::rev) - 77.0f) < 0.1f, "my preset kept with its new name and settings after restart");
        CHECK (up2.remove (id) && up2.list().size() == 1, "my preset deleted, the other one kept");
    }

    // ---- MP4 with lyrics
    std::cout << "\nMP4 video export\n";
    if (media::ffmpeg().existsAsFile())
    {
        videoexport::Options vo;
        vo.state = project.state();
        vo.mixWav = tmp.getChildFile ("part.wav");
        vo.out = tmp.getChildFile ("lyrics video.mp4");
        vo.lyrics = Lyrics::parse (juce::String::fromUTF8 ("[00:04.00]প্রথম লাইন\n[00:05.00]দ্বিতীয় লাইন"));
        vo.title = "Test";
        vo.rangeStart = 4.2; vo.length = 1.6;
        std::atomic<bool> cancel { false };
        auto e = videoexport::render (vo, nullptr, cancel);
        auto info = media::probe (vo.out);
        CHECK (e.isEmpty() && info.hasVideo && info.hasAudio && info.width == 1280 && std::abs (info.duration - 1.6) < 0.15,
               "MP4 with lyrics: video + sound, 1280x720, " + juce::String (info.duration, 2) + " s" + (e.isEmpty() ? juce::String() : " (" + e + ")"));
        // the lyrics are really drawn: the frame at the line's time differs from an empty one
        juce::Image a (juce::Image::RGB, 640, 360, true), b (juce::Image::RGB, 640, 360, true);
        juce::TextLayout tl; float tw = 0;
        vo.width = 640; vo.height = 360;
        { juce::Graphics g (a); videoexport::drawLyricsFrame (g, vo, 4.5, tl, tw); }
        vo.lyrics = {};
        { juce::Graphics g (b); videoexport::drawLyricsFrame (g, vo, 4.5, tl, tw); }
        int diff = 0;
        for (int y = 0; y < 360; y += 2) for (int x = 0; x < 640; x += 2) diff += a.getPixelAt (x, y) != b.getPixelAt (x, y) ? 1 : 0;
        CHECK (diff > 500, "lyrics drawn in the video (" + juce::String (diff) + " pixels)");
    }
    else std::cout << "  SKIP  ffmpeg not found next to the test\n";

    // ---- tempo change keeps the pitch
    std::cout << "\nSpeed change (pitch kept)\n";
    {
        auto tone = tmp.getChildFile ("tone.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (tone);
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (2).withBitsPerSample (24));
            juce::AudioBuffer<float> b (2, 48000 * 5);
            for (int i = 0; i < b.getNumSamples(); ++i) { auto x = 0.3f * (float) std::sin (2 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0); b.setSample (0, i, x); b.setSample (1, i, x); }
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        std::atomic<bool> cancel { false };
        auto fast = tmp.getChildFile ("tone125.wav");
        float lastP = 0;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto e = renderTempo (tone, fast, 1.25, 0, [&] (float p) { lastP = p; }, cancel);
        const auto secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        auto x = readMono (fast);
        CHECK (e.isEmpty() && std::abs (x.size() / 48000.0 - 4.0) < 0.05, "125% speed: 5 s -> " + juce::String (x.size() / 48000.0, 2) + " s (" + juce::String (secs, 1) + " s to render)");
        std::vector<float> mid (x.begin() + 48000, x.begin() + 48000 * 3);
        auto hz = estimateHz (mid, 48000.0);
        CHECK (std::abs (hz - 440.0f) < 2.0f, "pitch unchanged (" + juce::String (hz, 1) + " Hz)");

        // music track at 125%: voice clips keep their place relative to the music
        project.replaceMusicAudio (musicId, fast, 1.25, tone);
        auto* mt = project.state().musicTrack();
        CHECK (mt != nullptr && std::abs (mt->clips.getReference (0).length() - 16.0) < 1e-6 && std::abs (mt->speed - 1.25) < 1e-9, "music track switched to the faster audio");
        CHECK (std::abs (project.state().musicTimeAt (2.0) - 2.5) < 1e-9, "lyrics/video time follows the speed (2 s -> 2.5 s of song)");
        project.undo();
        engine.setProject (project.state());
    }

    // ---- loading/editing never blocks the audio thread
    std::cout << "\nGlitch-free editing while playing\n";
    {
        engine.setPosition (0.0);
        engine.play();
        pump (300);
        dev->maxCallbackMs = 0;
        for (int i = 0; i < 6; ++i)
        {
            project.deleteRange (voiceId, { 2.2 + i * 0.1, 2.3 + i * 0.1 });
            engine.setProject (project.state());
            pump (80);
            project.undo();
            engine.setProject (project.state());
            pump (80);
        }
        engine.pause();
        CHECK (dev->maxCallbackMs.load() < 2.5, "max audio callback while editing: " + juce::String (dev->maxCallbackMs.load(), 3) + " ms");
    }

    // ---- performance
    std::cout << "\nPerformance (128-sample blocks @ 48 kHz = 2.67 ms budget)\n";
    {
        auto second = project.addVocalTrack();
        engine.setProject (project.state());
        engine.setNoiseEnabled (true); engine.setNoiseInMonitor (true); engine.setMonitorEnabled (true);
        engine.setPosition (0.0);
        pump (300);
        dev->totalCallbackMs = 0; dev->maxCallbackMs = 0; dev->callbacks = 0;
        CHECK (engine.startRecording (second, {}, err), "recording track 2 while music + track 1 play (noise removal, live mic)");
        pump (5000);
        auto r = finishRecording();
        engine.setMonitorEnabled (false);
        auto avg = dev->totalCallbackMs.load() / juce::jmax (1, dev->callbacks.load());
        std::cout << "        average callback: " << juce::String (avg, 4) << " ms (" << juce::String (100.0 * avg / 2.6667, 1) << "% of budget), max "
                  << juce::String (dev->maxCallbackMs.load(), 3) << " ms\n";
        CHECK (avg < 0.6, "average DSP load well under budget");
        CHECK (r.ok && r.trackId == second, "second voice layer recorded");
    }

    testMedia (tmp, song);

    // remove this run's recordings and project files
    for (auto& d : AudioEngine::recordingsFolder().findChildFiles (juce::File::findDirectories, false))
        if (d.getCreationTime() > juce::Time::getCurrentTime() - juce::RelativeTime::minutes (10) && d.getChildFile ("session.xml").existsAsFile()
            && d.getChildFile ("session.xml").loadFileAsString().isNotEmpty() && testStart < d.getCreationTime()) d.deleteRecursively();
    for (auto& f : Project::projectsFolder().findChildFiles (juce::File::findFiles, false, "engine test *.tfvs;free *.tfvs;join test *.tfvs;engine test *.tmp;free *.tmp;join test *.tmp"))
        f.deleteFile();   // only the test creates projects with these names

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << "\n";
    engine.getDeviceManager().closeAudioDevice();
    tmp.deleteRecursively();
    return failures == 0 ? 0 : 1;
}
