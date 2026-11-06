#include "MixTools.h"
#include <complex>

namespace tf
{
using cpx = std::complex<float>;

//==============================================================================
// MusicDucker
//==============================================================================
void MusicDucker::prepare (double sampleRate) { sr = sampleRate; reset(); }
void MusicDucker::reset() { env = 0.0f; act = 0.0f; lastDip = 99.0f; sinceCoef = 0; pkL.reset(); pkR.reset(); }

void MusicDucker::process (float* L, float* R, const float* voice, int n, float amount) noexcept
{
    const float aEnvUp = 1.0f - std::exp (-1.0f / (0.005f * (float) sr)), aEnvDn = 1.0f - std::exp (-1.0f / (0.12f * (float) sr));
    const float aActUp = 1.0f - std::exp (-1.0f / (0.06f * (float) sr)), aActDn = 1.0f - std::exp (-1.0f / (0.35f * (float) sr));
    for (int i = 0; i < n; ++i)
    {
        const float v = voice != nullptr ? std::abs (voice[i]) : 0.0f;
        env += (v > env ? aEnvUp : aEnvDn) * (v - env);
        const float envDb = juce::Decibels::gainToDecibels (env + 1e-9f);
        const float target = juce::jlimit (0.0f, 1.0f, (envDb + 48.0f) / 18.0f) * juce::jlimit (0.0f, 1.0f, amount);
        act += (target > act ? aActUp : aActDn) * (target - act);

        if (--sinceCoef <= 0)
        {
            sinceCoef = 32;
            const float dip = -3.5f * act;
            if (std::abs (dip - lastDip) > 0.05f)
            {
                pkL.setPeak (sr, 2800.0, 0.9, dip);
                pkR.b0 = pkL.b0; pkR.b1 = pkL.b1; pkR.b2 = pkL.b2; pkR.a1 = pkL.a1; pkR.a2 = pkL.a2;
                lastDip = dip;
            }
        }
        const float g = juce::Decibels::decibelsToGain (-2.5f * act);
        if (act > 0.002f || std::abs (lastDip) > 0.01f)
        {
            L[i] = pkL.process (L[i]) * g;
            if (R != nullptr) R[i] = pkR.process (R[i]) * g;
        }
    }
}

//==============================================================================
// MasterBus
//==============================================================================
void MasterBus::prepare (double sampleRate)
{
    sr = sampleRate;
    la = juce::jmax (8, (int) std::lround (lookahead * sr / 48000.0));
    dL.assign ((size_t) la, 0.0f); dR.assign ((size_t) la, 0.0f); pk.assign ((size_t) la, 1.0f);
    attG = 1.0f - std::exp (-1.0f / (0.025f * (float) sr));
    relG = 1.0f - std::exp (-1.0f / (0.30f * (float) sr));
    attD = 1.0f - std::exp (-1.0f / (0.015f * (float) sr));
    limRel = 1.0f - std::exp (-1.0f / (0.12f * (float) sr));
    reset();
}

void MasterBus::reset()
{
    det = 0.0f; gr = 1.0f; lim = 1.0f; w = 0;
    std::fill (dL.begin(), dL.end(), 0.0f); std::fill (dR.begin(), dR.end(), 0.0f); std::fill (pk.begin(), pk.end(), 1.0f);
}

void MasterBus::process (float* L, float* R, int n, bool glue) noexcept
{
    const float ceiling = 0.891f;   // -1 dBFS
    const float thr = -18.0f, ratio = 1.8f, knee = 8.0f;
    for (int i = 0; i < n; ++i)
    {
        float l = L[i], r = R != nullptr ? R[i] : l;
        if (glue)
        {
            det += attD * (0.5f * (l * l + r * r) - det);
            const float lv = 10.0f * std::log10 (det + 1e-12f);
            const float over = lv - thr;
            float grDb = 0.0f;
            if (over > knee * 0.5f) grDb = (1.0f / ratio - 1.0f) * over;
            else if (over > -knee * 0.5f) grDb = (1.0f / ratio - 1.0f) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
            const float t = juce::Decibels::decibelsToGain (juce::jmax (-6.0f, grDb));
            gr += (t < gr ? attG : relG) * (t - gr);
            l *= gr; r *= gr;
        }
        // look-ahead peak limiter
        const float p = juce::jmax (std::abs (l), std::abs (r));
        pk[(size_t) w] = p > ceiling ? ceiling / p : 1.0f;
        float need = 1.0f;
        for (auto v : pk) need = juce::jmin (need, v);
        if (need < lim) lim += (need - lim) * juce::jmin (1.0f, 4.0f / (float) la);
        else lim += (need - lim) * limRel;
        const float ol = dL[(size_t) w] * lim, orr = dR[(size_t) w] * lim;
        dL[(size_t) w] = l; dR[(size_t) w] = r;
        w = (w + 1) % la;
        auto soft = [ceiling] (float x) { const float a = std::abs (x); return a <= ceiling ? x : std::copysign (ceiling + (1.0f - ceiling) * std::tanh ((a - ceiling) / (1.0f - ceiling)), x); };
        L[i] = soft (ol);
        if (R != nullptr) R[i] = soft (orr);
    }
}

namespace mixtools
{
//==============================================================================
// Loudness (ITU-R BS.1770-4)
//==============================================================================
static std::vector<double> blockPowers (const juce::AudioBuffer<float>& b, int numSamples, double sr)
{
    const int chs = juce::jmin (2, b.getNumChannels());
    std::vector<double> sq ((size_t) numSamples, 0.0);
    for (int ch = 0; ch < chs; ++ch)
    {
        Biquad sh, hp;
        sh.setHighShelf (sr, 1500.0, 0.7071, 4.0);
        hp.setHighPass (sr, 38.0, 0.5);
        const float* x = b.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const double y = hp.process (sh.process (x[i]));
            sq[(size_t) i] += y * y * (chs == 1 ? 2.0 : 1.0);   // a mono file is played on both speakers
        }
    }
    const int blk = (int) std::lround (0.4 * sr), hop = (int) std::lround (0.1 * sr);
    std::vector<double> out;
    double run = 0.0;
    std::vector<double> prefix ((size_t) numSamples + 1, 0.0);
    for (int i = 0; i < numSamples; ++i) { run += sq[(size_t) i]; prefix[(size_t) i + 1] = run; }
    for (int s = 0; s + blk <= numSamples; s += hop) out.push_back ((prefix[(size_t) (s + blk)] - prefix[(size_t) s]) / blk);
    return out;
}

double gatedLoudness (const juce::AudioBuffer<float>& b, int numSamples, double sr, const std::vector<bool>* active)
{
    auto p = blockPowers (b, numSamples, sr);
    auto lk = [] (double ms) { return -0.691 + 10.0 * std::log10 (ms + 1e-20); };
    double sum = 0.0; int cnt = 0;
    for (size_t i = 0; i < p.size(); ++i)
        if (lk (p[i]) > -70.0 && (active == nullptr || (i < active->size() && (*active)[i]))) { sum += p[i]; ++cnt; }
    if (cnt == 0) return -100.0;
    const double rel = lk (sum / cnt) - 10.0;
    sum = 0.0; cnt = 0;
    for (size_t i = 0; i < p.size(); ++i)
        if (lk (p[i]) > -70.0 && lk (p[i]) > rel && (active == nullptr || (i < active->size() && (*active)[i]))) { sum += p[i]; ++cnt; }
    return cnt > 0 ? lk (sum / cnt) : -100.0;
}

double integratedLufs (const juce::AudioBuffer<float>& b, int numSamples, double sr) { return gatedLoudness (b, numSamples, sr, nullptr); }

void applyFades (juce::AudioBuffer<float>& b, int numSamples, double sr, double fin, double fout)
{
    const int a = juce::jmin (numSamples, (int) std::lround (fin * sr)), z = juce::jmin (numSamples, (int) std::lround (fout * sr));
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* x = b.getWritePointer (ch);
        for (int i = 0; i < a; ++i) x[i] *= (float) std::sin (0.5 * juce::MathConstants<double>::pi * i / a);
        for (int i = 0; i < z; ++i) x[numSamples - 1 - i] *= (float) std::sin (0.5 * juce::MathConstants<double>::pi * i / z);
    }
}

