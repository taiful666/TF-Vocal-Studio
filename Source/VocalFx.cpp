#include "VocalFx.h"
#include <rubberband/RubberBandStretcher.h>
#include <mutex>
#include <iostream>
#include <map>

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

//==============================================================================
// FxParams
//==============================================================================
static const char* paramKeys[] = { "tuneAmt", "tuneSpd", "bass", "mid", "bright", "body", "warm", "rev", "room", "echo", "etime", "dbl", "deess", "level" };
static const char* switchKeys[] = { "tuneOn", "toneOn", "bodyOn", "revOn", "echoOn", "dblOn", "deessOn", "levelOn" };
static const float paramDefaults[] = { 40, 35, 50, 50, 50, 50, 25, 32, 55, 12, 45, 0, 45, 60 };
static const bool switchDefaults[] = { true, true, true, true, true, false, true, true };

FxParams::FxParams()
{
    for (int i = 0; i < numParams; ++i) p[(size_t) i].store (paramDefaults[i]);
    for (int i = 0; i < numSwitches; ++i) on[(size_t) i].store (switchDefaults[i]);
}

juce::String FxParams::toString() const
{
    juce::StringArray a;
    for (int i = 0; i < numParams; ++i) a.add (juce::String (paramKeys[i]) + "=" + juce::String (get ((Param) i), 1));
    for (int i = 0; i < numSwitches; ++i) a.add (juce::String (switchKeys[i]) + "=" + (isOn ((Switch) i) ? "1" : "0"));
    return a.joinIntoString (";");
}

void FxParams::fromString (const juce::String& s)
{
    for (auto& kv : juce::StringArray::fromTokens (s, ";", ""))
    {
        auto k = kv.upToFirstOccurrenceOf ("=", false, false), v = kv.fromFirstOccurrenceOf ("=", false, false);
        for (int i = 0; i < numParams; ++i) if (k == paramKeys[i]) set ((Param) i, v.getFloatValue());
        for (int i = 0; i < numSwitches; ++i) if (k == switchKeys[i]) setOn ((Switch) i, v.getIntValue() != 0);
    }
}

//==============================================================================
// Biquad (RBJ cookbook)
//==============================================================================
void Biquad::norm (double nb0, double nb1, double nb2, double a0, double na1, double na2) noexcept
{
    b0 = (float) (nb0 / a0); b1 = (float) (nb1 / a0); b2 = (float) (nb2 / a0);
    a1 = (float) (na1 / a0); a2 = (float) (na2 / a0);
}

void Biquad::setLowShelf (double sr, double f, double q, double g) noexcept
{
    const double A = std::pow (10.0, g / 40.0), w = 2 * juce::MathConstants<double>::pi * f / sr, cs = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
    norm (A * ((A + 1) - (A - 1) * cs + s), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - s),
          (A + 1) + (A - 1) * cs + s, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - s);
}

void Biquad::setHighShelf (double sr, double f, double q, double g) noexcept
{
    const double A = std::pow (10.0, g / 40.0), w = 2 * juce::MathConstants<double>::pi * f / sr, cs = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
    norm (A * ((A + 1) + (A - 1) * cs + s), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - s),
          (A + 1) - (A - 1) * cs + s, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - s);
}

