#include "VocalFx.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

//==============================================================================
// FxParams
//==============================================================================
static const char* paramKeys[] = { "tuneAmt", "tuneSpd", "bass", "mid", "bright", "body", "warm", "rev", "room", "echo", "etime", "dbl", "deess" };
static const char* switchKeys[] = { "tuneOn", "toneOn", "bodyOn", "revOn", "echoOn", "dblOn", "deessOn" };
static const float paramDefaults[] = { 50, 40, 50, 50, 50, 55, 30, 35, 55, 15, 45, 0, 40 };
static const bool switchDefaults[] = { true, true, true, true, true, false, true };

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
// AutoTune
//==============================================================================
void AutoTune::prepare (double sampleRate)
{
    sr = sampleRate;
    latency = juce::roundToInt (2048.0 * sr / 48000.0);
    hop = juce::jmax (64, juce::roundToInt (256.0 * sr / 48000.0));
    inRing.assign (ringSize, 0.0f);
    outAcc.assign (ringSize, 0.0f);
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);
    fa.assign (2 * fftSize, 0.0f);
    fb.assign (2 * fftSize, 0.0f);
    frame.assign (frameLen, 0.0f);
    dfun.assign (frameLen / 2 + 1, 0.0f);
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
    // frame = last frameLen input samples
    float energy = 0.0f;
    for (int j = 0; j < frameLen; ++j)
    {
        frame[(size_t) j] = inRing[(size_t) ((now - frameLen + j) & ringMask)];
        energy += frame[(size_t) j] * frame[(size_t) j];
    }
    const float rms = std::sqrt (energy / frameLen);
    const int half = frameLen / 2;
    const int tauMin = juce::jmax (2, (int) (sr / 1000.0));
    const int tauMax = juce::jmin (half - 2, (int) (sr / 60.0));

    if (rms < 0.004f) { periodSamples = 0.0f; detectedHz = 0.0f; return; }   // below about -48 dBFS: silence/unvoiced

    // r(tau) = sum_{j<half} f[j] f[j+tau] via FFT cross-correlation
    std::fill (fa.begin(), fa.end(), 0.0f);
    std::fill (fb.begin(), fb.end(), 0.0f);
    for (int j = 0; j < half; ++j) fa[(size_t) j] = frame[(size_t) j];
    for (int j = 0; j < frameLen; ++j) fb[(size_t) j] = frame[(size_t) j];
    fft->performRealOnlyForwardTransform (fa.data());
    fft->performRealOnlyForwardTransform (fb.data());
    for (int k = 0; k < fftSize; ++k)
    {
        const float ar = fa[(size_t) (2 * k)], ai = fa[(size_t) (2 * k + 1)];
        const float br = fb[(size_t) (2 * k)], bi = fb[(size_t) (2 * k + 1)];
        fb[(size_t) (2 * k)] = ar * br + ai * bi;       // conj(A) * B
        fb[(size_t) (2 * k + 1)] = ar * bi - ai * br;
    }
    fft->performRealOnlyInverseTransform (fb.data());

    float e1 = 0.0f;
    for (int j = 0; j < half; ++j) e1 += frame[(size_t) j] * frame[(size_t) j];
    const float scale = (std::abs (fb[0]) > 1e-12f) ? e1 / fb[0] : 1.0f;   // makes r(0) == e1 whatever the FFT scaling

    // d(tau) and cumulative mean normalised difference
    float e2 = e1;   // sum_{j<half} f[j+tau]^2, updated incrementally
    dfun[0] = 1.0f;
    float running = 0.0f;
    int found = -1;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        e2 += frame[(size_t) (tau + half - 1)] * frame[(size_t) (tau + half - 1)] - frame[(size_t) (tau - 1)] * frame[(size_t) (tau - 1)];
        const float d = juce::jmax (0.0f, e1 + e2 - 2.0f * fb[(size_t) tau] * scale);
        running += d;
        dfun[(size_t) tau] = running > 0 ? d * (float) tau / running : 1.0f;
    }
    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (dfun[(size_t) tau] < 0.15f)
        {
            while (tau + 1 <= tauMax && dfun[(size_t) (tau + 1)] < dfun[(size_t) tau]) ++tau;
            found = tau;
            break;
        }
    }
    if (found < 0)
    {
        // no clear period: unvoiced (breath, consonant, noise)
        periodSamples = 0.0f; detectedHz = 0.0f;
        return;
    }
    // parabolic interpolation
    float t = (float) found;
    if (found > tauMin && found < tauMax)
    {
        const float a = dfun[(size_t) (found - 1)], b = dfun[(size_t) found], c = dfun[(size_t) (found + 1)];
        const float den = a - 2 * b + c;
        if (std::abs (den) > 1e-9f) t += 0.5f * (a - c) / den;
    }
    periodSamples = t;
    detectedHz = (float) (sr / t);
}

