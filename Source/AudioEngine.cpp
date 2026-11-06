#include "AudioEngine.h"
#include <rnnoise.h>
#include "Media.h"

namespace tf
{
//==============================================================================
NoiseSuppressor::NoiseSuppressor()  { state = rnnoise_create (nullptr); }
NoiseSuppressor::~NoiseSuppressor() { if (state != nullptr) rnnoise_destroy (state); }

void NoiseSuppressor::reset()
{
    if (state != nullptr) rnnoise_destroy (state);
    state = rnnoise_create (nullptr);
    std::fill (std::begin (inFrame), std::end (inFrame), 0.0f);
    std::fill (std::begin (prevIn), std::end (prevIn), 0.0f);
    std::fill (std::begin (dryFrame), std::end (dryFrame), 0.0f);
    std::fill (std::begin (outFrame), std::end (outFrame), 0.0f);
    pos = 0;
}

void NoiseSuppressor::process (float* data, int numSamples, float amount) noexcept
{
    if (state == nullptr) return;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = data[i];
        const float wet = outFrame[pos];
        const float dry = dryFrame[pos];
        inFrame[pos] = x;
        data[i] = dry + amount * (wet - dry);

        if (++pos == frameSize)
        {
            pos = 0;
            for (int k = 0; k < frameSize; ++k) scaled[k] = inFrame[k] * 32768.0f;
            // RNNoise returns the previous frame, so the matching dry signal is the previous frame too
            std::copy (std::begin (prevIn), std::end (prevIn), std::begin (dryFrame));
            std::copy (std::begin (inFrame), std::end (inFrame), std::begin (prevIn));
            rnnoise_process_frame (state, outFrame, scaled);
            for (int k = 0; k < frameSize; ++k) outFrame[k] *= (1.0f / 32768.0f);
        }
    }
}

//==============================================================================
bool Take::save() const
{
    juce::XmlElement x ("TFVocalTake");
    x.setAttribute ("version", 1);
    x.setAttribute ("backing", backing.getFullPathName());
    x.setAttribute ("start", startSeconds);
    x.setAttribute ("sampleRate", sampleRate);
    x.setAttribute ("vocalOffset", (double) vocalOffset);
    x.setAttribute ("speaker", speaker.getFileName());
    x.setAttribute ("length", lengthSeconds);
    return x.writeTo (folder.getChildFile ("session.xml"));
}

Take Take::load (const juce::File& folder)
{
    Take t;
    auto xml = juce::XmlDocument::parse (folder.getChildFile ("session.xml"));
    if (xml == nullptr || ! xml->hasTagName ("TFVocalTake"))
        return t;
    t.folder = folder;
    t.vocal = folder.getChildFile ("vocal.wav");
    t.speaker = folder.getChildFile ("speaker.wav");
    auto b = xml->getStringAttribute ("backing");
    if (b.isNotEmpty() && juce::File::isAbsolutePath (b)) t.backing = juce::File (b);
    t.startSeconds = xml->getDoubleAttribute ("start");
    t.sampleRate = xml->getDoubleAttribute ("sampleRate", 48000.0);
    t.vocalOffset = (juce::int64) xml->getDoubleAttribute ("vocalOffset");
    t.lengthSeconds = xml->getDoubleAttribute ("length");
    return t;
}

//==============================================================================
// TimelineTrack: song/take reader -> 20 s read-ahead buffer -> (optional) resampler.
// The engine owns one playhead; every track is read in lock-step from it, so the song,
// the take and the recording can never drift apart.
//==============================================================================
void AudioEngine::TimelineTrack::prepare (double deviceRate, int maxBlock)
{
    const double ratio = sourceRate / deviceRate;
    if (std::abs (ratio - 1.0) < 1e-9)
    {
        resampler.reset();
        buffer->prepareToPlay (maxBlock, deviceRate);
    }
    else
    {
        if (resampler == nullptr)
            resampler = std::make_unique<juce::ResamplingAudioSource> (buffer.get(), false, channels);
        resampler->setResamplingRatio (ratio);
        resampler->prepareToPlay (maxBlock, deviceRate);   // also prepares (pre-fills) the buffer
    }
}

void AudioEngine::TimelineTrack::seek (juce::int64 timelineSamples, double deviceRate)
{
    buffer->setNextReadPosition ((juce::int64) std::llround ((double) timelineSamples * sourceRate / deviceRate));
    if (resampler != nullptr) resampler->flushBuffers();
}

void AudioEngine::TimelineTrack::read (juce::AudioBuffer<float>& dest, int n, juce::int64 timelineAfter, double deviceRate) noexcept
{
    juce::AudioSourceChannelInfo info (&dest, 0, n);
    if (resampler != nullptr) resampler->getNextAudioBlock (info);
    else                      buffer->getNextAudioBlock (info);

    // If the read-ahead missed (e.g. right after a seek), it does not advance.
    // Re-align it to the playhead so the track stays in sync with everything else.
    const auto expected = (juce::int64) std::llround ((double) timelineAfter * sourceRate / deviceRate);
    const auto actual = buffer->getNextReadPosition();
    if (resampler == nullptr)
    {
        if (actual != expected) buffer->setNextReadPosition (expected);
    }
    else if (std::abs (actual - expected) > (juce::int64) (n * sourceRate / deviceRate) + 64)
    {
        buffer->setNextReadPosition (expected);
        resampler->flushBuffers();
    }
}

bool AudioEngine::TimelineTrack::waitReady (int timeoutMs)
{
    juce::AudioBuffer<float> tmp (channels, 4096);
    juce::AudioSourceChannelInfo info (&tmp, 0, tmp.getNumSamples());
    return buffer->waitForNextAudioBlockReady (info, (juce::uint32) timeoutMs);
}

std::unique_ptr<AudioEngine::TimelineTrack> AudioEngine::makeTrack (std::unique_ptr<juce::PositionableAudioSource> src, double rate, int channels, bool prefill)
{
    auto t = std::make_unique<TimelineTrack>();
    t->sourceRate = rate;
    t->channels = channels;
    t->source = std::move (src);
    t->buffer = std::make_unique<juce::BufferingAudioSource> (t->source.get(), readAheadThread, false, (int) (rate * 20.0), channels, prefill);
    // Prepare + pre-fill here on the message thread, so swapping it in never blocks the audio thread.
    const double devRate = currentRate.load();
    const int block = maxBlockShared.load();
    if (devRate > 0 && block > 0)
    {
        t->prepare (devRate, block);
        t->seek (playhead.load(), devRate);
    }
    return t;
}

juce::File AudioEngine::playableAudioFor (const juce::File& f, juce::AudioFormatManager& fm)
{
    if (! f.existsAsFile()) return {};
    if (media::canReadDirectly (f, fm)) return f;
    auto c = media::cacheFor (f);
    return c.existsAsFile() ? c : juce::File();
}

juce::File AudioEngine::appFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TF Vocal Studio");
    f.createDirectory();
    return f;
}
juce::File AudioEngine::songsFolder()      { auto f = appFolder().getChildFile ("Songs");      f.createDirectory(); return f; }
juce::File AudioEngine::recordingsFolder() { auto f = appFolder().getChildFile ("Recordings"); f.createDirectory(); return f; }

void AudioEngine::initialise (const juce::XmlElement* saved, const juce::String& mic)
{
    micName = mic;
    deviceManager.initialise (2, 2, saved, true);

    if (saved == nullptr)
    {
        // First run: pick the lowest-latency driver available on this PC.
        juce::String chosen;
        for (auto name : { "Windows Audio (Low Latency Mode)", "Windows Audio" })
        {
            for (auto* type : deviceManager.getAvailableDeviceTypes())
                if (type->getTypeName() == name) { chosen = name; break; }
            if (chosen.isNotEmpty()) break;
        }
        if (chosen.isNotEmpty())
            deviceManager.setCurrentAudioDeviceType (chosen, true);

        auto setup = deviceManager.getAudioDeviceSetup();
        setup.sampleRate = 48000.0;   // RNNoise runs natively at 48 kHz
        setup.bufferSize = 256;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    deviceManager.addAudioCallback (this);
    deviceManager.addChangeListener (this);
    updateInputRouting();
}

juce::StringArray AudioEngine::getMicrophoneNames()
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();
        return type->getDeviceNames (true);
    }
    return {};
}

