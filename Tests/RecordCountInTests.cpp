#include <JuceHeader.h>
#include "../Source/RecordCountIn.h"

// Count-in before recording: when there is one, how many clicks, where they fall (on the song's grid,
// before the cursor) and where the take starts.
struct RecordCountInTests : UnitTest
{
    RecordCountInTests() : UnitTest ("Record count-in") {}

    void runTest() override
    {
        using namespace RecordCountIn;

        beginTest ("options like Electron (off / 1 / 2 / 4, default 1); anything else falls back to 1");
        expectEquals (names.size(), barsOptions.size());
        expectEquals (clampBars (4), 4);
        expectEquals (clampBars (3), 1);
        expectEquals (clampBars (0), 0);

        Metronome::SongMeta meta;
        meta.bpm = 120;          // 0.5 s per beat
        meta.firstBeatOffset = 0.25;

        beginTest ("no count-in when it's off or the song has no tempo: record right at the cursor");
        auto p = plan (meta, 10.25, 1.0, 0);
        expect (! p.hasCountIn() && p.punchIn == 10.25);
        Metronome::SongMeta noTempo;
        expect (! plan (noTempo, 10.0, 1.0, 2).hasCountIn());

        beginTest ("2 bars: 8 clicks on the grid ending one beat before the cursor's beat; the take starts at the cursor");
        p = plan (meta, 10.25, 1.0, 2);
        expectEquals ((int) p.clicks.size(), 8);
        expectWithinAbsoluteError (p.clicks.front().time, 6.25, 1e-9);
        expectWithinAbsoluteError (p.clicks.back().time, 9.75, 1e-9);
        expect (p.clicks[0].accent && ! p.clicks[1].accent && p.clicks[4].accent);
        expectEquals (p.punchIn, 10.25);
        for (auto& c : p.clicks)
            expectLessThan (c.time, p.punchIn);

        beginTest ("on the real beat grid when the song has one");
        meta.beatTimes = { 1.0, 1.6, 2.2, 2.8, 3.4, 4.0, 4.6, 5.2 };
        p = plan (meta, 4.0, 1.0, 1);
        expectEquals ((int) p.clicks.size(), 4);
        expectWithinAbsoluteError (p.clicks.front().time, 1.6, 1e-6);
        expectWithinAbsoluteError (p.clicks.back().time, 3.4, 1e-6);

        beginTest ("4 bars, and edit time follows the speed");
        meta.beatTimes.clear();
        p = plan (meta, 10.25, 1.0, 4);
        expectEquals ((int) p.clicks.size(), 16);
        auto slow = plan (meta, 20.5, 0.5, 1); // edit 20.5 s at 50 % = song 10.25 s
        expectWithinAbsoluteError (slow.clicks.back().time, 19.5, 1e-9); // song 9.75 s
        expectEquals (slow.punchIn, 20.5);
    }
};

static RecordCountInTests recordCountInTests;
