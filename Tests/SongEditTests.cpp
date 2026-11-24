#include <JuceHeader.h>
#include "../Source/SongEdit.h"

// Integration: real Tracktion engine, real audio files on disk, no audio device or window.
struct SongEditTests : UnitTest
{
    SongEditTests() : UnitTest ("SongEdit") {}

    static void writeWav (const File& f, double seconds)
    {
        f.getParentDirectory().createDirectory();
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (f), 44100, 2, 16, {}, 0));
        AudioBuffer<float> b (2, (int) (44100 * seconds));
        b.clear();
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-song");
        writeWav (dir.getChildFile ("drums.wav"), 2.0);
        writeWav (dir.getChildFile ("vocals.wav"), 3.0);

        beginTest ("MP3 decoding is compiled in (library stems are MP3)");
        expect (engine.getAudioFileFormatManager().getFormatFromFileName (File ("/x/vocals.mp3")) != nullptr);

        beginTest ("a song edit gets one named track with one clip per stem");
        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("song.tracktionedit"));
        SongEdit::addStemTracks (*edit, { dir.getChildFile ("drums.wav"), dir.getChildFile ("vocals.wav") });

        auto tracks = te::getAudioTracks (*edit);
        expectEquals (tracks.size(), 2);
        expectEquals (tracks[0]->getName(), String ("drums"));
        expectEquals (tracks[1]->getName(), String ("vocals"));
        expectEquals (tracks[1]->getClips().size(), 1);
        expectWithinAbsoluteError (tracks[1]->getClips()[0]->getPosition().getLength().inSeconds(), 3.0, 0.01);

        beginTest ("input tracks start after the stem tracks");
        expectEquals (SongEdit::firstInputTrack (*edit), 2);
        edit->ensureNumberOfAudioTracks (3);
        expectEquals (SongEdit::firstInputTrack (*edit), 2);

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static SongEditTests songEditTests;

// Click track: placed after the stems, not armed, replaced (not duplicated) on reopen.
struct ClickTrackTests : UnitTest
{
    ClickTrackTests() : UnitTest ("SongEdit click track") {}

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-click");
        SongEditTests::writeWav (dir.getChildFile ("drums.wav"), 4.0);
        SongEditTests::writeWav (dir.getChildFile ("vocals.wav"), 3.0);
        Library::Song song { "x", dir, { dir.getChildFile ("drums.wav"), dir.getChildFile ("vocals.wav") } };
        auto wav = dir.getChildFile ("click.wav");

        beginTest ("unanalyzed song: no click track");
        expect (! SongEdit::writeClickWav (engine, song, wav));

        beginTest ("analyzed song: click WAV as long as the longest stem, track after the stems");
        dir.getChildFile (".velody").replaceWithText (R"({"bpm":120,"beatTimes":[0.5,1.0,1.5,2.0]})");
        expect (SongEdit::writeClickWav (engine, song, wav));
        expectWithinAbsoluteError (te::AudioFile (engine, wav).getLength(), 4.0, 0.01);

        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("song.tracktionedit"));
        SongEdit::addStemTracks (*edit, song.stems);
        edit->ensureNumberOfAudioTracks (3); // an input track the user already had
        SongEdit::setClickTrack (*edit, wav);

        auto tracks = te::getAudioTracks (*edit);
        expectEquals (tracks.size(), 4);
        expectEquals (tracks[2]->getName(), SongEdit::clickTrackName);
        expectEquals (tracks[2]->getClips().size(), 1);
        expectEquals (SongEdit::firstInputTrack (*edit), 3);

        beginTest ("setting it again replaces the clip instead of adding a track");
        SongEdit::setClickTrack (*edit, wav);
        expectEquals (te::getAudioTracks (*edit).size(), 4);
        expectEquals (te::getAudioTracks (*edit)[2]->getClips().size(), 1);

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static ClickTrackTests clickTrackTests;

#include "../Source/CountInPlugin.h"

struct CountInPluginTests : UnitTest
{
    CountInPluginTests() : UnitTest ("CountInPlugin") {}

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        engine.getPluginManager().createBuiltInType<CountInPlugin>();
        auto file = File::createTempFile (".tracktionedit");

        beginTest ("one count-in plugin on the master bus, not duplicated, survives save + reopen");
        {
            auto edit = SongEdit::openOrCreate (engine, file);
            auto p = CountInPlugin::findOrInsert (*edit);
            expect (p != nullptr);
            expect (CountInPlugin::findOrInsert (*edit) == p);
            expect (! p->isArmed());
            p->arm ({ { 1.0, true } }, 2.0);
            expect (p->isArmed());
            p->disarm();
            expect (! p->isArmed());
            te::EditFileOperations (*edit).save (true, true, false);
        }

        auto edit = SongEdit::openOrCreate (engine, file);
        int n = 0;
        for (auto p : edit->getMasterPluginList())
            if (dynamic_cast<CountInPlugin*> (p) != nullptr) ++n;
        expectEquals (n, 1);
        CountInPlugin::findOrInsert (*edit);
        n = 0;
        for (auto p : edit->getMasterPluginList())
            if (dynamic_cast<CountInPlugin*> (p) != nullptr) ++n;
        expectEquals (n, 1);

        edit = nullptr;
        file.deleteFile();
    }
};

static CountInPluginTests countInPluginTests;

// Regression: a WAV beside the edit (metronome, recorded takes) must still be found after reopening.
struct EditPathTests : UnitTest
{
    EditPathTests() : UnitTest ("SongEdit paths") {}

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-paths");
        auto editFile = dir.getChildFile ("Song.tracktionedit");
        auto beside = dir.getChildFile ("Song - metronomo.wav");
        SongEditTests::writeWav (beside, 1.0);

        beginTest ("a clip beside the edit resolves to the right file after save + reopen");
        {
            auto edit = SongEdit::openOrCreate (engine, editFile);
            SongEdit::setClickTrack (*edit, beside);
            te::EditFileOperations (*edit).save (true, true, false);
        }
        auto edit = SongEdit::openOrCreate (engine, editFile);
        auto click = te::getAudioTracks (*edit)[0];
        expectEquals (click->getName(), SongEdit::clickTrackName);
        auto clip = dynamic_cast<te::WaveAudioClip*> (click->getClips()[0]);
        expect (clip != nullptr);
        if (clip != nullptr)
        {
            expectEquals (clip->getOriginalFile().getFullPathName(), beside.getFullPathName());
            expect (clip->getAudioFile().isValid());
        }

        beginTest ("old edits saved with \"../x.wav\" (before the fix) resolve too");
        expectEquals (edit->filePathResolver ("../Song - metronomo.wav").getFullPathName(), beside.getFullPathName());

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static EditPathTests editPathTests;
