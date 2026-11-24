#pragma once
#include <JuceHeader.h>
#include "Library.h"
#include "Metronome.h"

namespace te = tracktion;

namespace SongEdit
{
    inline const String clickTrackName { "Metrónomo" };

    // Tracks that belong to the song (stems + click), never armed for recording.
    inline bool isSongTrack (const String& name) { return Library::isStemName (name) || name == clickTrackName; }

    inline void insertWholeFile (te::AudioTrack& t, const File& f)
    {
        te::AudioFile audio (t.edit.engine, f);
        if (audio.isValid())
            t.insertWaveClip (t.getName(), f, { { {}, te::TimeDuration::fromSeconds (audio.getLength()) }, {} }, false);
    }

    // One track per stem, named after the file, with the whole file as a clip at 0 s.
    inline void addStemTracks (te::Edit& edit, const Array<File>& stems)
    {
        edit.ensureNumberOfAudioTracks (stems.size());

        for (int i = 0; i < stems.size(); ++i)
        {
            auto t = te::getAudioTracks (edit)[i];
            t->setName (stems[i].getFileNameWithoutExtension());
            insertWholeFile (*t, stems[i]);
        }
    }

    // Renders the song's metronome (from its .velody analysis) to `wav`, as long as its longest stem.
    // False when the song hasn't been analyzed yet (no BPM), so there's nothing to click.
    inline bool writeClickWav (te::Engine& engine, const Library::Song& song, const File& wav)
    {
        auto meta = Metronome::readMeta (song.dir);
        double duration = 0;
        for (auto& f : song.stems)
            duration = jmax (duration, te::AudioFile (engine, f).getLength());

        auto cs = Metronome::clicks (meta, duration);
        if (cs.empty() || ! Metronome::renderWav (cs, duration, wav))
            return false;

        engine.getAudioFileManager().forceFileUpdate (te::AudioFile (engine, wav)); // drop cached length/thumbnail
        return true;
    }

    // Puts the click WAV on a "Metrónomo" track right after the song tracks (replacing its clip if
    // it exists), so edits made before the metronome existed get one too.
    inline void setClickTrack (te::Edit& edit, const File& wav)
    {
        auto tracks = te::getAudioTracks (edit);
        te::AudioTrack* click = nullptr;
        te::AudioTrack* lastStem = nullptr;
        for (auto t : tracks)
        {
            if (t->getName() == clickTrackName) click = t;
            else if (Library::isStemName (t->getName())) lastStem = t;
        }

        if (click == nullptr)
        {
            auto t = edit.insertNewAudioTrack (lastStem != nullptr ? te::TrackInsertPoint (*lastStem, false)
                                                                   : te::TrackInsertPoint (nullptr, nullptr), nullptr);
            click = t.get();
            click->setName (clickTrackName);
        }

        auto old = click->getClips(); // copy: removing edits the live list
        for (auto c : old)
            c->removeFromParent();
        insertWholeFile (*click, wav);
    }

    // Input tracks go after the leading song tracks so a stem is never recorded over.
    inline int firstInputTrack (te::Edit& edit)
    {
        int i = 0;
        while (auto t = te::getAudioTracks (edit)[i])
        {
            if (! isSongTrack (t->getName()))
                break;
            ++i;
        }
        return i;
    }
}
