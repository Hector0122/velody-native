#pragma once
#include <JuceHeader.h>

namespace te = tracktion;

// MIDI with instruments (REAPER-style): an instrument track is an audio track whose first plugin is a
// synth (Surge XT, AVLDrums, sfizz… or Tracktion's built-in 4OSC, always there). MIDI keyboards (e.g. an
// M-Audio Oxygen, class compliant) feed the selected instrument track: armed, and heard live (monitor
// automatic) so playing it sounds right away. Recording makes MIDI clips; empty clips can be drawn too.
namespace Instruments
{
    struct Choice
    {
        String name;
        PluginDescription description; // empty for the built-in one
        bool builtIn = false;
    };

    inline const String builtInName { "4OSC (sintetizador integrado)" };

    // The built-in synth first, then every scanned instrument plugin, by name.
    inline std::vector<Choice> available (te::Engine& engine)
    {
        std::vector<Choice> out { { builtInName, {}, true } };
        auto types = engine.getPluginManager().knownPluginList.getTypes();
        std::sort (types.begin(), types.end(), [] (auto& a, auto& b) { return a.name.compareNatural (b.name) < 0; });
        for (auto& d : types)
            if (d.isInstrument)
                out.push_back ({ d.name + "  (" + d.pluginFormatName + ")", d, false });
        return out;
    }

    inline te::Plugin::Ptr createPlugin (te::Edit& edit, const Choice& c)
    {
        if (c.builtIn)
            return edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
        return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, c.description);
    }

    inline bool isInstrumentTrack (te::AudioTrack& t)
    {
        for (auto p : t.pluginList)
            if (p->isSynth())
                return true;
        return false;
    }

    // A new track at the end, named after the instrument, with it as its first plugin.
    inline te::AudioTrack* addInstrumentTrack (te::Edit& edit, const Choice& c)
    {
        edit.ensureNumberOfAudioTracks (te::getAudioTracks (edit).size() + 1);
        auto* t = te::getAudioTracks (edit).getLast();
        if (t == nullptr)
            return nullptr;
        t->setName (c.builtIn ? String ("4OSC") : c.description.name);
        if (auto p = createPlugin (edit, c))
            t->pluginList.insertPlugin (p, 0, nullptr);
        return t;
    }

    // MIDI inputs on, heard live while armed (automatic monitoring), like REAPER's armed instrument track.
    inline void enableMidiInputs (te::DeviceManager& dm)
    {
        dm.dispatchPendingUpdates(); // a keyboard plugged in just now
        for (auto& in : dm.getMidiInDevices())
        {
            in->setMonitorMode (te::InputDevice::MonitorMode::automatic);
            in->setEnabled (true);
        }
    }

    inline bool isMidi (te::InputDevice& d)
    {
        return d.getDeviceType() == te::InputDevice::physicalMidiDevice || d.getDeviceType() == te::InputDevice::virtualMidiDevice;
    }

    // Tracktion's "All MIDI Ins" merges every keyboard plugged in (now or later): only that one feeds the
    // instrument track, so a note never plays twice; when it isn't there, each keyboard does.
    inline int routeMidiInputs (te::Edit& edit, te::AudioTrack& target)
    {
        bool haveAll = false;
        for (auto instance : edit.getAllInputDevices())
            haveAll |= instance->getInputDevice().getDeviceID() == "all_midi_in";

        int routed = 0;
        for (auto instance : edit.getAllInputDevices())
        {
            auto& device = instance->getInputDevice();
            if (! isMidi (device))
                continue;
            for (auto t : te::getAudioTracks (edit))
                [[maybe_unused]] auto r = instance->removeTarget (t->itemID, &edit.getUndoManager());
            const bool feeds = haveAll ? device.getDeviceID() == "all_midi_in" : true;
            if (! feeds)
                continue;
            [[maybe_unused]] auto res = instance->setTarget (target.itemID, true, &edit.getUndoManager(), 0);
            instance->setRecordingEnabled (target.itemID, true);
            ++routed;
        }
        return routed;
    }

    // An empty MIDI clip of `bars` bars at `start` (rounded down to a bar), to draw notes in.
    inline te::MidiClip::Ptr addEmptyClip (te::AudioTrack& t, te::TimePosition start, int bars = 4)
    {
        auto& ts = t.edit.tempoSequence;
        auto* sig = ts.getTimeSig (0);
        const int beatsPerBar = sig != nullptr ? sig->numerator.get() : 4;
        const double startBeat = std::floor (ts.toBeats (start).inBeats() / beatsPerBar) * beatsPerBar;
        const auto from = ts.toTime (te::BeatPosition::fromBeats (startBeat));
        const auto to = ts.toTime (te::BeatPosition::fromBeats (startBeat + bars * beatsPerBar));
        return t.insertMIDIClip ("MIDI", { from, to }, nullptr);
    }
}
