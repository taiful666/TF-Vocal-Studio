#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "VocalFx.h"

struct DenoiseState;

namespace tf
{
//==============================================================================
/** RNNoise wrapper: 48 kHz mono, 480-sample frames, fixed 480-sample latency, dry/wet mix. */
class NoiseSuppressor
{
public:
    static constexpr int frameSize = 480;
    NoiseSuppressor();
    ~NoiseSuppressor();
    void reset();
    void process (float* data, int numSamples, float amount) noexcept;
private:
    DenoiseState* state = nullptr;
    float inFrame[frameSize] {}, dryFrame[frameSize] {}, outFrame[frameSize] {}, scaled[frameSize] {};
    int pos = 0;
    JUCE_DECLARE_NON_COPYABLE (NoiseSuppressor)
};

//==============================================================================
/** Transparent safety limiter: unity gain below the ceiling, instant gain reduction on peaks,
    smooth release. No look-ahead, so it adds zero latency to monitoring. */
class SafetyLimiter
{
public:
    void prepare (double sampleRate, float ceilingGain, float releaseMs = 80.0f)
    {
        ceiling = ceilingGain;
        release = 1.0f - std::exp (-1.0f / (releaseMs * 0.001f * (float) sampleRate));
        env = 1.0f;
    }
    void process (float* l, float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float p = r != nullptr ? juce::jmax (std::abs (l[i]), std::abs (r[i])) : std::abs (l[i]);
            const float target = p > ceiling ? ceiling / p : 1.0f;
            env = target < env ? target : env + (target - env) * release;
            l[i] *= env;
            if (r != nullptr) r[i] *= env;
        }
    }
private:
    float ceiling = 0.97f, release = 0.001f, env = 1.0f;
};

//==============================================================================
/** Peak envelope of the live vocal on the song timeline (10 ms buckets), written by the audio thread. */
class PeakTimeline
{
public:
    static constexpr int bucketsPerSecond = 100;
    static constexpr int maxSeconds = 60 * 60;   // 1 hour
    PeakTimeline() : peaks (new std::atomic<float>[(size_t) (bucketsPerSecond * maxSeconds)]) { clear(); }
    void clear() noexcept { for (int i = 0; i < size(); ++i) peaks[(size_t) i].store (0.0f, std::memory_order_relaxed); maxIndex.store (-1); }
    int size() const noexcept { return bucketsPerSecond * maxSeconds; }
    void push (double seconds, float peak) noexcept
    {
        auto i = (int) (seconds * bucketsPerSecond);
        if (i < 0 || i >= size()) return;
        auto& p = peaks[(size_t) i];
        if (peak > p.load (std::memory_order_relaxed)) p.store (peak, std::memory_order_relaxed);
        if (i > maxIndex.load (std::memory_order_relaxed)) maxIndex.store (i, std::memory_order_relaxed);
    }
    float maxBetween (double t0, double t1) const noexcept
    {
        auto a = juce::jmax (0, (int) (t0 * bucketsPerSecond)), b = juce::jmin (size() - 1, (int) (t1 * bucketsPerSecond));
        float m = 0.0f;
        for (int i = a; i <= b; ++i) m = juce::jmax (m, peaks[(size_t) i].load (std::memory_order_relaxed));
        return m;
    }
    double lastSecond() const noexcept { return maxIndex.load() / (double) bucketsPerSecond; }
private:
    std::unique_ptr<std::atomic<float>[]> peaks;
    std::atomic<int> maxIndex { -1 };
};

//==============================================================================
/** A finished recording ("take"), stored in its own folder with session.xml. */
struct Take
{
    juce::File folder, vocal, raw, backing;
    double startSeconds = 0.0;      // song position where recording started
    double sampleRate = 48000.0;
    juce::int64 vocalOffset = 0;    // samples to skip at the start of vocal.wav (latency compensation)
    juce::int64 rawOffset = 0;
    double lengthSeconds = 0.0;

    bool isValid() const { return vocal.existsAsFile(); }
    bool save() const;
    static Take load (const juce::File& folder);
};