void AudioEngine::setMicrophone (const juce::String& name)
{
    if (recording.load()) return;
    micName = name;
    routedKey = {};
    updateInputRouting();
    sendChangeMessage();
}

void AudioEngine::updateInputRouting()
{
    // Sound cards with separate inputs and outputs (Windows): the speaker runs as an output-only
    // device and the microphone has its own input stream (InputBridge) — never the combined device,
    // whose tiny shared buffer inserts silence when the two clocks drift apart.
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    auto setup = deviceManager.getAudioDeviceSetup();
    const bool separate = type != nullptr && type->hasSeparateInputsAndOutputs();
    if (! separate)
    {
        if (splitInput.exchange (false)) micBridge.close();
        routedKey = {};
        return;
    }
    if (micName.isEmpty() && setup.inputDeviceName.isNotEmpty()) micName = setup.inputDeviceName;   // the earlier version's choice
    if (setup.inputDeviceName.isNotEmpty() || ! setup.inputChannels.isZero() || setup.useDefaultInputChannels)
    {
        setup.inputDeviceName = {};
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        routedKey = {};
        deviceManager.setAudioDeviceSetup (setup, true);   // re-enters through the change message
        setup = deviceManager.getAudioDeviceSetup();
    }
    const double rate = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
    const auto key = type->getTypeName() + "|" + micName + "|" + juce::String (rate);
    if (key == routedKey && micBridge.isOpen()) return;
    routedKey = key;
    auto err = micBridge.open (*type, micName, rate, juce::jmax (128, setup.bufferSize));
    splitInput = err.isEmpty();
    if (err.isEmpty() && micName.isEmpty()) micName = micBridge.getDeviceName();
    if (auto* d = deviceManager.getCurrentAudioDevice()) micBridge.prepareOutput (d->getCurrentSampleRate());
}

std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> AudioEngine::makeWriter (const juce::File& f, juce::String& error)
{
    auto stream = std::make_unique<juce::FileOutputStream> (f);
    if (! stream->openedOk()) { error = juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না: ") + f.getFullPathName(); return {}; }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (stream.release());
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions()
                                                .withSampleRate (currentRate.load())
                                                .withNumChannels (1)
                                                .withBitsPerSample (32));   // 32-bit float: a loud note can never clip in the file
    if (writer == nullptr) { error = juce::String::fromUTF8 ("WAV ফাইল তৈরি করা যাচ্ছে না।"); return {}; }
    return std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 1 << 18);
}

int AudioEngine::getRoundTripLatencySamples() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        int l = 0;
        if (splitInput.load() && micBridge.isOpen())
            l = d->getOutputLatencyInSamples() + micBridge.deviceInputLatency() + (int) std::lround (micBridge.bufferedSamples());
        else
            l = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
        if (l <= 0) l = 2 * d->getCurrentBufferSizeSamples();
        return l + (int) std::lround (MasterBus::lookahead * currentRate.load() / 48000.0);   // the master limiter delays what we hear
    }
    return 0;
}

double AudioEngine::getRoundTripLatencyMs() const
{
    auto sr = currentRate.load();
    return sr > 0 ? 1000.0 * getRoundTripLatencySamples() / sr : 0.0;
}

juce::String AudioEngine::getDeviceName() const
{
    if (splitInput.load() && micBridge.isOpen()) return micBridge.getDeviceName();
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        return setup.inputDeviceName.isNotEmpty() ? setup.inputDeviceName : d->getName();
    }
    return {};
}

bool AudioEngine::isDeviceOpen() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    return d != nullptr && d->isOpen() && d->isPlaying();
}

int AudioEngine::getXruns() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    return (d != nullptr ? juce::jmax (0, d->getXRunCount()) : 0) + micBridge.getUnderruns();
}

//==============================================================================
/** Plays one clip (a part of an audio file) at its place on the timeline; silence elsewhere.
    Positions are timeline samples at the file's rate. Short fades at the clip edges avoid clicks. */
class AudioEngine::ClipSource : public juce::PositionableAudioSource
{
public:
    ClipSource (juce::AudioFormatReader* r, const ClipInfo& c, juce::int64 readAheadSamples)
        : reader (r),
          startS ((juce::int64) std::llround (c.start * r->sampleRate)),
          inS ((juce::int64) std::llround (c.in * r->sampleRate)),
          lenS ((juce::int64) std::llround (c.length() * r->sampleRate)),
          ahead (readAheadSamples),
          fade (juce::jmax ((juce::int64) 1, (juce::int64) std::llround (clipFadeSeconds * r->sampleRate))) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const auto t0 = position + ahead;
        const auto t1 = t0 + info.numSamples;
        const auto a = juce::jmax (t0, startS), b = juce::jmin (t1, startS + lenS);
        if (b > a)
        {
            const int dest = info.startSample + (int) (a - t0);
            const int n = (int) (b - a);
            const auto fileIdx = inS + (a - startS);
            reader->read (info.buffer, dest, n, fileIdx, true, true);
            for (int ch = (int) reader->numChannels; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->copyFrom (ch, dest, *info.buffer, 0, dest, n);
            // edge fades
            for (int i = 0; i < n; ++i)
            {
                const auto t = a + i - startS;
                const auto distEnd = lenS - 1 - t;
                if (t >= fade && distEnd >= fade) { i = (int) juce::jmax ((juce::int64) i, lenS - fade - (a - startS) - 1); continue; }
                const float g = (float) juce::jmin (t, distEnd) / (float) fade;
                for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                    info.buffer->setSample (ch, dest + i, info.buffer->getSample (ch, dest + i) * juce::jlimit (0.0f, 1.0f, g));
            }
        }
        position += info.numSamples;
    }

    void setNextReadPosition (juce::int64 p) override { position = p; }
    juce::int64 getNextReadPosition() const override  { return position; }
    juce::int64 getTotalLength() const override       { return startS + lenS + (juce::int64) (reader->sampleRate * 7200.0); }
    bool isLooping() const override                   { return false; }

private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::int64 startS, inS, lenS, ahead, fade, position = 0;
};

//==============================================================================
/** A voice clip with studio-quality pitch correction (Rubber Band). It is computed on the read-ahead
    thread, seconds before it is heard — never on the audio thread — so it cannot cause crackle.
    Output for timeline sample t = the corrected voice of input sample t (its delay is absorbed here). */
class AudioEngine::TunedClipSource : public juce::PositionableAudioSource
{
public:
    TunedClipSource (std::unique_ptr<ClipSource> r, const FxParams* f, std::function<int()> k, double rate)
        : raw (std::move (r)), fx (f), key (std::move (k))
    {
        tune.prepare (rate, block);
        lat = tune.latencySamples();
        in.assign ((size_t) block, 0.0f);
        out.assign ((size_t) block, 0.0f);
        tmp.setSize (1, block);
    }
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void setNextReadPosition (juce::int64 p) override { position = p; }
    juce::int64 getNextReadPosition() const override  { return position; }
    juce::int64 getTotalLength() const override       { return raw->getTotalLength(); }
    bool isLooping() const override                   { return false; }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const bool on = fx->isOn (FxParams::tuneOn) && fx->get (FxParams::tuneAmt) > 0.5f && ! fx->bypass.load();
        if (! on)
        {
            primed = false;
            raw->setNextReadPosition (position);
            raw->getNextAudioBlock (info);
            position += info.numSamples;
            return;
        }
        const float amt = fx->get (FxParams::tuneAmt) / 100.0f, spd = fx->get (FxParams::tuneSpd);
        const int k = key ? key() : -1;
        if (! primed || position != nextPos || k != lastKey)
        {
            // (re)start: run the corrector over 3x its delay first, so the output is settled
            tune.reset();
            feed = position - 2 * lat;
            pump (position + lat, amt, spd, k, nullptr, 0);
            primed = true;
            lastKey = k;
        }
        info.clearActiveBufferRegion();
        auto* d = info.buffer->getWritePointer (0, info.startSample);
        pump (position + lat + info.numSamples, amt, spd, k, d, position + lat);
        for (int ch = 1; ch < info.buffer->getNumChannels(); ++ch) info.buffer->copyFrom (ch, info.startSample, *info.buffer, 0, info.startSample, info.numSamples);
        position += info.numSamples;
        nextPos = position;
    }