void Biquad::setPeak (double sr, double f, double q, double g) noexcept
{
    const double A = std::pow (10.0, g / 40.0), w = 2 * juce::MathConstants<double>::pi * f / sr, cs = std::cos (w), al = std::sin (w) / (2 * q);
    norm (1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}

void Biquad::setHighPass (double sr, double f, double q) noexcept
{
    const double w = 2 * juce::MathConstants<double>::pi * f / sr, cs = std::cos (w), al = std::sin (w) / (2 * q);
    norm ((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
}

void Biquad::setLowPass (double sr, double f, double q) noexcept
{
    const double w = 2 * juce::MathConstants<double>::pi * f / sr, cs = std::cos (w), al = std::sin (w) / (2 * q);
    norm ((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
}

//==============================================================================
// PitchDetector (YIN, cumulative mean normalised difference, FFT autocorrelation)
//==============================================================================
void PitchDetector::prepare (double sampleRate)
{
    sr = sampleRate;
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);
    fa.assign (2 * fftSize, 0.0f);
    fb.assign (2 * fftSize, 0.0f);
    dfun.assign (frameLen / 2 + 1, 0.0f);
}

float PitchDetector::analyse (const float* frame) noexcept
{
    float energy = 0.0f;
    for (int j = 0; j < frameLen; ++j) energy += frame[j] * frame[j];
    const float rms = std::sqrt (energy / frameLen);
    const int half = frameLen / 2;
    const int tauMin = juce::jmax (2, (int) (sr / 1000.0));
    const int tauMax = juce::jmin (half - 2, (int) (sr / 60.0));
    if (rms < 0.004f) return 0.0f;   // below about -48 dBFS: silence/unvoiced

    std::fill (fa.begin(), fa.end(), 0.0f);
    std::fill (fb.begin(), fb.end(), 0.0f);
    for (int j = 0; j < half; ++j) fa[(size_t) j] = frame[j];
    for (int j = 0; j < frameLen; ++j) fb[(size_t) j] = frame[j];
    fft->performRealOnlyForwardTransform (fa.data());
    fft->performRealOnlyForwardTransform (fb.data());
    for (int k = 0; k < fftSize; ++k)
    {
        const float ar = fa[(size_t) (2 * k)], ai = fa[(size_t) (2 * k + 1)];
        const float br = fb[(size_t) (2 * k)], bi = fb[(size_t) (2 * k + 1)];
        fb[(size_t) (2 * k)] = ar * br + ai * bi;
        fb[(size_t) (2 * k + 1)] = ar * bi - ai * br;
    }
    fft->performRealOnlyInverseTransform (fb.data());

    float e1 = 0.0f;
    for (int j = 0; j < half; ++j) e1 += frame[j] * frame[j];
    const float scale = (std::abs (fb[0]) > 1e-12f) ? e1 / fb[0] : 1.0f;

    float e2 = e1;
    dfun[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        e2 += frame[tau + half - 1] * frame[tau + half - 1] - frame[tau - 1] * frame[tau - 1];
        const float d = juce::jmax (0.0f, e1 + e2 - 2.0f * fb[(size_t) tau] * scale);
        running += d;
        dfun[(size_t) tau] = running > 0 ? d * (float) tau / running : 1.0f;
    }
    int found = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (dfun[(size_t) tau] < 0.15f)
        {
            while (tau + 1 <= tauMax && dfun[(size_t) (tau + 1)] < dfun[(size_t) tau]) ++tau;
            found = tau;
            break;
        }
    }
    if (found < 0) return 0.0f;
    float t = (float) found;
    if (found > tauMin && found < tauMax)
    {
        const float a = dfun[(size_t) (found - 1)], b = dfun[(size_t) found], c = dfun[(size_t) (found + 1)];
        const float den = a - 2 * b + c;
        if (std::abs (den) > 1e-9f) t += 0.5f * (a - c) / den;
    }
    return t;
}

//==============================================================================
void LivePitch::prepare (double sampleRate)
{
    sr = sampleRate;
    det.prepare (sr);
    ring.assign (PitchDetector::frameLen, 0.0f);
    frame.assign (PitchDetector::frameLen, 0.0f);
    w = 0; since = 0;
    hop = juce::jmax (256, (int) (sr / 47.0));
    hzNow = 0.0f;
}

void LivePitch::push (const float* x, int n) noexcept
{
    if (ring.empty()) return;
    for (int i = 0; i < n; ++i)
    {
        ring[(size_t) w] = x[i];
        w = (w + 1) % PitchDetector::frameLen;
        if (++since >= hop)
        {
            since = 0;
            for (int j = 0; j < PitchDetector::frameLen; ++j) frame[(size_t) j] = ring[(size_t) ((w + j) % PitchDetector::frameLen)];
            const float p = det.analyse (frame.data());
            hzNow.store (p > 0 ? (float) (sr / p) : 0.0f, std::memory_order_relaxed);
        }
    }
}

//==============================================================================
// AutoTune (live monitor: PSOLA, low latency)
//==============================================================================
void AutoTune::prepare (double sampleRate)
{
    sr = sampleRate;
    latency = juce::roundToInt (2048.0 * sr / 48000.0);
    hop = juce::jmax (64, juce::roundToInt (256.0 * sr / 48000.0));
    inRing.assign (ringSize, 0.0f);
    outAcc.assign (ringSize, 0.0f);
    detector.prepare (sr);
    frame.assign (frameLen, 0.0f);
    hann.assign (4096, 0.0f);
    marks.reserve (512); markPeriod.reserve (512); markRatio.reserve (512);
    reset();
}

void AutoTune::reset()
{
    std::fill (inRing.begin(), inRing.end(), 0.0f);
    std::fill (outAcc.begin(), outAcc.end(), 0.0f);
    now = 0;
    sinceDetect = 0;
    periodSamples = 0.0f;
    detectedHz = 0.0f;
    targetNote = -1000;
    shiftSemis = 0.0f;
    nextAnalysis = 0;
    marks.clear(); markPeriod.clear(); markRatio.clear();
    nextSynthesis = (double) latency;
}

int AutoTune::nearestScaleNote (float midi, int key, int current) noexcept
{
    static const int major[] = { 0, 2, 4, 5, 7, 9, 11 }, minor[] = { 0, 2, 3, 5, 7, 8, 10 };
    auto inScale = [key] (int n)
    {
        if (key < 0) return true;
        const int tonic = key % 12;
        const int* s = key < 12 ? major : minor;
        const int pc = ((n - tonic) % 12 + 12) % 12;
        for (int i = 0; i < 7; ++i) if (s[i] == pc) return true;
        return false;
    };
    if (current > -100 && inScale (current) && std::abs (midi - (float) current) < 0.62f)
        return current;   // hysteresis: stay on the note while the singer is near it
    int best = (int) std::round (midi);
    float bestD = 1e9f;
    for (int n = (int) std::floor (midi) - 2; n <= (int) std::ceil (midi) + 2; ++n)
        if (inScale (n) && std::abs ((float) n - midi) < bestD) { bestD = std::abs ((float) n - midi); best = n; }
    return best;
}

void AutoTune::detect() noexcept
{
    for (int j = 0; j < frameLen; ++j) frame[(size_t) j] = inRing[(size_t) ((now - frameLen + j) & ringMask)];
    periodSamples = detector.analyse (frame.data());
    detectedHz = periodSamples > 0 ? (float) (sr / periodSamples) : 0.0f;
}

void AutoTune::generateMarks() noexcept
{
    const float unvoicedP = (float) (sr / 200.0);
    while (true)
    {
        const float P = juce::jlimit (32.0f, 800.0f * (float) (sr / 48000.0), periodSamples > 0 ? periodSamples : unvoicedP);
        const int Pi = juce::roundToInt (P);
        if (nextAnalysis + Pi > now) break;
        marks.push_back (nextAnalysis);
        markPeriod.push_back ((float) Pi);
        markRatio.push_back (periodSamples > 0 ? std::pow (2.0f, shiftSemis / 12.0f) : 1.0f);
        nextAnalysis += Pi;
    }
}

void AutoTune::process (const float* in, float* out, int n, float amount, float speed, int scaleKey) noexcept
{
    amountNow = amount; speedNow = speed; keyNow = scaleKey;
    for (int i = 0; i < n; ++i)
    {
        inRing[(size_t) (now & ringMask)] = in[i];
        ++now;
        if (++sinceDetect >= hop)
        {
            sinceDetect = 0;
            detect();
            float desired = 0.0f;
            if (periodSamples > 0)
            {
                const float midi = 69.0f + 12.0f * std::log2 (detectedHz / 440.0f);
                targetNote = nearestScaleNote (midi, keyNow, targetNote);
                desired = juce::jlimit (-3.0f, 3.0f, ((float) targetNote - midi) * amountNow);
            }
            else targetNote = -1000;
            const float tauSec = 0.003f + (1.0f - speedNow / 100.0f) * 0.15f;
            const float alpha = 1.0f - std::exp (-(float) hop / (float) sr / tauSec);
            shiftSemis += (desired - shiftSemis) * alpha;
        }
        generateMarks();
    }

    const juce::int64 t0 = now - n;
    const juce::int64 tEnd = now;
    while (! marks.empty())
    {
        if (nextSynthesis >= (double) (tEnd + 820)) break;
        const double ti = nextSynthesis - latency;
        size_t best = 0; double bd = 1e18;
        for (size_t k = 0; k < marks.size(); ++k)
        {
            const double d = std::abs ((double) marks[k] - ti);
            if (d < bd) { bd = d; best = k; }
            else if ((double) marks[k] > ti) break;
        }
        const int P = (int) markPeriod[best];
        const juce::int64 ta = marks[best];
        if (ta + P > now) break;
        const auto ts = (juce::int64) std::llround (nextSynthesis);
        if (ts - P >= tEnd) break;

        const int len = 2 * P;
        for (int k = 0; k < len; ++k)
        {
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) k / (float) len);
            outAcc[(size_t) ((ts - P + k) & ringMask)] += w * inRing[(size_t) ((ta - P + k) & ringMask)];
        }
        nextSynthesis += (double) P / juce::jlimit (0.5, 2.0, (double) markRatio[best]);

        const double keepFrom = nextSynthesis - latency - 2000.0;
        size_t drop = 0;
        while (drop + 1 < marks.size() && (double) marks[drop] < keepFrom) ++drop;
        if (drop > 0)
        {
            marks.erase (marks.begin(), marks.begin() + (long) drop);
            markPeriod.erase (markPeriod.begin(), markPeriod.begin() + (long) drop);
            markRatio.erase (markRatio.begin(), markRatio.begin() + (long) drop);
        }
    }

    for (int i = 0; i < n; ++i)
    {
        auto& s = outAcc[(size_t) ((t0 + i) & ringMask)];
        out[i] = s;
        s = 0.0f;
    }
}

//==============================================================================
// SmoothTune (playback/export: Rubber Band R3 pitch shifting with formants kept)
//==============================================================================
struct SmoothTune::Impl
{
    using RB = RubberBand::RubberBandStretcher;
    std::unique_ptr<RB> rb;
    PitchDetector det;
    static constexpr int ringSize = 1 << 16, ringMask = ringSize - 1;
    std::vector<float> in, outR, frame, tmp, tmpOut;
    juce::int64 now = 0, fed = 0, produced = 0, feedStart = 0;
    int discard = 0, since = 0, hop = 256, align = PitchDetector::frameLen / 2, safety = 4096, maxBlock = 1024;
    int targetNote = -1000, offset = 0;
    bool active = false;
    float mix = 0.0f;
};

/** Rubber Band's real-time output can sit a few samples off its reported delay; measured once per
    sample rate so the corrected voice lines up with the music to the sample. */
static int rubberBandOffset (double sr)
{
    static std::mutex m;
    static std::map<int, int> cache;
    const std::lock_guard<std::mutex> lock (m);
    const int key = (int) std::lround (sr);
    if (auto it = cache.find (key); it != cache.end()) return it->second;

    using RB = RubberBand::RubberBandStretcher;
    RB rb ((size_t) sr, 1, RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionFormantPreserved
                           | RB::OptionPitchHighConsistency | RB::OptionThreadingNever);
    const int block = 512, len = (int) (sr * 1.5);
    rb.setMaxProcessSize ((size_t) block);
    rb.setPitchScale (1.0 + 1e-6);   // the same path as in use (exactly 1.0 takes a different route)
    std::vector<float> in ((size_t) len), out, buf ((size_t) block), ob (16384);
    juce::Random rnd (11);
    double ph = 0.0;
    for (int i = 0; i < len; ++i)
    {
        ph += 2.0 * juce::MathConstants<double>::pi * 150.0 / sr;
        double v = 0.0;
        for (int h = 1; h < 20; ++h) v += std::sin (h * ph) / h;
        in[(size_t) i] = (float) (0.2 * v) + ((i / 4800) % 3 == 0 ? 0.05f * (rnd.nextFloat() - 0.5f) : 0.0f);
    }
    std::fill (buf.begin(), buf.end(), 0.0f);
    for (int left = (int) rb.getPreferredStartPad(); left > 0;) { const int k = juce::jmin (left, block); const float* p[] = { buf.data() }; rb.process (p, (size_t) k, false); left -= k; }
    int discard = (int) rb.getStartDelay();
    for (int pos = 0; pos < len; pos += block)
    {
        const int k = juce::jmin (block, len - pos);
        const float* p[] = { in.data() + pos };
        rb.process (p, (size_t) k, pos + k >= len);
        for (;;)
        {
            const int a = (int) rb.available();
            if (a <= 0) break;
            float* q[] = { ob.data() };
            const int got = (int) rb.retrieve (q, (size_t) juce::jmin (a, (int) ob.size()));
            for (int j = 0; j < got; ++j) { if (discard > 0) { --discard; continue; } out.push_back (ob[(size_t) j]); }
        }
    }
    int best = 0; double bc = -1.0;
    const int from = (int) (sr * 0.3), n = (int) (sr * 0.8);
    for (int l = -128; l <= 128; ++l)
    {
        double sab = 0, saa = 0, sbb = 0;
        for (int i = from; i < from + n; ++i)
        {
            const int j = i + l;
            if (j < 0 || j >= (int) out.size()) continue;
            sab += in[(size_t) i] * out[(size_t) j]; saa += in[(size_t) i] * in[(size_t) i]; sbb += out[(size_t) j] * out[(size_t) j];
        }
        const double c = sab / std::sqrt (saa * sbb + 1e-20);
        if (c > bc) { bc = c; best = l; }
    }
    cache[key] = best;
    if (juce::SystemStats::getEnvironmentVariable ("TF_DEBUG_RB", {}).isNotEmpty()) std::cout << "RB offset " << best << " corr " << bc << std::endl;
    return best;
}

SmoothTune::SmoothTune() : impl (std::make_unique<Impl>()) {}
SmoothTune::~SmoothTune() = default;

void SmoothTune::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    auto& m = *impl;
    m.maxBlock = juce::jmax (64, maxBlock);
    m.det.prepare (sr);
    m.in.assign (Impl::ringSize, 0.0f);
    m.outR.assign (Impl::ringSize, 0.0f);
    m.frame.assign (PitchDetector::frameLen, 0.0f);
    m.tmp.assign ((size_t) m.maxBlock + 8192, 0.0f);
    m.tmpOut.assign ((size_t) m.maxBlock + 8192, 0.0f);
    m.hop = juce::jmax (64, juce::roundToInt (256.0 * sr / 48000.0));
    m.align = PitchDetector::frameLen / 2;
    m.safety = juce::roundToInt (4096.0 * sr / 48000.0) + m.maxBlock;
    m.rb = std::make_unique<Impl::RB> ((size_t) sr, 1,
        Impl::RB::OptionProcessRealTime | Impl::RB::OptionEngineFiner | Impl::RB::OptionFormantPreserved
        | Impl::RB::OptionPitchHighConsistency | Impl::RB::OptionThreadingNever);
    m.rb->setMaxProcessSize ((size_t) m.maxBlock);
    m.offset = rubberBandOffset (sr);
    latency = m.align + m.safety;
    reset();
}

