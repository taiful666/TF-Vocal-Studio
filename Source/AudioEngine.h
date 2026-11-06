#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "VocalFx.h"
#include "Project.h"
#include "AudioInput.h"
#include "MixTools.h"

struct DenoiseState;

namespace tf
{
//==============================================================================
/** RNNoise wrapper for the live microphone: 48 kHz mono, 480-sample frames, dry/wet mix.
    RNNoise itself answers one frame late, so the dry signal is delayed by the same 960 samples —
    otherwise mixing the two would comb-filter the voice (a hollow, harsh sound). */
class NoiseSuppressor
{
public:
    static constexpr int frameSize = 480, latency = 2 * frameSize;
    NoiseSuppressor();
    ~NoiseSuppressor();
    void reset();
    void process (float* data, int numSamples, float amount) noexcept;
private:
    DenoiseState* state = nullptr;
    float inFrame[frameSize] {}, prevIn[frameSize] {}, dryFrame[frameSize] {}, outFrame[frameSize] {}, scaled[frameSize] {};
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
struct ExportOptions
{
    double fadeIn = 0.0, fadeOut = 0.0;   // seconds
    bool normalise = false;               // -14 LUFS (YouTube / Facebook loudness)
};

//==============================================================================
/** A finished recording ("take"), stored in its own folder with session.xml. */
struct Take
{
    juce::File folder, vocal, speaker, backing;
    double startSeconds = 0.0;      // song position where recording started
    double sampleRate = 48000.0;
    juce::int64 vocalOffset = 0;    // samples to skip at the start of vocal.wav (latency compensation)
    juce::Time startedAt;           // wall-clock time of the first recorded sample (camera sync)
    double lengthSeconds = 0.0;

