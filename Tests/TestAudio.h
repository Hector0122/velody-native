#pragma once
#include <JuceHeader.h>

// Shared by the render tests: a sine WAV on disk, and length + dominant frequency of a rendered file.
namespace TestAudio
{
    inline void writeSine (const File& f, double seconds, double freq)
    {
        f.getParentDirectory().createDirectory();
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (f), 44100, 1, 16, {}, 0));
        AudioBuffer<float> b (1, (int) (44100 * seconds));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * (float) std::sin (MathConstants<double>::twoPi * freq * i / 44100.0));
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    inline std::pair<double, double> analyse (const File& f)
    {
        AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) return { 0, 0 };
        AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, b.getNumSamples(), 0, true, true);
        const int from = b.getNumSamples() / 4, to = 3 * b.getNumSamples() / 4;
        int crossings = 0;
        for (int i = from + 1; i < to; ++i)
            if ((b.getSample (0, i - 1) < 0) != (b.getSample (0, i) < 0)) ++crossings;
        return { r->lengthInSamples / r->sampleRate, crossings / 2.0 / ((to - from) / r->sampleRate) };
    }
}
