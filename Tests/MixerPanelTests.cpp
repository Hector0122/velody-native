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

            beginTest ("strip M/S buttons mute and solo the track");
            mixer.rows[0]->mute.setToggleState (true, dontSendNotification);
            mixer.rows[0]->mute.onClick();
            mixer.rows[1]->solo.setToggleState (true, dontSendNotification);
            mixer.rows[1]->solo.onClick();
            expect (te::getAudioTracks (*edit)[0]->isMuted (false));
            expect (te::getAudioTracks (*edit)[1]->isSolo (false));
            mixer.rows[0]->mute.setToggleState (false, dontSendNotification);
            mixer.rows[0]->mute.onClick();
            mixer.rows[1]->solo.setToggleState (false, dontSendNotification);
            mixer.rows[1]->solo.onClick();
            expect (! te::getAudioTracks (*edit)[0]->isMuted (false));

            beginTest ("renaming: from the strip's name renames the track, and a rename elsewhere shows up");
            mixer.rows[0]->label.setText ("Voz", sendNotificationSync);
            expectEquals (te::getAudioTracks (*edit)[0]->getName(), String ("Voz"));
            te::getAudioTracks (*edit)[1]->setName ("Guitarra");
            expectEquals (mixer.rows[1]->label.getText(), String ("Guitarra"));
            mixer.rows[0]->label.setText ("   ", sendNotificationSync); // empty: keeps the name
            expectEquals (te::getAudioTracks (*edit)[0]->getName(), String ("Voz"));
            expectEquals (mixer.rows[0]->label.getText(), String ("Voz"));

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
