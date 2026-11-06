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
            std::copy (std::begin (inFrame), std::end (inFrame), std::begin (dryFrame));
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
    x.setAttribute ("rawOffset", (double) rawOffset);
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
    t.raw = folder.getChildFile ("vocal_raw.wav");
    auto b = xml->getStringAttribute ("backing");
    if (b.isNotEmpty() && juce::File::isAbsolutePath (b)) t.backing = juce::File (b);
    t.startSeconds = xml->getDoubleAttribute ("start");
    t.sampleRate = xml->getDoubleAttribute ("sampleRate", 48000.0);
    t.vocalOffset = (juce::int64) xml->getDoubleAttribute ("vocalOffset");
    t.rawOffset = (juce::int64) xml->getDoubleAttribute ("rawOffset");
    t.lengthSeconds = xml->getDoubleAttribute ("length");
    return t;
}

//==============================================================================
/** Plays vocal.wav on the song timeline (position = song seconds * sr), silence outside the take. */
class AudioEngine::TakeSource : public juce::PositionableAudioSource
{
public:
    TakeSource (juce::AudioFormatReader* r, const Take& t, juce::int64 chainLatency)
        : reader (r),
          startSample ((juce::int64) std::llround (t.startSeconds * r->sampleRate)),
          offset (t.vocalOffset + chainLatency),   // read ahead by the effect chain's delay so the result lines up
          totalTimeline (startSample + r->lengthInSamples + (juce::int64) (r->sampleRate * 3600.0)) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        auto fileIndex = position - startSample + offset;
        auto n = info.numSamples;
        int destStart = info.startSample;
        if (fileIndex < 0)
        {
            auto skip = (int) juce::jmin ((juce::int64) n, -fileIndex);
            destStart += skip; n -= skip; fileIndex += skip;
        }
        if (n > 0 && fileIndex < reader->lengthInSamples)
            reader->read (info.buffer, destStart, n, fileIndex, true, false);
        // mono take -> copy into other channels
        for (int ch = 1; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->copyFrom (ch, info.startSample, *info.buffer, 0, info.startSample, info.numSamples);
        position += info.numSamples;
    }

    void setNextReadPosition (juce::int64 p) override { position = p; }
    juce::int64 getNextReadPosition() const override  { return position; }
    juce::int64 getTotalLength() const override       { return totalTimeline; }
    bool isLooping() const override                   { return false; }

private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::int64 startSample, offset, totalTimeline, position = 0;
};

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

std::unique_ptr<AudioEngine::TimelineTrack> AudioEngine::makeTrack (std::unique_ptr<juce::PositionableAudioSource> src, double rate, int channels)
{
    auto t = std::make_unique<TimelineTrack>();
    t->sourceRate = rate;
    t->channels = channels;
    t->source = std::move (src);
    t->buffer = std::make_unique<juce::BufferingAudioSource> (t->source.get(), readAheadThread, false, (int) (rate * 20.0), channels, true);
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

void AudioEngine::waitUntilBuffered()
{
    if (songTrack != nullptr) songTrack->waitReady (2000);
    if (takeTrack != nullptr) takeTrack->waitReady (2000);
}

//==============================================================================
AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
    readAheadThread.startThread (juce::Thread::Priority::high);
    writerThread.startThread (juce::Thread::Priority::high);
    exportPool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions().withThreadName ("TF export").withNumberOfThreads (1));
    keyPool = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions().withThreadName ("TF key").withNumberOfThreads (1));
}

juce::File AudioEngine::playableAudioFor (const juce::File& f, juce::AudioFormatManager& fm)
{
    if (! f.existsAsFile()) return {};
    if (media::canReadDirectly (f, fm)) return f;
    auto c = media::cacheFor (f);
    return c.existsAsFile() ? c : juce::File();
}

AudioEngine::~AudioEngine()
{
    if (recording.load()) stopRecording();
    deviceManager.removeAudioCallback (this);
    deviceManager.removeChangeListener (this);
    deviceManager.closeAudioDevice();
    exportPool->removeAllJobs (true, 15000);
    keyCancel = true;
    keyPool->removeAllJobs (true, 5000);
    songTrack.reset();
    takeTrack.reset();
    readAheadThread.stopThread (2000);
    writerThread.stopThread (4000);
}

