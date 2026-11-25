#include <JuceHeader.h>
#include "../Source/LiveMonitoring.h"

// Low latency for playing through plugins: the buffer choices, the round trip they mean, and the
// PipeWire command (forcing the quantum while Velody runs, then giving it back).
struct LiveMonitoringTests : UnitTest
{
    LiveMonitoringTests() : UnitTest ("Live monitoring / latency") {}

    void runTest() override
    {
        using namespace LiveMonitoring;

        beginTest ("buffers and the round trip they give");
        expect (buffers == Array<int> { 64, 128, 256, 512 });
        expectWithinAbsoluteError (roundTripMs (128, 48000.0), 5.33, 0.01);
        expectWithinAbsoluteError (roundTripMs (64, 48000.0), 2.67, 0.01);
        expectEquals (roundTripMs (128, 0.0), 0.0);

        beginTest ("PipeWire: force the quantum, and 0 gives it back to automatic");
        expectEquals (forceQuantumArgs (128).joinIntoString (" "), String ("/usr/bin/pw-metadata -n settings 0 clock.force-quantum 128"));
        StringArray ran;
        releaseBuffer ([&] (const StringArray& a) { ran.add (a.joinIntoString (" ")); return 0; });
        expect (ran.size() == 1 && ran[0].endsWith ("clock.force-quantum 0"));

        beginTest ("JACK is recognised by its device type name");
        expect (isJack ("JACK") && isJack ("JACK Audio Connection Kit") && ! isJack ("ALSA"));
    }
};

static LiveMonitoringTests liveMonitoringTests;