private:
    /** Feeds input until `until`; outputs from index `from` on are written to dest (if any). */
    void pump (juce::int64 until, float amt, float spd, int k, float* dest, juce::int64 from)
    {
        while (feed < until)
        {
            const int m = (int) juce::jmin ((juce::int64) block, until - feed);
            juce::AudioSourceChannelInfo ri (&tmp, 0, m);
            raw->setNextReadPosition (feed);
            raw->getNextAudioBlock (ri);
            tune.process (tmp.getReadPointer (0), out.data(), m, true, amt, spd, k);
            if (dest != nullptr)
                for (int i = 0; i < m; ++i)
                    if (const auto j = feed + i; j >= from) dest[j - from] = out[(size_t) i];
            feed += m;
        }
    }

    std::unique_ptr<ClipSource> raw;
    const FxParams* fx;
    std::function<int()> key;
    SmoothTune tune;
    static constexpr int block = 1024;
    int lat = 0, lastKey = -999;
    std::vector<float> in, out;
    juce::AudioBuffer<float> tmp;
    juce::int64 position = 0, nextPos = -1, feed = 0;
    bool primed = false;
};

//==============================================================================
void AudioEngine::waitUntilBuffered()
{
    for (auto& t : tracks)
        for (auto& c : t->clips)
            if (c->endSec > getPosition() && c->startSec < getPosition() + 25.0)
                c->track->waitReady (2000);
}

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
    readAheadThread.startThread (juce::Thread::Priority::high);
    writerThread.startThread (juce::Thread::Priority::high);
    exportPool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions().withThreadName ("TF export").withNumberOfThreads (1));
    keyPool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions().withThreadName ("TF key").withNumberOfThreads (1));
    for (auto& b : builtInPresets()) if (b.id == "ghazal") b.applyTo (defaultFx);
    defaultMonitorChain = std::make_unique<VocalChain> (defaultFx);
}

AudioEngine::~AudioEngine()
{
    alive->store (false);
    if (recording.load()) stopRecording();
    deviceManager.removeAudioCallback (this);
    deviceManager.removeChangeListener (this);
    micBridge.close();
    deviceManager.closeAudioDevice();
    exportPool->removeAllJobs (true, 15000);
    keyCancel = true;
    keyPool->removeAllJobs (true, 5000);
    tracks.clear();
    readAheadThread.stopThread (2000);
    writerThread.stopThread (4000);
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        if (recording.load() && ! isDeviceOpen()) stopRecording();
        if (! recording.load()) updateInputRouting();
    }
    sendChangeMessage();
}

//==============================================================================
// Project -> engine tracks
//==============================================================================
juce::String AudioEngine::clipKey (const ClipInfo& c, juce::int64 lat)
{
    return c.file.getFullPathName() + "|" + juce::String (c.start, 6) + "|" + juce::String (c.in, 6) + "|" + juce::String (c.out, 6) + "|" + juce::String (lat);
}

std::unique_ptr<AudioEngine::EngClip> AudioEngine::makeClip (const ClipInfo& c, TrackKind kind, const FxParams* fx, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (c.file));
    if (reader == nullptr) { error = juce::String::fromUTF8 ("ফাইল পাওয়া যাচ্ছে না: ") + c.file.getFileName(); return {}; }
    const auto rate = reader->sampleRate;
    auto clip = std::make_unique<EngClip>();
    clip->key = clipKey (c, 0);
    clip->startSec = c.start;
    clip->endSec = c.end();
    auto raw = std::make_unique<ClipSource> (reader.release(), c, 0);
    if (kind == TrackKind::vocal && fx != nullptr)
        clip->track = makeTrack (std::make_unique<TunedClipSource> (std::move (raw), fx, [this] { return getEffectiveKey(); }, rate), rate, 1, false);
    else
        clip->track = makeTrack (std::move (raw), rate, kind == TrackKind::music ? 2 : 1);
    return clip;
}

void AudioEngine::prepareTrack (EngTrack& t, double sr, int block)
{
    if (t.kind == TrackKind::vocal)
    {
        t.playChain->prepare (sr, block, false);    // pitch correction happens ahead of time, in the clip sources
        t.monitorChain->prepare (sr, block, false);
        t.chainLatency = 0;
    }
    t.clipBuf.setSize (2, block);
    t.sum.setSize (2, block);
    t.fxOut.setSize (2, block);
    t.levelL.reset (sr, 0.03);
    t.levelR.reset (sr, 0.03);
    t.ducker.prepare (sr);
}

std::unique_ptr<AudioEngine::EngTrack> AudioEngine::makeEngTrack (const TrackInfo& info, std::vector<std::unique_ptr<EngClip>>&)
{
    auto t = std::make_unique<EngTrack>();
    t->id = info.id;
    t->kind = info.kind;
    if (info.kind == TrackKind::vocal)
    {
        t->fx = std::make_unique<FxParams>();
        if (info.fx.isNotEmpty()) t->fx->fromString (info.fx);
        else for (auto& b : builtInPresets()) if (b.id == "ghazal") b.applyTo (*t->fx);
        t->fxState = info.fx;
        t->playChain = std::make_unique<VocalChain> (*t->fx);
        t->monitorChain = std::make_unique<VocalChain> (*t->fx);
    }
    prepareTrack (*t, currentRate.load(), juce::jmax (1024, maxBlockShared.load()));
    return t;
}