void AutoTune::generateMarks() noexcept
{
    // analysis marks: one per pitch period while voiced, fixed small spacing while unvoiced
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

    // synthesis: emit output samples [t0, t0+n) where output time t <-> input time t - latency
    const juce::int64 t0 = now - n;
    const juce::int64 tEnd = now;
    while (! marks.empty())
    {
        // the largest grain half-width we may need
        if (nextSynthesis >= (double) (tEnd + 820)) break;
        const double ti = nextSynthesis - latency;
        // nearest analysis mark to ti
        size_t best = 0; double bd = 1e18;
        for (size_t k = 0; k < marks.size(); ++k)
        {
            const double d = std::abs ((double) marks[k] - ti);
            if (d < bd) { bd = d; best = k; }
            else if ((double) marks[k] > ti) break;
        }
        const int P = (int) markPeriod[best];
        const juce::int64 ta = marks[best];
        if (ta + P > now) break;                       // not enough input yet (cannot happen with our latency)
        const auto ts = (juce::int64) std::llround (nextSynthesis);
        if (ts - P >= tEnd) break;

        const int len = 2 * P;
        for (int k = 0; k < len; ++k)
        {
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) k / (float) len);
            outAcc[(size_t) ((ts - P + k) & ringMask)] += w * inRing[(size_t) ((ta - P + k) & ringMask)];
        }
        nextSynthesis += (double) P / juce::jlimit (0.5, 2.0, (double) markRatio[best]);

        // forget marks that are far in the past
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
// PlateReverb (Dattorro 1997)
//==============================================================================
void PlateReverb::prepare (double sampleRate)
{
    sr = sampleRate;
    scale = sr / 29761.0;
    auto L = [this] (int x) { return juce::jmax (1, (int) std::round (x * scale)); };
    lpre = L (600);  pre.init (lpre + 2);
    l1 = L (142); l2 = L (107); l3 = L (379); l4 = L (277);
    ap1.init (l1); ap2.init (l2); ap3.init (l3); ap4.init (l4);
    lmA = L (672); mapA.init (lmA + L (32));
    ldA = L (4453); dlA.init (ldA);
    l5 = L (1800); ap5.init (l5);
    ldA2 = L (3720); dlA2.init (ldA2);
    lmB = L (908); mapB.init (lmB + L (32));
    ldB = L (4217); dlB.init (ldB);
    l6 = L (2656); ap6.init (l6);
    ldB2 = L (3163); dlB2.init (ldB2);
    const int tl[7] = { 266, 2974, 1913, 1996, 1990, 187, 1066 }, tr[7] = { 353, 3627, 1228, 2673, 2111, 335, 121 };
    for (int i = 0; i < 7; ++i) { tapsL[i] = L (tl[i]); tapsR[i] = L (tr[i]); }
    lfoInc = (float) (juce::MathConstants<double>::twoPi * 0.9 / sr);
    reset();
}

void PlateReverb::reset()
{
    for (auto* d : { &pre, &ap1, &ap2, &ap3, &ap4, &mapA, &dlA, &ap5, &dlA2, &mapB, &dlB, &ap6, &dlB2 })
        std::fill (d->buf.begin(), d->buf.end(), 0.0f);
    bwState = dampA = dampB = lfo = 0.0f;
}

