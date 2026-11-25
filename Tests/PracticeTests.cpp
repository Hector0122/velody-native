#include <JuceHeader.h>
#include "../Source/SongEdit.h"
#include "TestAudio.h"

using TestAudio::writeSine;
using TestAudio::analyse;

// Speed/transpose: unit checks on the clamps and .velody fields, then a real offline render through
// Tracktion + Signalsmith to prove the audio is actually stretched/shifted, not just flagged.
struct PracticeTests : UnitTest
{
    PracticeTests() : UnitTest ("Practice speed/pitch") {}

    void runTest() override
    {
        using namespace Metronome;

        beginTest (String (u8"clamps: 0.5–1.5 at any value (to 0.1 %), ±12 semitones"));
        expectEquals (clampSpeed (0.8), 0.8);
        expectWithinAbsoluteError (clampSpeed (0.81), 0.81, 1e-9);
        expectWithinAbsoluteError (clampSpeed (110.0 / 120.0), 0.917, 1e-9);
        expectEquals (clampSpeed (3), 1.5);
        expectEquals (clampSpeed (0.1), 0.5);
        expectEquals (clampSpeed (std::nan ("")), 1.0);
        expectEquals (clampSemitones (13), 12);
        expectEquals (clampSemitones (-2.4), -2);

        beginTest ("readMeta picks up practiceSpeed and transposeSemitones");
        auto dir = File::createTempFile ("velody-practice");
        dir.createDirectory();
        dir.getChildFile (".velody").replaceWithText (R"({"bpm":100,"practiceSpeed":0.8,"transposeSemitones":-2})");
        auto meta = readMeta (dir);
        expectWithinAbsoluteError (meta.practiceSpeed, 0.8, 1e-9);
        expectEquals (meta.transposeSemitones, -2);
        dir.getChildFile (".velody").replaceWithText (R"({"bpm":100})");
        expectEquals (readMeta (dir).practiceSpeed, 1.0);

        te::Engine engine { "VelodyTests" };
        writeSine (dir.getChildFile ("vocals.wav"), 2.0, 440);
        writeSine (dir.getChildFile ("take.wav"), 2.0, 440);
        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("song.tracktionedit"));
        SongEdit::addStemTracks (*edit, { dir.getChildFile ("vocals.wav") });
        SongEdit::setClickTrack (*edit, dir.getChildFile ("vocals.wav")); // any audio will do as the click here
        edit->ensureNumberOfAudioTracks (3);
        auto input = te::getAudioTracks (*edit)[2];
        input->setName (String (u8"Grabación"));
        SongEdit::insertWholeFile (*input, dir.getChildFile ("take.wav"));

        auto clipOf = [&] (int track) { return dynamic_cast<te::WaveAudioClip*> (te::getAudioTracks (*edit)[track]->getClips()[0]); };

        beginTest ("setPractice stretches song clips, transposes stems but not the click, leaves takes alone");
        SongEdit::setPractice (*edit, 0.5, 3);
        expectWithinAbsoluteError (clipOf (0)->getPosition().getLength().inSeconds(), 4.0, 0.01);
        expectEquals (clipOf (0)->getPitchChange(), 3.0f);
        expectWithinAbsoluteError (clipOf (1)->getPosition().getLength().inSeconds(), 4.0, 0.01);
        expectEquals (clipOf (1)->getPitchChange(), 0.0f);
        expectWithinAbsoluteError (clipOf (2)->getPosition().getLength().inSeconds(), 2.0, 0.01);
        expectEquals (clipOf (2)->getSpeedRatio(), 1.0);

        beginTest ("back to 100 % / 0 st restores the original clip");
        SongEdit::setPractice (*edit, 1.0, 0);
        expectWithinAbsoluteError (clipOf (0)->getPosition().getLength().inSeconds(), 2.0, 0.01);
        expect (clipOf (0)->getTimeStretchMode() == te::TimeStretcher::disabled);

        // Only the stem for the render checks.
        te::getAudioTracks (*edit)[1]->setMute (true);
        te::getAudioTracks (*edit)[2]->setMute (true);
        auto render = [&] (double speed, int semis)
        {
            SongEdit::setPractice (*edit, speed, semis);
            auto out = dir.getChildFile ("render.wav");
            out.deleteFile();
            te::Renderer::Parameters params (*edit);
            params.destFile = out;
            params.time = params.time.withLength (edit->getLength());
            params.audioFormat = engine.getAudioFileFormatManager().getWavFormat();
            std::atomic<bool> done { false };
            auto handle = te::EditRenderer::render (std::move (params), [&] (auto) { done = true; });
            for (int i = 0; i < 500 && ! done; ++i)
                MessageManager::getInstance()->runDispatchLoopUntil (10);
            return analyse (out);
        };

        beginTest ("render at 50 %: twice as long, same pitch");
        auto [len, freq] = render (0.5, 0);
        expectGreaterThan (len, 3.9);
        expectWithinAbsoluteError (freq, 440.0, 15.0);

        beginTest ("render at 100 % +12 st: same length, an octave up");
        std::tie (len, freq) = render (1.0, 12);
        expectWithinAbsoluteError (len, 2.0, 0.2);
        expectWithinAbsoluteError (freq, 880.0, 30.0);

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static PracticeTests practiceTests;