void SmoothTune::reset()
{
    auto& m = *impl;
    std::fill (m.in.begin(), m.in.end(), 0.0f);
    m.now = 0; m.fed = 0; m.produced = 0; m.since = 0; m.targetNote = -1000;
    m.active = false; m.mix = 0.0f;
    shiftSemis = 0.0f;
    if (m.rb != nullptr) m.rb->reset();
}

void SmoothTune::process (const float* x, float* out, int n, bool enabled, float amount, float speed, int key) noexcept
{
    auto& m = *impl;
    const juce::int64 t0 = m.now;
    for (int i = 0; i < n; ++i) m.in[(size_t) ((t0 + i) & Impl::ringMask)] = x[i];
    m.now += n;

    // note decisions (same rules as the live auto-tune)
    for (int i = 0; i < n; ++i)
    {
        if (++m.since < m.hop) continue;
        m.since = 0;
        const auto end = t0 + i + 1;
        for (int j = 0; j < PitchDetector::frameLen; ++j) m.frame[(size_t) j] = m.in[(size_t) ((end - PitchDetector::frameLen + j) & Impl::ringMask)];
        const float P = m.det.analyse (m.frame.data());
        float desired = 0.0f;
        if (P > 0)
        {
            const float midi = 69.0f + 12.0f * std::log2 ((float) (sr / P) / 440.0f);
            m.targetNote = AutoTune::nearestScaleNote (midi, key, m.targetNote);
            desired = juce::jlimit (-3.0f, 3.0f, ((float) m.targetNote - midi) * amount);
        }
        else m.targetNote = -1000;
        const float tauSec = 0.003f + (1.0f - speed / 100.0f) * 0.15f;
        const float alpha = 1.0f - std::exp (-(float) m.hop / (float) sr / tauSec);
        shiftSemis += (desired - shiftSemis) * alpha;
    }

    const bool want = enabled && amount > 0.001f;
    if (want && ! m.active)
    {
        m.rb->reset();
        m.rb->setPitchScale (1.0 + 1e-6);
        const int pad = (int) m.rb->getPreferredStartPad();
        std::fill (m.tmp.begin(), m.tmp.end(), 0.0f);
        for (int left = pad; left > 0;)
        {
            const int k = juce::jmin (left, m.maxBlock);
            const float* p[] = { m.tmp.data() };
            m.rb->process (p, (size_t) k, false);
            left -= k;
        }
        m.discard = (int) m.rb->getStartDelay();
        m.fed = t0;          // feeding starts now (the input delayed by `align`)
        m.produced = t0;     // next output sample index to be written
        m.feedStart = t0;
        m.active = true;
    }

    if (m.active)
    {
        // feed: f[t] = x[t - align]
        for (int i = 0; i < n; ++i) m.tmp[(size_t) i] = m.in[(size_t) ((t0 + i - m.align) & Impl::ringMask)];
        double scale = std::pow (2.0, (double) shiftSemis / 12.0);
        if (std::abs (scale - 1.0) < 1e-6) scale = 1.0 + 1e-6;   // never exactly 1.0: Rubber Band switches path there
        m.rb->setPitchScale (scale);
        const float* p[] = { m.tmp.data() };
        m.rb->process (p, (size_t) n, false);
        m.fed += n;
        for (;;)
        {
            const int avail = (int) m.rb->available();
            if (avail <= 0) break;
            const int k = juce::jmin (avail, (int) m.tmpOut.size());
            float* q[] = { m.tmpOut.data() };
            m.rb->retrieve (q, (size_t) k);
            int from = 0;
            if (m.discard > 0) { from = juce::jmin (k, m.discard); m.discard -= from; }
            for (int j = from; j < k; ++j) m.outR[(size_t) (m.produced++ & Impl::ringMask)] = m.tmpOut[(size_t) j];
        }
    }

    // output time t plays input time t - latency. Dry = delayed input; wet = RB output at index t - safety.
    const float step = 1.0f / (0.02f * (float) sr);
    for (int i = 0; i < n; ++i)
    {
        const auto t = t0 + i;
        const float dry = m.in[(size_t) ((t - latency) & Impl::ringMask)];
        const auto wi = t - m.safety + m.offset;
        const bool haveWet = m.active && wi >= m.feedStart && wi < m.produced && wi >= m.produced - Impl::ringSize / 2;
        const float targetMix = (want && haveWet) ? 1.0f : 0.0f;
        if (want && m.active && ! haveWet && m.mix > 0.0f) ++underruns;
        m.mix = targetMix > m.mix ? juce::jmin (targetMix, m.mix + step) : juce::jmax (targetMix, m.mix - step);
        const float wet = haveWet ? m.outR[(size_t) (wi & Impl::ringMask)] : dry;
        out[i] = dry + m.mix * (wet - dry);
    }
    if (! want && m.mix <= 0.0f) m.active = false;
}

