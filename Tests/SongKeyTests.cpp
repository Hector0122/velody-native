#include <JuceHeader.h>
#include "../Source/SongKey.h"

// The key estimated from the chords, and what the "Tono" control shows when transposing.
struct SongKeyTests : UnitTest
{
    SongKeyTests() : UnitTest ("Song key") {}

    static std::vector<Metronome::Chord> progression (StringArray names, int repeats = 4)
    {
        std::vector<Metronome::Chord> out;
        double t = 0;
        for (int r = 0; r < repeats; ++r)
            for (auto& n : names)
            {
                out.push_back ({ t, n });
                t += 2.0;
            }
        return out;
    }

    void runTest() override
    {
        using namespace SongKey;

        beginTest ("chord names: roots with sharps/flats, minor vs maj");
        expect (parse ("F#m7").root == 6 && parse ("F#m7").minor);
        expect (parse ("Bbmaj7").root == 10 && ! parse ("Bbmaj7").minor);
        expect (parse ("Ebmin").root == 3 && parse ("Ebmin").minor);
        expect (parse ("C").root == 0 && ! parse ("C").minor);
        expect (parse ("N").root < 0 && parse ("").root < 0);

        beginTest ("typical progressions give their key");
        expectEquals (SongKey::name (estimate (progression ({ "A", "D", "E", "F#m" }))), String ("A"));
        expectEquals (SongKey::name (estimate (progression ({ "G", "C", "D", "Em" }))), String ("G"));
        expectEquals (SongKey::name (estimate (progression ({ "Am", "Dm", "E", "Am" }))), String ("Am"));
        expectEquals (SongKey::name (estimate (progression ({ "Em", "C", "G", "D", "Em" }))), String ("Em"));
        expectEquals (SongKey::name (estimate (progression ({ "C", "Am", "F", "G" }))), String ("C"));
        expect (! estimate ({}).valid());
        expect (! estimate (progression ({ "N" })).valid());

        beginTest ("the Tono control: original, then original → transposed (±n)");
        Key a { 9, false };
        expectEquals (describe (a, 0), String ("A"));
        expectEquals (describe (a, 2), String (u8"A → B (+2)"));
        expectEquals (describe (a, -2), String (u8"A → G (-2)"));
        expectEquals (describe ({ 4, true }, 1), String (u8"Em → Fm (+1)"));
        expectEquals (describe (Key(), 0), String ("Original"));
        expectEquals (describe (Key(), -3), String ("-3 st"));

        beginTest ("tempo: any target, e.g. 120 BPM practised at 110");
        expectWithinAbsoluteError (Metronome::clampSpeed (110.0 / 120.0) * 120.0, 110.0, 0.1);
        expectWithinAbsoluteError (Metronome::clampSpeed (100.0 / 143.0) * 143.0, 100.0, 0.1);
        expectEquals (Metronome::clampSpeed (3.0), 1.5);
    }
};

static SongKeyTests songKeyTests;
