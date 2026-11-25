#pragma once
#include <JuceHeader.h>

namespace te = tracktion;

// Mixing like REAPER: effect buses (a "Bus" track with one reverb/delay that several tracks feed through a
// send knob, post-fader) and automation (Automatizar on, move a fader or knob while it plays: the move is
// written as a curve the track then follows; drawn over the track, and can be cleared).
namespace Mixing
{
    struct Bus { int number = 0; te::AudioTrack* track = nullptr; String name; };

    inline te::AuxReturnPlugin* returnOf (te::AudioTrack& t)
    {
        for (auto p : t.pluginList)
            if (auto* r = dynamic_cast<te::AuxReturnPlugin*> (p))
                return r;
        return nullptr;
    }

    inline std::vector<Bus> buses (te::Edit& edit)
    {
        std::vector<Bus> out;
        for (auto t : te::getAudioTracks (edit))
            if (auto* r = returnOf (*t))
                out.push_back ({ r->busNumber.get(), t, t->getName() });
        return out;
    }

    inline int nextBusNumber (te::Edit& edit)
    {
        int n = 0;
        for (auto& b : buses (edit))
            n = jmax (n, b.number + 1);
        return n;
    }

    // A new track "Bus: <name>": the return of a new bus, then the effect (if any).
    inline te::AudioTrack* addBus (te::Edit& edit, const String& name, te::Plugin::Ptr effect)
    {
        const int number = nextBusNumber (edit);
        edit.ensureNumberOfAudioTracks (te::getAudioTracks (edit).size() + 1);
        auto* t = te::getAudioTracks (edit).getLast();
        if (t == nullptr)
            return nullptr;
        t->setName ("Bus: " + name);
        if (auto ret = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {}))
        {
            if (auto* r = dynamic_cast<te::AuxReturnPlugin*> (ret.get()))
                r->busNumber = number;
            t->pluginList.insertPlugin (ret, 0, nullptr);
        }
        if (effect != nullptr)
            t->pluginList.insertPlugin (effect, 1, nullptr);
        return t;
    }

    inline te::AuxSendPlugin* sendOf (te::AudioTrack& t, int bus)
    {
        for (auto p : t.pluginList)
            if (auto* s = dynamic_cast<te::AuxSendPlugin*> (p); s != nullptr && s->busNumber.get() == bus)
                return s;
        return nullptr;
    }

    // The send from `t` to `bus`, created at the end of its chain (after the fader: post-fader, like REAPER).
    inline te::AuxSendPlugin* sendTo (te::AudioTrack& t, int bus)
    {
        if (auto* s = sendOf (t, bus))
            return s;
        auto p = t.edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});
        if (auto* s = dynamic_cast<te::AuxSendPlugin*> (p.get()))
        {
            s->busNumber = bus;
            s->setGainDb (-100.0f); // starts silent: the knob brings it up
            t.pluginList.insertPlugin (p, -1, nullptr);
        }
        return sendOf (t, bus);
    }

    inline float sendLevelDb (te::AudioTrack& t, int bus)
    {
        auto* s = sendOf (t, bus);
        return s != nullptr ? s->getGainDb() : -100.0f;
    }

    inline void setSendLevelDb (te::AudioTrack& t, int bus, float db)
    {
        if (auto* s = sendTo (t, bus))
            s->setGainDb (jlimit (-100.0f, 6.0f, db));
    }

    // Automation -------------------------------------------------------------------------------------

    inline void setWriting (te::Edit& edit, bool on) { edit.getAutomationRecordManager().setWritingAutomation (on); }
    inline bool isWriting (te::Edit& edit)           { return edit.getAutomationRecordManager().isWritingAutomation(); }

    // The track's volume and pan (its fader plugin): what the mixer moves.
    inline Array<te::AutomatableParameter*> faderParameters (te::AudioTrack& t)
    {
        Array<te::AutomatableParameter*> out;
        if (auto* vp = t.getVolumePlugin())
            out.add (vp->volParam.get(), vp->panParam.get());
        return out;
    }

    inline bool hasAutomation (te::AudioTrack& t)
    {
        for (auto* p : t.getAllAutomatableParams())
            if (p->hasAutomationPoints())
                return true;
        return false;
    }

    inline int clearAutomation (te::AudioTrack& t)
    {
        int n = 0;
        for (auto* p : t.getAllAutomatableParams())
            if (p->hasAutomationPoints())
            {
                p->getCurve().clear (&t.edit.getUndoManager());
                ++n;
            }
        return n;
    }
}