//==============================================================================
// SmoothReverb (8-line FDN)
//==============================================================================
void SmoothReverb::prepare (double sampleRate)
{
    sr = sampleRate;
    static const float ms[8] = { 31.1f, 37.3f, 41.9f, 47.3f, 53.9f, 59.1f, 66.7f, 73.7f };
    modDepth = (float) (0.0011 * sr);
    for (int i = 0; i < 8; ++i)
    {
        auto& l = lines[(size_t) i];
        l.len = (float) (ms[i] * 0.001 * sr);
        int s = 1; while (s < (int) l.len + (int) modDepth + 8) s <<= 1;
        l.buf.assign ((size_t) s, 0.0f); l.mask = s - 1; l.w = 0; l.lp = 0;
    }
    static const float dms[4] = { 4.77f, 3.59f, 12.73f, 9.31f };
    for (int i = 0; i < 4; ++i)
    {
        diffLen[(size_t) i] = juce::jmax (1, (int) (dms[i] * 0.001 * sr));
        diff[(size_t) i].assign ((size_t) diffLen[(size_t) i], 0.0f);
        diffW[(size_t) i] = 0;
    }
    preLen = (int) (0.024 * sr);
    int s = 1; while (s < preLen + 2) s <<= 1;
    pre.assign ((size_t) s, 0.0f); preMask = s - 1; preW = 0;
    sendHp.setHighPass (sr, 220.0, 0.707);
    sendLp.setLowPass (sr, 6500.0, 0.707);
    inc1 = (float) (juce::MathConstants<double>::twoPi * 0.37 / sr);
    inc2 = (float) (juce::MathConstants<double>::twoPi * 0.53 / sr);
    lastRt = lastDamp = -1.0f;
    reset();
}

void SmoothReverb::reset()
{
    for (auto& l : lines) { std::fill (l.buf.begin(), l.buf.end(), 0.0f); l.lp = 0; l.w = 0; }
    for (auto& d : diff) std::fill (d.begin(), d.end(), 0.0f);
    std::fill (pre.begin(), pre.end(), 0.0f);
    sendHp.reset(); sendLp.reset();
    ph1 = ph2 = 0.0f;
}

void SmoothReverb::process (const float* in, float* outL, float* outR, int n, float rt60, float damping, float wet) noexcept
{
    if (std::abs (rt60 - lastRt) > 1e-3f || std::abs (damping - lastDamp) > 1e-3f)
    {
        for (auto& l : lines) l.g = (float) std::pow (10.0, -3.0 * l.len / (juce::jmax (0.2f, rt60) * sr));
        dampCoef = 1.0f - 0.65f * juce::jlimit (0.0f, 1.0f, damping);
        lastRt = rt60; lastDamp = damping;
    }
    const float inv = 0.35355339f;   // 1/sqrt(8)
    for (int i = 0; i < n; ++i)
    {
        float x = sendLp.process (sendHp.process (in[i]));
        pre[(size_t) preW] = x;
        x = pre[(size_t) ((preW - preLen) & preMask)];
        preW = (preW + 1) & preMask;
        for (int d = 0; d < 4; ++d)
        {
            auto& b = diff[(size_t) d];
            auto& w = diffW[(size_t) d];
            const float v = b[(size_t) w];
            const float u = x + 0.6f * v;
            b[(size_t) w] = u;
            w = (w + 1) % diffLen[(size_t) d];
            x = v - 0.6f * u;
        }
        ph1 += inc1; if (ph1 > juce::MathConstants<float>::twoPi) ph1 -= juce::MathConstants<float>::twoPi;
        ph2 += inc2; if (ph2 > juce::MathConstants<float>::twoPi) ph2 -= juce::MathConstants<float>::twoPi;

        float y[8], z[8];
        const float s1 = std::sin (ph1), c1 = std::cos (ph1), s2 = std::sin (ph2), c2 = std::cos (ph2);
        static const float rs[8] = { 0.0f, 0.71035f, 0.99996f, 0.69728f, -0.01841f, -0.72319f, -0.99962f, -0.68397f };   // sin(0.79k)
        static const float rc[8] = { 1.0f, 0.70385f, -0.00920f, -0.71680f, -0.99983f, -0.69065f, 0.02761f, 0.72951f };   // cos(0.79k)
        for (int k = 0; k < 8; ++k)
        {
            auto& l = lines[(size_t) k];
            const float sn = (k & 1) ? (s2 * rc[k] + c2 * rs[k]) : (s1 * rc[k] + c1 * rs[k]);   // sin(ph + 0.79k)
            const float mod = modDepth * 0.5f * (1.0f + sn);
            const float d = l.len + mod;
            const int i0 = (int) d; const float f = d - (float) i0;
            const float a = l.buf[(size_t) ((l.w - i0) & l.mask)], b = l.buf[(size_t) ((l.w - i0 - 1) & l.mask)];
            y[k] = a + f * (b - a);
            l.lp += dampCoef * (y[k] - l.lp);
            z[k] = l.lp * l.g;
        }
        // fast Walsh-Hadamard transform (lossless mixing of the 8 lines)
        for (int len = 1; len < 8; len <<= 1)
            for (int s = 0; s < 8; s += 2 * len)
                for (int k = s; k < s + len; ++k) { const float a = z[k], b = z[k + len]; z[k] = a + b; z[k + len] = a - b; }
        for (int k = 0; k < 8; ++k)
        {
            auto& l = lines[(size_t) k];
            l.buf[(size_t) l.w] = z[k] * inv + x * ((k & 1) ? -0.5f : 0.5f);
            l.w = (l.w + 1) & l.mask;
        }
        const float L = (y[0] - y[2] + y[4] - y[6]) + 0.5f * (y[1] - y[5]);
        const float R = (y[1] - y[3] + y[5] - y[7]) + 0.5f * (y[2] - y[6]);
        outL[i] += L * 0.42f * wet;
        outR[i] += R * 0.42f * wet;
    }
}

