#pragma once
#include <JuceHeader.h>
#include "SongEdit.h"

namespace te = tracktion;

// "Quitar de esta laptop": the song's folder in the library plus its native project (edit + click WAV),
// and optionally the takes recorded in it. Everything goes to the system trash, never deleted outright.
// shortcut: local only; "borrar en todos lados" comes with cloud sync.
namespace SongDelete
{
    struct Plan
    {
        File songDir, editFile, clickWav;
        Array<File> takes; // audio recorded in this song's project
    };

    // Audio on the user's own tracks (not stems/click) that lives in the projects folder: the takes.
    // Files elsewhere (e.g. a WAV dragged in from ~/Music) are never touched.
    inline Array<File> takesIn (te::Edit& edit, const File& projectsDir)
    {
        Array<File> out;
        for (auto t : te::getAudioTracks (edit))
            if (! SongEdit::isSongTrack (t->getName()))
                for (auto c : t->getClips())
                    if (auto wc = dynamic_cast<te::WaveAudioClip*> (c))
                        if (auto f = wc->getOriginalFile(); f.isAChildOf (projectsDir) && f.existsAsFile())
                            out.addIfNotAlreadyThere (f);
        return out;
    }

    inline Plan plan (const File& songDir, const File& editFile, Array<File> takes)
    {
        return { songDir, editFile, editFile.getSiblingFile (editFile.getFileNameWithoutExtension() + " - metronomo.wav"), std::move (takes) };
    }

    using Trash = std::function<bool (const File&)>;
    inline bool systemTrash (const File& f) { return f.moveToTrash(); }

    // The library folder goes first: if that fails nothing else is touched.
    inline Result execute (const Plan& p, bool includeTakes, const File& libraryRoot, const File& projectsDir, Trash trash = systemTrash)
    {
        // Same guard as Electron's isInsideDir: only a folder strictly inside the library.
        if (! p.songDir.isDirectory() || ! p.songDir.isAChildOf (libraryRoot))
            return Result::fail ("La canción no está en la biblioteca: " + p.songDir.getFullPathName());

        if (! trash (p.songDir))
            return Result::fail ("No se pudo mandar a la papelera " + p.songDir.getFullPathName());

        Array<File> rest { p.editFile, p.clickWav };
        if (includeTakes)
            rest.addArray (p.takes);

        StringArray failed;
        for (auto& f : rest)
            if (f.isAChildOf (projectsDir) && f.existsAsFile() && ! trash (f))
                failed.add (f.getFileName());

        return failed.isEmpty() ? Result::ok()
                                : Result::fail ("La canción se quitó, pero no se pudo mandar a la papelera: " + failed.joinIntoString (", "));
    }
}
