#include <JuceHeader.h>
#include "../Source/PracticeMeta.h"

// Speed / transpose / count-in in the .velody (like Electron), so they sync with the cloud.
struct PracticeMetaTests : UnitTest
{
    PracticeMetaTests() : UnitTest ("Practice values in .velody") {}

    File dir;

    void write (const String& json) { dir.getChildFile (".velody").replaceWithText (json); }
    var meta() { return SongMeta::read (dir); }

    void runTest() override
    {
        using namespace PracticeMeta;
        dir = File::createTempFile ("velody-practice-meta");
        dir.createDirectory();
        ValueTree edit ("EDIT");

        beginTest ("nothing saved: 100 %, 0 st, no count-in; nothing written");
        write (R"({"bpm":100,"updatedAt":5})");
        auto v = load (dir, edit);
        expect (v.speed == 1.0 && v.semitones == 0 && v.countInBars == 0);
        expectEquals ((int) meta()["updatedAt"], 5);

        beginTest (".velody wins over the edit's old copy, and is clamped");
        write (R"({"practiceSpeed":0.83,"transposeSemitones":-20,"playbackCountInBars":4,"updatedAt":5})");
        edit.setProperty (speedProp, 1.2, nullptr);
        v = load (dir, edit);
        expectWithinAbsoluteError (v.speed, 0.85, 1e-9);
        expectEquals (v.semitones, -12);
        expectEquals (v.countInBars, 4);
        expectEquals ((int) meta()["updatedAt"], 5); // nothing moved

        beginTest ("values only in the edit (from before) move into .velody once, stamped so they sync");
        write (R"({"bpm":100,"updatedAt":5})");
        edit.setProperty (speedProp, 0.7, nullptr);
        edit.setProperty (pitchProp, 3, nullptr);
        edit.setProperty (countInProp, 2, nullptr);
        v = load (dir, edit);
        expectWithinAbsoluteError (v.speed, 0.7, 1e-9);
        expect (v.semitones == 3 && v.countInBars == 2);
        auto m = meta();
        expect (std::abs ((double) m["practiceSpeed"] - 0.7) < 1e-9 && (int) m["transposeSemitones"] == 3 && (int) m["playbackCountInBars"] == 2);
        expect ((int64) m["updatedAt"] > 5);
        expectEquals ((int) m["bpm"], 100);
        expect (SongMeta::syncedPart (m).hasProperty ("practiceSpeed")); // goes up with the cloud sync

        beginTest ("saving writes only what changed; the same value again doesn't touch the file");
        write (R"({"practiceSpeed":0.8,"transposeSemitones":0,"playbackCountInBars":4,"updatedAt":5})");
        expect (saveSpeedPitch (dir, 0.8, 0));
        expectEquals ((int) meta()["updatedAt"], 5);
        expect (saveSpeedPitch (dir, 0.9, -2));
        m = meta();
        expect (std::abs ((double) m["practiceSpeed"] - 0.9) < 1e-9 && (int) m["transposeSemitones"] == -2);
        expectEquals ((int) m["playbackCountInBars"], 4); // Electron's 4 bars survive a speed change here
        expect ((int64) m["updatedAt"] > 5);
        expect (saveCountIn (dir, 1));
        expectEquals ((int) meta()["playbackCountInBars"], 1);
        expect (saveSpeedPitch (dir, 9.0, 40));
        expect (std::abs ((double) meta()["practiceSpeed"] - 1.5) < 1e-9 && (int) meta()["transposeSemitones"] == 12);

        dir.deleteRecursively();
    }
};

static PracticeMetaTests practiceMetaTests;
