#include <JuceHeader.h>
#include "../Source/Projects.h"
#include "../Source/SongEdit.h"

// Projects from scratch: where they live, names, the list, tempo/time signature/click on a real edit,
// renaming, and the old single "Proyecto" moved into the projects folder.
struct ProjectsTests : UnitTest
{
    ProjectsTests() : UnitTest ("Projects (from scratch)") {}

    void runTest() override
    {
        using namespace Projects;
        auto base = File::createTempFile ("velody-projects");
        base.createDirectory();

        beginTest ("the old empty project moves into Proyectos/ once");
        base.getChildFile ("Proyecto.tracktionedit").replaceWithText ("<EDIT/>");
        base.getChildFile ("Song.tracktionedit").replaceWithText ("<EDIT/>"); // a song's edit stays
        migrateLegacy (base);
        expect (fileFor (base, "Proyecto").existsAsFile() && ! base.getChildFile ("Proyecto.tracktionedit").exists());
        expect (base.getChildFile ("Song.tracktionedit").existsAsFile());
        expect (isProject (base, fileFor (base, "Proyecto")) && ! isProject (base, base.getChildFile ("Song.tracktionedit")));

        beginTest ("names: required, legal, not taken; the next free Proyecto N");
        expect (checkName (base, "  ").isNotEmpty());
        expect (checkName (base, "a/b").isNotEmpty() && checkName (base, ".oculto").isNotEmpty());
        expect (checkName (base, "Proyecto").contains ("Ya hay"));
        expect (checkName (base, "Proyecto", fileFor (base, "Proyecto")).isEmpty()); // renaming to itself
        expect (checkName (base, String (u8"Canción nueva")).isEmpty());
        expectEquals (suggestName (base), String ("Proyecto 1"));
        fileFor (base, "Proyecto 1").replaceWithText ("<EDIT/>");
        expectEquals (suggestName (base), String ("Proyecto 2"));

        beginTest ("the list: most recently used first, hidden files left out");
        fileFor (base, "Proyecto").setLastModificationTime (Time::getCurrentTime() - RelativeTime::hours (2));
        fileFor (base, "Proyecto 1").setLastModificationTime (Time::getCurrentTime());
        folder (base).getChildFile (".velody-closing.tracktionedit").replaceWithText ("<EDIT/>");
        auto l = list (base);
        expectEquals (l.size(), 2);
        expect (l[0].name == "Proyecto 1" && l[1].name == "Proyecto");

        beginTest ("a new project: tempo, time signature and the click, saved and read back");
        {
            te::Engine engine { "VelodyTests" };
            auto f = fileFor (base, "Rola");
            {
                auto edit = SongEdit::openOrCreate (engine, f);
                setUp (*edit, 96.0, "6/8");
                expect (edit->clickTrackEnabled.get());
                te::EditFileOperations (*edit).save (true, true, false);
            }
            auto edit = SongEdit::openOrCreate (engine, f);
            expectWithinAbsoluteError (bpm (*edit), 96.0, 0.01);
            expectEquals (signature (*edit), String ("6/8"));
            expect (edit->clickTrackEnabled.get());
            setUp (*edit, 1000.0, "4/4");
            expectWithinAbsoluteError (bpm (*edit), 300.0, 0.01); // clamped
        }

        beginTest ("rename moves the file; a taken name is refused");
        expect (rename (base, fileFor (base, "Rola"), "Rola final"));
        expect (fileFor (base, "Rola final").existsAsFile() && ! fileFor (base, "Rola").exists());
        expect (! rename (base, fileFor (base, "Rola final"), "Proyecto"));

        beginTest ("record count-in for a project: Tracktion's whole bars");
        expect (countInFor (0) == te::Edit::CountIn::none && countInFor (1) == te::Edit::CountIn::oneBar);
        expect (countInFor (2) == te::Edit::CountIn::twoBar && countInFor (4) == te::Edit::CountIn::twoBar);

        base.deleteRecursively();
    }
};

static ProjectsTests projectsTests;