juce::File AudioEngine::appFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TF Vocal Studio");
    f.createDirectory();
    return f;
}
juce::File AudioEngine::songsFolder()      { auto f = appFolder().getChildFile ("Songs");      f.createDirectory(); return f; }
juce::File AudioEngine::recordingsFolder() { auto f = appFolder().getChildFile ("Recordings"); f.createDirectory(); return f; }

void AudioEngine::initialise (const juce::XmlElement* saved)
{
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
        setup.bufferSize = 128;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    deviceManager.addAudioCallback (this);
    deviceManager.addChangeListener (this);
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager && recording.load() && ! isDeviceOpen())
        stopRecording();
    sendChangeMessage();
}

//==============================================================================
bool AudioEngine::loadSong (const juce::File& f, juce::String& error)
{
    if (recording.load()) { error = juce::String::fromUTF8 ("রেকর্ডিং চলার সময় গান বদলানো যাবে না।"); return false; }

    const auto audio = playableAudioFor (f, formatManager);
    if (audio == juce::File())
    {
        error = "needs-import";
        return false;
    }
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audio));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
    {
        error = juce::String::fromUTF8 ("এই ফাইলটি খোলা যাচ্ছে না। অন্য কোনো অডিও বা ভিডিও ফাইল দিন।");
        return false;
    }

    const auto rate = reader->sampleRate;
    const auto len = (double) reader->lengthInSamples / rate;

    {   // stop and rewind first, so the new track is pre-filled from the start
        const juce::ScopedLock sl (transportLock);
        playing = false;
        playhead = 0;
    }
    auto track = makeTrack (std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true), rate, 2);

    std::unique_ptr<TimelineTrack> oldSong, oldTake;
    {
        const juce::ScopedLock sl (transportLock);
        oldSong = std::move (songTrack);
        oldTake = std::move (takeTrack);
        takeLoaded = false;
        songTrack = std::move (track);
        songLength = len;
        endReached = false;
    }
    // old tracks are destroyed here, outside the audio lock
    songFile = f;
    vocalPeaks.clear();

    // find the song's musical key in the background (auto-tune snaps to its scale)
    songKey = -1;
    keyCancel = true;
    keyPool->removeAllJobs (true, 3000);
    keyCancel = false;
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
        sendChangeMessage();
    });

    sendChangeMessage();
    return true;
}

void AudioEngine::unloadSong()
{
    if (recording.load()) stopRecording();
    std::unique_ptr<TimelineTrack> oldSong, oldTake;
    {
        const juce::ScopedLock sl (transportLock);
        playing = false;
        playhead = 0;
        oldSong = std::move (songTrack);
        oldTake = std::move (takeTrack);
        takeLoaded = false;
        songLength = 0.0;
    }
    songFile = juce::File();
    sendChangeMessage();
}

void AudioEngine::play()
{
    if (! hasSong()) return;
    if (getPosition() >= songLength - 0.05) setPosition (0.0);
    waitUntilBuffered();   // never start on an empty read-ahead buffer
    {
        const juce::ScopedLock sl (transportLock);
        endReached = false;
        playing = true;
    }
    sendChangeMessage();
}

void AudioEngine::pause()
{
    if (recording.load()) { stopRecording(); return; }
    {
        const juce::ScopedLock sl (transportLock);
        playing = false;
    }
    sendChangeMessage();
}

void AudioEngine::stop()
{
    if (recording.load()) stopRecording();
    {
        const juce::ScopedLock sl (transportLock);
        playing = false;
    }
    setPosition (0.0);
    sendChangeMessage();
}

void AudioEngine::setPosition (double s)
{
    if (recording.load() || ! hasSong()) return;
    s = juce::jlimit (0.0, juce::jmax (0.0, songLength - 0.01), s);
    const double sr = currentRate.load();
    const auto ph = (juce::int64) std::llround (s * sr);
    const juce::ScopedLock sl (transportLock);
    playhead = ph;
    endReached = false;
    if (songTrack != nullptr) songTrack->seek (ph, sr);
    if (takeTrack != nullptr) { takeTrack->seek (ph, sr); playChain.reset(); }
}

double AudioEngine::getPosition() const
{
    return (double) playhead.load() / juce::jmax (1.0, currentRate.load());
}

bool AudioEngine::isPlaying() const { return playing.load(); }

//==============================================================================
std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> AudioEngine::makeWriter (const juce::File& f, juce::String& error)
{
    auto stream = std::make_unique<juce::FileOutputStream> (f);
    if (! stream->openedOk()) { error = juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না: ") + f.getFullPathName(); return {}; }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (stream.release());
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions()
                                                .withSampleRate (currentRate.load())
                                                .withNumChannels (1)
                                                .withBitsPerSample (24));
    if (writer == nullptr) { error = juce::String::fromUTF8 ("WAV ফাইল তৈরি করা যাচ্ছে না।"); return {}; }
    return std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 1 << 17);
}