//==============================================================================
// Files
//==============================================================================
std::vector<float> readMono (const juce::File& f, double* rate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::vector<float> out;
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) return out;
    if (rate != nullptr) *rate = r->sampleRate;
    out.resize ((size_t) r->lengthInSamples);
    const int chs = (int) juce::jmax (1u, r->numChannels);
    juce::AudioBuffer<float> b (chs, 1 << 16);
    for (juce::int64 p = 0; p < r->lengthInSamples; p += b.getNumSamples())
    {
        const int n = (int) juce::jmin ((juce::int64) b.getNumSamples(), r->lengthInSamples - p);
        r->read (&b, 0, n, p, true, true);
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < chs; ++c) s += b.getSample (c, i);
            out[(size_t) (p + i)] = s / (float) chs;
        }
    }
    return out;
}

bool writeMono (const juce::File& f, const std::vector<float>& x, double sr)
{
    f.deleteFile();
    auto fos = std::make_unique<juce::FileOutputStream> (f);
    if (! fos->openedOk()) return false;
    std::unique_ptr<juce::OutputStream> os (fos.release());
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (32));
    if (w == nullptr) return false;
    const float* ch[] = { x.data() };
    return w->writeFromFloatArrays (ch, 1, (int) x.size());
}

//==============================================================================
// FFT helper (real signal, N points <-> N/2+1 complex bins)
//==============================================================================
struct RFFT
{
    explicit RFFT (int order) : fft (order), n (1 << order), buf ((size_t) (2 << order), 0.0f) {}
    void forward (const float* x, cpx* X)
    {
        std::copy (x, x + n, buf.begin());
        std::fill (buf.begin() + n, buf.end(), 0.0f);
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= n / 2; ++k) X[k] = { buf[(size_t) (2 * k)], buf[(size_t) (2 * k + 1)] };
    }
    void inverse (const cpx* X, float* x)
    {
        for (int k = 0; k <= n / 2; ++k) { buf[(size_t) (2 * k)] = X[k].real(); buf[(size_t) (2 * k + 1)] = X[k].imag(); }
        for (int k = n / 2 + 1; k < n; ++k) { buf[(size_t) (2 * k)] = X[n - k].real(); buf[(size_t) (2 * k + 1)] = -X[n - k].imag(); }
        fft.performRealOnlyInverseTransform (buf.data());
        std::copy (buf.begin(), buf.begin() + n, x);   // JUCE scales the inverse by 1/n
    }
    juce::dsp::FFT fft;
    int n;
    std::vector<float> buf;
};