void AudioEngine::setProject (const ProjectState& p)
{
    // 1. create everything new outside the audio lock (file opening, buffering, effect chains)
    std::map<juce::String, std::unique_ptr<EngTrack>> newTracks;
    std::map<juce::String, std::unique_ptr<EngClip>> newClips;
    std::vector<std::unique_ptr<EngClip>> unused;
    for (auto& ti : p.tracks)
    {
        EngTrack* existing = nullptr;
        for (auto& t : tracks) if (t->id == ti.id && t->kind == ti.kind) existing = t.get();
        if (existing == nullptr) newTracks[ti.id] = makeEngTrack (ti, unused);
        EngTrack* tr = existing != nullptr ? existing : newTracks[ti.id].get();
        for (auto& c : ti.clips)
        {
            const auto key = clipKey (c, 0);
            bool have = false;
            if (existing != nullptr) for (auto& ec : existing->clips) if (ec->key == key) { have = true; break; }
            if (! have && newClips.count (key) == 0)
            {
                juce::String err;
                if (auto ec = makeClip (c, ti.kind, tr->fx.get(), err)) newClips[key] = std::move (ec);
            }
        }
    }

    // 2. swap in under the lock (only pointer moves and seeks)
    std::vector<std::unique_ptr<EngTrack>> garbageTracks;
    std::vector<std::unique_ptr<EngClip>> garbageClips;
    {
        const juce::ScopedLock sl (transportLock);
        const double sr = currentRate.load();
        const auto ph = playhead.load();
        std::vector<std::unique_ptr<EngTrack>> result;
        for (auto& ti : p.tracks)
        {
            std::unique_ptr<EngTrack> tr;
            for (auto& t : tracks) if (t != nullptr && t->id == ti.id && t->kind == ti.kind) { tr = std::move (t); break; }
            if (tr == nullptr) { tr = std::move (newTracks[ti.id]); if (tr == nullptr) continue; }

            tr->gain = ti.gain;
            tr->pan = ti.pan;
            tr->mute = ti.mute;
            tr->solo = ti.solo;
            bool retune = false;
            if (tr->fx != nullptr && ti.fx.isNotEmpty() && ti.fx != tr->fxState)
            {
                const auto before = juce::String ((int) tr->fx->isOn (FxParams::tuneOn)) + juce::String (tr->fx->get (FxParams::tuneAmt)) + juce::String (tr->fx->get (FxParams::tuneSpd));
                tr->fx->fromString (ti.fx);
                tr->fxState = ti.fx;
                retune = before != juce::String ((int) tr->fx->isOn (FxParams::tuneOn)) + juce::String (tr->fx->get (FxParams::tuneAmt)) + juce::String (tr->fx->get (FxParams::tuneSpd));
            }

            std::vector<std::unique_ptr<EngClip>> clips;
            for (auto& c : ti.clips)
            {
                const auto key = clipKey (c, 0);
                std::unique_ptr<EngClip> ec;
                for (auto& old : tr->clips) if (old != nullptr && old->key == key) { ec = std::move (old); break; }
                if (ec == nullptr)
                {
                    auto it = newClips.find (key);
                    if (it != newClips.end() && it->second != nullptr) { ec = std::move (it->second); ec->track->seek (ph, sr); }
                }
                if (ec != nullptr) clips.push_back (std::move (ec));
            }
            for (auto& old : tr->clips) if (old != nullptr) garbageClips.push_back (std::move (old));
            tr->clips = std::move (clips);
            if (retune) for (auto& c : tr->clips) c->track->seek (ph, sr);
            result.push_back (std::move (tr));
        }
        for (auto& t : tracks) if (t != nullptr) garbageTracks.push_back (std::move (t));
        tracks = std::move (result);
        projectLength = p.length();
        smartMixAmt = p.smartMix;
        glueOn = p.glue;

        armedTrack = nullptr;
        for (auto& t : tracks) if (t->id == armedTrackId && t->kind == TrackKind::vocal) armedTrack = t.get();
        recordTrack = nullptr;
        for (auto& t : tracks) if (t->id == recordTrackId) recordTrack = t.get();
    }
    // old objects are destroyed here, outside the lock
}

void AudioEngine::retuneAll()
{
    const juce::ScopedLock sl (transportLock);
    const double sr = currentRate.load();
    const auto ph = playhead.load();
    for (auto& t : tracks)
        if (t->kind == TrackKind::vocal)
            for (auto& c : t->clips) c->track->seek (ph, sr);
}

FxParams* AudioEngine::getTrackFx (const juce::String& id)
{
    for (auto& t : tracks) if (t->id == id && t->fx != nullptr) return t->fx.get();
    return nullptr;
}

void AudioEngine::setArmedTrack (const juce::String& id)
{
    const juce::ScopedLock sl (transportLock);
    armedTrackId = id;
    armedTrack = nullptr;
    for (auto& t : tracks) if (t->id == id && t->kind == TrackKind::vocal) armedTrack = t.get();
}

juce::String AudioEngine::getArmedTrack() const { return armedTrackId; }

float AudioEngine::popTrackPeak (const juce::String& id)
{
    for (auto& t : tracks) if (t->id == id) return t->meter.exchange (0.0f);
    return 0.0f;
}

void AudioEngine::detectKeyOf (const juce::File& audio)
{
    if (audio == keyFile) return;
    keyFile = audio;
    songKey = -1;
    keyCancel = true;
    keyPool->removeAllJobs (true, 3000);
    keyCancel = false;
    if (! audio.existsAsFile()) { sendChangeMessage(); return; }
    keyBusy = true;
    keyPool->addJob ([this, audio]
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        int k = -1;
        if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (audio) })
            k = detectSongKey (*r, &keyCancel);
        if (! keyCancel.load()) songKey = k;
        keyBusy = false;
        juce::MessageManager::callAsync ([this, a = alive] { if (a->load() && scaleMode.load() == -2) retuneAll(); });
        sendChangeMessage();
    });
}

//==============================================================================
// Transport
//==============================================================================
void AudioEngine::play()
{
    if (! hasContent()) return;
    if (getPosition() >= getLength() - 0.05 && ! loopOn.load()) setPosition (0.0);
    waitUntilBuffered();
    { const juce::ScopedLock sl (transportLock); playing = true; }
    sendChangeMessage();
}

void AudioEngine::pause()
{
    if (recording.load()) { stopRecording(); return; }
    { const juce::ScopedLock sl (transportLock); playing = false; }
    sendChangeMessage();
}

void AudioEngine::stop()
{
    if (recording.load()) stopRecording();
    { const juce::ScopedLock sl (transportLock); playing = false; }
    setPosition (0.0);
    sendChangeMessage();
}

void AudioEngine::setPosition (double s)
{
    if (recording.load()) return;
    s = juce::jmax (0.0, s);
    const double sr = currentRate.load();
    const auto ph = (juce::int64) std::llround (s * sr);
    const juce::ScopedLock sl (transportLock);
    playhead = ph;
    for (auto& t : tracks)
    {
        for (auto& c : t->clips) c->track->seek (ph, sr);
        if (t->playChain != nullptr) t->playChain->reset();
    }
}

double AudioEngine::getPosition() const { return (double) playhead.load() / juce::jmax (1.0, currentRate.load()); }
bool AudioEngine::isPlaying() const { return playing.load(); }

void AudioEngine::setLoop (juce::Range<double> r, bool enabled)
{
    const double sr = currentRate.load();
    const juce::ScopedLock sl (transportLock);
    loopStart = (juce::int64) std::llround (r.getStart() * sr);
    loopEnd = (juce::int64) std::llround (r.getEnd() * sr);
    loopOn = enabled && r.getLength() > 0.2 && ! recording.load();
}

//==============================================================================
// Recording
//==============================================================================
bool AudioEngine::startRecording (const juce::String& trackId, juce::Range<double> punch, juce::String& error)
{
    if (recording.load()) return true;
    if (! isDeviceOpen()) { error = juce::String::fromUTF8 ("অডিও ডিভাইস চালু নেই। অডিও সেটিংস থেকে মাইক ও স্পিকার বাছাই করুন।"); return false; }
    if (auto* type = deviceManager.getCurrentDeviceTypeObject(); type != nullptr && type->hasSeparateInputsAndOutputs() && ! micBridge.isOpen())
    {
        updateInputRouting();
        if (! micBridge.isOpen()) { error = juce::String::fromUTF8 ("মাইক চালু করা যাচ্ছে না। মাইক লাগানো আছে কিনা দেখুন, তারপর অডিও সেটিংস থেকে মাইক বাছাই করুন।"); return false; }
    }

    auto folder = recordingsFolder().getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S")).getNonexistentSibling();
    if (! folder.createDirectory()) { error = juce::String::fromUTF8 ("রেকর্ডিং ফোল্ডার তৈরি করা যাচ্ছে না।"); return false; }

    Take t;
    t.folder = folder;
    t.vocal = folder.getChildFile ("vocal.wav");
    t.speaker = folder.getChildFile ("speaker.wav");
    t.sampleRate = currentRate.load();

    auto vw = makeWriter (t.vocal, error);
    auto sw = vw != nullptr ? makeWriter (t.speaker, error) : nullptr;
    if (vw == nullptr || sw == nullptr) { folder.deleteRecursively(); return false; }

    vocalPeaks.clear();
    punchRange = punch;
    if (! punch.isEmpty())
        setPosition (juce::jmax (0.0, punch.getStart() - prerollSec.load()));   // cue: hear the music before the part
    waitUntilBuffered();

    const auto latency = (juce::int64) getRoundTripLatencySamples()
                       + (juce::int64) std::llround (userOffsetMs.load() * t.sampleRate / 1000.0);
    {
        const juce::ScopedLock sl (transportLock);
        const juce::ScopedLock wl (writerLock);
        recordWithNoise = false;   // the recording is always the clean, untouched microphone
        t.vocalOffset = juce::jmax ((juce::int64) 0, latency);
        recordStartSeconds = getPosition();
        recordFirstBlock = true;
        vocalWriter = std::move (vw);
        speakerWriter = std::move (sw);
        recordedSamples = 0;
        pendingTake = t;
        recordTrackId = trackId;
        recordTrack = nullptr;
        for (auto& tr : tracks) if (tr->id == trackId) recordTrack = tr.get();
        punchDone = false;
        // the take reaches the end of the part only once the latency has passed as well
        // (+ a little more, so the new part can cross-fade into the old take after it)
        punchEndSample = punch.isEmpty() ? -1 : (juce::int64) std::llround ((punch.getEnd() + 0.03) * t.sampleRate) + t.vocalOffset;
        loopOn = false;
        recording = true;
        playing = true;
    }
    sendChangeMessage();
    return true;
}

