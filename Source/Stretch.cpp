#include "Stretch.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <rubberband/RubberBandStretcher.h>

namespace tf
{
juce::File tempoCacheFor (const juce::File& audio, double speed, int semitones)
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("TF Vocal Studio").getChildFile ("TempoCache");
    dir.createDirectory();
    auto key = audio.getFullPathName() + "|" + juce::String (audio.getSize()) + "|" + juce::String (speed, 3) + "|" + juce::String (semitones);
    return dir.getChildFile (juce::File::createLegalFileName (audio.getFileNameWithoutExtension()).substring (0, 40)
                             + "-x" + juce::String (speed, 2) + (semitones != 0 ? "-k" + juce::String (semitones) : juce::String())
                             + "-" + juce::String::toHexString (key.hashCode64()) + ".wav");
}

juce::String renderTempo (const juce::File& in, const juce::File& out, double speed, int semitones,
                          std::function<void (float)> progress, std::atomic<bool>& cancel)
{
    using RB = RubberBand::RubberBandStretcher;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (in));
    if (r == nullptr) return juce::String::fromUTF8 ("গানের ফাইল পড়া যাচ্ছে না।");
    const int chans = (int) juce::jlimit (1u, 2u, r->numChannels);
    const auto total = r->lengthInSamples;
    const auto rate = r->sampleRate;

    RB rb ((size_t) rate, (size_t) chans,
           RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionChannelsTogether | RB::OptionPitchHighQuality);
    rb.setTimeRatio (1.0 / speed);
    rb.setPitchScale (std::pow (2.0, semitones / 12.0));
    rb.setExpectedInputDuration ((size_t) total);

    constexpr int block = 8192;
    juce::AudioBuffer<float> buf (chans, block);

    // pass 1: study
    for (juce::int64 pos = 0; pos < total; pos += block)
    {
        if (cancel.load()) return juce::String::fromUTF8 ("বাতিল করা হয়েছে।");
        const int n = (int) juce::jmin ((juce::int64) block, total - pos);
        r->read (&buf, 0, n, pos, true, chans > 1);
        rb.study (buf.getArrayOfReadPointers(), (size_t) n, pos + n >= total);
        if (progress) progress (0.25f * (float) (pos + n) / (float) total);
    }

    auto tmp = out.getSiblingFile (out.getFileNameWithoutExtension() + ".part.wav");
    tmp.deleteFile();
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (tmp);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (chans).withBitsPerSample (24));
    if (w == nullptr) return juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না।");

    juce::AudioBuffer<float> outBuf (chans, block);
    auto drain = [&]
    {
        while (true)
        {
            const int avail = rb.available();
            if (avail <= 0) break;
            const int m = juce::jmin (avail, block);
            rb.retrieve (outBuf.getArrayOfWritePointers(), (size_t) m);
            w->writeFromAudioSampleBuffer (outBuf, 0, m);
        }
    };

    // pass 2: process
    for (juce::int64 pos = 0; pos < total; pos += block)
    {
        if (cancel.load()) { w.reset(); tmp.deleteFile(); return juce::String::fromUTF8 ("বাতিল করা হয়েছে।"); }
        const int n = (int) juce::jmin ((juce::int64) block, total - pos);
        r->read (&buf, 0, n, pos, true, chans > 1);
        rb.process (buf.getArrayOfReadPointers(), (size_t) n, pos + n >= total);
        drain();
        if (progress) progress (0.25f + 0.75f * (float) (pos + n) / (float) total);
    }
    drain();
    w.reset();
    out.deleteFile();
    if (! tmp.moveFileTo (out)) return juce::String::fromUTF8 ("ফাইল সেভ করা যাচ্ছে না।");
    if (progress) progress (1.0f);
    return {};
}
}
