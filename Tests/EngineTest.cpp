// Automated test for the TF Vocal Studio audio engine using a simulated sound card.
// The fake device feeds the speaker output back into the microphone with a known delay
// (like a singer singing exactly in time with what they hear), so we can verify that
// recordings line up perfectly with the song after latency compensation.

#include "AudioEngine.h"
#include "Lyrics.h"
#include <juce_events/juce_events.h>

using namespace tf;

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) std::cout << "  PASS  " << msg << "\n"; else { std::cout << "  FAIL  " << msg << "\n"; ++failures; } } while (0)

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
        juce::Random rnd (1);
        auto next = juce::Time::getMillisecondCounterHiRes();
        const double period = 1000.0 * bs / sr;
        while (! threadShouldExit())
        {
            auto* inp = in.getWritePointer (0);
            for (int i = 0; i < bs; ++i)
            {
                auto idx = written + i - loopDelay;
                inp[i] = (idx >= 0 ? outHistory[(size_t) idx % outHistory.size()] : 0.0f)
                       + extraNoise.load() * (rnd.nextFloat() * 2.0f - 1.0f);
            }
            const float* ins[] = { inp };
            float* outs[] = { out.getWritePointer (0), out.getWritePointer (1) };
            auto t0 = juce::Time::getMillisecondCounterHiRes();
            cb->audioDeviceIOCallbackWithContext (ins, 1, outs, 2, bs, {});
            auto dt = juce::Time::getMillisecondCounterHiRes() - t0;
            totalCallbackMs = totalCallbackMs + dt;
            if (dt > maxCallbackMs) maxCallbackMs = dt;
            ++callbacks;
            for (int i = 0; i < bs; ++i)
                outHistory[(size_t) (written + i) % outHistory.size()] = 0.5f * (outs[0][i] + outs[1][i]);
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
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
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
    CHECK (engine.getRoundTripLatencySamples() == FakeDevice::loopDelay, "round-trip latency read from device");

    juce::String err;
    CHECK (engine.loadSong (song, err), "song loads");
    engine.setMonitorEnabled (false);   // no vocal feedback in the loopback test
    engine.setMusicVolume (1.0f);
    engine.setVoiceVolume (1.0f);

    // ---- 3. recording WITHOUT noise removal: vocal must line up exactly with the song
    std::cout << "\nRecording alignment (noise removal off)\n";
    engine.setNoiseEnabled (false);
    engine.setPosition (2.0);
    pump (200);
    CHECK (engine.startRecording (err), "recording starts");
    pump (4000);
    engine.stopRecording();
    pump (200);
    auto take = engine.getLastTake();
    CHECK (take.isValid() && take.folder.getChildFile ("session.xml").existsAsFile(), "take saved with session.xml");
    CHECK (std::abs (take.lengthSeconds - 4.0) < 0.3, "take length ~4 s (" + juce::String (take.lengthSeconds, 2) + " s)");
    CHECK (take.vocalOffset == FakeDevice::loopDelay, "vocal offset = latency (" + juce::String (take.vocalOffset) + ")");

    {
        auto vocal = readMono (take.vocal, take.vocalOffset, 48000 * 2);
        auto ref = readMono (song, (juce::int64) std::llround (take.startSeconds * 48000), 48000 * 2);
        double c = 0;
        auto lag = bestLag (vocal, ref, 600, c);
        CHECK (std::abs (lag) <= 1, "recorded vocal aligned with song (lag " + juce::String (lag) + " samples, corr " + juce::String (c, 3) + ")");
        for (int w = 0; w < 3; ++w)
        {
            auto v2 = readMono (take.vocal, take.vocalOffset + w * 48000, 48000);
            auto r2 = readMono (song, (juce::int64) std::llround (take.startSeconds * 48000) + w * 48000, 48000);
            double c2 = 0; auto l2 = bestLag (v2, r2, 600, c2);
            auto rw = readMono (take.raw, take.rawOffset + w * 48000, 48000);
            double c3 = 0; auto l3 = bestLag (rw, r2, 600, c3);
            std::cout << "        window " << w << ": lag " << l2 << " corr " << juce::String (c2, 3) << " | raw lag " << l3 << " corr " << juce::String (c3, 3)
                      << " | rms vocal " << rms (v2) << " ref " << rms (r2) << "\n";
        }
    }

    // ---- 4. recording WITH noise removal: +480 samples compensated
    std::cout << "\nRecording alignment (AI noise removal on)\n";
    engine.setNoiseEnabled (true);
    engine.setNoiseAmount (1.0f);
    engine.setPosition (5.0);
    pump (200);
    CHECK (engine.startRecording (err), "recording starts");
    pump (4000);
    engine.stopRecording();
    pump (200);
    auto take2 = engine.getLastTake();
    CHECK (take2.vocalOffset == FakeDevice::loopDelay + NoiseSuppressor::frameSize, "vocal offset includes RNNoise frame");
    {
        auto raw = readMono (take2.raw, take2.rawOffset, 48000 * 2);
        auto ref = readMono (song, (juce::int64) std::llround (take2.startSeconds * 48000), 48000 * 2);
        double c = 0;
        auto lag = bestLag (raw, ref, 600, c);
        CHECK (std::abs (lag) <= 1, "raw vocal aligned (lag " + juce::String (lag) + ")");
    }

    // ---- 5. noise suppression actually removes steady noise (silence + hiss input)
    std::cout << "\nAI noise removal\n";
    {
        engine.stop();
        engine.unloadSong();
        auto* dev = dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
        dev->extraNoise = 0.05f;
        engine.setNoiseAmount (1.0f);
        CHECK (engine.startRecording (err), "free recording (no song) starts");
        pump (3000);
        engine.stopRecording();
        pump (200);
        dev->extraNoise = 0.0f;
        auto t = engine.getLastTake();
        auto v = readMono (t.vocal, t.vocalOffset + 48000);   // skip first second (model settling)
        auto r = readMono (t.raw, t.rawOffset + 48000);
        auto db = juce::Decibels::gainToDecibels ((float) (rms (v) / juce::jmax (1e-9, rms (r))));
        CHECK (db < -15.0f, "hiss reduced by " + juce::String (-db, 1) + " dB");
        CHECK (! t.backing.existsAsFile() && std::abs (t.startSeconds) < 1e-9, "free recording has no backing");
    }

    // ---- 6. export mix
    std::cout << "\nExport\n";
    {
        engine.setMusicVolume (0.65f); engine.setVoiceVolume (0.8f);
        auto dest = take.folder.getChildFile ("mix.wav");
        bool done = false, ok = false;
        engine.exportMix (take, dest, nullptr, [&] (bool o, juce::String) { ok = o; done = true; });
        for (int i = 0; i < 100 && ! done; ++i) pump (100);
        CHECK (done && ok && dest.existsAsFile(), "mix exported");
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (dest));
        CHECK (r != nullptr && r->numChannels == 2 && std::abs (r->sampleRate - 48000.0) < 0.5, "stereo 48 kHz WAV");
        if (r != nullptr)
        {
            auto expectLen = (double) (readMono (take.vocal).size() - (size_t) take.vocalOffset) / 48000.0;
            CHECK (std::abs (r->lengthInSamples / 48000.0 - expectLen) < 0.01, "mix length matches compensated vocal");
            juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, b.getNumSamples(), 0, true, true);
            auto mag = b.getMagnitude (0, b.getNumSamples());
            CHECK (mag <= 0.9f, "safety limiter ceiling respected in mix (peak " + juce::String (mag, 3) + ")");
        }
    }

    // ---- 7. take playback
    std::cout << "\nListen to take\n";
    {
        engine.setMonitorEnabled (true);
        CHECK (engine.loadTakeForPlayback (take, err), "take loads for playback (song reloaded)");
        CHECK (engine.hasSong() && std::abs (engine.getPosition() - take.startSeconds) < 0.01, "position set to take start");
        engine.play(); pump (1000); engine.pause();
        CHECK (engine.getPosition() > take.startSeconds + 0.8, "song + take play together");
        engine.clearTakePlayback();
        CHECK (! engine.hasTakePlayback(), "take playback cleared");
    }

    // ---- 8a. loading a song must not block the audio thread
    std::cout << "\nGlitch-free loading\n";
    {
        auto* dev = dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
        pump (200);
        dev->maxCallbackMs = 0;
        for (int i = 0; i < 3; ++i) { engine.loadSong (song, err); pump (100); engine.loadTakeForPlayback (take, err); pump (100); engine.clearTakePlayback(); }
        CHECK (dev->maxCallbackMs.load() < 2.0, "max audio callback while loading: " + juce::String (dev->maxCallbackMs.load(), 3) + " ms");
    }

    // ---- 8. performance
    std::cout << "\nPerformance (128-sample blocks @ 48 kHz = 2.67 ms budget)\n";
    {
        auto* dev = dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
        engine.setNoiseEnabled (true); engine.setNoiseInMonitor (true);
        engine.loadSong (song, err);
        pump (300);
        dev->totalCallbackMs = 0; dev->maxCallbackMs = 0; dev->callbacks = 0;
        engine.play();
        CHECK (engine.startRecording (err), "recording with noise removal + song playback");
        pump (5000);
        engine.stopRecording();
        auto avg = dev->totalCallbackMs.load() / juce::jmax (1, dev->callbacks.load());
        std::cout << "        average callback: " << juce::String (avg, 4) << " ms (" << juce::String (100.0 * avg / 2.6667, 1) << "% of budget), max "
                  << juce::String (dev->maxCallbackMs.load(), 3) << " ms\n";
        CHECK (avg < 0.6, "average DSP load well under budget");
    }

    for (auto& d : AudioEngine::recordingsFolder().findChildFiles (juce::File::findDirectories, false))
        if (Take::load (d).backing == song || d.getFileName().startsWith (juce::String::fromUTF8 ("মুক্ত রেকর্ডিং")))
            if (d.getCreationTime() > juce::Time::getCurrentTime() - juce::RelativeTime::minutes (10)) d.deleteRecursively();

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << "\n";
    engine.getDeviceManager().closeAudioDevice();
    tmp.deleteRecursively();
    return failures == 0 ? 0 : 1;
}
