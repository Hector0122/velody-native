#pragma once
#include <JuceHeader.h>
#include "Metronome.h"

// Count-in before recording (Electron's recording pre-roll, useRecording + useCountIn): N bars of clicks
// on the song's own grid (the playback count-in's planCountIn), then the take starts exactly at the
// cursor. Velody plays from the first click with the song silenced (CountInPlugin) and records with
// punch-in at the cursor, so the take is aligned and doesn't contain the count-in.
namespace RecordCountIn
{
    inline const Array<int> barsOptions { 0, 1, 2, 4 }; // like Electron's OFF / 1 / 2 / 4
    inline const StringArray names { "Grabar sin count-in", String (u8"Grabar: count-in 1 compás"),
                                     "Grabar: count-in 2 compases", "Grabar: count-in 4 compases" };
    constexpr int defaultBars = 1; // Electron's default

    inline int clampBars (int bars) { return barsOptions.contains (bars) ? bars : defaultBars; }

    struct Plan
    {
        std::vector<Metronome::Click> clicks; // edit time; empty = record right away
        double punchIn = 0;                   // edit time where the take starts (the cursor)
        bool hasCountIn() const { return ! clicks.empty(); }
    };

    // `cursor` in edit time; the edit runs at song time / speed.
    inline Plan plan (const Metronome::SongMeta& meta, double cursor, double speed, int bars)
    {
        Plan p;
        p.punchIn = cursor;
        if (bars <= 0 || ! (meta.bpm > 0) || speed <= 0)
            return p;
        p.clicks = Metronome::planCountIn (meta, cursor * speed, bars);
        for (auto& c : p.clicks)
            c.time /= speed;
        return p;
    }
}
