#include <JuceHeader.h>
#include "../Source/Metronome.h"

// Same vectors as velody/src/renderer/src/__tests__/downbeat.test.ts, so both apps click alike.
struct MetronomeTests : UnitTest
{
    MetronomeTests() : UnitTest ("Metronome") {}

    static std::vector<double> gaps (const std::vector<double>& t)
    {
        std::vector<double> g;
        for (size_t i = 1; i < t.size(); ++i) g.push_back (t[i] - t[i - 1]);
        return g;
    }

    static double spread (const std::vector<double>& xs, size_t from, size_t to)
    {
        auto [lo, hi] = std::minmax_element (xs.begin() + (long) from, xs.begin() + (long) to);
        return *hi - *lo;
    }

    void runTest() override
    {
        using namespace Metronome;

        beginTest ("accents");
        expect (isGridAccent (0, 0) && ! isGridAccent (1, 0) && isGridAccent (4, 0));
        expect (isGridAccent (2, 2) && isGridAccent (-2, 2) && ! isGridAccent (-2, 0) && isGridAccent (-4, 0));
        expect (isConstantAccent (1, 0) && isConstantAccent (5, 0) && ! isConstantAccent (2, 0));
        expect (isConstantAccent (2, 1));

        beginTest ("medianBeatGap");
        expectWithinAbsoluteError (medianBeatGap ({ 0, 0.5, 1.0, 1.7, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5 }, 0.6), 0.5, 1e-9);
        expectWithinAbsoluteError (medianBeatGap ({ 0, 0.75, 1.5, 2.25, 2.97, 3.69, 4.41, 5.13, 5.85, 6.57, 7.29, 8.01 }, 0.6), 0.72, 1e-9);
        expectEquals (medianBeatGap ({ 5 }, 0.6), 0.6);

        beginTest ("smoothBeatGrid removes frame jitter");
        const double pattern[] = { 0.372, 0.395, 0.372, 0.395, 0.372, 0.395, 0.395, 0.418, 0.372, 0.395, 0.372, 0.395 };
        std::vector<double> raw { 0.9288 };
        for (int k = 0; k < 48; ++k) raw.push_back (raw.back() + pattern[k % 12]);
        auto smooth = smoothBeatGrid (raw);
        expectEquals ((int) smooth.size(), (int) raw.size());
        expectGreaterThan (spread (gaps (raw), 0, raw.size() - 1), 0.04);
        auto sg = gaps (smooth);
        expectLessThan (spread (sg, 4, sg.size() - 4), 0.012);

        beginTest ("smoothBeatGrid keeps a gradual tempo change and leaves tiny grids alone");
        std::vector<double> truth { 0 };
        for (int i = 1; i < 40; ++i) truth.push_back (truth.back() + 0.5 + 0.06 * i / 40);
        auto g = gaps (smoothBeatGrid (truth));
        expectWithinAbsoluteError (g[5], 0.5 + 0.06 * 6 / 40, 0.005);
        expectWithinAbsoluteError (g[33], 0.5 + 0.06 * 34 / 40, 0.005);
        expect (smoothBeatGrid ({ 1, 1.5 }) == std::vector<double> { 1, 1.5 });

        beginTest ("clicks follow the grid, extrapolate before/after it and accent with the shift");
        SongMeta m;
        m.bpm = 100;
        m.beatTimes = { 1.0, 1.5, 2.0, 2.5 };
        m.downbeatShift = 1;
        auto cs = clicks (m, 3.2);
        const double want[] = { 0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0 };
        expectEquals ((int) cs.size(), 7);
        for (size_t i = 0; i < cs.size() && i < 7; ++i)
        {
            expectWithinAbsoluteError (cs[i].time, want[i], 1e-9);
            expect (cs[i].accent == (i == 3)); // only 1.5 s
        }

        beginTest ("constant tempo from the offset without a grid");
        SongMeta c;
        c.bpm = 60;
        c.firstBeatOffset = 0.25;
        cs = clicks (c, 4.5);
        expectEquals ((int) cs.size(), 4);
        expectWithinAbsoluteError (cs[0].time, 1.25, 1e-9);
        expectWithinAbsoluteError (cs[3].time, 4.25, 1e-9);
        expect (cs[0].accent && ! cs[1].accent);

        beginTest ("no BPM (song not analyzed) means no clicks");
        expect (clicks (SongMeta(), 10).empty());

        beginTest ("readMeta: manual downbeatShift wins over detected, missing file is empty");
        auto dir = File::createTempFile ("velody-meta");
        dir.createDirectory();
        auto sidecar = dir.getChildFile (".velody");
        sidecar.replaceWithText (R"({"bpm":120.5,"firstBeatOffset":0.14,"beatTimes":[0.14,0.64],"detectedDownbeatShift":2,"downbeatShift":-1})");
        auto meta = readMeta (dir);
        expectEquals (meta.bpm, 120.5);
        expectEquals ((int) meta.beatTimes.size(), 2);
        expectEquals (meta.downbeatShift, 3); // -1 normalized
        sidecar.replaceWithText (R"({"bpm":90,"detectedDownbeatShift":2,"downbeatShift":null})");
        expectEquals (readMeta (dir).downbeatShift, 2);
        sidecar.deleteFile();
        expectEquals (readMeta (dir).bpm, 0.0);

        beginTest ("renderWav puts a click at each beat and silence between");
        auto wav = dir.getChildFile ("click.wav");
        expect (renderWav ({ { 0.5, true }, { 1.0, false } }, 1.5, wav));
        WavAudioFormat fmt;
        std::unique_ptr<AudioFormatReader> r (fmt.createReaderFor (wav.createInputStream().release(), true));
        expect (r != nullptr);
        if (r != nullptr)
        {
            expectEquals ((int) r->lengthInSamples, 66150);
            AudioBuffer<float> b (1, (int) r->lengthInSamples);
            r->read (&b, 0, b.getNumSamples(), 0, true, false);
            expectLessThan (b.getMagnitude (0, 0, 22000), 0.001f);         // before the first click
            expectGreaterThan (b.getMagnitude (0, 22050, 1323), 0.3f);     // accented click at 0.5 s
            expectLessThan (b.getMagnitude (0, 23500, 20000), 0.001f);     // gap
            expectGreaterThan (b.getMagnitude (0, 44100, 1323), 0.2f);     // click at 1.0 s
        }
        dir.deleteRecursively();
    }
};

static MetronomeTests metronomeTests;