//==============================================================================
// Noise reduction
//==============================================================================
void reduceNoise (std::vector<float>& x, double sr, float reductionDb, Progress progress, std::atomic<bool>& cancel, double* floorOut)
{
    constexpr int order = 11, N = 1 << order, hop = N / 4, K = N / 2 + 1;
    const int len = (int) x.size();
    if (len < N) return;
    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / N));
    RFFT fft (order);
    const int frames = (len + N) / hop + 1;
    auto frameAt = [&] (int f, std::vector<float>& tmp)
    {
        const int s = f * hop - N;   // first frame starts before the audio (zero padded)
        for (int i = 0; i < N; ++i)
        {
            const int j = s + i;
            tmp[(size_t) i] = (j >= 0 && j < len ? x[(size_t) j] : 0.0f) * win[(size_t) i];
        }
    };

    // 1. noise profile from the quietest 15% of frames
    std::vector<float> tmp ((size_t) N);
    std::vector<cpx> X ((size_t) K);
    std::vector<std::pair<float, int>> energy;
    for (int f = 0; f < frames; ++f)
    {
        const int s = f * hop - N / 2;
        double e = 0; int c = 0;
        for (int i = 0; i < hop; ++i) { const int j = s + i; if (j >= 0 && j < len) { e += x[(size_t) j] * x[(size_t) j]; ++c; } }
        if (c == hop) energy.push_back ({ (float) (e / c), f });
    }
    if (energy.empty()) return;
    std::sort (energy.begin(), energy.end());
    const size_t take = juce::jmax ((size_t) 4, energy.size() * 15 / 100);
    std::vector<double> noise ((size_t) K, 0.0);
    for (size_t q = 0; q < take && q < energy.size(); ++q)
    {
        frameAt (energy[q].second, tmp);
        fft.forward (tmp.data(), X.data());
        for (int k = 0; k < K; ++k) noise[(size_t) k] += std::norm (X[(size_t) k]);
    }
    for (auto& v : noise) v /= (double) take;
    if (floorOut != nullptr) *floorOut = 10.0 * std::log10 (energy[take / 2].first + 1e-20);

    // 2. spectral gate: a bin keeps its level while it is clearly above the noise in the frame
    //    before, now or after (protects word starts); otherwise it is lowered. Gains move smoothly.
    const float floorG = juce::Decibels::decibelsToGain (-juce::jlimit (3.0f, 30.0f, reductionDb));
    const float sens = 4.0f;   // 6 dB above the noise counts as voice
    const float decay = std::pow (floorG, (float) hop / (float) (0.10 * sr));   // 100 ms release
    std::vector<float> out ((size_t) len + (size_t) N, 0.0f);
    std::vector<std::vector<float>> P (3, std::vector<float> ((size_t) K, 0.0f));
    std::vector<std::vector<cpx>> S (3, std::vector<cpx> ((size_t) K));
    std::vector<float> g ((size_t) K, 1.0f), gs ((size_t) K, 1.0f), y ((size_t) N);
    for (int f = 0; f <= frames; ++f)
    {
        if (cancel.load()) return;
        // shift the 3-frame window
        std::swap (P[0], P[1]); std::swap (P[1], P[2]);
        std::swap (S[0], S[1]); std::swap (S[1], S[2]);
        if (f < frames)
        {
            frameAt (f, tmp);
            fft.forward (tmp.data(), S[2].data());
            for (int k = 0; k < K; ++k) P[2][(size_t) k] = std::norm (S[2][(size_t) k]);
        }
        else std::fill (P[2].begin(), P[2].end(), 0.0f);
        const int cur = f - 1;
        if (cur < 0) continue;
        // a bin counts as voice when its level (averaged with its neighbours, so random noise peaks do not
        // count) stands clearly above the noise in at least two of the frames before, now and after
        for (int k = 0; k < K; ++k)
        {
            float lv[3];
            for (int f3 = 0; f3 < 3; ++f3)
            {
                const auto& pp = P[(size_t) f3];
                lv[f3] = (pp[(size_t) juce::jmax (0, k - 1)] + pp[(size_t) k] + pp[(size_t) juce::jmin (K - 1, k + 1)]) / 3.0f;
            }
            const float med = juce::jmax (juce::jmin (lv[0], lv[1]), juce::jmin (juce::jmax (lv[0], lv[1]), lv[2]));
            const float nref = (float) (noise[(size_t) juce::jmax (0, k - 1)] + noise[(size_t) k] + noise[(size_t) juce::jmin (K - 1, k + 1)]) / 3.0f;
            const float target = med > sens * nref ? 1.0f : floorG;
            g[(size_t) k] = target >= g[(size_t) k] ? target : juce::jmax (target, g[(size_t) k] * decay);
        }
        // light smoothing across frequency so single bins do not "twinkle" (musical noise)
        for (int k = 0; k < K; ++k)
        {
            const float a = g[(size_t) juce::jmax (0, k - 1)], b = g[(size_t) k], c = g[(size_t) juce::jmin (K - 1, k + 1)];
            gs[(size_t) k] = juce::jmax (b * 0.5f + 0.25f * (a + c), juce::jmin (a, c));
        }
        std::vector<cpx> Y ((size_t) K);
        for (int k = 0; k < K; ++k) Y[(size_t) k] = S[1][(size_t) k] * gs[(size_t) k];
        fft.inverse (Y.data(), y.data());
        const int s = cur * hop - N;
        for (int i = 0; i < N; ++i)
        {
            const int j = s + i;
            if (j >= 0 && j < len) out[(size_t) j] += y[(size_t) i] * win[(size_t) i] * 0.5f;   // sqrt-Hann x2 at 75% overlap sums to 2
        }
        if (progress && (f % 200) == 0) progress ((float) f / (float) frames);
    }
    std::copy (out.begin(), out.begin() + len, x.begin());
}

