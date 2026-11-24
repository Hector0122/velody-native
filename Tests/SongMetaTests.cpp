#include <JuceHeader.h>
#include "../Source/SongMeta.h"
#include "../Source/Metronome.h"

struct SongMetaTests : UnitTest
{
    SongMetaTests() : UnitTest ("SongMeta (.velody writes)") {}

    void runTest() override
    {
        auto dir = File::createTempFile ("velody-meta");
        dir.createDirectory();
        auto file = dir.getChildFile (".velody");

        beginTest ("update merges, keeps other keys, stamps updatedAt, leaves no temp file");
        file.replaceWithText (R"({"cloudId":"abc","downbeatShift":2,"updatedAt":1})");
        NamedValueSet patch;
        patch.set ("bpm", 120.5);
        expect (SongMeta::update (dir, patch));
        auto j = JSON::parse (file);
        expectEquals (j["cloudId"].toString(), String ("abc"));
        expectEquals ((int) j["downbeatShift"], 2);
        expectEquals ((double) j["bpm"], 120.5);
        expectGreaterThan ((int64) j["updatedAt"], (int64) 1);
        expectEquals (dir.findChildFiles (File::findFiles, false, "*.tmp").size(), 0);

        beginTest ("update creates the file when missing or replaces invalid JSON");
        file.deleteFile();
        expect (SongMeta::update (dir, patch));
        expectEquals ((double) JSON::parse (file)["bpm"], 120.5);
        file.replaceWithText ("{not json");
        expect (SongMeta::update (dir, patch));
        expect (JSON::parse (file).isObject());

        beginTest ("analysisPatch: valid detection becomes Electron's cache shape");
        auto p = SongMeta::analysisPatch (R"({"bpm":98.7,"firstBeatOffset":0.31,"beatTimes":[0.31,0.92],"tracker":"beat_this","downbeatShift":1,"chords":[{"time":0,"chord":"E:min"},{"time":"x","chord":"bad"},{"time":2,"chord":"this-name-is-too-long"}]})");
        expect (p.has_value());
        if (p)
        {
            expectEquals ((double) (*p)["bpm"], 98.7);
            expectEquals ((*p)["tracker"].toString(), String ("beat_this"));
            expectEquals ((int) (*p)["detectedDownbeatShift"], 1);
            expectEquals ((int) (*p)["_version"], 22);
            expectEquals ((*p)["chords"].size(), 1);
        }

        beginTest ("analysisPatch: fallback tracker, missing grid becomes []");
        auto f = SongMeta::analysisPatch (R"({"bpm":120,"firstBeatOffset":0})");
        expect (f.has_value() && (*f)["tracker"].toString() == "fallback" && (*f)["beatTimes"].isArray());

        beginTest ("analysisPatch: failures are never cached");
        expect (! SongMeta::analysisPatch ("").has_value());
        expect (! SongMeta::analysisPatch ("Traceback (most recent call last):").has_value());
        expect (! SongMeta::analysisPatch (R"({"bpm":0,"firstBeatOffset":0})").has_value());
        expect (! SongMeta::analysisPatch (R"({"bpm":120})").has_value());

        beginTest ("the written analysis is what the metronome reads back");
        file.replaceWithText (R"({"cloudId":"abc"})");
        expect (SongMeta::update (dir, *SongMeta::analysisPatch (R"({"bpm":100,"firstBeatOffset":0.5,"beatTimes":[0.5,1.1,1.7],"downbeatShift":3})")));
        auto meta = Metronome::readMeta (dir);
        expectEquals (meta.bpm, 100.0);
        expectEquals ((int) meta.beatTimes.size(), 3);
        expectEquals (meta.downbeatShift, 3);

        beginTest ("pickBeatTrackingStem: exact drums first, never a take like drums_take");
        expectEquals (SongMeta::pickBeatTrackingStem ({ File ("/s/drums_take.wav"), File ("/s/bass.mp3"), File ("/s/drums.mp3") }).getFileName(), String ("drums.mp3"));
        expectEquals (SongMeta::pickBeatTrackingStem ({ File ("/s/vocals.mp3"), File ("/s/bass.mp3") }).getFileName(), String ("bass.mp3"));
        expect (SongMeta::pickBeatTrackingStem ({}) == File());

        dir.deleteRecursively();
    }
};

static SongMetaTests songMetaTests;
