#pragma once
#include <JuceHeader.h>

// The Electron app's song library: one folder per song with its stems (read-only from here).
namespace Library
{
    // Same names and order as velody/src/shared/analysis.ts STEM_NAMES.
    inline const StringArray stemNames { "drums", "bass", "other", "guitar", "piano", "vocals" };

    // Like Electron's scan: any file whose name starts with a stem name ("vocals.mp3", "vocals_take.wav").
    inline bool isStemName (const String& s)
    {
        for (auto& stem : stemNames)
            if (s.startsWith (stem))
                return true;
        return false;
    }

    inline File defaultRoot()
    {
        return File::getSpecialLocation (File::userHomeDirectory).getChildFile (".config/velody/output/stems");
    }

    struct Song
    {
        String name;
        Array<File> stems; // in stemNames order
    };

    // Mirrors scanSongsLibrary(): skips dot entries (cloud downloads in progress) and folders without stems.
    inline Array<Song> scan (const File& root)
    {
        Array<Song> songs;

        for (auto& dir : root.findChildFiles (File::findDirectories, false))
        {
            if (dir.getFileName().startsWith ("."))
                continue;

            Song song { dir.getFileName(), {} };
            for (auto& stem : stemNames)
                for (auto& f : dir.findChildFiles (File::findFiles, false, stem + "*"))
                    song.stems.add (f);

            if (! song.stems.isEmpty())
                songs.add (song);
        }

        std::sort (songs.begin(), songs.end(), [] (auto& a, auto& b) { return a.name.compareNatural (b.name) < 0; });
        return songs;
    }
}