juce::String reduceNoise (const juce::File& in, const juce::File& out, float reductionDb, Progress progress, std::atomic<bool>& cancel, double* floorOut)
{
    double sr = 48000.0;
    auto x = readMono (in, &sr);
    if (x.empty()) return juce::String::fromUTF8 ("রেকর্ডিং পড়া যাচ্ছে না।");
    reduceNoise (x, sr, reductionDb, progress, cancel, floorOut);
    if (cancel.load()) return juce::String::fromUTF8 ("বাতিল করা হয়েছে।");
    if (! writeMono (out, x, sr)) return juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না।");
    return {};
}

//==============================================================================
// Delay finding (cross-correlation, PHAT weighted)
//==============================================================================
static int gccDelay (const std::vector<float>& mic, const std::vector<float>& ref, int maxLag, double& confidence)
{
    constexpr int order = 16, N = 1 << order, K = N / 2 + 1;
    RFFT fft (order);
    std::vector<cpx> A ((size_t) K), B ((size_t) K), acc ((size_t) K, cpx (0, 0));
    std::vector<float> ta ((size_t) N), tb ((size_t) N);
    const int len = (int) juce::jmin (mic.size(), ref.size());
    const int seg = N / 2;   // half of the FFT holds the signal, so lags up to N/2 are not wrapped
    int used = 0;
    for (int s = 0; s + seg <= len && used < 40; s += seg)
    {
        double er = 0;
        for (int i = 0; i < seg; ++i) er += ref[(size_t) (s + i)] * ref[(size_t) (s + i)];
        if (er / seg < 1e-7) continue;
        std::fill (ta.begin(), ta.end(), 0.0f); std::fill (tb.begin(), tb.end(), 0.0f);
        for (int i = 0; i < seg; ++i) { ta[(size_t) i] = mic[(size_t) (s + i)]; tb[(size_t) i] = ref[(size_t) (s + i)]; }
        // mic segment is longer (it contains the delayed echo): extend it
        for (int i = seg; i < N && s + i < len; ++i) ta[(size_t) i] = mic[(size_t) (s + i)];
        fft.forward (ta.data(), A.data());
        fft.forward (tb.data(), B.data());
        for (int k = 0; k < K; ++k)
        {
            auto c = A[(size_t) k] * std::conj (B[(size_t) k]);
            acc[(size_t) k] += c / (std::abs (c) + 1e-12f);
        }
        ++used;
    }
    confidence = 0.0;
    if (used == 0) return -1;
    std::vector<float> r ((size_t) N);
    fft.inverse (acc.data(), r.data());
    int best = 0; float bv = 0.0f; double mean = 0.0;
    const int lim = juce::jmin (maxLag, N / 2 - 1);
    for (int l = 0; l <= lim; ++l) { mean += std::abs (r[(size_t) l]); if (r[(size_t) l] > bv) { bv = r[(size_t) l]; best = l; } }
    mean /= (lim + 1);
    confidence = mean > 0 ? bv / mean : 0.0;
    return best;
}

