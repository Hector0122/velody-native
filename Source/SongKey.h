#pragma once
#include <JuceHeader.h>
#include "Metronome.h"

// The song's key, estimated from its chords (the analysis has chords, not the key): the major or minor key
// whose diatonic chords cover most of the song's time, its tonic chord counting extra, and the first and last
// chords too (songs tend to start and end on the tonic). An estimate: shown as the song's key in "Tono".
// Then the key after transposing, for the "Tono" control: "A", or "A → B (+2)".
namespace SongKey
{
    inline const StringArray noteNames { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

    struct Chord { int root = -1; bool minor = false; };

    // "F#m7" → F#, minor; "Bbmaj7" → Bb, major; "N" (no chord) → root -1.
    inline Chord parse (const String& name)
    {
        Chord c;
        auto n = name.trim();
        if (n.isEmpty())
            return c;
        static const String letters = "C D EF G A B";
        const int base = letters.indexOfChar (CharacterFunctions::toUpperCase (n[0]));
        if (base < 0 || n[0] == ' ')
            return c;
        int root = base, i = 1;
        if (i < n.length() && (n[i] == '#' || n[i] == 0x266f)) { ++root; ++i; }
        else if (i < n.length() && (n[i] == 'b' || n[i] == 0x266d)) { --root; ++i; }
        c.root = (root + 12) % 12;
        auto rest = n.substring (i);
        c.minor = (rest.startsWith ("m") && ! rest.startsWith ("maj")) || rest.startsWith ("min");
        return c;
    }

    struct Key { int tonic = -1; bool minor = false; bool valid() const { return tonic >= 0; } };

    inline String name (const Key& k) { return k.valid() ? noteNames[k.tonic] + (k.minor ? "m" : "") : String(); }

    // Diatonic triads of a key: degree offsets with their quality (minor also takes the major V).
    inline bool isDiatonic (const Key& k, const Chord& c)
    {
        static const std::pair<int, bool> major[] { { 0, false }, { 2, true }, { 4, true }, { 5, false }, { 7, false }, { 9, true } };
        static const std::pair<int, bool> minor[] { { 0, true }, { 3, false }, { 5, true }, { 7, true }, { 7, false }, { 8, false }, { 10, false } };
        const int rel = (c.root - k.tonic + 12) % 12;
        if (k.minor) { for (auto& [o, m] : minor) if (o == rel && m == c.minor) return true; }
        else         { for (auto& [o, m] : major) if (o == rel && m == c.minor) return true; }
        return false;
    }

    // `songEnd`: the last chord lasts until there.
    inline Key estimate (const std::vector<Metronome::Chord>& chords, double songEnd = 0)
    {
        std::vector<std::pair<Chord, double>> timed;
        for (size_t i = 0; i < chords.size(); ++i)
        {
            const double end = i + 1 < chords.size() ? chords[i + 1].time : jmax (chords[i].time + 2.0, songEnd);
            auto c = parse (chords[i].chord);
            if (c.root >= 0 && end > chords[i].time)
                timed.push_back ({ c, end - chords[i].time });
        }
        if (timed.empty())
            return {};

        double total = 0;
        for (auto& [c, d] : timed)
            total += d;
        const auto first = timed.front().first, last = timed.back().first;

        Key best;
        double bestScore = -1;
        for (bool minor : { false, true }) // majors first: on a full tie the major key wins (more common)
            for (int t = 0; t < 12; ++t)
            {
                Key k { t, minor };
                double score = 0;
                for (auto& [c, d] : timed)
                {
                    if (isDiatonic (k, c))
                        score += d;
                    if (c.root == t && c.minor == minor)
                        score += d * 0.5; // the tonic chord itself counts extra
                }
                // Songs tend to start and end on the tonic (that's what tells A from its relative F#m).
                if (first.root == t && first.minor == minor) score += total * 0.25;
                if (last.root == t && last.minor == minor)   score += total * 0.25;
                if (score > bestScore + 1e-9)
                {
                    bestScore = score;
                    best = k;
                }
            }
        return best;
    }

    inline Key transposed (const Key& k, int semitones)
    {
        return k.valid() ? Key { ((k.tonic + semitones) % 12 + 12) % 12, k.minor } : k;
    }

    // What the "Tono" control shows.
    inline String describe (const Key& k, int semitones)
    {
        const String shift = semitones == 0 ? String() : " (" + String (semitones > 0 ? "+" : "") + String (semitones) + ")";
        if (! k.valid())
            return semitones == 0 ? String ("Original") : String (semitones > 0 ? "+" : "") + String (semitones) + " st";
        if (semitones == 0)
            return name (k);
        return name (k) + String (u8" → ") + name (transposed (k, semitones)) + shift;
    }
}
