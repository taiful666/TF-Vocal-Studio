// Simulated sound card for the automated tests: whatever the speaker plays comes back into the
// microphone after a fixed delay (like a singer singing in time with what they hear), plus optional noise.
#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <random>
#include <functional>

namespace tf
{
//==============================================================================
struct FakeDevice : public juce::AudioIODevice, private juce::Thread
{
    static constexpr int inLat = 200, outLat = 300, loopDelay = inLat + outLat;
    double sr = 48000.0; int bs = 128;
    juce::AudioIODeviceCallback* cb = nullptr;
    bool opened = false, started = false;
    std::vector<float> outHistory;   // mono output history for acoustic loopback
    juce::int64 written = 0;
    std::atomic<double> totalCallbackMs { 0 }, maxCallbackMs { 0 };
    std::atomic<int> callbacks { 0 };
    std::atomic<float> extraNoise { 0.0f };
    // output capture: what the speaker plays, with the engine position at the first captured sample
    std::function<double()> positionProbe;
    std::atomic<bool> capture { false };
    std::vector<float> captured;
    double captureStartPos = -1.0;

    FakeDevice() : AudioIODevice ("Fake", "Fake"), Thread ("fake audio") { outHistory.assign (1 << 22, 0.0f); }
    ~FakeDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override  { return { "Mic" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128 }; }
    int getDefaultBufferSize() override { return 128; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { opened = true; return {}; }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* c) override
    {
        if (c == nullptr || started) return;
        cb = c; c->audioDeviceAboutToStart (this); started = true; startRealtimeThread ({});
    }
    void stop() override
    {
        if (! started) return;
        stopThread (2000); started = false;
        if (cb != nullptr) cb->audioDeviceStopped();
        cb = nullptr;
    }
    bool isPlaying() override { return started; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bs; }
    double getCurrentSampleRate() override { return sr; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return 3; }
    juce::BigInteger getActiveInputChannels() const override { return 1; }
    int getOutputLatencyInSamples() override { return outLat; }
    int getInputLatencyInSamples() override { return inLat; }

    void run() override
    {
        juce::AudioBuffer<float> in (1, bs), out (2, bs);
        std::mt19937 rng (12345);
        std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
        float p0 = 0, p1 = 0, p2 = 0;   // pink noise (like a fan or AC), not a pure test hiss
        auto next = juce::Time::getMillisecondCounterHiRes();
        const double period = 1000.0 * bs / sr;
        while (! threadShouldExit())
        {
            auto* inp = in.getWritePointer (0);
            auto pink = [&]
            {
                const float w = uni (rng);
                p0 = 0.99765f * p0 + w * 0.0990460f; p1 = 0.96300f * p1 + w * 0.2965164f; p2 = 0.57000f * p2 + w * 1.0526913f;
                return 0.25f * (p0 + p1 + p2 + w * 0.1848f);
            };
            for (int i = 0; i < bs; ++i)
            {
                auto idx = written + i - loopDelay;
                inp[i] = (idx >= 0 ? outHistory[(size_t) idx % outHistory.size()] : 0.0f)
                       + extraNoise.load() * pink();
            }
            const float* ins[] = { inp };
            float* outs[] = { out.getWritePointer (0), out.getWritePointer (1) };
            const bool cap = capture.load();
            if (cap && captureStartPos < 0 && positionProbe) captureStartPos = positionProbe();
            auto t0 = juce::Time::getMillisecondCounterHiRes();
            cb->audioDeviceIOCallbackWithContext (ins, 1, outs, 2, bs, {});
            auto dt = juce::Time::getMillisecondCounterHiRes() - t0;
            totalCallbackMs = totalCallbackMs + dt;
            if (dt > maxCallbackMs) maxCallbackMs = dt;
            ++callbacks;
            for (int i = 0; i < bs; ++i)
                outHistory[(size_t) (written + i) % outHistory.size()] = 0.5f * (outs[0][i] + outs[1][i]);
            if (cap && captureStartPos >= 0 && captured.size() < 48000 * 30)
                for (int i = 0; i < bs; ++i) captured.push_back (0.5f * (outs[0][i] + outs[1][i]));
            written += bs;

            next += period;
            auto wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0) juce::Thread::sleep ((int) wait);
        }
    }
};

struct FakeType : public juce::AudioIODeviceType
{
    FakeType() : AudioIODeviceType ("FakeType") {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { "Fake" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override { last = new FakeDevice(); return last; }
    FakeDevice* last = nullptr;
};

}