void AudioEngine::stopRecording()
{
    if (! recording.load()) return;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> vw, sw;
    {
        const juce::ScopedLock sl (transportLock);
        const juce::ScopedLock wl (writerLock);
        recording = false;
        vw = std::move (vocalWriter);
        sw = std::move (speakerWriter);
        playing = false;
        recordTrack = nullptr;
    }
    vw.reset();
    sw.reset();

    auto& t = pendingTake;
    t.startSeconds = recordStartSeconds.load();
    t.lengthSeconds = (double) recordedSamples.load() / t.sampleRate;
    RecordingResult r;
    r.trackId = recordTrackId;
    r.wav = t.vocal;
    r.speaker = t.speaker;
    r.startedAt = t.startedAt;
    if (t.lengthSeconds < 0.3) { t.folder.deleteRecursively(); }
    else
    {
        // the speaker file is only useful if something was played
        double peakRef = 0.0;
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (t.speaker) })
            {
                juce::Range<float> lv[1];
                rd->readMaxLevels (0, rd->lengthInSamples, lv, 1);
                peakRef = juce::jmax (std::abs (lv[0].getStart()), std::abs (lv[0].getEnd()));
            }
        }
        if (peakRef < 1e-4) { t.speaker.deleteFile(); r.speaker = juce::File(); }
        t.save();
        r.start = t.startSeconds;
        r.in = (double) t.vocalOffset / t.sampleRate;
        r.out = t.lengthSeconds;                       // file seconds (includes the latency head)
        if (! punchRange.isEmpty())
        {
            r.punch = true;
            const double end = r.start + (r.out - r.in);
            const double a = juce::jmax (r.start, punchRange.getStart());
            const double b = juce::jmin (end, punchRange.getEnd());
            r.in += a - r.start;
            r.start = a;
            r.out = r.in + (b - a);
        }
        r.ok = r.out - r.in > 0.05;
    }
    lastResult = r;
    punchRange = {};
    punchEndSample = -1;
    sendChangeMessage();
}

double AudioEngine::getRecordedSeconds() const noexcept
{
    return (double) recordedSamples.load() / juce::jmax (1.0, currentRate.load());
}

//==============================================================================
// Export (offline render of the whole project, same sound as playback)
//==============================================================================
bool AudioEngine::renderProject (const ProjectState& p, juce::Range<double> range, double sr, int scaleKey,
                                 juce::AudioBuffer<float>& mix, int& total, std::function<void (float)> report, juce::String& error,
                                 const std::atomic<bool>* cancel)
{
    const double t0 = range.isEmpty() ? 0.0 : range.getStart();
    double t1 = range.isEmpty() ? p.length() : range.getEnd();
    if (range.isEmpty()) t1 += 2.0;   // let reverb/echo tails ring out
    const int la = (int) std::lround (MasterBus::lookahead * sr / 48000.0);
    total = (int) std::llround ((t1 - t0) * sr);
    if (total <= 0) { error = juce::String::fromUTF8 ("সেভ করার মতো কিছু নেই।"); return false; }
    if (total > (int) (sr * 60 * 30)) { error = juce::String::fromUTF8 ("৩০ মিনিটের বেশি লম্বা — অংশ বাছাই করে সেভ করুন।"); return false; }
    const int len = total + la;   // the master limiter's look-ahead is removed at the end

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    mix.setSize (2, len);
    mix.clear();
    juce::AudioBuffer<float> trk (2, len), voices (1, len);
    voices.clear();
    bool anySolo = false;
    for (auto& t : p.tracks) anySolo |= t.solo;

    int tracksDone = 0;
    // voices first: the music "listens" to them for the smart mix
    for (int pass = 0; pass < 2; ++pass)
    for (auto& t : p.tracks)
    {
        if (cancel != nullptr && cancel->load()) { error = juce::String::fromUTF8 ("বাতিল করা হয়েছে।"); return false; }
        const bool vocal = t.kind == TrackKind::vocal;
        if (vocal != (pass == 0)) continue;
        ++tracksDone;
        const bool audible = ! t.mute && (! anySolo || t.solo);
        // a muted voice still steers the music only if it is heard — so skip it entirely
        if (! audible) continue;
        FxParams fxp;
        std::unique_ptr<VocalChain> chain;
        int lat = 0;
        if (vocal)
        {
            if (t.fx.isNotEmpty()) fxp.fromString (t.fx);
            else for (auto& b : builtInPresets()) if (b.id == "ghazal") b.applyTo (fxp);
            fxp.scaleKey = scaleKey;
            chain = std::make_unique<VocalChain> (fxp);
            chain->prepare (sr, 4096, true);
            lat = chain->latencySamples (false);
        }
        // render the dry track (vocal: mono, shifted by the chain latency)
        juce::AudioBuffer<float> dry (vocal ? 1 : 2, len + lat);
        dry.clear();
        for (auto& c : t.clips)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (c.file));
            if (r == nullptr) continue;
            const double a = juce::jmax (t0, c.start), b = juce::jmin (t1 + (double) (lat + la) / sr, c.end());
            if (b <= a) continue;
            auto src = std::make_unique<juce::AudioFormatReaderSource> (r.get(), false);
            src->setNextReadPosition ((juce::int64) std::llround ((c.in + (a - c.start)) * r->sampleRate));
            juce::ResamplingAudioSource rs (src.get(), false, dry.getNumChannels());
            rs.setResamplingRatio (r->sampleRate / sr);
            rs.prepareToPlay (8192, sr);
            const int destStart = (int) std::llround ((a - t0) * sr);
            const int n = juce::jmin (dry.getNumSamples() - destStart, (int) std::llround ((b - a) * sr));
            juce::AudioBuffer<float> tmp (dry.getNumChannels(), 8192);
            const double fade = std::round (clipFadeSeconds * sr);
            for (int pos = 0; pos < n; pos += 8192)
            {
                const int m = juce::jmin (8192, n - pos);
                juce::AudioSourceChannelInfo info (&tmp, 0, m);
                rs.getNextAudioBlock (info);
                for (int ch = 0; ch < dry.getNumChannels(); ++ch)
                {
                    for (int i = 0; i < m; ++i)
                    {
                        const int k = pos + i;
                        const double tl = a + (double) k / sr;
                        const double fromStart = (tl - c.start) * sr, toEnd = (c.end() - tl) * sr;
                        const float g = (float) juce::jlimit (0.0, 1.0, juce::jmin (fromStart, toEnd) / fade);
                        dry.addSample (ch, destStart + k, tmp.getSample (ch, i) * g);
                    }
                }
            }
        }
        trk.clear();
        if (vocal)
        {
            // process with the voice effects; the chain delays by lat, so feed lat samples ahead
            const float* inP = dry.getReadPointer (0);
            juce::AudioBuffer<float> outb (2, 4096);
            int written = -lat;
            for (int pos = 0; pos < dry.getNumSamples(); pos += 4096)
            {
                const int m = juce::jmin (4096, dry.getNumSamples() - pos);
                chain->process (inP + pos, outb.getWritePointer (0), outb.getWritePointer (1), m, false);
                for (int i = 0; i < m; ++i, ++written)
                    if (written >= 0 && written < len)
                    {
                        trk.setSample (0, written, outb.getSample (0, i));
                        trk.setSample (1, written, outb.getSample (1, i));
                    }
            }
        }
        else
        {
            trk.copyFrom (0, 0, dry, 0, 0, len);
            trk.copyFrom (1, 0, dry, dry.getNumChannels() > 1 ? 1 : 0, 0, len);
        }
        const float gL = t.gain * juce::jmin (1.0f, 1.0f - t.pan), gR = t.gain * juce::jmin (1.0f, 1.0f + t.pan);
        trk.applyGain (0, 0, len, gL);
        trk.applyGain (1, 0, len, gR);
        if (vocal)
        {
            voices.addFrom (0, 0, trk, 0, 0, len, 0.5f);
            voices.addFrom (0, 0, trk, 1, 0, len, 0.5f);
        }
        else
        {
            // the smart mix gives way to the voices, exactly as during playback
            MusicDucker dk;
            dk.prepare (sr);
            for (int pos = 0; pos < len; pos += 4096)
            {
                const int m = juce::jmin (4096, len - pos);
                dk.process (trk.getWritePointer (0, pos), trk.getWritePointer (1, pos), voices.getReadPointer (0, pos), m, p.smartMix);
            }
        }
        mix.addFrom (0, 0, trk, 0, 0, len);
        mix.addFrom (1, 0, trk, 1, 0, len);
        if (report) report (0.85f * (float) tracksDone / (float) juce::jmax (1, p.tracks.size()));
    }

    // master bus (glue + limiter), then drop its look-ahead so the file starts on time
    MasterBus mb;
    mb.prepare (sr);
    for (int pos = 0; pos < len; pos += 4096)
    {
        const int m = juce::jmin (4096, len - pos);
        mb.process (mix.getWritePointer (0, pos), mix.getWritePointer (1, pos), m, p.glue);
    }
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::copy (mix.getWritePointer (ch), mix.getReadPointer (ch, la), total);
    return true;
}

