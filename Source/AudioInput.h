#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace tf
{
//==============================================================================
/** High-quality fractional resampler (windowed sinc, 32 taps, 512 phases) for one mono stream.
    Used to bridge two sound cards that run on their own clocks without any gap or click. */
class SincInterpolator
{
public:
    static constexpr int taps = 32, half = taps / 2, phases = 512;
    SincInterpolator();
    /** Value of the signal at fractional position pos, reading x[i] for i in [floor(pos)-15, floor(pos)+16]. */
    template <typename Reader>
    float at (double pos, Reader&& x) const noexcept
    {
        const auto i0 = (juce::int64) std::floor (pos);
        const double frac = pos - (double) i0;
        const double ph = frac * phases;
        const int p = juce::jmin (phases - 1, (int) ph);
        const float f = (float) (ph - p);
        const float* a = table.data() + (size_t) p * taps;
        const float* b = table.data() + (size_t) (p + 1) * taps;
        float acc = 0.0f;
        for (int k = 0; k < taps; ++k)
            acc += x (i0 - half + 1 + k) * (a[k] + f * (b[k] - a[k]));
        return acc;
    }
private:
    std::vector<float> table;   // (phases + 1) * taps
};

//==============================================================================
/** The microphone on its own sound card (e.g. a USB mic while the speaker is on the PC's own sound card).

    Windows only offers "combined" devices that copy the input into the output callback through a tiny
    buffer: when the two cards' clocks differ, that buffer runs dry and silence is inserted — heard as
    crackle ("cat-cat") in recordings. Here the microphone has its own input-only stream that writes
    every sample into a large ring buffer; the output side reads it back through a drift-following
    resampler, so the voice is continuous, sample for sample, whatever the two clocks do. */
class InputBridge : private juce::AudioIODeviceCallback
{
public:
    InputBridge();
    ~InputBridge() override;

    /** Opens an input-only device of this type. Returns an error text, or empty on success. */
    juce::String open (juce::AudioIODeviceType& type, const juce::String& inputName, double sampleRate, int blockSize);
    /** For the automated test: use a device created by the caller. */
    juce::String openDevice (std::unique_ptr<juce::AudioIODevice> device, double sampleRate, int blockSize);
    void close();
    bool isOpen() const noexcept        { return device != nullptr && running.load(); }
    juce::String getDeviceName() const  { return device != nullptr ? device->getName() : juce::String(); }
    juce::AudioIODevice* getDevice() const noexcept { return device.get(); }

    /** Output side (audio thread). Call when the output device starts. */
    void prepareOutput (double outputRate) noexcept;
    /** Fills n samples at the output rate. Never blocks; on an underrun it fills silence and re-primes. */
    void pull (float* dest, int n) noexcept;

    /** Extra input delay added by the bridge (ring-buffer fill), in output samples. */
    double bufferedSamples() const noexcept { return bufferedOut.load(); }
    /** Delay of the input device itself, in output samples. */
    int deviceInputLatency() const noexcept { return inLatencyOut.load(); }
    int getUnderruns() const noexcept       { return underruns.load(); }
    double getDriftPpm() const noexcept     { return driftPpm.load(); }
    double getInputRate() const noexcept    { return inRate.load(); }

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    std::unique_ptr<juce::AudioIODevice> device;
    std::atomic<bool> running { false };
    static constexpr int ringSize = 1 << 18, ringMask = ringSize - 1;   // 5.4 s at 48 kHz
    std::vector<float> ring;
    std::atomic<juce::int64> written { 0 };
    std::atomic<int> inBlock { 480 };
    std::atomic<double> inRate { 48000.0 };
    std::atomic<int> inLatencyOut { 0 }, underruns { 0 };
    std::atomic<double> bufferedOut { 0.0 }, driftPpm { 0.0 };
    std::vector<float> chanRms;           // slow level per input channel: dead channels are left out

    // output side (audio thread only)
    SincInterpolator interp;
    double outRate = 48000.0, readPos = 0.0, errSmooth = 0.0, target = 1024.0, maxTarget = 0.0;
    bool primed = false;
    juce::int64 lastWritten = -1;
    int sinceUnderrun = 0;
};
}
