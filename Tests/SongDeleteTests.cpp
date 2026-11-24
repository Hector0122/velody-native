#include <JuceHeader.h>
#include "../Source/SongDelete.h"
#include "TestAudio.h"

struct SongDeleteTests : UnitTest
{
    SongDeleteTests() : UnitTest ("SongDelete") {}

    File root, library, projects, trashDir;

    // Stand-in for the system trash: moves into a folder we can inspect (and that isn't the user's trash).
    SongDelete::Trash fakeTrash()
    {
        return [this] (const File& f) { return f.moveFileTo (trashDir.getChildFile (f.getFileName())); };
    }

    void runTest() override
    {
        root = File::createTempFile ("velody-delete");
        library = root.getChildFile ("stems");
        projects = root.getChildFile ("Velody");
        trashDir = root.getChildFile ("trash");
        trashDir.createDirectory();

        auto songDir = library.getChildFile ("Song");
        TestAudio::writeSine (songDir.getChildFile ("drums.wav"), 1, 100);
        songDir.getChildFile (".velody").replaceWithText ("{}");
        auto editFile = projects.getChildFile ("Song.tracktionedit");
        auto clickWav = projects.getChildFile ("Song - metronomo.wav");
        auto take = projects.getChildFile ("Song take 1.wav");
        auto outside = root.getChildFile ("music/mine.wav");
        TestAudio::writeSine (clickWav, 1, 1000);
        TestAudio::writeSine (take, 1, 300);
        TestAudio::writeSine (outside, 1, 400);

        beginTest ("takesIn: audio on the user's tracks inside the projects folder only");
        te::Engine engine { "VelodyTests" };
        {
            auto edit = SongEdit::openOrCreate (engine, editFile);
            SongEdit::addStemTracks (*edit, { songDir.getChildFile ("drums.wav") });
            SongEdit::setClickTrack (*edit, clickWav);
            edit->ensureNumberOfAudioTracks (4);
            SongEdit::insertWholeFile (*te::getAudioTracks (*edit)[2], take);
            SongEdit::insertWholeFile (*te::getAudioTracks (*edit)[3], outside);
            auto takes = SongDelete::takesIn (*edit, projects);
            expectEquals (takes.size(), 1);
            expect (takes.contains (take));
            te::EditFileOperations (*edit).save (true, true, false);
        }

        beginTest ("refuses anything that isn't a song folder inside the library");
        auto p = SongDelete::plan (songDir, editFile, { take });
        auto bad = p;
        bad.songDir = library;
        expect (SongDelete::execute (bad, true, library, projects, fakeTrash()).failed());
        bad.songDir = outside.getParentDirectory();
        expect (SongDelete::execute (bad, true, library, projects, fakeTrash()).failed());
        expect (library.exists() && outside.existsAsFile());

        beginTest ("if the song folder can't be trashed, nothing else is touched");
        auto r = SongDelete::execute (p, true, library, projects, [] (const File&) { return false; });
        expect (r.failed());
        expect (songDir.isDirectory() && editFile.existsAsFile() && clickWav.existsAsFile() && take.existsAsFile());

        beginTest ("default: song folder, project and click to the trash; takes and outside files kept");
        r = SongDelete::execute (p, false, library, projects, fakeTrash());
        expect (r.wasOk(), r.getErrorMessage());
        expect (! songDir.exists() && ! editFile.exists() && ! clickWav.exists());
        expect (trashDir.getChildFile ("Song").getChildFile ("drums.wav").existsAsFile()); // recoverable
        expect (take.existsAsFile());
        expect (outside.existsAsFile());

        beginTest ("with takes: the recordings go to the trash too");
        TestAudio::writeSine (songDir.getChildFile ("drums.wav"), 1, 100);
        trashDir.deleteRecursively();
        trashDir.createDirectory();
        r = SongDelete::execute (p, true, library, projects, fakeTrash());
        expect (r.wasOk(), r.getErrorMessage());
        expect (! take.exists());
        expect (trashDir.getChildFile (take.getFileName()).existsAsFile());
        expect (outside.existsAsFile());

        root.deleteRecursively();
    }
};

static SongDeleteTests songDeleteTests;