//==============================================================================
// VocalChain
//==============================================================================
void VocalChain::prepare (double sampleRate, int maxBlock, bool tuneAlwaysInPath)
{
    sr = sampleRate;
    tuneInPath = tuneAlwaysInPath;
    tune.prepare (sr);
    if (tuneInPath) smoothTune.prepare (sr, maxBlock);
    reverb.prepare (sr);
    work.assign ((size_t) maxBlock, 0.0f);
    tuneOut.assign ((size_t) maxBlock, 0.0f);
    sendBuf.assign ((size_t) maxBlock, 0.0f);

    int s = 1; while (s < (int) (0.05 * sr)) s <<= 1;
    delayLine.assign ((size_t) s, 0.0f); dlMask = s - 1; dlW = 0;
    s = 1; while (s < (int) (0.7 * sr)) s <<= 1;
    echoL.assign ((size_t) s, 0.0f); echoR.assign ((size_t) s, 0.0f); echoMask = s - 1; echoW = 0;

    deLp1.setLowPass (sr, 5500.0, 0.7071); deLp2.setLowPass (sr, 5500.0, 0.7071);
    wLp1.setLowPass (sr, 3500.0, 0.7071); wLp2.setLowPass (sr, 3500.0, 0.7071);
    wHp1.setHighPass (sr, 3500.0, 0.7071); wHp2.setHighPass (sr, 3500.0, 0.7071);
    deHp1.setHighPass (sr, 5500.0, 0.7071); deHp2.setHighPass (sr, 5500.0, 0.7071);
    echoLpL.setLowPass (sr, 3800.0, 0.707); echoLpR.setLowPass (sr, 3800.0, 0.707);
    echoHpL.setHighPass (sr, 250.0, 0.707); echoHpR.setHighPass (sr, 250.0, 0.707);
    for (auto* v : { &wetRev, &wetEcho, &wetDbl, &warmMix }) v->reset (sr, 0.05);

    lastBass = lastMid = lastBright = -1;
    reset();
}

void VocalChain::reset()
{
    tune.reset();
    smoothTune.reset();
    reverb.reset();
    for (auto* b : { &wLp1, &wLp2, &wHp1, &wHp2, &deLp1, &deLp2, &deHp1, &deHp2, &low, &midPk, &high, &echoLpL, &echoLpR, &echoHpL, &echoHpR }) b->reset();
    std::fill (delayLine.begin(), delayLine.end(), 0.0f);
    std::fill (echoL.begin(), echoL.end(), 0.0f);
    std::fill (echoR.begin(), echoR.end(), 0.0f);
    deessEnv = deessFull = compDet = riderDet = 0.0f;
    compGain = riderGain = 1.0f;
    dcX = 0.0f;
}

int VocalChain::latencySamples (bool live) const noexcept
{
    if (tuneInPath) return smoothTune.latencySamples();
    return (live && params.isOn (FxParams::tuneOn) && params.liveTune.load()) ? tune.latencySamples() : 0;
}

void VocalChain::updateFilters() noexcept
{
    const float b = params.get (FxParams::bass), m = params.get (FxParams::mid), h = params.get (FxParams::bright);
    if (std::abs (b - lastBass) > 1e-4f)   { low.setLowShelf (sr, 170.0, 0.7, (b - 50.0f) / 50.0f * 9.0f); lastBass = b; }
    if (std::abs (m - lastMid) > 1e-4f)    { midPk.setPeak (sr, 1800.0, 0.9, (m - 50.0f) / 50.0f * 6.0f); lastMid = m; }
    if (std::abs (h - lastBright) > 1e-4f) { high.setHighShelf (sr, 7000.0, 0.7, (h - 50.0f) / 50.0f * 8.0f); lastBright = h; }
}

