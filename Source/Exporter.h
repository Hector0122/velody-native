#pragma once
#include <JuceHeader.h>
#include "SongEdit.h"
#include "PythonWorker.h"

namespace te = tracktion;

// Exports what you hear: Tracktion renders the edit offline, so volume, pan, mute/solo, plugins,
// speed/pitch and the metronome (only if its track isn't muted) end up in the file, like playback.
// One render at a time; MP3 is the WAV converted with ffmpeg.
class Exporter
{
public:
    enum class Format { wav, mp3 };
    struct Item { BigInteger tracks; File out; }; // tracks = indices into te::getAllTracks(), or empty for the whole mix

    using Progress = std::function<void (int done, int total)>;
    using Done = std::function<void (const String& error)>; // empty = every file written

    Exporter (te::Edit& e, Array<Item> list, Format f, Progress p, Done d)
        : edit (e), items (std::move (list)), format (f), onProgress (std::move (p)), onDone (std::move (d)) {}

    ~Exporter() { *alive = false; } // late callbacks become no-ops; the handle cancels the render

    void start() { next(); }

    // The song's tracks worth exporting on their own: audio tracks with clips, minus the click.
    static Array<te::AudioTrack*> exportableTracks (te::Edit& e)
    {
        Array<te::AudioTrack*> out;
        for (auto t : te::getAudioTracks (e))
            if (t->getName() != SongEdit::clickTrackName && ! t->getClips().isEmpty())
                out.add (t);
        return out;
    }

    static Item stemItem (te::Edit& e, te::AudioTrack& t, const File& out)
    {
        Item it { {}, out };
        it.tracks.setBit (te::getAllTracks (e).indexOf (&t));
        return it;
    }

    static StringArray mp3Args (const File& wav, const File& mp3)
    {
        return { PythonWorker::ffmpegPath().getFullPathName(), "-y", "-loglevel", "error", "-i", wav.getFullPathName(),
                 "-c:a", "libmp3lame", "-q:a", "2", mp3.getFullPathName() };
    }

private:
    te::Edit& edit;
    Array<Item> items;
    Format format;
    Progress onProgress;
    Done onDone;
    int index = 0;
    std::shared_ptr<te::EditRenderer::Handle> handle;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    void next()
    {
        if (index >= items.size())
            return onDone ({});

        onProgress (index, items.size());
        const auto item = items[index];
        const auto wav = format == Format::wav ? item.out
                                               : File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("velody-export", ".wav");

        // The save dialog already confirmed overwriting; Tracktion won't replace an existing file.
        item.out.deleteFile();

        te::Renderer::Parameters p (edit);
        p.destFile = wav;
        p.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
        p.bitDepth = format == Format::wav ? 24 : 16;
        p.time = { te::TimePosition(), te::TimePosition::fromSeconds (SongEdit::contentLength (edit)) };
        if (! item.tracks.isZero())
            p.tracksToDo = item.tracks;

        // The callback runs on the render thread: hop to the message thread before starting the next
        // render (replacing `handle` from its own thread would join that thread with itself).
        handle = te::EditRenderer::render (std::move (p), [this, a = alive, wav, out = item.out] (auto result)
        {
            const String error = result ? String() : String (result.error());
            MessageManager::callAsync ([this, a, wav, out, error] { if (*a) rendered (wav, out, error); });
        });
    }

    void rendered (const File& wav, const File& out, const String& error)
    {
        if (error.isNotEmpty())
            return onDone (error);
        if (format == Format::wav)
            return advance();

        // ffmpeg off the message thread; back here when it's done.
        Thread::launch ([this, a = alive, wav, out]
        {
            auto ran = PythonWorker::drain (mp3Args (wav, out), [] (const String&) {}, [] { return false; }, 5 * 60 * 1000);
            wav.deleteFile();
            const bool ok = ran.started && ran.code == 0 && out.existsAsFile();
            MessageManager::callAsync ([this, a, ok, out]
            {
                if (! *a) return;
                if (ok) advance();
                else onDone ("No se pudo convertir a MP3: " + out.getFileName());
            });
        });
    }

    void advance()
    {
        ++index;
        next();
    }
};
