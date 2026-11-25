#pragma once
#include <JuceHeader.h>
#include "Library.h"
#include "Metronome.h"

namespace te = tracktion;

namespace SongEdit
{
    inline const String clickTrackName { String (u8"Metrónomo") };
    // Before the UTF-8 fix the name was built from a plain char literal, which JUCE reads as Latin-1;
    // edits saved back then hold this form, so it's recognised and renamed on open.
    inline const String legacyClickTrackName { CharPointer_ASCII ("Metr\xc3\xb3nomo") };

    // Opens `file` (or creates it empty) with a path resolver that matches how Tracktion writes
    // relative paths: getRelativePathFrom (editFile) gives "../x.wav" for a file beside the edit, but
    // Tracktion's default resolver reads it from the edit's sibling (one folder too high), so the
    // metronome WAV and recorded takes beside the edit went missing on reopen.
    inline std::unique_ptr<te::Edit> openOrCreate (te::Engine& engine, const File& file)
    {
        auto state = file.existsAsFile() ? te::loadEditFromFile (engine, file, te::ProjectItemID())
                                         : te::createEmptyEdit (engine);
        if (! state.isValid())
            return {};

        auto id = te::ProjectItemID::fromProperty (state, te::IDs::projectID);
        if (! id.isValid())
            id = te::ProjectItemID::createNewID (te::ProjectID());

        te::Edit::Options o { engine, state, id };
        o.editFileRetriever = [file] { return file; };
        o.filePathResolver = [file] (const String& path) -> File
        {
            if (File::isAbsolutePath (path))
                return File (path);
            auto f = file.getChildFile (path); // written relative to the edit file itself
            return f.existsAsFile() ? f : file.getSiblingFile (path);
        };
        return te::Edit::createEdit (std::move (o));
    }

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
    inline bool writeClickWav (te::Engine& engine, const Library::Song& song, const File& wav,
                               Metronome::Sound sound = Metronome::Sound::woodblock)
    {
        auto meta = Metronome::readMeta (song.dir);
        double duration = 0;
        for (auto& f : song.stems)
            duration = jmax (duration, te::AudioFile (engine, f).getLength());

        auto cs = Metronome::clicks (meta, duration);
        if (cs.empty() || ! Metronome::renderWav (cs, duration, wav, sound))
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
            if (t->getName() == legacyClickTrackName) t->setName (clickTrackName);
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

    // Practice speed/transpose on the song's own clips (stems + click), real-time Signalsmith stretch
    // so pitch is kept at any speed. The click keeps its pitch, like Electron's (it bypasses the shifter).
    // Takes on the input tracks are left alone. Edit time = song time / speed.
    inline void setPractice (te::Edit& edit, double speed, int semitones)
    {
        for (auto t : te::getAudioTracks (edit))
        {
            if (! isSongTrack (t->getName()))
                continue;

            for (auto c : t->getClips())
                if (auto wc = dynamic_cast<te::WaveAudioClip*> (c))
                {
                    const bool normal = speed == 1.0 && semitones == 0;
                    const double sourceLength = wc->getPosition().getLength().inSeconds() * wc->getSpeedRatio();
                    wc->setUsesProxy (false);
                    wc->setTimeStretchMode (normal ? te::TimeStretcher::disabled : te::TimeStretcher::signalsmithDefault);
                    wc->setSpeedRatio (speed);
                    wc->setLength (te::TimeDuration::fromSeconds (sourceLength / speed), true);
                    wc->setPitchChange (t->getName() == clickTrackName ? 0.0f : (float) semitones);
                }
        }
    }

    // End of the last clip, from the clips themselves: Edit::getLength() is updated asynchronously,
    // so right after a speed change it still reports the old length.
    inline double contentLength (te::Edit& edit)
    {
        double end = 0;
        for (auto t : te::getAudioTracks (edit))
            for (auto c : t->getClips())
                end = jmax (end, c->getPosition().getEnd().inSeconds());
        return end;
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

    // Mix Pan (Electron's "Mix Pan"): the whole mix to one side and the metronome, with its own pan in
    // the mixer, to the other, e.g. mix to the PA and click to the in-ears. Done with one extra
    // Volume & Pan (before the track's fader) on every track except the metronome, marked as Velody's.
    inline const Identifier mixPanProp { "velodyMixPan" };

    inline te::VolumeAndPanPlugin* mixPanPlugin (te::AudioTrack& t)
    {
        for (auto p : t.pluginList)
            if (auto v = dynamic_cast<te::VolumeAndPanPlugin*> (p); v != nullptr && (bool) v->state[mixPanProp])
                return v;
        return nullptr;
    }

    // Also call it after tracks are added (new input tracks get it too).
    inline void setMixPan (te::Edit& edit, float pan)
    {
        pan = jlimit (-1.0f, 1.0f, pan);
        for (auto t : te::getAudioTracks (edit))
        {
            auto* ours = mixPanPlugin (*t);
            if (t->getName() == clickTrackName || t->getName() == legacyClickTrackName)
            {
                if (ours != nullptr)
                    ours->deleteFromParent();
                continue;
            }
            if (ours == nullptr && pan == 0.0f)
                continue;
            if (ours == nullptr)
            {
                // Right before the track's own fader: Tracktion takes the *last* Volume & Pan as the
                // track's (getVolumePlugin), which the mixer row must keep controlling.
                int index = -1, i = 0;
                for (auto p : t->pluginList)
                {
                    if (p == t->getVolumePlugin())
                        index = i;
                    ++i;
                }
                auto p = edit.getPluginCache().createNewPlugin (te::VolumeAndPanPlugin::xmlTypeName, {});
                p->state.setProperty (mixPanProp, true, nullptr);
                t->pluginList.insertPlugin (p, index, nullptr);
                ours = mixPanPlugin (*t);
            }
            if (ours != nullptr)
                ours->setPan (pan);
        }
    }

    inline float mixPan (te::Edit& edit)
    {
        for (auto t : te::getAudioTracks (edit))
            if (auto* p = mixPanPlugin (*t))
                return p->getPan();
        return 0.0f;
    }

    // Position among the audio tracks (Tracktion's own index also counts its hidden tempo/marker tracks),
    // for the per-track colour.
    inline int audioTrackIndex (te::Track& t)
    {
        return te::getAudioTracks (t.edit).indexOf (dynamic_cast<te::AudioTrack*> (&t));
    }
}