void VocalChain::process (const float* in, float* outL, float* outR, int n, bool live) noexcept
{
    jassert (n <= (int) work.size());
    float* x = work.data();
    std::copy (in, in + n, x);

    const bool bypass = params.bypass.load();
    const bool tuneOn = params.isOn (FxParams::tuneOn) && ! bypass;
    const float amt = tuneOn ? params.get (FxParams::tuneAmt) / 100.0f : 0.0f;
    if (tuneInPath)
    {
        smoothTune.process (x, tuneOut.data(), n, tuneOn, amt, params.get (FxParams::tuneSpd), params.scaleKey.load());
        std::copy (tuneOut.begin(), tuneOut.begin() + n, x);
    }
    else if (live && tuneOn && params.liveTune.load())
    {
        tune.process (x, tuneOut.data(), n, amt, params.get (FxParams::tuneSpd), params.scaleKey.load());
        std::copy (tuneOut.begin(), tuneOut.begin() + n, x);
    }

    if (bypass)
    {
        std::copy (x, x + n, outL);
        std::copy (x, x + n, outR);
        return;
    }

    // ---- auto level (vocal rider): soft singing comes up, loud singing goes down, smoothly
    if (params.isOn (FxParams::levelOn))
    {
        const float strength = params.get (FxParams::level) / 100.0f;
        const float aDet = 1.0f - std::exp (-1.0f / (0.06f * (float) sr));
        const float aUp = 1.0f - std::exp (-1.0f / (0.2f * (float) sr)), aDown = 1.0f - std::exp (-1.0f / (0.08f * (float) sr));
        float target = riderGain;
        for (int i = 0; i < n; ++i)
        {
            riderDet += aDet * (x[i] * x[i] - riderDet);
            if ((i & 31) == 0)
            {
                const float lv = 10.0f * std::log10 (riderDet + 1e-12f);
                if (lv > -48.0f) target = juce::Decibels::decibelsToGain (juce::jlimit (-9.0f, 12.0f, (-20.0f - lv) * strength));
                // in pauses the gain stays where it was (no breathing noise pumped up)
            }
            riderGain += (target < riderGain ? aDown : aUp) * (target - riderGain);
            x[i] *= riderGain;
        }
    }
    else riderGain = 1.0f;

    // ---- tone (EQ)
    if (params.isOn (FxParams::toneOn))
    {
        updateFilters();
        for (int i = 0; i < n; ++i) x[i] = high.process (midPk.process (low.process (x[i])));
    }

    // ---- full voice (compressor): RMS detector, soft knee, smooth gain
    if (params.isOn (FxParams::bodyOn))
    {
        const float body = params.get (FxParams::body) / 100.0f;
        const float thrDb = -12.0f - body * 20.0f, ratio = 2.0f + body * 3.5f, knee = 6.0f;
        const float makeupDb = -thrDb * (1.0f - 1.0f / ratio) * 0.4f;
        const float aDet = 1.0f - std::exp (-1.0f / (0.008f * (float) sr));
        const float att = 1.0f - std::exp (-1.0f / (0.006f * (float) sr)), rel = 1.0f - std::exp (-1.0f / (0.14f * (float) sr));
        float target = compGain;
        for (int i = 0; i < n; ++i)
        {
            compDet += aDet * (x[i] * x[i] - compDet);
            if ((i & 7) == 0)
            {
                const float over = 10.0f * std::log10 (compDet + 1e-12f) - thrDb;
                float gDb = 0.0f;
                if (over > knee * 0.5f) gDb = (1.0f / ratio - 1.0f) * over;
                else if (over > -knee * 0.5f) gDb = (1.0f / ratio - 1.0f) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
                target = juce::Decibels::decibelsToGain (gDb + makeupDb);
            }
            compGain += (target < compGain ? att : rel) * (target - compGain);
            x[i] *= compGain;
        }
    }

    // ---- de-esser (after the compressor, which brings "s" sounds up): only the top band is turned
    //      down while an "s"/"sh" is strong compared with the rest of the voice
    if (params.isOn (FxParams::deessOn))
    {
        const float d = params.get (FxParams::deess) / 100.0f;
        const float att = 1.0f - std::exp (-1.0f / (0.001f * (float) sr)), rel = 1.0f - std::exp (-1.0f / (0.06f * (float) sr));
        for (int i = 0; i < n; ++i)
        {
            const float lo = deLp2.process (deLp1.process (x[i]));
            const float hi = deHp2.process (deHp1.process (x[i]));
            const float a = std::abs (hi), f = std::abs (x[i]);
            deessEnv += (a > deessEnv ? att : rel) * (a - deessEnv);
            deessFull += (f > deessFull ? att : rel) * (f - deessFull);
            const float thr = juce::jmax (0.012f, 0.3f * deessFull);
            float g = 1.0f;
            if (deessEnv > thr) g = std::pow (thr / deessEnv, d);
            x[i] = lo + hi * g;
        }
    }

    // ---- warmth: gentle tube-like saturation. Anti-derivative anti-aliasing keeps it free of the
    //      harsh, hissy "aliasing" that plain saturation adds to a digital voice.
    {
        const float warm = params.isOn (FxParams::bodyOn) ? params.get (FxParams::warm) / 100.0f : 0.0f;
        warmMix.setTargetValue (warm * 0.5f);
        if (warm > 0.005f || warmMix.isSmoothing() || warmMix.getCurrentValue() > 0.0f)
        {
            const float k = 1.0f + warm * 1.8f, k2 = k * k;
            auto F = [k, k2] (float v) { const float a = std::abs (k * v); return (a + std::log1p (std::exp (-2.0f * a)) - 0.69314718f) / k2; };   // log(cosh(kv)) / k^2
            for (int i = 0; i < n; ++i)
            {
                // only the body of the voice (below 3.5 kHz) is warmed; the "s" range stays clean
                const float lo = wLp2.process (wLp1.process (x[i]));
                const float hi = wHp2.process (wHp1.process (x[i]));
                const float v = lo, dv = v - dcX;
                const float sat = std::abs (dv) > 1e-4f ? (F (v) - F (dcX)) / dv : std::tanh (k * 0.5f * (v + dcX)) / k;
                dcX = v;
                x[i] = lo + warmMix.getNextValue() * (sat - lo) + hi;
            }
        }
        else dcX = x[n - 1];
        // soft ceiling: loud attacks are rounded off, never clipped
        for (int i = 0; i < n; ++i)
        {
            const float ay = std::abs (x[i]);
            if (ay > 0.8f) x[i] = std::copysign (0.8f + 0.18f * std::tanh ((ay - 0.8f) / 0.18f), x[i]);
        }
    }

    for (int i = 0; i < n; ++i) { outL[i] = x[i]; outR[i] = x[i]; sendBuf[(size_t) i] = x[i]; }

    // ---- double voice: two short, slowly moving delays spread left/right
    wetDbl.setTargetValue (params.isOn (FxParams::dblOn) ? params.get (FxParams::dbl) / 100.0f * 0.5f : 0.0f);
    {
        const float inc1 = (float) (juce::MathConstants<double>::twoPi * 0.31 / sr), inc2 = (float) (juce::MathConstants<double>::twoPi * 0.43 / sr);
        for (int i = 0; i < n; ++i)
        {
            delayLine[(size_t) dlW] = x[i];
            lfo1 += inc1; if (lfo1 > juce::MathConstants<float>::twoPi) lfo1 -= juce::MathConstants<float>::twoPi;
            lfo2 += inc2; if (lfo2 > juce::MathConstants<float>::twoPi) lfo2 -= juce::MathConstants<float>::twoPi;
            const float g = wetDbl.getNextValue();
            if (g > 0.0005f)
            {
                auto tapAt = [this] (float dSamples)
                {
                    const int i0 = (int) dSamples; const float f = dSamples - (float) i0;
                    const float a = delayLine[(size_t) ((dlW - i0) & dlMask)], b = delayLine[(size_t) ((dlW - i0 - 1) & dlMask)];
                    return a + f * (b - a);
                };
                const float d1 = (float) sr * (0.0115f + 0.0016f * std::sin (lfo1));
                const float d2 = (float) sr * (0.0175f + 0.0019f * std::sin (lfo2));
                outL[i] += g * tapAt (d1);
                outR[i] += g * tapAt (d2);
            }
            dlW = (dlW + 1) & dlMask;
        }
    }

    // ---- echo (ping-pong, darker repeats); the repeats also go into the reverb so they melt in
    wetEcho.setTargetValue (params.isOn (FxParams::echoOn) ? params.get (FxParams::echo) / 100.0f * 0.45f : 0.0f);
    {
        const int dly = (int) (sr * (0.09 + params.get (FxParams::etime) / 100.0f * 0.46));
        const float fb = 0.34f;
        for (int i = 0; i < n; ++i)
        {
            const float eL = echoL[(size_t) ((echoW - dly) & echoMask)];
            const float eR = echoR[(size_t) ((echoW - dly) & echoMask)];
            echoL[(size_t) echoW] = echoHpL.process (echoLpL.process (x[i] * 0.8f + fb * eR));
            echoR[(size_t) echoW] = echoHpR.process (echoLpR.process (fb * eL));
            echoW = (echoW + 1) & echoMask;
            const float g = wetEcho.getNextValue();
            outL[i] += g * eL;
            outR[i] += g * eR;
            sendBuf[(size_t) i] += 0.35f * g * (eL + eR);
        }
    }

    // ---- reverb (smooth hall)
    {
        const bool on = params.isOn (FxParams::revOn);
        const float wet = on ? params.get (FxParams::rev) / 100.0f * 0.55f : 0.0f;
        const float rt = 0.9f + params.get (FxParams::room) / 100.0f * 2.6f;
        if (on || wetRev.isSmoothing() || wetRev.getCurrentValue() > 0.0f)
        {
            wetRev.setTargetValue (wet);
            const float w = wetRev.getNextValue();
            wetRev.skip (n - 1);
            reverb.process (sendBuf.data(), outL, outR, n, rt, 0.45f, w);
        }
    }
}

