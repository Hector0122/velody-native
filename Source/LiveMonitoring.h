#pragma once
#include <JuceHeader.h>
#include "PythonWorker.h"

// Playing live through plugins (NAM, Ratatouille…): low latency and monitoring.
// With PipeWire's JACK (how the .deb starts Velody: `pw-jack velody-native`) the buffer is PipeWire's
// quantum, the same for every app, so Velody forces it while it runs and gives it back on exit. With a
// plain ALSA device it's the device's buffer size.
namespace LiveMonitoring
{
    inline const Array<int> buffers { 64, 128, 256, 512 };

    // Round trip in ms of a buffer at a sample rate (in + out, the part a buffer change moves).
    inline double roundTripMs (int buffer, double sampleRate) { return sampleRate > 0 ? 2000.0 * buffer / sampleRate : 0.0; }

    inline StringArray forceQuantumArgs (int quantum)
    {
        return { "/usr/bin/pw-metadata", "-n", "settings", "0", "clock.force-quantum", String (jmax (0, quantum)) };
    }

    inline bool isJack (const String& deviceType) { return deviceType.containsIgnoreCase ("JACK"); }

    // Sets the buffer: PipeWire's quantum under JACK, else the device's own buffer size. Empty = done.
    inline String setBuffer (AudioDeviceManager& dm, int buffer, std::function<int (const StringArray&)> run = {})
    {
        if (! run)
            run = [] (const StringArray& a) { return PythonWorker::drain (a, [] (const String&) {}, [] { return false; }, 5000).code; };
        if (isJack (dm.getCurrentAudioDeviceType()))
            return run (forceQuantumArgs (buffer)) == 0 ? String() : String (u8"No se pudo cambiar el buffer de PipeWire (pw-metadata)");

        auto setup = dm.getAudioDeviceSetup();
        setup.bufferSize = buffer;
        auto error = dm.setAudioDeviceSetup (setup, true);
        return error;
    }

    // On exit: PipeWire's quantum back to automatic (only if Velody changed it).
    inline void releaseBuffer (std::function<int (const StringArray&)> run = {})
    {
        if (! run)
            run = [] (const StringArray& a) { return PythonWorker::drain (a, [] (const String&) {}, [] { return false; }, 5000).code; };
        run (forceQuantumArgs (0));
    }

    // The first time, JACK (PipeWire) if it's there: low latency and other apps keep their sound.
    inline bool preferJack (AudioDeviceManager& dm)
    {
        for (auto* type : dm.getAvailableDeviceTypes())
            if (isJack (type->getTypeName()) && ! isJack (dm.getCurrentAudioDeviceType()))
            {
                type->scanForDevices();
                if (type->getDeviceNames().isEmpty())
                    return false; // no JACK server (Velody wasn't started with pw-jack)
                dm.setCurrentAudioDeviceType (type->getTypeName(), true);
                return isJack (dm.getCurrentAudioDeviceType()) && dm.getCurrentAudioDevice() != nullptr;
            }
        return false;
    }
}