void AudioEngine::exportMix (const ProjectState& p, const juce::File& dest, juce::Range<double> range,
                             std::function<void (float)> progress, std::function<void (bool, juce::String)> done, ExportOptions opt)
{
    if (exporting.exchange (true)) return;
    const int key = getEffectiveKey();
    exportPool->addJob ([this, p, dest, range, progress, done, opt, key]
    {
        auto finish = [this, done] (bool ok, juce::String msg)
        {
            exporting = false;
            juce::MessageManager::callAsync ([done, ok, msg] { if (done) done (ok, msg); });
        };
        auto report = [progress] (float f) { if (progress) juce::MessageManager::callAsync ([progress, f] { progress (f); }); };

        const double sr = 48000.0;
        juce::AudioBuffer<float> mix;
        int total = 0;
        juce::String err;
        if (! renderProject (p, range, sr, key, mix, total, report, err)) { finish (false, err); return; }

        if (opt.normalise)
        {
            // social-media loudness: -14 LUFS, peaks kept below -1 dBFS
            const double lufs = mixtools::integratedLufs (mix, total, sr);
            if (lufs > -70.0)
            {
                const float g = juce::Decibels::decibelsToGain ((float) juce::jlimit (-12.0, 12.0, -14.0 - lufs));
                mix.applyGain (0, total, g);
                MasterBus lim;
                lim.prepare (sr);
                const int la = (int) std::lround (MasterBus::lookahead * sr / 48000.0);
                juce::AudioBuffer<float> padded (2, total + la);
                padded.clear();
                for (int ch = 0; ch < 2; ++ch) padded.copyFrom (ch, 0, mix, ch, 0, total);
                lim.process (padded.getWritePointer (0), padded.getWritePointer (1), total + la, false);
                for (int ch = 0; ch < 2; ++ch) mix.copyFrom (ch, 0, padded, ch, la, total);
            }
        }
        if (opt.fadeIn > 0.0 || opt.fadeOut > 0.0) mixtools::applyFades (mix, total, sr, opt.fadeIn, opt.fadeOut);

        dest.deleteFile();
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (dest);
        if (! static_cast<juce::FileOutputStream*> (os.get())->openedOk()) { finish (false, juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না।")); return; }
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr) { finish (false, juce::String::fromUTF8 ("WAV ফাইল তৈরি করা যাচ্ছে না।")); return; }
        writer->writeFromAudioSampleBuffer (mix, 0, total);
        writer.reset();
        report (1.0f);
        finish (true, dest.getFullPathName());
    });
}

std::map<juce::String, float> AudioEngine::computeBalance (const ProjectState& p, int scaleKey)
{
    // render the voices (with their effects) and the music separately, measure loudness where both
    // play, and set each voice ~1.5 LU above the music (the "radio" balance of released songs)
    std::map<juce::String, float> result;
    const double sr = 48000.0;
    auto renderOnly = [&] (TrackKind k, ProjectState s, juce::AudioBuffer<float>& b, int& n)
    {
        s = withOnly (s, k);
        s.smartMix = 0.0f; s.glue = false;
        for (auto& t : s.tracks) t.gain = t.kind == k ? 1.0f : t.gain;
        juce::String e;
        return renderProject (s, {}, sr, scaleKey, b, n, nullptr, e);
    };
    juce::AudioBuffer<float> music, voice;
    int nm = 0, nv = 0;
    if (p.musicTrack() == nullptr || ! renderOnly (TrackKind::music, p, music, nm)) return result;
    for (auto& t : p.tracks)
    {
        if (t.kind != TrackKind::vocal || t.clips.isEmpty()) continue;
        auto s = p;
        for (auto& x : s.tracks) if (x.kind == TrackKind::vocal) x.mute = x.id != t.id;
        if (! renderOnly (TrackKind::vocal, s, voice, nv)) continue;
        const int n = juce::jmin (nm, nv);
        // blocks (100 ms) where this voice is singing
        std::vector<bool> active;
        const int hop = (int) (0.1 * sr), blk = (int) (0.4 * sr);
        for (int st = 0; st + blk <= n; st += hop)
        {
            const float rmsV = voice.getRMSLevel (0, st, blk);
            active.push_back (juce::Decibels::gainToDecibels (rmsV) > -40.0f);
        }
        const double lv = mixtools::gatedLoudness (voice, n, sr, &active);
        const double lm = mixtools::gatedLoudness (music, n, sr, &active);
        if (lv < -69.0 || lm < -69.0) continue;
        const double musicGainDb = juce::Decibels::gainToDecibels (p.musicTrack()->gain);
        const double wantDb = (lm + musicGainDb + 1.5) - lv;
        result[t.id] = (float) juce::jlimit (0.1, 1.5, (double) juce::Decibels::decibelsToGain ((float) wantDb));
    }
    return result;
}

void AudioEngine::playBeep (float hz, double seconds, float level)
{
    beepHz = hz;
    beepLevel = level;
    beepTotal = juce::jmax (1, (int) (currentRate.load() * seconds));
    beepLeft = beepTotal;
}

void AudioEngine::measureLatency (std::function<void (bool, double, juce::String)> done)
{
    if (measuring.load() || recording.load() || ! isDeviceOpen()) { if (done) done (false, 0.0, juce::String::fromUTF8 ("এখন মাপা যাচ্ছে না — রেকর্ডিং থামিয়ে আবার চেষ্টা করুন।")); return; }
    const double sr = currentRate.load();
    const int len = (int) (sr * 2.4);
    measMic.assign ((size_t) len, 0.0f);
    measOut.assign ((size_t) len, 0.0f);
    // four short noise bursts (easy to find in the room, sharp correlation peak)
    juce::Random rnd (7);
    for (int b = 0; b < 4; ++b)
    {
        const int at = (int) (sr * (0.15 + 0.45 * b)), blen = (int) (sr * 0.012);
        for (int i = 0; i < blen; ++i)
        {
            const float env = std::sin (juce::MathConstants<float>::pi * (float) i / (float) blen);
            measOut[(size_t) (at + i)] = 0.5f * env * (rnd.nextFloat() * 2.0f - 1.0f);
        }
    }
    measureDone = std::move (done);
    measPos = 0;
    measuring.store (true, std::memory_order_release);
    juce::Thread::launch ([this, sr, len, a = alive]
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + 6000.0;
        while (measPos.load (std::memory_order_acquire) < len && juce::Time::getMillisecondCounterHiRes() < until) juce::Thread::sleep (20);
        measuring.store (false, std::memory_order_release);
        double conf = 0.0;
        const int d = mixtools::findClickDelay (measMic, measOut, (int) (0.6 * sr), &conf);
        const double ms = d >= 0 ? 1000.0 * d / sr : 0.0;
        if (! a->load()) return;
        juce::MessageManager::callAsync ([this, d, ms, a]
        {
            if (! a->load()) return;
            auto cb = std::move (measureDone);
            if (! cb) return;
            if (d < 0) { cb (false, 0.0, juce::String::fromUTF8 ("মাইকে শব্দটা শোনা যায়নি। স্পিকারের ভলিউম একটু বাড়িয়ে, মাইক স্পিকারের দিকে রেখে আবার চেষ্টা করুন।")); return; }
            // the recording already skips the reported delay; the rest becomes the sync offset
            const double reportedMs = getRoundTripLatencyMs();
            setUserOffsetMs ((float) juce::jlimit (-150.0, 150.0, ms - reportedMs));
            cb (true, ms, {});
        });
    });
}