bool AudioEngine::startRecording (juce::String& error)
{
    if (recording.load()) return true;
    if (! isDeviceOpen()) { error = juce::String::fromUTF8 ("অডিও ডিভাইস চালু নেই। সেটিংস থেকে মাইক ও স্পিকার বাছাই করুন।"); return false; }

    auto title = hasSong() ? songFile.getFileNameWithoutExtension() : juce::String::fromUTF8 ("মুক্ত রেকর্ডিং");
    auto folder = recordingsFolder().getChildFile (juce::File::createLegalFileName (title)
                                                   + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"))
                                    .getNonexistentSibling();
    if (! folder.createDirectory()) { error = juce::String::fromUTF8 ("রেকর্ডিং ফোল্ডার তৈরি করা যাচ্ছে না।"); return false; }

    Take t;
    t.folder = folder;
    t.vocal = folder.getChildFile ("vocal.wav");
    t.raw = folder.getChildFile ("vocal_raw.wav");
    t.backing = songFile;
    t.sampleRate = currentRate.load();

    auto vw = makeWriter (t.vocal, error);
    auto rw = vw != nullptr ? makeWriter (t.raw, error) : nullptr;
    if (vw == nullptr || rw == nullptr) { folder.deleteRecursively(); return false; }

    clearTakePlayback();
    vocalPeaks.clear();
    if (hasSong())
    {
        if (getPosition() >= songLength - 0.1) setPosition (0.0);
        waitUntilBuffered();
    }

    const auto latency = (juce::int64) getRoundTripLatencySamples()
                       + (juce::int64) std::llround (userOffsetMs.load() * t.sampleRate / 1000.0);

    {
        const juce::ScopedLock sl (transportLock);
        const juce::ScopedLock wl (writerLock);

        recordWithNoise = noiseOn.load() && noiseAvailable();
        t.rawOffset = juce::jmax ((juce::int64) 0, latency);
        t.vocalOffset = juce::jmax ((juce::int64) 0, latency + (recordWithNoise ? NoiseSuppressor::frameSize : 0));

        if (! hasSong()) playhead = 0;          // free recording: timeline starts at 0
        recordStartSeconds = getPosition();
        recordFirstBlock = true;                 // the audio thread stores the exact position of the first recorded sample

        vocalWriter = std::move (vw);
        rawWriter = std::move (rw);
        recordedSamples = 0;
        pendingTake = t;
        endReached = false;
        recording = true;
        playing = true;
    }
    sendChangeMessage();
    return true;
}

void AudioEngine::stopRecording()
{
    if (! recording.load()) return;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> vw, rw;
    {
        const juce::ScopedLock sl (transportLock);
        const juce::ScopedLock wl (writerLock);
        recording = false;
        vw = std::move (vocalWriter);
        rw = std::move (rawWriter);
        playing = false;
    }
    vw.reset();   // flushes and closes the files
    rw.reset();

    pendingTake.startSeconds = recordStartSeconds.load();
    pendingTake.lengthSeconds = (double) recordedSamples.load() / pendingTake.sampleRate;
    if (pendingTake.lengthSeconds < 0.3)
    {
        pendingTake.folder.deleteRecursively();   // accidental click, nothing recorded
    }
    else
    {
        pendingTake.save();
        lastTake = pendingTake;
    }
    sendChangeMessage();
}

double AudioEngine::getRecordedSeconds() const noexcept
{
    return (double) recordedSamples.load() / juce::jmax (1.0, currentRate.load());
}

//==============================================================================
bool AudioEngine::loadTakeForPlayback (const Take& t, juce::String& error)
{
    if (recording.load()) return false;
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (t.vocal));
    if (reader == nullptr) { error = juce::String::fromUTF8 ("রেকর্ডিং ফাইল পাওয়া যাচ্ছে না।"); return false; }

    if (t.backing.existsAsFile() && t.backing != songFile)
        if (! loadSong (t.backing, error)) return false;

    // fill the vocal waveform from the file
    vocalPeaks.clear();
    {
        const int chunk = (int) (reader->sampleRate / PeakTimeline::bucketsPerSecond);
        juce::AudioBuffer<float> buf (1, chunk);
        for (juce::int64 p = t.vocalOffset; p < reader->lengthInSamples; p += chunk)
        {
            reader->read (&buf, 0, chunk, p, true, false);
            vocalPeaks.push (t.startSeconds + (double) (p - t.vocalOffset) / reader->sampleRate, buf.getMagnitude (0, 0, chunk));
        }
    }

    {   // stop at the take's start, then pre-fill both tracks from there
        const juce::ScopedLock sl (transportLock);
        playing = false;
    }
    if (hasSong()) setPosition (t.startSeconds);

    const auto rate = reader->sampleRate;
    const auto chainLat = (juce::int64) std::llround (playChain.latencySamples (false) * rate / juce::jmax (1.0, currentRate.load()));
    auto track = makeTrack (std::make_unique<TakeSource> (reader.release(), t, chainLat), rate, 1);

    std::unique_ptr<TimelineTrack> old;
    {
        const juce::ScopedLock sl (transportLock);
        old = std::move (takeTrack);
        takeTrack = std::move (track);
        takeTrack->seek (playhead.load(), currentRate.load());
        playChain.reset();
        takeLoaded = true;
    }
    lastTake = t;
    sendChangeMessage();
    return true;
}

