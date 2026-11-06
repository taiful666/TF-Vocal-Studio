#include "AudioInput.h"

namespace tf
{
//==============================================================================
static double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

SincInterpolator::SincInterpolator()
{
    table.assign ((size_t) (phases + 1) * taps, 0.0f);
    const double beta = 8.6, cutoff = 0.94, i0b = besselI0 (beta);
    for (int p = 0; p <= phases; ++p)
    {
        const double frac = (double) p / phases;
        double sum = 0.0;
        std::array<double, taps> h {};
        for (int k = 0; k < taps; ++k)
        {
            const double d = (double) (k - half + 1) - frac;          // distance from the wanted position
            const double x = juce::MathConstants<double>::pi * d * cutoff;
            const double s = std::abs (d) < 1e-9 ? cutoff : cutoff * std::sin (x) / x;
            const double r = d / (double) half;
            const double w = std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b;
            h[(size_t) k] = s * w;
            sum += h[(size_t) k];
        }
        for (int k = 0; k < taps; ++k) table[(size_t) (p * taps + k)] = (float) (h[(size_t) k] / sum);
    }
}

//==============================================================================
InputBridge::InputBridge() { ring.assign ((size_t) ringSize, 0.0f); }
InputBridge::~InputBridge() { close(); }

juce::String InputBridge::open (juce::AudioIODeviceType& type, const juce::String& inputName, double sampleRate, int blockSize)
{
    close();
    type.scanForDevices();
    auto names = type.getDeviceNames (true);
    if (names.isEmpty()) return juce::String::fromUTF8 ("কোনো মাইক পাওয়া যায়নি। মাইক লাগিয়ে আবার চেষ্টা করুন।");
    auto name = names.contains (inputName) ? inputName : names[juce::jmax (0, type.getDefaultDeviceIndex (true))];
    std::unique_ptr<juce::AudioIODevice> d (type.createDevice ({}, name));
    if (d == nullptr) return juce::String::fromUTF8 ("মাইক চালু করা যাচ্ছে না: ") + name;
    return openDevice (std::move (d), sampleRate, blockSize);
}

juce::String InputBridge::openDevice (std::unique_ptr<juce::AudioIODevice> d, double sampleRate, int blockSize)
{
    close();
    juce::BigInteger ins;
    ins.setRange (0, juce::jlimit (1, 2, d->getInputChannelNames().size()), true);
    auto rates = d->getAvailableSampleRates();
    double rate = sampleRate;
    if (! rates.isEmpty() && ! rates.contains (rate)) rate = rates.contains (48000.0) ? 48000.0 : rates.getLast();
    auto err = d->open (ins, {}, rate, blockSize);
    if (err.isNotEmpty()) return err;
    device = std::move (d);
    written = 0;
    std::fill (ring.begin(), ring.end(), 0.0f);
    running = true;
    device->start (this);
    return {};
}

void InputBridge::close()
{
    if (device != nullptr)
    {
        device->stop();
        device->close();
        device.reset();
    }
    running = false;
}

void InputBridge::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    inRate = d->getCurrentSampleRate();
    inBlock = juce::jmax (16, d->getCurrentBufferSizeSamples());
    chanRms.assign (8, 0.0f);
}

void InputBridge::audioDeviceStopped() {}

void InputBridge::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int n, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOut; ++ch) if (out[ch] != nullptr) juce::FloatVectorOperations::clear (out[ch], n);
    // which channels carry the voice? (a stereo USB mic may have one silent side)
    const int chs = juce::jmin (numIn, (int) chanRms.size());
    float maxRms = 0.0f;
    const float a = 1.0f - std::exp (-(float) n / (float) (inRate.load() * 2.0));
    for (int ch = 0; ch < chs; ++ch)
    {
        if (in[ch] == nullptr) continue;
        float s = 0.0f;
        for (int i = 0; i < n; ++i) s += in[ch][i] * in[ch][i];
        chanRms[(size_t) ch] += (std::sqrt (s / (float) n) - chanRms[(size_t) ch]) * a;
        maxRms = juce::jmax (maxRms, chanRms[(size_t) ch]);
    }
    const auto w = written.load (std::memory_order_relaxed);
    int used = 0;
    bool use[8] {};
    for (int ch = 0; ch < chs; ++ch)
        if (in[ch] != nullptr && (maxRms < 1e-5f || chanRms[(size_t) ch] >= 0.08f * maxRms)) { use[ch] = true; ++used; }
    const float g = used > 0 ? 1.0f / (float) used : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < chs; ++ch) if (use[ch]) v += in[ch][i];
        ring[(size_t) ((w + i) & ringMask)] = v * g;
    }
    written.store (w + n, std::memory_order_release);
}

void InputBridge::prepareOutput (double rate) noexcept
{
    outRate = rate;
    primed = false;
    maxTarget = 0.0;
}

void InputBridge::pull (float* dest, int n) noexcept
{
    const auto w = written.load (std::memory_order_acquire);
    const double ratio = inRate.load (std::memory_order_relaxed) / outRate;
    if (auto* d = device.get()) inLatencyOut.store ((int) std::lround (d->getInputLatencyInSamples() / ratio), std::memory_order_relaxed);

    if (! running.load() || w == 0) { juce::FloatVectorOperations::clear (dest, n); return; }

    const double baseTarget = 2.0 * inBlock.load() + n * ratio + SincInterpolator::half + 32.0;
    if (! primed)
    {
        target = juce::jmax (baseTarget, maxTarget);
        if ((double) w < target + SincInterpolator::half) { juce::FloatVectorOperations::clear (dest, n); return; }
        readPos = (double) w - target;
        errSmooth = 0.0;
        primed = true;
    }
    // stalled for long (device paused, debugger...) — start again at the newest audio
    if ((double) w - readPos > ringSize / 2) { primed = false; juce::FloatVectorOperations::clear (dest, n); return; }

    // follow the input clock: keep the buffer around the target fill
    const double fill = (double) w - readPos;
    const double alpha = 1.0 - std::exp (-(double) n / (outRate * 0.6));
    errSmooth += ((fill - target) - errSmooth) * alpha;
    const double corr = juce::jlimit (-0.004, 0.004, errSmooth / inRate.load (std::memory_order_relaxed) * 0.5);
    const double step = ratio * (1.0 + corr);

    if (readPos + (double) n * step + SincInterpolator::half + 1.0 > (double) w)
    {
        // underrun: the microphone is late. Insert silence once and keep a bigger safety margin.
        juce::FloatVectorOperations::clear (dest, n);
        underruns.fetch_add (1);
        maxTarget = juce::jmin (0.25 * inRate.load(), juce::jmax (maxTarget, target) * 1.3);
        primed = false;
        return;
    }
    auto reader = [this] (juce::int64 i) { return ring[(size_t) (i & ringMask)]; };
    for (int i = 0; i < n; ++i)
    {
        dest[i] = interp.at (readPos, reader);
        readPos += step;
    }
    bufferedOut.store ((target + errSmooth) / ratio, std::memory_order_relaxed);   // steady value (the raw fill jumps with every input block)
    driftPpm.store (driftPpm.load (std::memory_order_relaxed) * 0.99 + corr * 1e6 * 0.01, std::memory_order_relaxed);
}
}