//==============================================================================
// Key detection
//==============================================================================
int detectSongKey (juce::AudioFormatReader& reader, std::atomic<bool>* cancel)
{
    if (reader.sampleRate <= 0 || reader.lengthInSamples <= 0) return -1;
    const int step = juce::jmax (1, (int) std::round (reader.sampleRate / 11025.0));
    const double rate = reader.sampleRate / step;
    constexpr int order = 13, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2), mono, win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);

    // read up to 4 minutes, decimated to ~11 kHz (box filter)
    const auto total = juce::jmin (reader.lengthInSamples, (juce::int64) (reader.sampleRate * 240));
    juce::AudioBuffer<float> b ((int) juce::jmax (1u, reader.numChannels), 65536);
    for (juce::int64 pos = 0; pos < total; pos += 65536)
    {
        if (cancel != nullptr && cancel->load()) return -1;
        const int n = (int) juce::jmin ((juce::int64) 65536, total - pos);
        reader.read (&b, 0, n, pos, true, true);
        for (int i = 0; i + step <= n; i += step)
        {
            float s = 0;
            for (int k = 0; k < step; ++k)
                for (int c = 0; c < b.getNumChannels(); ++c) s += b.getSample (c, i + k);
            mono.push_back (s / (float) (step * b.getNumChannels()));
        }
    }

    double chroma[12] {};
    for (size_t start = 0; start + (size_t) N <= mono.size(); start += (size_t) N / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i) buf[(size_t) i] = mono[start + (size_t) i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 1; k < N / 2; ++k)
        {
            const double f = k * rate / N;
            if (f < 55.0 || f > 1760.0) continue;
            const int pc = (((int) std::round (12.0 * std::log2 (f / 440.0)) + 69) % 12 + 12) % 12;
            chroma[pc] += std::sqrt ((double) buf[(size_t) k]);
        }
    }

    static const double maj[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double mnr[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    auto corr = [&chroma] (const double* prof, int tonic)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += chroma[(i + tonic) % 12]; my += prof[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double a = chroma[(i + tonic) % 12] - mx, c = prof[i] - my;
            sxy += a * c; sxx += a * a; syy += c * c;
        }
        return sxx > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    };
    int best = -1; double bestR = 0.3;
    for (int t = 0; t < 12; ++t)
    {
        const double a = corr (maj, t), m = corr (mnr, t);
        if (a > bestR) { bestR = a; best = t; }
        if (m > bestR) { bestR = m; best = 12 + t; }
    }
    return best;
}

juce::String keyName (int key)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (key < 0) return u8 ("সব নোট (ক্রোম্যাটিক)");
    return juce::String (names[key % 12]) + (key < 12 ? u8 (" মেজর") : u8 (" মাইনর"));
}

//==============================================================================
// Presets
//==============================================================================
void Preset::applyTo (FxParams& p) const
{
    for (int i = 0; i < FxParams::numParams; ++i) p.set ((FxParams::Param) i, paramDefaults[i]);
    for (int i = 0; i < FxParams::numSwitches; ++i) p.setOn ((FxParams::Switch) i, switchDefaults[i]);
    for (auto& v : values) p.set (v.first, v.second);
    for (auto& s : switches) p.setOn (s.first, s.second);
    // switches follow the amounts
    p.setOn (FxParams::tuneOn, p.get (FxParams::tuneAmt) > 0.5f);
    p.setOn (FxParams::echoOn, p.get (FxParams::echo) > 0.5f);
    p.setOn (FxParams::dblOn, p.get (FxParams::dbl) > 0.5f);
}