//==============================================================================
// Audio device
//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    const double oldRate = currentRate.load();

    const juce::ScopedLock sl (transportLock);
    if (oldRate > 0 && std::abs (oldRate - sr) > 0.5)
        playhead = (juce::int64) std::llround ((double) playhead.load() * sr / oldRate);

    currentRate = sr;
    currentBlock = bs;
    maxBlock = juce::jmax (bs, 1024);
    maxBlockShared = maxBlock;

    micBuf.setSize (1, maxBlock);
    nsBuf.setSize (1, maxBlock);
    bridgeBuf.setSize (1, maxBlock);
    speakerBuf.setSize (1, maxBlock);
    master.setSize (2, maxBlock);
    monitorBuf.setSize (2, maxBlock);
    playBus.setSize (2, maxBlock);
    voiceBus.setSize (1, maxBlock);
    defaultMonitorChain->prepare (sr, maxBlock, false);
    masterBus.prepare (sr);
    livePitch.prepare (sr);
    micBridge.prepareOutput (sr);

    for (auto& t : tracks)
    {
        prepareTrack (*t, sr, maxBlock);
        for (auto& c : t->clips) { c->track->prepare (sr, maxBlock); c->track->seek (playhead.load(), sr); }
    }

    highPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 70.0f, 0.707f);
    highPass.reset();
    limiter.prepare (sr, juce::Decibels::decibelsToGain (-0.3f));
    denoiser.reset();

    for (auto* s : { &gainSm, &voiceSm, &monitorSm })
        s->reset (sr, 0.03);
    gainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));
    voiceSm.setCurrentAndTargetValue (voiceVol.load());
    monitorSm.setCurrentAndTargetValue (monitorOn.load() ? 1.0f : 0.0f);
}

void AudioEngine::audioDeviceStopped()
{
    const juce::ScopedLock sl (transportLock);
    playing = false;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int start = 0; start < numSamples; start += maxBlock)
        processChunk (in, numIn, out, numOut, start, juce::jmin (maxBlock, numSamples - start));
}

