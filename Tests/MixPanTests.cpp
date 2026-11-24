#include <JuceHeader.h>
#include "../Source/MixerPanel.h"
#include "TestAudio.h"

// Master row of the mixer: master volume, and Mix Pan (the mix to one side, the metronome keeps its
// own pan), from the slider down to the rendered audio, channel by channel.
struct MixPanTests : UnitTest
{
    MixPanTests() : UnitTest ("Mix Pan and master volume") {}

    // RMS of the left and right channels of a rendered file.
    static std::pair<double, double> channelRms (te::Engine& engine, const File& f)
    {
        std::unique_ptr<AudioFormatReader> r (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (f));
        if (r == nullptr || r->numChannels < 2)
            return { -1, -1 };
        AudioBuffer<float> buf (2, (int) r->lengthInSamples);
        r->read (&buf, 0, (int) r->lengthInSamples, 0, true, true);
        return { buf.getRMSLevel (0, 0, buf.getNumSamples()), buf.getRMSLevel (1, 0, buf.getNumSamples()) };
    }

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-mixpan");
        dir.createDirectory();
        TestAudio::writeSine (dir.getChildFile ("drums.wav"), 1.0, 440.0);
        TestAudio::writeSine (dir.getChildFile ("click.wav"), 1.0, 1000.0);

        auto file = dir.getChildFile ("song.tracktionedit");
        auto edit = SongEdit::openOrCreate (engine, file);
        edit->ensureNumberOfAudioTracks (2);
        auto drums = te::getAudioTracks (*edit)[0];
        auto click = te::getAudioTracks (*edit)[1];
        drums->setName ("drums");
        click->setName (SongEdit::clickTrackName);
        SongEdit::insertWholeFile (*drums, dir.getChildFile ("drums.wav"));
        SongEdit::insertWholeFile (*click, dir.getChildFile ("click.wav"));

        auto render = [&]
        {
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
            return channelRms (engine, out);
        };

        MixerPanel mixer;
        float saved = 99;
        mixer.onMixPanChanged = [&] (float p) { saved = p; };
        mixer.setEdit (edit.get());

        beginTest ("a Master row after the tracks; nothing added while Mix Pan is centred");
        expect (mixer.master != nullptr);
        expectEquals (mixer.getIdealHeight(), 3 * MixerPanel::rowHeight);
        SongEdit::setMixPan (*edit, 0.0f);
        expect (SongEdit::mixPanPlugin (*drums) == nullptr);

        beginTest ("Mix Pan slider: every track but the metronome, and the owner is told (to save it)");
        mixer.master->mixPan.setValue (-1.0, sendNotificationSync);
        expectEquals (saved, -1.0f);
        expect (SongEdit::mixPanPlugin (*drums) != nullptr && SongEdit::mixPanPlugin (*drums)->getPan() == -1.0f);
        expect (SongEdit::mixPanPlugin (*click) == nullptr);
        expect (drums->getVolumePlugin() != SongEdit::mixPanPlugin (*drums)); // the track's own fader is untouched
        expectWithinAbsoluteError (drums->getVolumePlugin()->getPan(), 0.0f, 0.001f);
        expectEquals (mixer.master->mixPan.getTextFromValue (-1.0), String ("Mix 100 % L"));

        beginTest ("rendered: the mix only on the left; the metronome still on both sides");
        click->setMute (true);
        auto [ml, mr] = render();
        expectGreaterThan (ml, 0.05);
        expectLessThan (mr, 0.001);
        click->setMute (false);
        drums->setMute (true);
        auto [cl, cr] = render();
        expectGreaterThan (cl, 0.05);
        expectGreaterThan (cr, 0.05);
        drums->setMute (false);

        beginTest ("the metronome's own pan sends it to the other side");
        click->getVolumePlugin()->setPan (1.0f);
        drums->setMute (true);
        auto [kl, kr] = render();
        expectLessThan (kl, 0.001);
        expectGreaterThan (kr, 0.05);
        drums->setMute (false);

        beginTest ("Master volume slider: the edit's master volume");
        mixer.master->volume.setValue (-12.0, sendNotificationSync);
        expectWithinAbsoluteError (edit->getMasterVolumePlugin()->getVolumeDb(), -12.0f, 0.05f);

        beginTest ("a new track (an input) gets it; it all survives save + reopen");
        edit->ensureNumberOfAudioTracks (3);
        SongEdit::setMixPan (*edit, -1.0f);
        expect (SongEdit::mixPanPlugin (*te::getAudioTracks (*edit)[2]) != nullptr);
        mixer.setEdit (nullptr);
        te::EditFileOperations (*edit).save (true, true, false);
        edit = nullptr;
        edit = SongEdit::openOrCreate (engine, file);
        expectEquals (SongEdit::mixPan (*edit), -1.0f);
        expectWithinAbsoluteError (edit->getMasterVolumePlugin()->getVolumeDb(), -12.0f, 0.05f);
        MixerPanel reopened;
        reopened.setEdit (edit.get());
        expectEquals (reopened.master->mixPan.getValue(), -1.0);
        reopened.setEdit (nullptr);

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static MixPanTests mixPanTests;
