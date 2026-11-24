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
        auto edit = te::createEmptyEdit (engine, dir.getChildFile ("song.tracktionedit"));
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

        auto edit = te::createEmptyEdit (engine, dir.getChildFile ("song.tracktionedit"));
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
