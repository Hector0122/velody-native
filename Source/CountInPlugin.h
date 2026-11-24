#pragma once
#include <JuceHeader.h>
#include "Metronome.h"

namespace te = tracktion;

// Master-bus plugin for the playback count-in: armed before Play, it mutes the song and plays the
// count-in clicks until the start position, then gets out of the way (sample-accurate, like Electron).
class CountInPlugin : public te::Plugin
{
public:
    static inline const char* xmlTypeName = "velodyCountIn";

    CountInPlugin (te::PluginCreationInfo info) : te::Plugin (info) {}
    ~CountInPlugin() override { notifyListenersOfDeletion(); }

    juce::String getName() const override              { return "Count-in"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return getName(); }
    BusLayout getBusses() const override               { return BusLayout::singlePassThrough(); }

    void initialise (const te::PluginInitialisationInfo& info) override { sampleRate = info.sampleRate; }
    void deinitialise() override {}

    void arm (std::vector<Metronome::Click> cs, double until, Metronome::Sound s = Metronome::Sound::woodblock)
    {
        const SpinLock::ScopedLockType sl (lock);
        sound = s;
        clicks = std::move (cs);
        gateEnd = until;
        armed = true;
    }

    void disarm()
    {
        const SpinLock::ScopedLockType sl (lock);
        armed = false;
    }

    bool isArmed() const { return armed; }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || ! fc.isPlaying)
            return;

        const SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked() || ! armed)
            return;

        const double t0 = fc.editTime.getStart().inSeconds();
        if (t0 >= gateEnd)
        {
            armed = false; // song has started; later jumps play normally
            return;
        }

        Metronome::renderCountIn (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples, t0, sampleRate, clicks, gateEnd, rng, sound);
    }

    static CountInPlugin* findOrInsert (te::Edit& edit)
    {
        auto& list = edit.getMasterPluginList();
        for (auto p : list)
            if (auto c = dynamic_cast<CountInPlugin*> (p))
                return c;

        auto p = edit.getPluginCache().createNewPlugin (xmlTypeName, {});
        list.insertPlugin (p, list.size(), nullptr);
        return dynamic_cast<CountInPlugin*> (p.get());
    }

private:
    SpinLock lock;
    std::vector<Metronome::Click> clicks;
    double gateEnd = 0, sampleRate = 44100;
    Metronome::Sound sound = Metronome::Sound::woodblock;
    std::atomic<bool> armed { false };
    Random rng { 2 };
};
