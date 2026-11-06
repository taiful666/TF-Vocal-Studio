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
#include <juce_events/juce_events.h>
#include <random>

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
        ch.prepare (sr, 512, true);
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
        std::cout << "        full chain CPU: " << juce::String (100.0 * ms / 10000.0, 2) << "% of one core\n";
        CHECK (ms < 1500.0, "full effects chain runs far faster than real time");
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

    std::cout << "\nAI music / voice separation\n";
    juce::String why;
    if (! Separator::isAvailable (&why)) { std::cout << "  SKIP  " << why << "\n"; return; }
    auto music = tmp.getChildFile ("music.wav"), vocals = tmp.getChildFile ("vocals.wav");
    float sp = 0;
    const auto st = juce::Time::getMillisecondCounterHiRes();
    auto res = Separator::run (song, music, vocals, [&] (float p, const juce::String&) { sp = p; }, cancel);
    const auto secs = (juce::Time::getMillisecondCounterHiRes() - st) / 1000.0;
    std::unique_ptr<juce::AudioFormatReader> rm (fm.createReaderFor (music)), rv (fm.createReaderFor (vocals));
    CHECK (res.error.isEmpty() && rm != nullptr && rv != nullptr, "separation produced music and vocal files" + (res.error.isEmpty() ? juce::String() : " (" + res.error + ")"));
    if (rm != nullptr)
    {
        std::unique_ptr<juce::AudioFormatReader> rs (fm.createReaderFor (song));
        const double lenS = (double) rs->lengthInSamples / rs->sampleRate, lenM = (double) rm->lengthInSamples / rm->sampleRate;
        CHECK (std::abs (lenS - lenM) < 0.1, "separated music has the song's length");
        std::cout << "        20 s song separated in " << juce::String (secs, 1) << " s\n";
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
    CHECK (engine.getRoundTripLatencySamples() == FakeDevice::loopDelay, "round-trip latency read from device");

    auto* dev = dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
    dev->positionProbe = [&engine] { return engine.getPosition(); };
    juce::String err;
    engine.setMonitorEnabled (false);   // no live-mic feedback in the loopback test

    // ---- project: music track + one voice track, neutral voice effects for exact comparisons
    std::cout << "\nProject\n";
    Project project;
    project.newProject ("engine test");
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
        if (r.ok) { project.insertRecording (r.trackId, r.wav, r.start, r.in, r.out); engine.setProject (project.state()); }
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
    CHECK (std::abs (rec1.in * 48000.0 - FakeDevice::loopDelay) < 0.5, "latency head skipped (" + juce::String (rec1.in * 48000.0, 1) + " samples)");
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
        return { dev->captured, dev->captureStartPos };
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
        CHECK (std::abs (r.in * 48000.0 - (FakeDevice::loopDelay + NoiseSuppressor::frameSize)) < 0.5, "noise removal delay compensated");
        auto take = Take::load (r.wav.getParentDirectory());
        auto v = readMono (r.wav, (juce::int64) std::llround (r.in * 48000) + 48000);
        auto raw = readMono (r.wav.getSiblingFile ("vocal_raw.wav"), take.rawOffset + 48000);
        auto db = juce::Decibels::gainToDecibels ((float) (rms (v) / juce::jmax (1e-9, rms (raw))));
        CHECK (db < -3.0f, "noise removal applied to the recording: " + juce::String (-db, 1) + " dB less noise");
        engine.setNoiseEnabled (false);
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
        auto e = renderTempo (tone, fast, 1.25, [&] (float p) { lastP = p; }, cancel);
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
    for (auto& f : Project::projectsFolder().findChildFiles (juce::File::findFiles, false, "engine test *.tfvs;free *.tfvs;engine test *.tmp;free *.tmp"))
        f.deleteFile();   // only the test creates projects with these names

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << "\n";
    engine.getDeviceManager().closeAudioDevice();
    tmp.deleteRecursively();
    return failures == 0 ? 0 : 1;
}
