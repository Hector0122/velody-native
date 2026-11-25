#include <JuceHeader.h>
#include "../Source/Mixing.h"
#include "../Source/SongEdit.h"
#include "../Source/AppSettings.h"
#include "TestAudio.h"

// Effect buses and sends (down to the rendered audio), automation clearing, and the settings merge.
struct MixingTests : UnitTest
{
    MixingTests() : UnitTest ("Mixing (buses, sends, automation)") {}

    static double rms (te::Engine& engine, const File& f)
    {
        std::unique_ptr<AudioFormatReader> r (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (f));
        if (r == nullptr) return -1;
        AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, b.getNumSamples(), 0, true, true);
        return b.getRMSLevel (0, 0, b.getNumSamples());
    }

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-mixing");
        dir.createDirectory();
        TestAudio::writeSine (dir.getChildFile ("guitar.wav"), 1.0, 440.0);
        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("p.tracktionedit"));
        edit->ensureNumberOfAudioTracks (1);
        auto* guitar = te::getAudioTracks (*edit)[0];
        SongEdit::insertWholeFile (*guitar, dir.getChildFile ("guitar.wav"));

        auto render = [&]
        {
            auto out = dir.getChildFile ("mix.wav");
            out.deleteFile();
            te::Renderer::Parameters p (*edit);
            p.destFile = out;
            p.time = { te::TimePosition(), te::TimePosition::fromSeconds (1.0) };
            p.audioFormat = engine.getAudioFileFormatManager().getWavFormat();
            std::atomic<bool> done { false };
            auto handle = te::EditRenderer::render (std::move (p), [&] (auto) { done = true; });
            for (int i = 0; i < 500 && ! done; ++i)
                MessageManager::getInstance()->runDispatchLoopUntil (10);
            return rms (engine, out);
        };

        beginTest ("a bus: its own track with the return of a new bus number");
        expect (Mixing::buses (*edit).empty());
        auto* bus = Mixing::addBus (*edit, "Reverb", {});
        expect (bus != nullptr && bus->getName() == "Bus: Reverb");
        auto list = Mixing::buses (*edit);
        expect (list.size() == 1 && list[0].number == 0 && list[0].track == bus);
        auto* bus2 = Mixing::addBus (*edit, "Delay", {});
        expectEquals (Mixing::buses (*edit)[1].number, 1);
        ignoreUnused (bus2);

        beginTest ("sends: one per bus, starts off, its level can be set");
        expectEquals (Mixing::sendLevelDb (*guitar, 0), -100.0f);
        auto* send = Mixing::sendTo (*guitar, 0);
        expect (send != nullptr && Mixing::sendTo (*guitar, 0) == send);
        expect (Mixing::sendLevelDb (*guitar, 0) < -90.0f);
        Mixing::setSendLevelDb (*guitar, 0, -6.0f);
        expectWithinAbsoluteError (Mixing::sendLevelDb (*guitar, 0), -6.0f, 0.1f);

        beginTest ("rendered: sending to the bus adds its signal to the mix");
        Mixing::setSendLevelDb (*guitar, 0, -100.0f);
        const double dry = render();
        Mixing::setSendLevelDb (*guitar, 0, 0.0f);
        const double wet = render();
        expectGreaterThan (dry, 0.01);
        expectWithinAbsoluteError (wet / dry, 2.0, 0.25); // the same signal again through the bus: ~twice

        beginTest ("automation: write mode toggles; a track's automation is drawn data and can be cleared");
        Mixing::setWriting (*edit, true);
        expect (Mixing::isWriting (*edit));
        Mixing::setWriting (*edit, false);
        expect (! Mixing::isWriting (*edit));
        auto params = Mixing::faderParameters (*guitar);
        expectEquals (params.size(), 2);
        expect (! Mixing::hasAutomation (*guitar));
        params[0]->getCurve().addPoint (te::TimePosition::fromSeconds (0.0), 0.8f, 0.0f, nullptr);
        params[0]->getCurve().addPoint (te::TimePosition::fromSeconds (0.5), 0.2f, 0.0f, nullptr);
        expect (Mixing::hasAutomation (*guitar));
        const double automated = render(); // fader goes down: quieter than the plain mix
        expectLessThan (automated, wet);
        expectEquals (Mixing::clearAutomation (*guitar), 1);
        expect (! Mixing::hasAutomation (*guitar));

        beginTest ("empty tracks: removed (keeping a few for recording), the rest untouched, undoable");
        {
            edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 5); // 5 empty ones
            const int before = te::getAudioTracks (*edit).size();
            edit->getUndoManager().beginNewTransaction();
            expectEquals (SongEdit::removeEmptyTracks (*edit, 2), 3);
            expectEquals (te::getAudioTracks (*edit).size(), before - 3);
            expect (te::getAudioTracks (*edit).contains (guitar)); // has a clip
            for (auto& b : Mixing::buses (*edit))
                expect (te::getAudioTracks (*edit).contains (b.track)); // have their return plugin
            edit->getUndoManager().undo();
            expectEquals (te::getAudioTracks (*edit).size(), before);
        }

        beginTest ("settings: the old ~/Velody file merges into ~/.config/Velody, its values winning");
        {
            auto oldF = dir.getChildFile ("home/Velody/Velody.settings"), newF = dir.getChildFile ("cfg/Velody/Velody.settings");
            oldF.getParentDirectory().createDirectory();
            newF.getParentDirectory().createDirectory();
            oldF.replaceWithText (R"(<?xml version="1.0"?><PROPERTIES><VALUE name="lastSong" val="Mine"/><VALUE name="mixerOpen" val="0"/></PROPERTIES>)");
            newF.replaceWithText (R"(<?xml version="1.0"?><PROPERTIES><VALUE name="lastSong" val="Test"/><VALUE name="clickSound" val="2"/></PROPERTIES>)");
            expect (AppSettings::migrate (oldF, newF));
            expect (! oldF.exists());
            auto xml = XmlDocument::parse (newF);
            StringPairArray v;
            for (auto* e : xml->getChildWithTagNameIterator ("VALUE"))
                v.set (e->getStringAttribute ("name"), e->getStringAttribute ("val"));
            expect (v["lastSong"] == "Mine" && v["mixerOpen"] == "0" && v["clickSound"] == "2");
            expect (! AppSettings::migrate (oldF, newF)); // nothing left to move
        }

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static MixingTests mixingTests;