void AudioEngine::processChunk (const float* const* in, int numIn, float* const* out, int numOut, int start, int n) noexcept
{
    // ---- 1. microphone -> mono, gain, rumble filter
    auto* mic = micBuf.getWritePointer (0);
    juce::FloatVectorOperations::clear (mic, n);
    if (splitInput.load (std::memory_order_relaxed))
    {
        micBridge.pull (mic, n);   // the mic's own sound card, clock drift followed sample by sample
    }
    else
    {
        int active = 0;
        for (int ch = 0; ch < numIn; ++ch)
            if (in[ch] != nullptr) { juce::FloatVectorOperations::add (mic, in[ch] + start, n); ++active; }
        if (active > 1) juce::FloatVectorOperations::multiply (mic, 1.0f / (float) active, n);
    }

    gainSm.setTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        auto v = highPass.processSample (mic[i] * gainSm.getNextValue());
        mic[i] = v;
        peak = juce::jmax (peak, std::abs (v));
    }
    if (peak > inPeak.load (std::memory_order_relaxed)) inPeak.store (peak, std::memory_order_relaxed);
    livePitch.push (mic, n);

    // ---- 2. AI noise removal for the live microphone only (the recording stays untouched)
    const bool isRec = recording.load (std::memory_order_acquire);
    const bool nsLive = noiseOn.load() && noiseInMonitor.load() && noiseAvailable() && monitorOn.load();
    const float* monVocal = mic;
    if (nsLive)
    {
        auto* ns = nsBuf.getWritePointer (0);
        if (nsResetPending.exchange (false)) denoiser.reset();
        juce::FloatVectorOperations::copy (ns, mic, n);
        denoiser.process (ns, n, noiseAmount.load());
        monVocal = ns;
    }

    // ---- 3. all tracks, read in lock-step from the single playhead
    float* mL = master.getWritePointer (0);
    float* mR = master.getWritePointer (1);
    juce::FloatVectorOperations::clear (mL, n);
    juce::FloatVectorOperations::clear (mR, n);
    double timelinePos = 0.0;
    {
        const juce::ScopedLock sl (transportLock);
        const double sr = currentRate.load (std::memory_order_relaxed);
        const auto ph = playhead.load (std::memory_order_relaxed);
        timelinePos = (double) ph / sr;

        // global auto-tune settings shared by every voice
        {
            const int key = getEffectiveKey();
            const bool lt = liveTuneAll.load (std::memory_order_relaxed);
            for (auto& t : tracks) if (t->fx != nullptr) { t->fx->scaleKey.store (key, std::memory_order_relaxed); t->fx->liveTune.store (lt, std::memory_order_relaxed); }
            defaultFx.scaleKey.store (key, std::memory_order_relaxed);
            defaultFx.liveTune.store (lt, std::memory_order_relaxed);
        }

        // live microphone through the armed track's effects
        monitorSm.setTargetValue (monitorOn.load() ? 1.0f : 0.0f);
        voiceSm.setTargetValue (voiceVol.load());
        if (monitorSm.getTargetValue() > 0.0f || monitorSm.isSmoothing())
        {
            auto* chain = armedTrack != nullptr ? armedTrack->monitorChain.get() : defaultMonitorChain.get();
            chain->process (monVocal, monitorBuf.getWritePointer (0), monitorBuf.getWritePointer (1), n, true);
            const auto* a = monitorBuf.getReadPointer (0);
            const auto* b = monitorBuf.getReadPointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float g = monitorSm.getNextValue() * voiceSm.getNextValue();
                mL[i] += a[i] * g;
                mR[i] += b[i] * g;
            }
        }

        float* pL = playBus.getWritePointer (0);
        float* pR = playBus.getWritePointer (1);
        float* vb = voiceBus.getWritePointer (0);
        juce::FloatVectorOperations::clear (pL, n);
        juce::FloatVectorOperations::clear (pR, n);
        juce::FloatVectorOperations::clear (vb, n);
        if (playing.load (std::memory_order_relaxed))
        {
            auto after = ph + n;
            bool anySolo = false;
            for (auto& t : tracks) anySolo |= t->solo.load();
            const float duck = smartMixAmt.load (std::memory_order_relaxed);

            // voices first (the music listens to them for the smart mix), then the music
            for (int pass = 0; pass < 2; ++pass)
            for (auto& tp : tracks)
            {
                auto& t = *tp;
                const bool vocal = t.kind == TrackKind::vocal;
                if (vocal != (pass == 0)) continue;
                const int chs = vocal ? 1 : 2;
                for (int ch = 0; ch < 2; ++ch) t.sum.clear (ch, 0, n);
                for (auto& c : t.clips)
                {
                    juce::AudioBuffer<float> view (t.clipBuf.getArrayOfWritePointers(), chs, n);
                    c->track->read (view, n, after, sr);
                    for (int ch = 0; ch < chs; ++ch) t.sum.addFrom (ch, 0, t.clipBuf, ch, 0, n);
                }
                bool audible = ! t.mute.load() && (! anySolo || t.solo.load());
                if (isRec && &t == recordTrack)
                {
                    // punch-in: hear the old take only until the punch point
                    const auto pe = punchEndSample.load();
                    const double ps = punchRange.isEmpty() ? -1.0 : punchRange.getStart();
                    if (pe < 0 || timelinePos >= ps) audible = false;
                }
                const float* outA;
                const float* outB;
                if (vocal)
                {
                    t.playChain->process (t.sum.getReadPointer (0), t.fxOut.getWritePointer (0), t.fxOut.getWritePointer (1), n, false);
                    outA = t.fxOut.getReadPointer (0);
                    outB = t.fxOut.getReadPointer (1);
                }
                else
                {
                    t.ducker.process (t.sum.getWritePointer (0), t.sum.getWritePointer (1), vb, n, duck);
                    outA = t.sum.getReadPointer (0);
                    outB = t.sum.getReadPointer (1);
                }
                const float g = audible ? t.gain.load() : 0.0f;
                const float pn = t.pan.load();
                t.levelL.setTargetValue (g * juce::jmin (1.0f, 1.0f - pn));
                t.levelR.setTargetValue (g * juce::jmin (1.0f, 1.0f + pn));
                float trPeak = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float a = outA[i] * t.levelL.getNextValue(), b = outB[i] * t.levelR.getNextValue();
                    pL[i] += a;
                    pR[i] += b;
                    if (vocal) vb[i] += 0.5f * (a + b);
                    trPeak = juce::jmax (trPeak, std::abs (a), std::abs (b));
                }
                if (trPeak > t.meter.load (std::memory_order_relaxed)) t.meter.store (trPeak, std::memory_order_relaxed);
            }

            if (isRec && punchEndSample.load() >= 0 && after >= punchEndSample.load())
                punchDone.store (true);

            if (loopOn.load() && ! isRec && after >= loopEnd.load())
            {
                after = loopStart.load();
                for (auto& t : tracks)
                {
                    for (auto& c : t->clips) c->track->seek (after, sr);
                    if (t->playChain != nullptr) t->playChain->reset();
                }
            }
            playhead.store (after, std::memory_order_relaxed);
            if (! isRec && ! loopOn.load() && (double) after >= projectLength.load() * sr)
                playing.store (false);
        }
    }
    // sync measurement: the clicks travel the same path as the music (through the master bus)
    if (measuring.load (std::memory_order_acquire))
    {
        const int p = measPos.load (std::memory_order_relaxed);
        const int m = juce::jmin (n, (int) measMic.size() - p);
        float* pL = playBus.getWritePointer (0);
        float* pR = playBus.getWritePointer (1);
        for (int i = 0; i < m; ++i)
        {
            const float c = measOut[(size_t) (p + i)];
            pL[i] += c; pR[i] += c;
            measMic[(size_t) (p + i)] = mic[i];
        }
        measPos.store (p + juce::jmax (0, m), std::memory_order_release);
    }
    // the master bus always runs (constant delay, so recording stays in sync)
    masterBus.process (playBus.getWritePointer (0), playBus.getWritePointer (1), n, glueOn.load (std::memory_order_relaxed));
    juce::FloatVectorOperations::add (mL, playBus.getReadPointer (0), n);
    juce::FloatVectorOperations::add (mR, playBus.getReadPointer (1), n);

    // ---- 4. beeps (count-in, settings test)
    if (auto bl = beepLeft.load(); bl > 0)
    {
        const double inc = juce::MathConstants<double>::twoPi * beepHz.load() / currentRate.load();
        const int m = juce::jmin (n, bl);
        const float lvl = beepLevel.load();
        const double fadeS = 0.006 * currentRate.load();
        for (int i = 0; i < m; ++i)
        {
            const int pos = beepTotal - (bl - i);
            const float env = (float) juce::jmin (1.0, juce::jmin ((double) pos, (double) (bl - i)) / fadeS);
            const float v = lvl * env * (float) std::sin (tonePhase);
            tonePhase += inc;
            mL[i] += v; mR[i] += v;
        }
        beepLeft = bl - m;
    }

    // ---- 5. output with safety limiter
    float* L = numOut > 0 ? out[0] : nullptr;
    float* R = numOut > 1 ? out[1] : nullptr;
    for (int ch = 2; ch < numOut; ++ch)
        if (out[ch] != nullptr) juce::FloatVectorOperations::clear (out[ch] + start, n);
    float* spk = speakerBuf.getWritePointer (0);
    if (L != nullptr)
    {
        if (R != nullptr)
        {
            juce::FloatVectorOperations::copy (L + start, mL, n);
            juce::FloatVectorOperations::copy (R + start, mR, n);
        }
        else
        {
            for (int i = 0; i < n; ++i) L[start + i] = 0.5f * (mL[i] + mR[i]);
        }
        limiter.process (L + start, R != nullptr ? R + start : nullptr, n);
        for (int i = 0; i < n; ++i) spk[i] = R != nullptr ? 0.5f * (L[start + i] + R[start + i]) : L[start + i];

        auto pk = [] (const float* x, int len) { return juce::jmax (juce::FloatVectorOperations::findMaximum (x, len), -juce::FloatVectorOperations::findMinimum (x, len)); };
        const float pl = pk (L + start, n), pr = R != nullptr ? pk (R + start, n) : pl;
        if (pl > outPeak.load (std::memory_order_relaxed)) outPeak.store (pl, std::memory_order_relaxed);
        if (pr > outPeakR.load (std::memory_order_relaxed)) outPeakR.store (pr, std::memory_order_relaxed);
    }
    else juce::FloatVectorOperations::clear (spk, n);

    // ---- 6. recording: the clean microphone, and what the speakers played at the same moment
    if (isRec)
    {
        const juce::ScopedLock wl (writerLock);
        if (recording.load() && vocalWriter != nullptr && speakerWriter != nullptr)
        {
            if (recordFirstBlock.exchange (false))
            {
                recordStartSeconds = timelinePos;
                pendingTake.startedAt = juce::Time::getCurrentTime();
            }
            const float* v[] = { mic };
            const float* r[] = { spk };
            vocalWriter->write (v, n);
            speakerWriter->write (r, n);
            vocalPeaks.push (timelinePos, peak);
            recordedSamples += n;
        }
    }
}
}
