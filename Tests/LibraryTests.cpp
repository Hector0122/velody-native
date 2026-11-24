#include <JuceHeader.h>
#include "../Source/Library.h"

struct LibraryTests : UnitTest
{
    LibraryTests() : UnitTest ("Library") {}

    void runTest() override
    {
        auto root = File::createTempFile ("velody-lib");
        root.createDirectory();
        auto touch = [&] (const String& rel) { root.getChildFile (rel).create(); };

        touch ("B song/vocals.mp3");
        touch ("B song/drums.mp3");
        touch ("B song/vocals_take.wav");
        touch ("B song/.velody");
        touch ("B song/_tmp_rec_x.wav");
        touch ("a song/bass.mp3");
        touch ("Empty/notes.txt");
        touch (".velody_dl_123/drums.mp3");
        touch ("loose.mp3");

        beginTest ("scan keeps folders with stems, skips dot folders and empty ones");
        auto songs = Library::scan (root);
        expectEquals (songs.size(), 2);
        expectEquals (songs[0].name, String ("a song"));
        expectEquals (songs[1].name, String ("B song"));

        beginTest ("stems come in STEM_NAMES order and ignore non-stem files");
        StringArray names;
        for (auto& f : songs[1].stems) names.add (f.getFileName());
        names.sort (false); // order within one stem prefix is filesystem-dependent
        expect (songs[1].stems[0].getFileName() == "drums.mp3");
        expectEquals (names.joinIntoString (","), String ("drums.mp3,vocals.mp3,vocals_take.wav"));

        beginTest ("missing root gives no songs");
        expectEquals (Library::scan (root.getChildFile ("nope")).size(), 0);

        beginTest ("isStemName");
        expect (Library::isStemName ("vocals_take"));
        expect (! Library::isStemName ("Track 1"));

        root.deleteRecursively();
    }
};

static LibraryTests libraryTests;

int main()
{
    ScopedJuceInitialiser_GUI juce; // Tracktion's Engine needs a MessageManager
    UnitTestRunner runner;
    runner.runAllTests();
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (runner.getResult (i)->failures > 0)
            return 1;
    return 0;
}
