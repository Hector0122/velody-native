#pragma once
#include <JuceHeader.h>

namespace te = tracktion;

// Projects from scratch (no song): <base>/Proyectos/<name>.tracktionedit, next to the songs' own edits
// in <base> (~/Documentos/Velody). Each has its tempo and time signature and uses Tracktion's click.
namespace Projects
{
    inline File folder (const File& base) { return base.getChildFile ("Proyectos"); }
    inline File fileFor (const File& base, const String& name) { return folder (base).getChildFile (name + ".tracktionedit"); }
    inline bool isProject (const File& base, const File& edit) { return edit.getParentDirectory() == folder (base); }

    struct Info { String name; File file; Time modified; };

    // Most recently used first.
    inline Array<Info> list (const File& base)
    {
        Array<Info> out;
        for (auto& f : folder (base).findChildFiles (File::findFiles | File::ignoreHiddenFiles, false, "*.tracktionedit"))
            out.add ({ f.getFileNameWithoutExtension(), f, f.getLastModificationTime() });
        std::sort (out.begin(), out.end(), [] (const Info& a, const Info& b) { return a.modified > b.modified; });
        return out;
    }

    // Empty when the name can be used, else why not.
    inline String checkName (const File& base, const String& name, const File& except = {})
    {
        const auto n = name.trim();
        if (n.isEmpty())
            return "Ponle un nombre al proyecto";
        if (n != File::createLegalFileName (n) || n.startsWithChar ('.'))
            return String (u8"El nombre no puede llevar / \\ : * ? \" < > | ni empezar con punto");
        if (auto f = fileFor (base, n); f.exists() && f != except)
            return String (u8"Ya hay un proyecto llamado «") + n + String (u8"»");
        return {};
    }

    inline String suggestName (const File& base)
    {
        for (int i = 1;; ++i)
            if (checkName (base, "Proyecto " + String (i)).isEmpty())
                return "Proyecto " + String (i);
    }

    // Before projects had their own folder, the empty project was <base>/Proyecto.tracktionedit.
    inline void migrateLegacy (const File& base)
    {
        auto legacy = base.getChildFile ("Proyecto.tracktionedit");
        if (legacy.existsAsFile() && ! fileFor (base, "Proyecto").exists())
        {
            folder (base).createDirectory();
            legacy.moveFileTo (fileFor (base, "Proyecto"));
        }
    }

    inline const StringArray signatures { "4/4", "3/4", "6/8", "2/4", "5/4", "7/8", "12/8" };

    // Tempo, time signature and Tracktion's click for a new project.
    inline void setUp (te::Edit& edit, double bpm, const String& signature)
    {
        if (auto t = edit.tempoSequence.getTempo (0))
            t->setBpm (jlimit (30.0, 300.0, bpm));
        if (auto ts = edit.tempoSequence.getTimeSig (0))
        {
            ts->numerator = jlimit (1, 16, signature.upToFirstOccurrenceOf ("/", false, false).getIntValue());
            ts->denominator = jlimit (1, 16, signature.fromFirstOccurrenceOf ("/", false, false).getIntValue());
        }
        edit.clickTrackEnabled = true;
    }

    inline double bpm (te::Edit& edit) { return edit.tempoSequence.getTempo (0) != nullptr ? edit.tempoSequence.getTempo (0)->getBpm() : 120.0; }

    inline String signature (te::Edit& edit)
    {
        auto* ts = edit.tempoSequence.getTimeSig (0);
        return ts != nullptr ? String (ts->numerator.get()) + "/" + String (ts->denominator.get()) : String ("4/4");
    }

    // Renames the edit file (its takes stay where they are, next to it).
    inline bool rename (const File& base, const File& edit, const String& newName)
    {
        return checkName (base, newName, edit).isEmpty() && edit.moveFileTo (fileFor (base, newName.trim()));
    }

    // Tracktion's count-in on record for a project (whole bars only: 1 or 2; 4 → 2).
    inline te::Edit::CountIn countInFor (int bars)
    {
        return bars <= 0 ? te::Edit::CountIn::none : bars == 1 ? te::Edit::CountIn::oneBar : te::Edit::CountIn::twoBar;
    }
}
