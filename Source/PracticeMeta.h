#pragma once
#include <JuceHeader.h>
#include "Metronome.h"
#include "SongMeta.h"

// A song's practice settings (speed, transpose, count-in) live in its .velody, like Electron's
// useSongSpeedPitch / useSongCountIn, so they travel with the cloud and show up on the other laptop.
// Before this, Velody kept them only in the song's local edit: those values are moved into the
// .velody the first time the song opens (unless the .velody already has its own).
namespace PracticeMeta
{
    static inline const Identifier countInProp { "velodyCountInBars" }, // the old, edit-only copies
                                   speedProp { "velodySpeed" }, pitchProp { "velodySemitones" };

    struct Values
    {
        double speed = 1;
        int semitones = 0;
        int countInBars = 0;
    };

    // What the song opens with: .velody first, else the edit's old value (moved into .velody now),
    // else the defaults.
    inline Values load (const File& songDir, const ValueTree& editState)
    {
        const auto meta = SongMeta::read (songDir);
        Values v;
        NamedValueSet moved;

        auto pick = [&] (const char* key, const Identifier& prop) -> var
        {
            if (meta.hasProperty (key))
                return meta[key];
            if (editState.hasProperty (prop))
            {
                moved.set (key, editState[prop]);
                return editState[prop];
            }
            return {};
        };

        if (auto s = pick ("practiceSpeed", speedProp); ! s.isVoid())             v.speed = Metronome::clampSpeed (s);
        if (auto t = pick ("transposeSemitones", pitchProp); ! t.isVoid())        v.semitones = Metronome::clampSemitones (t);
        if (auto c = pick ("playbackCountInBars", countInProp); ! c.isVoid())     v.countInBars = jlimit (0, 8, (int) c);

        if (! moved.isEmpty())
            SongMeta::update (songDir, moved); // a real edit of the user's: stamped, so it syncs
        return v;
    }

    // Writes only the keys whose value changed (each write stamps updatedAt, which the cloud sync
    // compares), so moving the speed never rewrites a count-in this screen can't show.
    inline bool saveChanged (const File& songDir, const NamedValueSet& values)
    {
        const auto meta = SongMeta::read (songDir);
        NamedValueSet patch;
        for (auto& kv : values)
            if (! meta.hasProperty (kv.name) || meta[kv.name] != kv.value)
                patch.set (kv.name, kv.value);
        return patch.isEmpty() || SongMeta::update (songDir, patch);
    }

    inline bool saveSpeedPitch (const File& songDir, double speed, int semitones)
    {
        NamedValueSet v;
        v.set ("practiceSpeed", Metronome::clampSpeed (speed));
        v.set ("transposeSemitones", Metronome::clampSemitones (semitones));
        return saveChanged (songDir, v);
    }

    inline bool saveCountIn (const File& songDir, int bars)
    {
        NamedValueSet v;
        v.set ("playbackCountInBars", jlimit (0, 8, bars));
        return saveChanged (songDir, v);
    }
}