const std::vector<Preset>& builtInPresets()
{
    using P = FxParams;
    static const std::vector<Preset> list = []
    {
        std::vector<Preset> v;
        auto add = [&v] (const char* id, const char* cat, const char* name, const char* note, std::vector<std::pair<P::Param, float>> vals)
        {
            Preset p; p.id = id; p.category = cat; p.name = juce::String::fromUTF8 (name); p.note = juce::String::fromUTF8 (note); p.values = std::move (vals);
            v.push_back (std::move (p));
        };
        add ("natural", "song", "ন্যাচারাল", "ইফেক্ট ছাড়া নিজের পরিষ্কার কণ্ঠ", { { P::tuneAmt, 20 }, { P::rev, 15 }, { P::room, 40 }, { P::echo, 0 }, { P::body, 35 }, { P::warm, 10 } });
        add ("studio", "song", "স্টুডিও ক্লিয়ার", "রেকর্ডিং স্টুডিওর মতো ঝকঝকে কণ্ঠ", { { P::tuneAmt, 45 }, { P::rev, 30 }, { P::room, 40 }, { P::echo, 10 }, { P::body, 60 }, { P::bright, 60 } });
        add ("modern", "song", "আধুনিক বাংলা", "মিষ্টি, উজ্জ্বল — এখনকার গানের মতো", { { P::tuneAmt, 50 }, { P::tuneSpd, 55 }, { P::rev, 40 }, { P::echo, 20 }, { P::body, 60 }, { P::bright, 62 } });
        add ("romantic", "song", "রোমান্টিক", "মোলায়েম, কাছের কণ্ঠ", { { P::tuneAmt, 50 }, { P::rev, 45 }, { P::room, 50 }, { P::echo, 15 }, { P::bass, 58 }, { P::bright, 42 } });
        add ("ghazal", "song", "গজল", "উষ্ণ কণ্ঠ, লম্বা হলঘরের রিভার্ব", { { P::tuneAmt, 45 }, { P::rev, 55 }, { P::room, 70 }, { P::echo, 25 }, { P::bass, 55 }, { P::warm, 60 } });
        add ("tagore", "song", "রবীন্দ্র / নজরুল", "নরম, স্বাভাবিক, হালকা ঘরের আওয়াজ", { { P::tuneAmt, 40 }, { P::rev, 35 }, { P::room, 45 }, { P::echo, 10 }, { P::bright, 48 } });
        add ("folk", "song", "লোকগীতি / বাউল", "খোলা গলা, খোলা মাঠের মতো আওয়াজ", { { P::tuneAmt, 35 }, { P::rev, 30 }, { P::room, 35 }, { P::echo, 20 }, { P::bright, 58 } });
        add ("qawwali", "song", "কাওয়ালি", "জোরালো, ভরাট, মঞ্চের মতো", { { P::tuneAmt, 40 }, { P::rev, 45 }, { P::body, 72 }, { P::bass, 60 }, { P::dbl, 25 } });
        add ("film", "song", "সিনেমার প্লেব্যাক", "ঝকঝকে, বড় রিভার্ব, হালকা ডাবল", { { P::tuneAmt, 55 }, { P::tuneSpd, 55 }, { P::rev, 55 }, { P::room, 65 }, { P::echo, 25 }, { P::dbl, 20 }, { P::bright, 60 } });
        add ("duet", "song", "দ্বৈত গান (মেয়ের সাথে)", "দুই কণ্ঠের সুর ও ভলিউম মিলিয়ে রাখে", { { P::tuneAmt, 55 }, { P::rev, 40 }, { P::body, 65 } });
        add ("kid", "song", "চিকন কণ্ঠ", "উঁচু সুরে কর্কশ শোনাবে না", { { P::tuneAmt, 55 }, { P::rev, 35 }, { P::bright, 38 }, { P::deess, 60 } });
        add ("bandrock", "band", "ব্যান্ড রক — ভরাট গলা", "মোটা, ভারী, শক্তিশালী — বাংলা ব্যান্ডের গানের মতো", { { P::tuneAmt, 35 }, { P::bass, 72 }, { P::body, 80 }, { P::warm, 70 }, { P::rev, 35 }, { P::room, 50 }, { P::echo, 20 }, { P::dbl, 30 } });
        add ("hardrock", "band", "হার্ড রক — খসখসে গলা", "খসখসে, ধারালো, চড়া গলা", { { P::tuneAmt, 30 }, { P::bass, 62 }, { P::body, 85 }, { P::warm, 85 }, { P::rev, 30 }, { P::echo, 25 }, { P::dbl, 35 } });
        add ("ballad", "band", "রক ব্যালাড", "ভরাট গলা, লম্বা রিভার্ব — দুঃখের গানে", { { P::tuneAmt, 40 }, { P::bass, 66 }, { P::body, 70 }, { P::warm, 55 }, { P::rev, 60 }, { P::room, 75 }, { P::echo, 30 } });
        add ("unplug", "band", "আনপ্লাগড / অ্যাকুস্টিক", "গিটারের সাথে কাছ থেকে, স্বাভাবিক", { { P::tuneAmt, 30 }, { P::bass, 58 }, { P::body, 55 }, { P::rev, 25 }, { P::room, 30 }, { P::echo, 5 } });
        add ("hamd", "rel", "হামদ ও নাত", "নরম, পবিত্র, প্রশান্ত আওয়াজ — মিউজিক ছাড়াও", { { P::tuneAmt, 50 }, { P::rev, 50 }, { P::room, 65 }, { P::echo, 15 }, { P::warm, 50 } });
        add ("waz", "rel", "ওয়াজ মাহফিল", "বড় মাহফিলের মতো জোরালো, দূর পর্যন্ত স্পষ্ট", { { P::tuneAmt, 0 }, { P::level, 75 }, { P::bass, 60 }, { P::body, 78 }, { P::rev, 35 }, { P::room, 80 }, { P::echo, 40 }, { P::etime, 65 }, { P::bright, 58 } });
        add ("quran", "rel", "তিলাওয়াত / আজান", "মসজিদের মতো লম্বা, গম্ভীর প্রতিধ্বনি", { { P::tuneAmt, 0 }, { P::rev, 70 }, { P::room, 90 }, { P::echo, 20 }, { P::bass, 58 }, { P::warm, 45 } });
        add ("speech", "talk", "বক্তৃতা", "স্পষ্ট কথা, ইকো নেই", { { P::tuneAmt, 0 }, { P::level, 75 }, { P::rev, 10 }, { P::room, 20 }, { P::echo, 0 }, { P::body, 70 }, { P::bright, 56 } });
        add ("recite", "talk", "আবৃত্তি", "ভরাট, আবেগী গলা, হালকা ঘরের আওয়াজ", { { P::tuneAmt, 0 }, { P::bass, 64 }, { P::body, 66 }, { P::warm, 55 }, { P::rev, 32 }, { P::room, 45 }, { P::echo, 8 } });
        add ("radio", "talk", "রেডিও / পডকাস্ট", "কাছের, ভারী, পেশাদার রেডিওর মতো", { { P::tuneAmt, 0 }, { P::bass, 68 }, { P::body, 80 }, { P::rev, 5 }, { P::echo, 0 } });
        add ("mc", "talk", "অনুষ্ঠান উপস্থাপনা", "উজ্জ্বল, প্রাণবন্ত, মঞ্চের ঘোষকের মতো", { { P::tuneAmt, 0 }, { P::body, 72 }, { P::bright, 64 }, { P::rev, 22 }, { P::room, 55 }, { P::echo, 12 } });
        return v;
    }();
    return list;
}

//==============================================================================
juce::Array<UserPreset> UserPresets::list() const
{
    juce::Array<UserPreset> out;
    for (auto line : juce::StringArray::fromLines (props.getValue ("userPresets")))
    {
        auto parts = juce::StringArray::fromTokens (line, "\t", "");
        if (parts.size() == 3 && parts[0].isNotEmpty()) out.add ({ parts[0], parts[1], parts[2] });
    }
    return out;
}

void UserPresets::store (const juce::Array<UserPreset>& l)
{
    juce::StringArray lines;
    for (auto& p : l) lines.add (p.id + "\t" + clean (p.name) + "\t" + p.state);
    props.setValue ("userPresets", lines.joinIntoString ("\n"));
    props.saveIfNeeded();
}

juce::String UserPresets::add (const juce::String& name, const juce::String& fxState)
{
    auto l = list();
    auto id = "user-" + juce::String (juce::Time::currentTimeMillis()) + "-" + juce::String (juce::Random::getSystemRandom().nextInt (100000));
    l.add ({ id, clean (name), fxState });
    store (l);
    return id;
}

bool UserPresets::rename (const juce::String& id, const juce::String& newName)
{
    auto l = list();
    for (auto& p : l) if (p.id == id && clean (newName).isNotEmpty()) { p.name = clean (newName); store (l); return true; }
    return false;
}

bool UserPresets::remove (const juce::String& id)
{
    auto l = list();
    for (int i = 0; i < l.size(); ++i) if (l.getReference (i).id == id) { l.remove (i); store (l); return true; }
    return false;
}

const UserPreset* UserPresets::find (const juce::String& id, juce::Array<UserPreset>& storage) const
{
    storage = list();
    for (auto& p : storage) if (p.id == id) return &p;
    return nullptr;
}
}