    bool isValid() const { return vocal.existsAsFile(); }
    bool save() const;
    static Take load (const juce::File& folder);
};

//==============================================================================
/** The audio engine: plays every track of the project in lock-step from one sample-accurate playhead,
    records the microphone into a voice track, and renders the mix. */
class AudioEngine : public juce::ChangeBroadcaster,
                    private juce::AudioIODeviceCallback,
                    private juce::ChangeListener
{
public:
    AudioEngine();
    ~AudioEngine() override;

    /** micName: the microphone to use (may be on another sound card than the speaker). */
    void initialise (const juce::XmlElement* savedDeviceState, const juce::String& micName = {});
    std::unique_ptr<juce::XmlElement> createDeviceState() const { return deviceManager.createStateXml(); }
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

    static juce::File appFolder();
    static juce::File songsFolder();
    static juce::File recordingsFolder();
    /** The decodable audio for a song file (itself, or its converted cache), or {} if it needs importing. */
    static juce::File playableAudioFor (const juce::File& f, juce::AudioFormatManager& fm);

    // ---- project
    /** Loads the project's tracks and clips. Unchanged clips keep playing without a gap. */
    void setProject (const ProjectState& p);
    double getLength() const noexcept { return projectLength.load(); }
    bool hasContent() const noexcept { return projectLength.load() > 0.0; }
    FxParams* getTrackFx (const juce::String& trackId);
    /** Effects heard on the live microphone = the armed (selected) voice track's effects. */
    void setArmedTrack (const juce::String& trackId);
    juce::String getArmedTrack() const;
    int getSongKey() const noexcept { return songKey.load(); }
    /** Auto-tune scale for every voice: -2 = follow the song (smart match), -1 = all notes, 0..23 = fixed key. */
    void setScaleMode (int m)            { if (scaleMode.exchange (m) != m) retuneAll(); }
    int getScaleMode() const noexcept    { return scaleMode.load(); }
    int getEffectiveKey() const noexcept { const int m = scaleMode.load(); return m == -2 ? songKey.load() : m; }
    void setLiveTune (bool b) noexcept   { liveTuneAll.store (b); }
    bool getLiveTune() const noexcept    { return liveTuneAll.load(); }
    /** Output level of a track since the last call (0..1). */
    float popTrackPeak (const juce::String& trackId);
    bool isDetectingKey() const noexcept { return keyBusy.load(); }
    /** Key detection runs on this file (the music). */
    void detectKeyOf (const juce::File& audio);

    // ---- transport
    void play();
    void pause();
    void stop();
    void togglePlay() { isPlaying() ? pause() : play(); }
    void setPosition (double seconds);
    double getPosition() const;
    bool isPlaying() const;
    void setLoop (juce::Range<double> seconds, bool enabled);
    bool isLooping() const noexcept { return loopOn.load(); }

    // ---- recording into a voice track
    /** punch: if not empty, only this part is kept (recording starts ~2 s earlier as a cue). */
    bool startRecording (const juce::String& trackId, juce::Range<double> punch, juce::String& error);
    void stopRecording();
    bool isRecording() const noexcept { return recording.load(); }
    bool punchFinished() const noexcept { return punchDone.load(); }
    double getRecordedSeconds() const noexcept;
    juce::String getRecordingTrack() const { return recordTrackId; }
    struct RecordingResult
    {
        juce::String trackId;
        juce::File wav, speaker;          // speaker: what the speakers played meanwhile (same timing as wav)
        juce::Time startedAt;             // wall-clock time of the first sample (camera sync)
        double start = 0, in = 0, out = 0;
        bool ok = false, punch = false;
    };
    RecordingResult takeLastRecording() { auto r = lastResult; lastResult = {}; return r; }
    const PeakTimeline& getLivePeaks() const noexcept { return vocalPeaks; }

    // ---- export (background thread). range empty = everything. progress 0..1, done(ok, message) on the message thread.
    void exportMix (const ProjectState& p, const juce::File& dest, juce::Range<double> range,
                    std::function<void (float)> progress, std::function<void (bool, juce::String)> done,
                    ExportOptions options = {});
    /** Renders the project offline exactly as it plays (effects, smart mix, master). Background thread. */
    static bool renderProject (const ProjectState& p, juce::Range<double> range, double sampleRate, int scaleKey,
                               juce::AudioBuffer<float>& out, int& numSamples, std::function<void (float)> progress, juce::String& error,
                               const std::atomic<bool>* cancel = nullptr);
    /** "Smart mix" balance: the voice volume that sits the voice just above the music (like released songs).
        Returns trackId -> gain. Background thread. */
    static std::map<juce::String, float> computeBalance (const ProjectState& p, int scaleKey);

    // ---- the microphone (its own sound card is fine: no crackle from clock drift)
    void setMicrophone (const juce::String& name);
    juce::String getMicrophone() const { return micName; }
    juce::StringArray getMicrophoneNames();
    bool isMicSeparate() const noexcept { return splitInput.load(); }
    InputBridge& getInputBridge() noexcept { return micBridge; }
    /** Pitch the microphone hears right now (Hz, 0 = no clear note). */
    float getLivePitchHz() const noexcept { return livePitch.hz(); }

    // ---- count-in beeps and automatic sync measurement
    void playBeep (float hz, double seconds, float level = 0.25f);
    /** Plays a few clicks through the speaker, listens with the mic and sets the recording sync.
        done(ok, total delay in ms, message) on the message thread. */
    void measureLatency (std::function<void (bool, double, juce::String)> done);
    bool isMeasuringLatency() const noexcept { return measuring.load(); }
    bool isExporting() const noexcept { return exporting.load(); }

    // ---- live input
    void setInputGainDb (float db)        { inputGainDb.store (db); }
    void setMonitorVolume (float v)       { voiceVol.store (v); }
    void setMonitorEnabled (bool b)       { monitorOn.store (b); }
    bool isMonitorEnabled() const noexcept { return monitorOn.load(); }
    void setNoiseEnabled (bool b)         { if (b && ! noiseOn.load()) nsResetPending.store (true); noiseOn.store (b); }
    void setNoiseAmount (float a)         { noiseAmount.store (a); }
    void setNoiseInMonitor (bool b)       { noiseInMonitor.store (b); }
    void setUserOffsetMs (float ms)       { userOffsetMs.store (ms); }
    float getUserOffsetMs() const         { return userOffsetMs.load(); }
    void setPrerollSeconds (double s)     { prerollSec.store (s); }
    void playTestTone()                   { playBeep (880.0f, 0.6, 0.2f); }

    // ---- status / meters
    float popInputPeak() noexcept  { return inPeak.exchange (0.0f); }
    float popOutputPeak() noexcept { return outPeak.exchange (0.0f); }
    float popOutputPeakR() noexcept { return outPeakR.exchange (0.0f); }
    bool noiseAvailable() const noexcept { return std::abs (currentRate.load() - 48000.0) < 1.0; }
    double getSampleRate() const noexcept { return currentRate.load(); }
    int getBufferSize() const noexcept { return currentBlock.load(); }
    int getRoundTripLatencySamples() const;
    double getRoundTripLatencyMs() const;
    juce::String getDeviceName() const;
    bool isDeviceOpen() const;
    int getXruns() const;

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void waitUntilBuffered();
    void processChunk (const float* const* in, int numIn, float* const* out, int numOut, int start, int n) noexcept;
    void updateInputRouting();
    /** The scale changed: the corrected voices are computed again from the playhead. */
    void retuneAll();
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> makeWriter (const juce::File& f, juce::String& error);

    //==============================================================================
    struct TimelineTrack
    {
        std::unique_ptr<juce::PositionableAudioSource> source;
        std::unique_ptr<juce::BufferingAudioSource> buffer;
        std::unique_ptr<juce::ResamplingAudioSource> resampler;
        double sourceRate = 48000.0;
        int channels = 2;
        void prepare (double deviceRate, int maxBlock);
        void seek (juce::int64 timelineSamples, double deviceRate);
        void read (juce::AudioBuffer<float>& dest, int n, juce::int64 timelineAfter, double deviceRate) noexcept;
        bool waitReady (int timeoutMs);
    };
    std::unique_ptr<TimelineTrack> makeTrack (std::unique_ptr<juce::PositionableAudioSource> src, double rate, int channels, bool prefill = true);

    class ClipSource;
    class TunedClipSource;
    struct EngClip
    {
        juce::String key;                 // file + positions: unchanged clips are reused
        std::unique_ptr<TimelineTrack> track;
        double startSec = 0, endSec = 0;
    };
    struct EngTrack
    {
        juce::String id, fxState;
        TrackKind kind = TrackKind::vocal;
        std::unique_ptr<FxParams> fx;
        std::unique_ptr<VocalChain> playChain, monitorChain;
        std::atomic<float> gain { 1.0f }, pan { 0.0f };
        std::atomic<bool> mute { false }, solo { false };
        juce::SmoothedValue<float> levelL, levelR;
        std::vector<std::unique_ptr<EngClip>> clips;
        juce::AudioBuffer<float> clipBuf, sum, fxOut;
        juce::int64 chainLatency = 0;
        std::atomic<float> meter { 0.0f };
        MusicDucker ducker;
    };
    std::unique_ptr<EngTrack> makeEngTrack (const TrackInfo& t, std::vector<std::unique_ptr<EngClip>>& reusable);
    std::unique_ptr<EngClip> makeClip (const ClipInfo& c, TrackKind kind, const FxParams* fx, juce::String& error);
    static juce::String clipKey (const ClipInfo& c, juce::int64 chainLatency);
    void prepareTrack (EngTrack& t, double sr, int block);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread readAheadThread { "TF read-ahead" }, writerThread { "TF writer" };

    juce::CriticalSection transportLock;   // held by the audio callback while pulling tracks; never held for long
    std::vector<std::unique_ptr<EngTrack>> tracks;
    std::atomic<bool> playing { false };
    std::atomic<juce::int64> playhead { 0 };
    std::atomic<double> projectLength { 0.0 };
    juce::String armedTrackId;
    EngTrack* armedTrack = nullptr;        // audio thread view (under transportLock)
    std::atomic<bool> loopOn { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };

    // recording
    juce::CriticalSection writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> vocalWriter, speakerWriter;
    std::atomic<bool> recording { false }, punchDone { false };
    bool recordWithNoise = false;
    std::atomic<double> recordStartSeconds { 0.0 };
    std::atomic<bool> recordFirstBlock { false };
    std::atomic<juce::int64> recordedSamples { 0 }, punchEndSample { -1 };
    juce::String recordTrackId;
    EngTrack* recordTrack = nullptr;       // muted while recording over it
    juce::Range<double> punchRange;
    Take pendingTake;
    RecordingResult lastResult;

    // processing state (audio thread)
    juce::AudioBuffer<float> micBuf, nsBuf, master, monitorBuf, playBus, voiceBus, bridgeBuf, speakerBuf;
    MasterBus masterBus;
    LivePitch livePitch;
    InputBridge micBridge;
    juce::String micName;
    std::atomic<bool> splitInput { false };
    juce::String routedKey;
    std::atomic<float> smartMixAmt { 1.0f };
    std::atomic<bool> glueOn { true };
    std::atomic<int> beepLeft { 0 };
    std::atomic<float> beepHz { 880.0f }, beepLevel { 0.2f };
    int beepTotal = 1;
    // latency measurement
    std::atomic<bool> measuring { false };
    std::vector<float> measMic, measOut;
    std::atomic<int> measPos { 0 };
    std::function<void (bool, double, juce::String)> measureDone;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    int maxBlock = 0;
    std::atomic<int> maxBlockShared { 0 };
    juce::dsp::IIR::Filter<float> highPass;
    SafetyLimiter limiter;
    NoiseSuppressor denoiser;
    juce::SmoothedValue<float> gainSm, voiceSm, monitorSm;
    double tonePhase = 0.0;

    std::atomic<float> inputGainDb { 0.0f }, voiceVol { 0.8f }, noiseAmount { 0.8f }, userOffsetMs { 0.0f };
    std::atomic<double> prerollSec { 2.0 };
    std::atomic<bool> monitorOn { false }, noiseOn { true }, noiseInMonitor { false }, nsResetPending { false };
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f }, outPeakR { 0.0f };
    std::atomic<double> currentRate { 48000.0 };
    std::atomic<int> currentBlock { 0 };
    std::atomic<bool> exporting { false };

    PeakTimeline vocalPeaks;
    FxParams defaultFx;                   // used when no voice track is armed
    std::unique_ptr<VocalChain> defaultMonitorChain;
    std::atomic<int> songKey { -1 }, scaleMode { -2 };
    std::atomic<bool> liveTuneAll { false };
    std::atomic<bool> keyBusy { false }, keyCancel { false };
    juce::File keyFile;
    std::unique_ptr<juce::ThreadPool> keyPool, exportPool;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
}