void AudioEngine::clearTakePlayback()
{
    std::unique_ptr<TimelineTrack> old;
    {
        const juce::ScopedLock sl (transportLock);
        takeLoaded = false;
        old = std::move (takeTrack);
    }
    sendChangeMessage();
}

//==============================================================================
void AudioEngine::exportMix (const Take& t, const juce::File& dest,
                             std::function<void (float)> progress, std::function<void (bool, juce::String)> done)
{
    if (exporting.exchange (true)) return;
    const float vVol = voiceVol.load(), mVol = musicVol.load();
    const auto fxState = fx.toString();
    const int key = fx.scaleKey.load();
    const bool fxBypass = fx.bypass.load();

    exportPool->addJob ([this, t, dest, progress, done, vVol, mVol, fxState, key, fxBypass]
    {
        auto finish = [this, done] (bool ok, juce::String msg)
        {
            exporting = false;
            juce::MessageManager::callAsync ([done, ok, msg] { if (done) done (ok, msg); });
        };

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> vocalR (fm.createReaderFor (t.vocal));
        if (vocalR == nullptr) { finish (false, juce::String::fromUTF8 ("রেকর্ডিং ফাইল পাওয়া যাচ্ছে না।")); return; }

        const double sr = vocalR->sampleRate;
        const auto total = vocalR->lengthInSamples - t.vocalOffset;
        if (total <= 0) { finish (false, juce::String::fromUTF8 ("রেকর্ডিং খুব ছোট।")); return; }

        const auto backingAudio = playableAudioFor (t.backing, fm);
        std::unique_ptr<juce::AudioFormatReader> songR (backingAudio.existsAsFile() ? fm.createReaderFor (backingAudio) : nullptr);
        std::unique_ptr<juce::AudioFormatReaderSource> songSrc;
        std::unique_ptr<juce::ResamplingAudioSource> resampler;
        constexpr int block = 4096;
        if (songR != nullptr)
        {
            songSrc = std::make_unique<juce::AudioFormatReaderSource> (songR.get(), false);
            songSrc->setNextReadPosition ((juce::int64) std::llround (t.startSeconds * songR->sampleRate));
            resampler = std::make_unique<juce::ResamplingAudioSource> (songSrc.get(), false, 2);
            resampler->setResamplingRatio (songR->sampleRate / sr);
            resampler->prepareToPlay (block, sr);
        }

        dest.deleteFile();
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (dest);
        if (! static_cast<juce::FileOutputStream*> (os.get())->openedOk()) { finish (false, juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না।")); return; }
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr) { finish (false, juce::String::fromUTF8 ("WAV ফাইল তৈরি করা যাচ্ছে না।")); return; }

        SafetyLimiter lim;
        lim.prepare (sr, juce::Decibels::decibelsToGain (-1.0f), 120.0f);

        // the same vocal effects you hear, rendered offline at full quality
        FxParams p;
        p.fromString (fxState);
        p.scaleKey = key;
        p.bypass = fxBypass;
        VocalChain chain (p);
        chain.prepare (sr, block, true);
        const int chainLat = chain.latencySamples (false);

        juce::AudioBuffer<float> mix (2, block), voc (1, block), fxv (2, block);
        // prime the chain so its delay is absorbed before the first output sample
        for (int primed = 0; primed < chainLat; )
        {
            const int n = juce::jmin (block, chainLat - primed);
            voc.clear();
            vocalR->read (&voc, 0, n, t.vocalOffset + primed, true, false);
            chain.process (voc.getReadPointer (0), fxv.getWritePointer (0), fxv.getWritePointer (1), n, false);
            primed += n;
        }
        int lastPct = -1;
        for (juce::int64 pos = 0; pos < total; pos += block)
        {
            const int n = (int) juce::jmin ((juce::int64) block, total - pos);
            voc.clear();
            vocalR->read (&voc, 0, n, t.vocalOffset + chainLat + pos, true, false);
            chain.process (voc.getReadPointer (0), fxv.getWritePointer (0), fxv.getWritePointer (1), n, false);
            mix.clear();
            if (resampler != nullptr)
            {
                juce::AudioSourceChannelInfo info (&mix, 0, n);
                resampler->getNextAudioBlock (info);
                mix.applyGain (0, n, mVol);
            }
            mix.addFrom (0, 0, fxv, 0, 0, n, vVol);
            mix.addFrom (1, 0, fxv, 1, 0, n, vVol);
            lim.process (mix.getWritePointer (0), mix.getWritePointer (1), n);
            writer->writeFromAudioSampleBuffer (mix, 0, n);

            auto pct = (int) (100 * (pos + n) / total);
            if (pct != lastPct && progress != nullptr)
            {
                lastPct = pct;
                juce::MessageManager::callAsync ([progress, pct] { progress ((float) pct / 100.0f); });
            }
        }
        writer.reset();
        finish (true, dest.getFullPathName());
    });
}

//==============================================================================
int AudioEngine::getRoundTripLatencySamples() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        auto l = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
        return l > 0 ? l : 2 * d->getCurrentBufferSizeSamples();
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
    return d != nullptr ? d->getXRunCount() : 0;
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    const double oldRate = currentRate.load();

    const juce::ScopedLock sl (transportLock);
    if (oldRate > 0 && std::abs (oldRate - sr) > 0.5)
        playhead = (juce::int64) std::llround ((double) playhead.load() * sr / oldRate);   // keep the same song position

    currentRate = sr;
    currentBlock = bs;
    maxBlock = juce::jmax (bs, 1024);
    maxBlockShared = maxBlock;

    micBuf.setSize (1, maxBlock);
    nsBuf.setSize (1, maxBlock);
    songBuf.setSize (2, maxBlock);
    takeBuf.setSize (1, maxBlock);
    fxBuf.setSize (2, maxBlock);
    takeFxBuf.setSize (2, maxBlock);
    monitorChain.prepare (sr, maxBlock, false);
    playChain.prepare (sr, maxBlock, true);

    for (auto* t : { songTrack.get(), takeTrack.get() })
        if (t != nullptr) { t->prepare (sr, maxBlock); t->seek (playhead.load(), sr); }

    highPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 75.0f, 0.707f);
    highPass.reset();
    limiter.prepare (sr, juce::Decibels::decibelsToGain (-0.3f));
    denoiser.reset();

    for (auto* s : { &gainSm, &voiceSm, &musicSm, &monitorSm })
        s->reset (sr, 0.03);
    gainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));
    voiceSm.setCurrentAndTargetValue (voiceVol.load());
    musicSm.setCurrentAndTargetValue (musicVol.load());
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
    int active = 0;
    for (int ch = 0; ch < numIn; ++ch)
        if (in[ch] != nullptr) { juce::FloatVectorOperations::add (mic, in[ch] + start, n); ++active; }
    if (active > 1) juce::FloatVectorOperations::multiply (mic, 1.0f / (float) active, n);

    gainSm.setTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        auto v = highPass.processSample (mic[i] * gainSm.getNextValue());
        mic[i] = v;
        peak = juce::jmax (peak, std::abs (v));
    }
    if (peak > inPeak.load (std::memory_order_relaxed)) inPeak.store (peak, std::memory_order_relaxed);

    // ---- 2. AI noise removal (RNNoise, 48 kHz)
    const bool isRec = recording.load (std::memory_order_acquire);
    const bool nsLive = noiseOn.load() && noiseAvailable();
    const bool runNs = nsLive || (isRec && recordWithNoise);
    const float* recVocal = mic;
    const float* monVocal = mic;
    if (runNs)
    {
        auto* ns = nsBuf.getWritePointer (0);
        juce::FloatVectorOperations::copy (ns, mic, n);
        denoiser.process (ns, n, noiseAmount.load());
        if (isRec && recordWithNoise) recVocal = ns;
        if (nsLive && noiseInMonitor.load()) monVocal = ns;
    }

    // ---- 3. song + take, read in lock-step from the single playhead
    double timelinePos = 0.0;
    bool takeActive = false;
    {
        const juce::ScopedLock sl (transportLock);
        const double sr = currentRate.load (std::memory_order_relaxed);
        const auto ph = playhead.load (std::memory_order_relaxed);
        timelinePos = (double) ph / sr;
        if (playing.load (std::memory_order_relaxed))
        {
            const auto after = ph + n;
            if (songTrack != nullptr) songTrack->read (songBuf, n, after, sr); else songBuf.clear (0, n);
            takeActive = takeTrack != nullptr;
            if (takeActive) takeTrack->read (takeBuf, n, after, sr); else takeBuf.clear (0, n);
            playhead.store (after, std::memory_order_relaxed);
            if (songTrack != nullptr && (double) after >= songLength * sr)
            {
                playing.store (false);
                endReached.store (true);
            }
        }
        else
        {
            songBuf.clear (0, n);
            takeBuf.clear (0, n);
        }
    }

    // ---- 4. recording (lock-free threaded writers)
    if (isRec)
    {
        const juce::ScopedLock wl (writerLock);
        if (recording.load() && vocalWriter != nullptr && rawWriter != nullptr)
        {
            if (recordFirstBlock.exchange (false))
                recordStartSeconds = timelinePos;
            const float* v[] = { recVocal };
            const float* r[] = { mic };
            vocalWriter->write (v, n);
            rawWriter->write (r, n);
            vocalPeaks.push (timelinePos, peak);
            recordedSamples += n;
        }
    }

    // ---- 5. effects + mix to output with safety limiter
    voiceSm.setTargetValue (voiceVol.load());
    musicSm.setTargetValue (musicVol.load());
    monitorSm.setTargetValue (monitorOn.load() && ! takeActive ? 1.0f : 0.0f);

    // live voice through the effect chain (only while the live mic is on or fading out)
    float* vL = fxBuf.getWritePointer (0);
    float* vR = fxBuf.getWritePointer (1);
    const bool monitorAudible = monitorSm.getTargetValue() > 0.0f || monitorSm.isSmoothing() || monitorSm.getCurrentValue() > 0.0f;
    if (monitorAudible)
        monitorChain.process (monVocal, vL, vR, n, true);
    else
    {
        juce::FloatVectorOperations::clear (vL, n);
        juce::FloatVectorOperations::clear (vR, n);
    }

    // recorded take through the playback chain (same settings, non-destructive)
    float* tL = takeFxBuf.getWritePointer (0);
    float* tR = takeFxBuf.getWritePointer (1);
    if (takeActive)
        playChain.process (takeBuf.getReadPointer (0), tL, tR, n, false);
    else
    {
        juce::FloatVectorOperations::clear (tL, n);
        juce::FloatVectorOperations::clear (tR, n);
    }

    const float* s0 = songBuf.getReadPointer (0);
    const float* s1 = songBuf.getReadPointer (1);

    float* L = numOut > 0 ? out[0] : nullptr;
    float* R = numOut > 1 ? out[1] : nullptr;
    for (int ch = 2; ch < numOut; ++ch)
        if (out[ch] != nullptr) juce::FloatVectorOperations::clear (out[ch] + start, n);
    if (L == nullptr) return;

    for (int i = 0; i < n; ++i)
    {
        const float voice = voiceSm.getNextValue();
        const float music = musicSm.getNextValue();
        const float mon = monitorSm.getNextValue();
        const float l = s0[i] * music + (vL[i] * mon + tL[i]) * voice;
        const float r = s1[i] * music + (vR[i] * mon + tR[i]) * voice;
        if (R != nullptr) { L[start + i] = l; R[start + i] = r; }
        else              { L[start + i] = 0.5f * (l + r); }
    }

    limiter.process (L + start, R != nullptr ? R + start : nullptr, n);

    float op = juce::FloatVectorOperations::findMaximum (L + start, n);
    op = juce::jmax (op, -juce::FloatVectorOperations::findMinimum (L + start, n));
    if (op > outPeak.load (std::memory_order_relaxed)) outPeak.store (op, std::memory_order_relaxed);
}
}