//==============================================================================
class AudioEngine : public juce::ChangeBroadcaster,
                    private juce::AudioIODeviceCallback,
                    private juce::ChangeListener
{
public:
    AudioEngine();
    ~AudioEngine() override;

    void initialise (const juce::XmlElement* savedDeviceState);
    std::unique_ptr<juce::XmlElement> createDeviceState() const { return deviceManager.createStateXml(); }
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

    static juce::File appFolder();
    static juce::File songsFolder();
    static juce::File recordingsFolder();

    // ---- song (karaoke track)
    /** f may be any audio or video file; video/unsupported formats must be imported first (cache). */
    bool loadSong (const juce::File& f, juce::String& error);
    /** The decodable audio for a song file (itself, or its converted cache), or {} if it needs importing. */
    static juce::File playableAudioFor (const juce::File& f, juce::AudioFormatManager& fm);
    int getSongKey() const noexcept { return songKey.load(); }
    bool isDetectingKey() const noexcept { return keyBusy.load(); }
    FxParams& getFx() noexcept { return fx; }
    bool isMonitorEnabled() const noexcept { return monitorOn.load(); }
    void unloadSong();
    juce::File getSongFile() const { return songFile; }
    double getSongLength() const noexcept { return songLength; }
    bool hasSong() const noexcept { return songFile != juce::File(); }

    // ---- transport
    void play();
    void pause();
    void stop();                       // pause + back to start
    void togglePlay() { isPlaying() ? pause() : play(); }
    bool reachedEnd() const noexcept { return endReached.load(); }
    void setPosition (double seconds);
    double getPosition() const;
    bool isPlaying() const;

    // ---- recording
    bool startRecording (juce::String& error);
    void stopRecording();
    bool isRecording() const noexcept { return recording.load(); }
    double getRecordedSeconds() const noexcept;

    // ---- listening to a take together with the song
    bool loadTakeForPlayback (const Take& t, juce::String& error);
    void clearTakePlayback();
    bool hasTakePlayback() const noexcept { return takeLoaded.load(); }
    const Take& getLastTake() const noexcept { return lastTake; }

    // ---- export (background thread). progress 0..1, done(ok, message) on the message thread.
    void exportMix (const Take& t, const juce::File& dest,
                    std::function<void (float)> progress, std::function<void (bool, juce::String)> done);
    bool isExporting() const noexcept { return exporting.load(); }

    // ---- parameters
    void setInputGainDb (float db)        { inputGainDb.store (db); }
    void setVoiceVolume (float v)         { voiceVol.store (v); }
    void setMusicVolume (float v)         { musicVol.store (v); }
    void setMonitorEnabled (bool b)       { monitorOn.store (b); }
    void setNoiseEnabled (bool b)         { noiseOn.store (b); }
    void setNoiseAmount (float a)         { noiseAmount.store (a); }
    void setNoiseInMonitor (bool b)       { noiseInMonitor.store (b); }
    void setUserOffsetMs (float ms)       { userOffsetMs.store (ms); }
    float getUserOffsetMs() const         { return userOffsetMs.load(); }

    // ---- status / meters
    float popInputPeak() noexcept  { return inPeak.exchange (0.0f); }
    float popOutputPeak() noexcept { return outPeak.exchange (0.0f); }
    bool noiseAvailable() const noexcept { return std::abs (currentRate.load() - 48000.0) < 1.0; }
    double getSampleRate() const noexcept { return currentRate.load(); }
    int getRoundTripLatencySamples() const;
    double getRoundTripLatencyMs() const;
    juce::String getDeviceName() const;
    bool isDeviceOpen() const;
    int getXruns() const;

    const PeakTimeline& getVocalPeaks() const noexcept { return vocalPeaks; }

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void waitUntilBuffered();
    void processChunk (const float* const* in, int numIn, float* const* out, int numOut, int start, int n) noexcept;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> makeWriter (const juce::File& f, juce::String& error);

    //==============================================================================
    /** A file played on the engine's sample-accurate timeline (song or take). */
    struct TimelineTrack
    {
        std::unique_ptr<juce::PositionableAudioSource> source;    // positions in source samples
        std::unique_ptr<juce::BufferingAudioSource> buffer;        // 20 s read-ahead
        std::unique_ptr<juce::ResamplingAudioSource> resampler;    // only when source rate != device rate
        double sourceRate = 48000.0;
        int channels = 2;
        void prepare (double deviceRate, int maxBlock);
        void seek (juce::int64 timelineSamples, double deviceRate);
        void read (juce::AudioBuffer<float>& dest, int n, juce::int64 timelineAfter, double deviceRate) noexcept;
        bool waitReady (int timeoutMs);
    };
    std::unique_ptr<TimelineTrack> makeTrack (std::unique_ptr<juce::PositionableAudioSource> src, double rate, int channels);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread readAheadThread { "TF read-ahead" }, writerThread { "TF writer" };

    juce::CriticalSection transportLock;   // held by the audio callback while pulling tracks; never held for long
    std::unique_ptr<TimelineTrack> songTrack, takeTrack;
    std::atomic<bool> playing { false };
    std::atomic<juce::int64> playhead { 0 };          // device samples from song start
    juce::File songFile;
    double songLength = 0.0;

    class TakeSource;
    std::atomic<bool> takeLoaded { false };
    Take lastTake;

    // recording
    juce::CriticalSection writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> vocalWriter, rawWriter;
    std::atomic<bool> recording { false };
    bool recordWithNoise = false;
    std::atomic<double> recordStartSeconds { 0.0 };
    std::atomic<bool> recordFirstBlock { false };
    std::atomic<juce::int64> recordedSamples { 0 };
    Take pendingTake;

    // processing state (audio thread)
    juce::AudioBuffer<float> micBuf, nsBuf, songBuf, takeBuf;
    int maxBlock = 0;
    std::atomic<int> maxBlockShared { 0 };
    juce::dsp::IIR::Filter<float> highPass;
    SafetyLimiter limiter;
    NoiseSuppressor denoiser;
    juce::SmoothedValue<float> gainSm, voiceSm, musicSm, monitorSm;

    std::atomic<float> inputGainDb { 0.0f }, voiceVol { 0.8f }, musicVol { 0.65f }, noiseAmount { 0.8f }, userOffsetMs { 0.0f };
    std::atomic<bool> monitorOn { false }, noiseOn { true }, noiseInMonitor { false };
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<double> currentRate { 48000.0 };
    std::atomic<int> currentBlock { 0 };
    std::atomic<bool> exporting { false }, endReached { false };

    PeakTimeline vocalPeaks;

    // vocal effects (non-destructive): live monitor chain and take-playback chain share the same settings
    FxParams fx;
    VocalChain monitorChain { fx }, playChain { fx };
    juce::AudioBuffer<float> fxBuf, takeFxBuf;
    std::atomic<int> songKey { -1 };
    std::atomic<bool> keyBusy { false }, keyCancel { false };
    std::unique_ptr<juce::ThreadPool> keyPool;
    std::unique_ptr<juce::ThreadPool> exportPool;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
}