void PlateReverb::process (const float* in, float* outL, float* outR, int n, float decay, float damping, float wet) noexcept
{
    const float bandwidth = 0.9995f, exc = (float) (16.0 * scale);
    for (int i = 0; i < n; ++i)
    {
        pre.push (in[i]);
        float x = pre.tap (lpre - 1);
        bwState += bandwidth * (x - bwState);
        x = bwState;
        x = allpass (ap1, l1, 0.75f, x);
        x = allpass (ap2, l2, 0.75f, x);
        x = allpass (ap3, l3, 0.625f, x);
        x = allpass (ap4, l4, 0.625f, x);

        lfo += lfoInc; if (lfo > juce::MathConstants<float>::twoPi) lfo -= juce::MathConstants<float>::twoPi;
        const float modA = exc * (1.0f + std::sin (lfo)) * 0.5f, modB = exc * (1.0f + std::cos (lfo)) * 0.5f;

        // tank A, fed by the end of tank B
        {
            float u = x + decay * dlB2.tap (ldB2 - 1);
            const float v = mapA.tapFrac ((float) (lmA - 1) + modA);
            const float w = u - 0.7f * v;          // decay diffusion 1 (sign-inverted)
            mapA.push (w);
            u = v + 0.7f * w;
            dlA.push (u);
            float y = dlA.tap (ldA - 1);
            dampA += (1.0f - damping) * (y - dampA);
            y = dampA * decay;
            y = allpass (ap5, l5, 0.5f, y);
            dlA2.push (y);
        }
        // tank B, fed by the end of tank A
        {
            float u = x + decay * dlA2.tap (ldA2 - 1);
            const float v = mapB.tapFrac ((float) (lmB - 1) + modB);
            const float w = u - 0.7f * v;
            mapB.push (w);
            u = v + 0.7f * w;
            dlB.push (u);
            float y = dlB.tap (ldB - 1);
            dampB += (1.0f - damping) * (y - dampB);
            y = dampB * decay;
            y = allpass (ap6, l6, 0.5f, y);
            dlB2.push (y);
        }

        const float l = dlB.tap (tapsL[0]) + dlB.tap (tapsL[1]) - ap6.tap (tapsL[2]) + dlB2.tap (tapsL[3])
                      - dlA.tap (tapsL[4]) - ap5.tap (tapsL[5]) - dlA2.tap (tapsL[6]);
        const float r = dlA.tap (tapsR[0]) + dlA.tap (tapsR[1]) - ap5.tap (tapsR[2]) + dlA2.tap (tapsR[3])
                      - dlB.tap (tapsR[4]) - ap6.tap (tapsR[5]) - dlB2.tap (tapsR[6]);
        outL[i] += l * 0.6f * wet;
        outR[i] += r * 0.6f * wet;
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
    reverb.prepare (sr);
    work.assign ((size_t) maxBlock, 0.0f);
    tuneOut.assign ((size_t) maxBlock, 0.0f);
    revL.assign ((size_t) maxBlock, 0.0f); revR.assign ((size_t) maxBlock, 0.0f);

    int s = 1; while (s < (int) (0.05 * sr)) s <<= 1;
    delayLine.assign ((size_t) s, 0.0f); dlMask = s - 1; dlW = 0;
    s = 1; while (s < (int) (0.7 * sr)) s <<= 1;
    echoL.assign ((size_t) s, 0.0f); echoR.assign ((size_t) s, 0.0f); echoMask = s - 1; echoW = 0;

    deLp1.setLowPass (sr, 5500.0, 0.7071); deLp2.setLowPass (sr, 5500.0, 0.7071);
    deHp1.setHighPass (sr, 5500.0, 0.7071); deHp2.setHighPass (sr, 5500.0, 0.7071);
    warmLp.setLowPass (sr, 9000.0, 0.707);
    echoLpL.setLowPass (sr, 4200.0, 0.707); echoLpR.setLowPass (sr, 4200.0, 0.707);
    echoHpL.setHighPass (sr, 220.0, 0.707); echoHpR.setHighPass (sr, 220.0, 0.707);
    for (auto* v : { &wetRev, &wetEcho, &wetDbl, &drive, &bypassMix }) v->reset (sr, 0.05);
    lastBass = lastMid = lastBright = -1;
    reset();
}

void VocalChain::reset()
{
    tune.reset();
    reverb.reset();
    for (auto* b : { &deLp1, &deLp2, &deHp1, &deHp2, &low, &midPk, &high, &warmLp, &echoLpL, &echoLpR, &echoHpL, &echoHpR }) b->reset();
    std::fill (delayLine.begin(), delayLine.end(), 0.0f);
    std::fill (echoL.begin(), echoL.end(), 0.0f);
    std::fill (echoR.begin(), echoR.end(), 0.0f);
    deessEnv = compEnv = 0.0f;
}

int VocalChain::latencySamples (bool live) const noexcept
{
    if (tuneInPath) return tune.latencySamples();
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
    if (tuneInPath || (live && tuneOn && params.liveTune.load()))
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

    // ---- de-esser: split at 5.5 kHz (Linkwitz-Riley, the two bands add back to the full voice),
    //      and only the upper band is turned down while an "s"/"sh" is loud
    if (params.isOn (FxParams::deessOn))
    {
        const float d = params.get (FxParams::deess) / 100.0f;
        const float att = 1.0f - std::exp (-1.0f / (0.001f * (float) sr)), rel = 1.0f - std::exp (-1.0f / (0.06f * (float) sr));
        const float thr = 0.035f;
        for (int i = 0; i < n; ++i)
        {
            const float lo = deLp2.process (deLp1.process (x[i]));
            const float hi = deHp2.process (deHp1.process (x[i]));
            const float a = std::abs (hi);
            deessEnv += (a > deessEnv ? att : rel) * (a - deessEnv);
            float g = 1.0f;
            if (deessEnv > thr) g = std::pow (thr / deessEnv, d * 0.85f);
            x[i] = lo + hi * g;
        }
    }

    // ---- tone (EQ)
    if (params.isOn (FxParams::toneOn))
    {
        updateFilters();
        for (int i = 0; i < n; ++i) x[i] = high.process (midPk.process (low.process (x[i])));
    }

    // ---- full voice (compressor) + warmth
    if (params.isOn (FxParams::bodyOn))
    {
        const float body = params.get (FxParams::body) / 100.0f, warm = params.get (FxParams::warm) / 100.0f;
        const float thrDb = -12.0f - body * 22.0f, ratio = 2.0f + body * 4.0f;
        const float mk = juce::Decibels::decibelsToGain (-thrDb * (1.0f - 1.0f / ratio) * 0.35f);
        const float att = 1.0f - std::exp (-1.0f / (0.004f * (float) sr)), rel = 1.0f - std::exp (-1.0f / (0.09f * (float) sr));
        drive.setTargetValue (1.0f + warm * 5.0f);
        const float satMix = warm * 0.7f;
        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            compEnv += (a > compEnv ? att : rel) * (a - compEnv);
            const float envDb = juce::Decibels::gainToDecibels (compEnv + 1e-9f);
            const float over = envDb - thrDb;
            const float gDb = over > 0.0f ? -over * (1.0f - 1.0f / ratio) : 0.0f;
            float y = x[i] * juce::Decibels::decibelsToGain (gDb) * mk;
            const float dr = drive.getNextValue();
            if (satMix > 0.01f)
            {
                const float s = warmLp.process (std::tanh (dr * y) / dr);
                y = y + satMix * (s - y);
            }
            // soft ceiling: loud attacks that slip past the compressor are rounded off, never clipped
            const float ay = std::abs (y);
            if (ay > 0.8f) y = std::copysign (0.8f + 0.18f * std::tanh ((ay - 0.8f) / 0.18f), y);
            x[i] = y;
        }
    }

    for (int i = 0; i < n; ++i) { outL[i] = x[i]; outR[i] = x[i]; }

    // ---- double voice: two short, slowly moving delays spread left/right
    wetDbl.setTargetValue (params.isOn (FxParams::dblOn) ? params.get (FxParams::dbl) / 100.0f * 0.55f : 0.0f);
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

    // ---- echo (ping-pong)
    wetEcho.setTargetValue (params.isOn (FxParams::echoOn) ? params.get (FxParams::echo) / 100.0f * 0.5f : 0.0f);
    {
        const int dly = (int) (sr * (0.09 + params.get (FxParams::etime) / 100.0f * 0.46));
        const float fb = 0.38f;
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
        }
    }

    // ---- reverb (plate)
    {
        const bool on = params.isOn (FxParams::revOn);
        const float wet = on ? params.get (FxParams::rev) / 100.0f * 0.6f : 0.0f;
        const float decay = 0.45f + params.get (FxParams::room) / 100.0f * 0.48f;
        if (on || wetRev.isSmoothing() || wetRev.getCurrentValue() > 0.0f)
        {
            wetRev.setTargetValue (wet);
            const float w = wetRev.getNextValue();
            wetRev.skip (n - 1);
            reverb.process (x, outL, outR, n, decay, 0.35f, w);
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
        add ("modern", "song", "আধুনিক বাংলা", "মিষ্টি, উজ্জ্বল — এখনকার গানের মতো", { { P::tuneAmt, 60 }, { P::tuneSpd, 55 }, { P::rev, 40 }, { P::echo, 20 }, { P::body, 60 }, { P::bright, 62 } });
        add ("romantic", "song", "রোমান্টিক", "মোলায়েম, কাছের কণ্ঠ", { { P::tuneAmt, 50 }, { P::rev, 45 }, { P::room, 50 }, { P::echo, 15 }, { P::bass, 58 }, { P::bright, 42 } });
        add ("ghazal", "song", "গজল", "উষ্ণ কণ্ঠ, লম্বা হলঘরের রিভার্ব", { { P::tuneAmt, 55 }, { P::rev, 55 }, { P::room, 70 }, { P::echo, 25 }, { P::bass, 55 }, { P::warm, 60 } });
        add ("tagore", "song", "রবীন্দ্র / নজরুল", "নরম, স্বাভাবিক, হালকা ঘরের আওয়াজ", { { P::tuneAmt, 40 }, { P::rev, 35 }, { P::room, 45 }, { P::echo, 10 }, { P::bright, 48 } });
        add ("folk", "song", "লোকগীতি / বাউল", "খোলা গলা, খোলা মাঠের মতো আওয়াজ", { { P::tuneAmt, 35 }, { P::rev, 30 }, { P::room, 35 }, { P::echo, 20 }, { P::bright, 58 } });
        add ("qawwali", "song", "কাওয়ালি", "জোরালো, ভরাট, মঞ্চের মতো", { { P::tuneAmt, 40 }, { P::rev, 45 }, { P::body, 72 }, { P::bass, 60 }, { P::dbl, 25 } });
        add ("film", "song", "সিনেমার প্লেব্যাক", "ঝকঝকে, বড় রিভার্ব, হালকা ডাবল", { { P::tuneAmt, 65 }, { P::tuneSpd, 55 }, { P::rev, 55 }, { P::room, 65 }, { P::echo, 25 }, { P::dbl, 20 }, { P::bright, 60 } });
        add ("duet", "song", "দ্বৈত গান (মেয়ের সাথে)", "দুই কণ্ঠের সুর ও ভলিউম মিলিয়ে রাখে", { { P::tuneAmt, 55 }, { P::rev, 40 }, { P::body, 65 } });
        add ("kid", "song", "চিকন কণ্ঠ", "উঁচু সুরে কর্কশ শোনাবে না", { { P::tuneAmt, 55 }, { P::rev, 35 }, { P::bright, 38 }, { P::deess, 60 } });
        add ("bandrock", "band", "ব্যান্ড রক — ভরাট গলা", "মোটা, ভারী, শক্তিশালী — বাংলা ব্যান্ডের গানের মতো", { { P::tuneAmt, 35 }, { P::bass, 72 }, { P::body, 80 }, { P::warm, 70 }, { P::rev, 35 }, { P::room, 50 }, { P::echo, 20 }, { P::dbl, 30 } });
        add ("hardrock", "band", "হার্ড রক — খসখসে গলা", "খসখসে, ধারালো, চড়া গলা", { { P::tuneAmt, 30 }, { P::bass, 62 }, { P::body, 85 }, { P::warm, 85 }, { P::rev, 30 }, { P::echo, 25 }, { P::dbl, 35 } });
        add ("ballad", "band", "রক ব্যালাড", "ভরাট গলা, লম্বা রিভার্ব — দুঃখের গানে", { { P::tuneAmt, 40 }, { P::bass, 66 }, { P::body, 70 }, { P::warm, 55 }, { P::rev, 60 }, { P::room, 75 }, { P::echo, 30 } });
        add ("unplug", "band", "আনপ্লাগড / অ্যাকুস্টিক", "গিটারের সাথে কাছ থেকে, স্বাভাবিক", { { P::tuneAmt, 30 }, { P::bass, 58 }, { P::body, 55 }, { P::rev, 25 }, { P::room, 30 }, { P::echo, 5 } });
        add ("hamd", "rel", "হামদ ও নাত", "নরম, পবিত্র, প্রশান্ত আওয়াজ — মিউজিক ছাড়াও", { { P::tuneAmt, 50 }, { P::rev, 50 }, { P::room, 65 }, { P::echo, 15 }, { P::warm, 50 } });
        add ("waz", "rel", "ওয়াজ মাহফিল", "বড় মাহফিলের মতো জোরালো, দূর পর্যন্ত স্পষ্ট", { { P::tuneAmt, 0 }, { P::bass, 60 }, { P::body, 78 }, { P::rev, 35 }, { P::room, 80 }, { P::echo, 40 }, { P::etime, 65 }, { P::bright, 58 } });
        add ("quran", "rel", "তিলাওয়াত / আজান", "মসজিদের মতো লম্বা, গম্ভীর প্রতিধ্বনি", { { P::tuneAmt, 0 }, { P::rev, 70 }, { P::room, 90 }, { P::echo, 20 }, { P::bass, 58 }, { P::warm, 45 } });
        add ("speech", "talk", "বক্তৃতা", "স্পষ্ট কথা, ইকো নেই", { { P::tuneAmt, 0 }, { P::rev, 10 }, { P::room, 20 }, { P::echo, 0 }, { P::body, 70 }, { P::bright, 56 } });
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