int findClickDelay (const std::vector<float>& mic, const std::vector<float>& played, int maxLag, double* conf)
{
    double c = 0.0;
    const int d = gccDelay (mic, played, maxLag, c);
    if (conf != nullptr) *conf = c;
    return c > 8.0 ? d : -1;
}

//==============================================================================
// Bleed removal: partitioned-block frequency-domain adaptive filter (8192 taps) + gentle
// residual suppression. Offline, so it runs twice: the second pass starts already "trained".
//==============================================================================
bool removeBleed (std::vector<float>& mic, const std::vector<float>& refIn, double sr, Progress progress, std::atomic<bool>& cancel,
                  double* erleOut, int* delayOut)
{
    const int len = (int) juce::jmin (mic.size(), refIn.size());
    if (len < (int) sr) return false;
    double eref = 0; for (int i = 0; i < len; ++i) eref += refIn[(size_t) i] * refIn[(size_t) i];
    if (eref / len < 1e-8) return false;   // nothing played through the speakers

    double conf = 0.0;
    const int delay = gccDelay (mic, refIn, (int) (0.5 * sr), conf);
    if (delayOut != nullptr) *delayOut = delay;
    if (delay < 0 || conf < 6.0) return false;   // the music did not reach the mic (headphones)

    constexpr int B = 512, order = 10, N = 2 * B, K = N / 2 + 1, P = 16;
    const int pre = 64;
    std::vector<float> ref ((size_t) len, 0.0f);
    for (int i = 0; i < len; ++i) { const int j = i - (delay - pre); if (j >= 0 && j < len) ref[(size_t) i] = refIn[(size_t) j]; }

    RFFT fft (order);
    const int blocks = len / B;
    std::vector<std::vector<cpx>> W ((size_t) P, std::vector<cpx> ((size_t) K, cpx (0, 0)));
    std::vector<float> e ((size_t) len, 0.0f), yhatAll ((size_t) len, 0.0f);

    int passNo = -1;
    auto runPass = [&] (float mu, bool keep) -> bool
    {
        ++passNo;
        std::vector<std::vector<cpx>> Xh ((size_t) P, std::vector<cpx> ((size_t) K, cpx (0, 0)));
        std::vector<float> Px ((size_t) K, 0.0f), tmp ((size_t) N), y ((size_t) N), g ((size_t) N);
        std::vector<cpx> Y ((size_t) K), E ((size_t) K), G ((size_t) K);
        int head = 0, rr = 0;
        for (int b = 0; b < blocks; ++b)
        {
            if (cancel.load()) return false;
            const int s = b * B;
            for (int i = 0; i < N; ++i) { const int j = s - B + i; tmp[(size_t) i] = j >= 0 ? ref[(size_t) j] : 0.0f; }
            head = (head + P - 1) % P;
            fft.forward (tmp.data(), Xh[(size_t) head].data());
            const auto& X0 = Xh[(size_t) head];
            for (int k = 0; k < K; ++k) Px[(size_t) k] = 0.9f * Px[(size_t) k] + 0.1f * std::norm (X0[(size_t) k]);
            std::fill (Y.begin(), Y.end(), cpx (0, 0));
            for (int p = 0; p < P; ++p)
            {
                const auto& Xp = Xh[(size_t) ((head + p) % P)];
                const auto& Wp = W[(size_t) p];
                for (int k = 0; k < K; ++k) Y[(size_t) k] += Wp[(size_t) k] * Xp[(size_t) k];
            }
            fft.inverse (Y.data(), y.data());
            double ed = 0, ee = 0;
            for (int i = 0; i < B; ++i)
            {
                const float yh = y[(size_t) (B + i)];
                const float d = mic[(size_t) (s + i)];
                const float ei = d - yh;
                tmp[(size_t) i] = 0.0f;
                tmp[(size_t) (B + i)] = ei;
                ed += d * d; ee += ei * ei;
                if (keep) { e[(size_t) (s + i)] = ei; yhatAll[(size_t) (s + i)] = yh; }
            }
            fft.forward (tmp.data(), E.data());
            // a loud voice (much more than the echo explains) slows learning, so the filter does not "learn" the voice
            const float dt = ee > 4.0 * (ed - ee + 1e-9) && ed > 1e-6 ? 0.25f : 1.0f;
            float pmean = 0; for (auto v : Px) pmean += v; pmean /= (float) K;
            for (int p = 0; p < P; ++p)
            {
                const auto& Xp = Xh[(size_t) ((head + p) % P)];
                auto& Wp = W[(size_t) p];
                for (int k = 0; k < K; ++k)
                    G[(size_t) k] = std::conj (Xp[(size_t) k]) * E[(size_t) k] * (mu * dt / (Px[(size_t) k] * (float) P + 0.01f * pmean + 1e-9f));
                if (p == rr)
                {
                    // gradient constraint (keeps it a true linear filter), one partition per block
                    fft.inverse (G.data(), g.data());
                    std::fill (g.begin() + B, g.end(), 0.0f);
                    fft.forward (g.data(), G.data());
                }
                for (int k = 0; k < K; ++k) Wp[(size_t) k] += G[(size_t) k];
            }
            rr = (rr + 1) % P;
            if (progress && (b % 64) == 0) progress (juce::jmin (0.9f, (float) (passNo * blocks + b) / (float) (3 * blocks) * 0.9f));
        }
        return true;
    };
    if (! runPass (0.8f, false)) return false;
    if (! runPass (0.4f, false)) return false;
    if (! runPass (0.15f, true)) return false;

    // gentle residual suppression: where the music still dominates a frequency, lower it a little
    {
        constexpr int ro = 11, RN = 1 << ro, hop = RN / 4, RK = RN / 2 + 1;
        RFFT f2 (ro);
        std::vector<float> win ((size_t) RN), a ((size_t) RN), c ((size_t) RN), o ((size_t) len + RN, 0.0f), yy ((size_t) RN);
        for (int i = 0; i < RN; ++i) win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / RN));
        std::vector<cpx> EA ((size_t) RK), YA ((size_t) RK);
        std::vector<float> gs ((size_t) RK, 1.0f);
        for (int s = -RN; s < len; s += hop)
        {
            if (cancel.load()) return false;
            for (int i = 0; i < RN; ++i)
            {
                const int j = s + i;
                a[(size_t) i] = (j >= 0 && j < len ? e[(size_t) j] : 0.0f) * win[(size_t) i];
                c[(size_t) i] = (j >= 0 && j < len ? yhatAll[(size_t) j] : 0.0f) * win[(size_t) i];
            }
            f2.forward (a.data(), EA.data());
            f2.forward (c.data(), YA.data());
            for (int k = 0; k < RK; ++k)
            {
                const float pe = std::norm (EA[(size_t) k]), py = std::norm (YA[(size_t) k]);
                const float gt = juce::jlimit (0.15f, 1.0f, 1.0f - 0.7f * py / (pe + 1e-12f));
                gs[(size_t) k] = gt < gs[(size_t) k] ? gt : gs[(size_t) k] + 0.3f * (gt - gs[(size_t) k]);
                EA[(size_t) k] *= gs[(size_t) k];
            }
            f2.inverse (EA.data(), yy.data());
            for (int i = 0; i < RN; ++i) { const int j = s + i; if (j >= 0 && j < len) o[(size_t) j] += yy[(size_t) i] * win[(size_t) i] * 0.5f; }
        }
        double ed = 0, eo = 0;
        for (int i = 0; i < len; ++i) { ed += mic[(size_t) i] * mic[(size_t) i]; eo += o[(size_t) i] * o[(size_t) i]; }
        if (erleOut != nullptr) *erleOut = 10.0 * std::log10 ((ed + 1e-12) / (eo + 1e-12));
        std::copy (o.begin(), o.begin() + len, mic.begin());
    }
    if (progress) progress (1.0f);
    return true;
}

juce::String removeBleed (const juce::File& micF, const juce::File& refF, const juce::File& out, Progress progress, std::atomic<bool>& cancel,
                          double* erle, int* delayOut)
{
    double sr = 48000.0, sr2 = 48000.0;
    auto mic = readMono (micF, &sr);
    auto ref = readMono (refF, &sr2);
    if (mic.empty()) return juce::String::fromUTF8 ("রেকর্ডিং পড়া যাচ্ছে না।");
    if (ref.empty() || std::abs (sr - sr2) > 0.5) return juce::String::fromUTF8 ("এই রেকর্ডিংয়ের সাথে স্পিকারের আওয়াজ রাখা হয়নি (পুরোনো রেকর্ডিং)।");
    if (! removeBleed (mic, ref, sr, progress, cancel, erle, delayOut))
        return cancel.load() ? juce::String::fromUTF8 ("বাতিল করা হয়েছে।") : juce::String::fromUTF8 ("NOLEAK");
    if (! writeMono (out, mic, sr)) return juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না।");
    return {};
}
}
}
