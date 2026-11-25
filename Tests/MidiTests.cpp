#include <JuceHeader.h>
#include "../Source/Instruments.h"
#include "../Source/ui/PianoRoll.h"

// MIDI: an instrument track (built-in 4OSC) that really sounds, a MIDI keyboard (a virtual one here)
// routed to the selected instrument track only, empty clips on bar lines, and the piano roll's editing.
struct MidiTests : UnitTest
{
    MidiTests() : UnitTest ("MIDI and instruments") {}

    static double rms (te::Engine& engine, const File& f)
    {
        std::unique_ptr<AudioFormatReader> r (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (f));
        if (r == nullptr)
            return -1;
        AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, b.getNumSamples(), 0, true, true);
        return b.getRMSLevel (0, 0, b.getNumSamples());
    }

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-midi");
        dir.createDirectory();
        auto edit = te::createEmptyEdit (engine, dir.getChildFile ("p.tracktionedit"));
        edit->ensureNumberOfAudioTracks (1);

        beginTest ("instruments: the built-in 4OSC is always offered first");
        auto choices = Instruments::available (engine);
        expect (! choices.empty() && choices[0].builtIn);

        beginTest ("an instrument track: named after it, the synth first in its chain");
        auto* synth = Instruments::addInstrumentTrack (*edit, choices[0]);
        expect (synth != nullptr && synth->getName() == "4OSC");
        expect (Instruments::isInstrumentTrack (*synth));
        expect (! Instruments::isInstrumentTrack (*te::getAudioTracks (*edit)[0]));
        expectEquals (te::getAudioTracks (*edit).size(), 2);

        beginTest ("empty clip: on bar lines, 4 bars long");
        auto clip = Instruments::addEmptyClip (*synth, te::TimePosition::fromSeconds (2.7)); // 120 BPM: bar 2 starts at 2 s
        expect (clip != nullptr);
        expectWithinAbsoluteError (clip->getPosition().getStart().inSeconds(), 2.0, 1e-6);
        expectWithinAbsoluteError (clip->getLengthInBeats().inBeats(), 16.0, 1e-6);

        beginTest ("piano roll geometry: beats ↔ x, notes ↔ y, and the grid");
        PianoRoll::Geometry g;
        expectWithinAbsoluteError (g.xToBeat (g.beatToX (2.5)), 2.5, 1e-6);
        expectEquals (g.yToNote ((float) g.noteToY (60) + 3.0f), 60);
        expectWithinAbsoluteError (PianoRoll::floorTo (1.37, 0.25), 1.25, 1e-9);
        expectWithinAbsoluteError (PianoRoll::nearestTo (1.37, 0.25), 1.25, 1e-9);
        expectWithinAbsoluteError (PianoRoll::nearestTo (1.40, 0.25), 1.5, 1e-9);
        expect (PianoRoll::isBlackKey (61) && ! PianoRoll::isBlackKey (60));
        expectEquals (PianoRoll::noteName (60), String ("C4"));

        beginTest ("piano roll: click adds a note on the grid, drag moves it, wheel sets velocity, right-click deletes");
        {
            PianoRoll::Editor ed (*clip);
            auto at = [&] (double beat, int note) { return Point<float> (ed.geometry.beatToX (beat), (float) ed.geometry.noteToY (note) + 5.0f); };
            auto event = [&] (Point<float> p, Point<float> down, ModifierKeys mods = {})
            {
                return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods, 1.0f, 0, 0, 0, 0, &ed, &ed,
                                   Time::getCurrentTime(), down, Time::getCurrentTime(), 1, false);
            };
            ed.mouseDown (event (at (1.1, 60), at (1.1, 60)));
            ed.mouseUp (event (at (1.1, 60), at (1.1, 60)));
            // getNotes() is a sorted copy refreshed on each call: ask again every time.
            auto notes = [&] { return clip->getSequence().getNotes(); };
            expectEquals (notes().size(), 1);
            expect (notes()[0]->getNoteNumber() == 60 && std::abs (notes()[0]->getStartBeat().inBeats() - 1.0) < 1e-6);

            // Drag it one beat later and 2 semitones up.
            auto from = at (1.1, 60);
            ed.mouseDown (event (from, from));
            ed.mouseDrag (event (at (2.1, 62), from));
            ed.mouseUp (event (at (2.1, 62), from));
            expect (notes()[0]->getNoteNumber() == 62 && std::abs (notes()[0]->getStartBeat().inBeats() - 2.0) < 1e-6);

            const int v = notes()[0]->getVelocity();
            MouseWheelDetails up {};
            up.deltaY = 1.0f;
            ed.mouseWheelMove (event (at (2.1, 62), at (2.1, 62)), up);
            expectEquals (notes()[0]->getVelocity(), jmin (127, v + 8));

            ed.mouseDown (event (at (2.1, 62), at (2.1, 62), ModifierKeys::rightButtonModifier));
            expectEquals (notes().size(), 0);
            edit->getUndoManager().undo(); // Ctrl+Z brings it back
            expectEquals (notes().size(), 1);
        }

        beginTest ("quantize: starts and lengths to the grid");
        auto& list = clip->getSequence();
        list.addNote (64, te::BeatPosition::fromBeats (3.13), te::BeatDuration::fromBeats (0.2), 90, 0, nullptr);
        PianoRoll::quantize (list, 0.25, nullptr);
        for (auto* n : list.getNotes())
        {
            expectWithinAbsoluteError (std::fmod (n->getStartBeat().inBeats(), 0.25), 0.0, 1e-9);
            expectGreaterOrEqual (n->getLengthBeats().inBeats(), 0.25);
        }

        beginTest ("MIDI keyboards (a virtual one here) feed the chosen instrument track once, and nothing else");
        expect (engine.getDeviceManager().createVirtualMidiDevice ("Velody test keyboard").wasOk());
        Instruments::enableMidiInputs (engine.getDeviceManager());
        MessageManager::getInstance()->runDispatchLoopUntil (50);
        edit->getTransport().freePlaybackContext();
        edit->getTransport().ensureContextAllocated();
        auto* second = Instruments::addInstrumentTrack (*edit, choices[0]);
        expectGreaterThan (Instruments::routeMidiInputs (*edit, *second), 0);
        auto targetsOf = [&] (te::AudioTrack& t)
        {
            int n = 0;
            for (auto in : edit->getAllInputDevices())
                if (Instruments::isMidi (in->getInputDevice()) && te::isOnTargetTrack (*in, t, 0))
                    ++n;
            return n;
        };
        expectEquals (targetsOf (*second), 1);
        Instruments::routeMidiInputs (*edit, *synth);
        expectEquals (targetsOf (*synth), 1);
        expectEquals (targetsOf (*second), 0);

        beginTest ("the instrument really sounds: a rendered MIDI note isn't silent");
        {
            auto out = dir.getChildFile ("midi.wav");
            te::Renderer::Parameters p (*edit);
            p.destFile = out;
            p.time = clip->getPosition().time;
            p.audioFormat = engine.getAudioFileFormatManager().getWavFormat();
            std::atomic<bool> done { false };
            auto handle = te::EditRenderer::render (std::move (p), [&] (auto) { done = true; });
            for (int i = 0; i < 500 && ! done; ++i)
                MessageManager::getInstance()->runDispatchLoopUntil (10);
            expectGreaterThan (rms (engine, out), 0.001);
        }

        clip = nullptr;
        edit = nullptr; // before the device it was using
        for (auto& in : engine.getDeviceManager().getMidiInDevices())
            if (auto* v = dynamic_cast<te::VirtualMidiInputDevice*> (in.get()))
                engine.getDeviceManager().deleteVirtualMidiDevice (*v);
        dir.deleteRecursively();
    }
};

static MidiTests midiTests;
