#include <JuceHeader.h>
#include "../Source/MixerPanel.h"
#include "../Source/SongEdit.h"

// UI -> data: moving a mixer slider changes the track's volume/pan and survives save + reopen.
struct MixerPanelTests : UnitTest
{
    MixerPanelTests() : UnitTest ("MixerPanel") {}

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto file = File::createTempFile (".tracktionedit");

        {
            auto edit = SongEdit::openOrCreate (engine, file);
            edit->ensureNumberOfAudioTracks (2);

            MixerPanel mixer;
            mixer.setEdit (edit.get());

            beginTest ("one row per track");
            expectEquals (mixer.rows.size(), 2);

            beginTest ("sliders write volume and pan to the track");
            mixer.rows[1]->volume.setValue (-12.0, sendNotificationSync);
            mixer.rows[1]->pan.setValue (-0.5, sendNotificationSync);
            auto vp = te::getAudioTracks (*edit)[1]->getVolumePlugin();
            expectWithinAbsoluteError (vp->getVolumeDb(), -12.0f, 0.05f);
            expectWithinAbsoluteError (vp->getPan(), -0.5f, 0.01f);
            expectWithinAbsoluteError (te::getAudioTracks (*edit)[0]->getVolumePlugin()->getVolumeDb(), 0.0f, 0.05f);

            beginTest ("rebuild follows added tracks");
            edit->ensureNumberOfAudioTracks (3);
            mixer.rebuild();
            expectEquals (mixer.rows.size(), 3);

            mixer.setEdit (nullptr);
            te::EditFileOperations (*edit).save (true, true, false);
        }

        beginTest ("values survive save and reopen, and the mixer shows them");
        auto edit = SongEdit::openOrCreate (engine, file);
        auto vp = te::getAudioTracks (*edit)[1]->getVolumePlugin();
        expectWithinAbsoluteError (vp->getVolumeDb(), -12.0f, 0.05f);
        expectWithinAbsoluteError (vp->getPan(), -0.5f, 0.01f);

        MixerPanel mixer;
        mixer.setEdit (edit.get());
        expectWithinAbsoluteError (mixer.rows[1]->volume.getValue(), -12.0, 0.05);
        mixer.setEdit (nullptr);

        edit = nullptr;
        file.deleteFile();
    }
};

static MixerPanelTests mixerPanelTests;
