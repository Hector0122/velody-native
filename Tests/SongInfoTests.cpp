#include <JuceHeader.h>
#include "../Source/SongInfo.h"

struct SongInfoTests : UnitTest
{
    SongInfoTests() : UnitTest ("SongInfo (music start, chords)") {}

    static std::pair<AudioBuffer<float>, double> buf (std::vector<std::vector<float>> chans, double sr = 1000)
    {
        size_t n = 0;
        for (auto& c : chans) n = std::max (n, c.size());
        AudioBuffer<float> b ((int) chans.size(), (int) n);
        b.clear();
        for (int ch = 0; ch < (int) chans.size(); ++ch)
            for (size_t i = 0; i < chans[(size_t) ch].size(); ++i)
                b.setSample (ch, (int) i, chans[(size_t) ch][i]);
        return { std::move (b), sr };
    }

    static std::vector<float> at (size_t silence, float v) { std::vector<float> x (silence, 0.0f); x.push_back (v); x.resize (silence + 11); return x; }

    void runTest() override
    {
        using namespace SongInfo;

        beginTest ("findMusicStart: earliest sound across stems and channels, minus the pre-roll");
        std::vector<std::pair<AudioBuffer<float>, double>> stems;
        stems.push_back (buf ({ at (2000, 0.5f) }));                          // drums at 2.0 s
        stems.push_back (buf ({ std::vector<float> (3000, 0.0f), at (1500, 0.2f) })); // bass at 1.5 s, channel 1
        expectWithinAbsoluteError (findMusicStart (stems), 1.5 - 0.03, 1e-5);

        beginTest ("findMusicStart: ignores noise below the threshold");
        std::vector<float> quiet (500, 0.001f);
        quiet.resize (1000, 0.0f);
        quiet.push_back (0.3f);
        stems.clear();
        stems.push_back (buf ({ quiet }));
        expectWithinAbsoluteError (findMusicStart (stems), 1.0 - 0.03, 1e-5);

        beginTest ("findMusicStart: 0 when it starts right away or is silent in the scan window");
        stems.clear(); stems.push_back (buf ({ { 0.5f, 0, 0 } }));
        expectEquals (findMusicStart (stems), 0.0);
        stems.clear(); stems.push_back (buf ({ std::vector<float> (100, 0.0f) }));
        expectEquals (findMusicStart (stems), 0.0);
        stems.clear(); stems.push_back (buf ({ at (5000, 0.5f) }));
        expectEquals (findMusicStart (stems, 0.008f, 2), 0.0);

        beginTest ("findMusicStart from audio files on disk");
        auto dir = File::createTempFile ("velody-start");
        dir.createDirectory();
        auto f = dir.getChildFile ("drums.wav");
        {
            WavAudioFormat wav;
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (f), 44100, 1, 16, {}, 0));
            AudioBuffer<float> b (1, 44100 * 3);
            b.clear();
            for (int i = (int) (44100 * 1.5); i < b.getNumSamples(); ++i)
                b.setSample (0, i, 0.5f * (float) std::sin (i * 0.06));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        AudioFormatManager fm;
        fm.registerBasicFormats();
        expectWithinAbsoluteError (findMusicStart (Array<File> { f }, fm), 1.5 - 0.03, 0.01);

        beginTest ("chordIndexAt: chord sounding at a time, including exactly on a change");
        std::vector<Metronome::Chord> chords { { 1, "Em" }, { 3, "D" }, { 5, "G" } };
        expectEquals (chordIndexAt (chords, 0.5), -1);
        expectEquals (chordIndexAt (chords, 1), 0);
        expectEquals (chordIndexAt (chords, 4.9), 1);
        expectEquals (chordIndexAt (chords, 99), 2);
        expectEquals (chordIndexAt ({}, 2), -1);

        beginTest ("chordLabel: no chord shows a dash");
        std::vector<Metronome::Chord> withN { { 0, "N" }, { 2, "A" } };
        expectEquals (chordLabel (withN, 0), String (u8"–"));
        expectEquals (chordLabel (withN, 1), String ("A"));
        expectEquals (chordLabel (withN, 2), String (u8"–"));
        expectEquals (chordLabel (withN, -1), String (u8"–"));

        beginTest ("readMeta loads the chords sorted and skips malformed ones");
        dir.getChildFile (".velody").replaceWithText (R"({"bpm":100,"chords":[{"time":3,"chord":"D"},{"time":1,"chord":"Em"},{"time":"x","chord":"G"}]})");
        auto meta = Metronome::readMeta (dir);
        expectEquals ((int) meta.chords.size(), 2);
        expectEquals (meta.chords[0].chord, String ("Em"));

        beginTest (String (u8"ChordDisplay follows the song time (now → next)"));
        double t = 0;
        ChordDisplay d;
        d.songTime = [&] { return t; };
        d.setChords (meta.chords);
        expectEquals (d.getText(), String (u8"–  →  Em"));
        t = 1.2; d.update();
        expectEquals (d.getText(), String (u8"Em  →  D"));
        t = 3.5; d.update();
        expectEquals (d.getText(), String (u8"D  →  –"));
        d.setChords ({});
        expectEquals (d.getText(), String());

        dir.deleteRecursively();
    }
};

static SongInfoTests songInfoTests;
