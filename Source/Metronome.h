#pragma once
#include <JuceHeader.h>

// Port of the Electron metronome (velody/src/renderer/src/hooks/downbeat.ts, metronomeClicks, and
// handlers.ts clickSampleValue) so a song clicks on the same beats with the same accents.
namespace Metronome
{
    constexpr int beatsPerBar = 4;

    inline int mod (int n, int m) { return ((n % m) + m) % m; }

    struct Chord { double time; String chord; };

    // The analysis fields of a song's .velody sidecar.
    struct SongMeta
    {
        double bpm = 0, firstBeatOffset = 0;
        std::vector<double> beatTimes;
        int downbeatShift = 0; // manual "this is the 1" wins over the detected one
        int countInBars = 0;   // playbackCountInBars
        double practiceSpeed = 1;  // 0.5–1.5 in 0.05 steps
        int transposeSemitones = 0; // −12…+12
        std::vector<Chord> chords;  // changes in song time, sorted ("N" = no chord)
    };

    // Same clamps as Electron's pitchShifter.ts.
    inline double clampSpeed (double s)
    {
        if (! std::isfinite (s)) return 1;
        return jlimit (0.5, 1.5, std::round (s / 0.05) * 0.05);
    }

    inline int clampSemitones (double s) { return std::isfinite (s) ? jlimit (-12, 12, roundToInt (s)) : 0; }

    inline SongMeta readMeta (const File& songDir)
    {
        SongMeta m;
        auto json = JSON::parse (songDir.getChildFile (".velody"));
        if (! json.isObject())
            return m;

        m.bpm = json["bpm"];
        m.firstBeatOffset = json["firstBeatOffset"];
        if (auto* beats = json["beatTimes"].getArray())
            for (auto& b : *beats)
                m.beatTimes.push_back (b);

        m.countInBars = jlimit (0, 8, (int) json["playbackCountInBars"]);
        if (json.hasProperty ("practiceSpeed"))      m.practiceSpeed = clampSpeed (json["practiceSpeed"]);
        if (json.hasProperty ("transposeSemitones")) m.transposeSemitones = clampSemitones (json["transposeSemitones"]);

        if (auto* cs = json["chords"].getArray())
            for (auto& c : *cs)
                if ((c["time"].isDouble() || c["time"].isInt()) && c["chord"].isString())
                    m.chords.push_back ({ (double) c["time"], c["chord"].toString() });
        std::sort (m.chords.begin(), m.chords.end(), [] (auto& a, auto& b) { return a.time < b.time; });

        auto shift = json["downbeatShift"].isVoid() || json["downbeatShift"].isUndefined()
                       ? json["detectedDownbeatShift"] : json["downbeatShift"];
        m.downbeatShift = shift.isDouble() || shift.isInt() || shift.isInt64() ? mod (roundToInt ((double) shift), beatsPerBar) : 0;
        return m;
    }

    inline bool isGridAccent (int idx, int shift)     { return mod (idx - shift, beatsPerBar) == 0; }
    inline bool isConstantAccent (int beat, int shift) { return mod (beat - 1 - shift, beatsPerBar) == 0; }

    inline double medianBeatGap (const std::vector<double>& t, double fallback)
    {
        if (t.size() < 2)
            return fallback;
        std::vector<double> gaps;
        for (size_t i = 1; i < t.size(); ++i)
            gaps.push_back (t[i] - t[i - 1]);
        std::sort (gaps.begin(), gaps.end());
        return gaps[gaps.size() >> 1];
    }

    // Least-squares line through each beat's ±radius neighbours: removes librosa's frame jitter.
    inline std::vector<double> smoothBeatGrid (const std::vector<double>& t, int radius = 4)
    {
        const int n = (int) t.size();
        if (n < 3)
            return t;

        std::vector<double> out ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const int lo = jmax (0, i - radius), hi = jmin (n - 1, i + radius), m = hi - lo + 1;
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int j = lo; j <= hi; ++j)
            {
                sx += j; sy += t[(size_t) j]; sxx += (double) j * j; sxy += j * t[(size_t) j];
            }
            const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
            out[(size_t) i] = (sy - slope * sx) / m + slope * i;
        }
        return out;
    }

    struct Click { double time; bool accent; };

    // Every click over [0, duration]: the smoothed grid, extrapolated before/after with the median
    // gap; constant tempo from firstBeatOffset when there's no grid.
    inline std::vector<Click> clicks (const SongMeta& meta, double duration)
    {
        std::vector<Click> out;
        if (! (meta.bpm > 0) || ! std::isfinite (duration))
            return out;

        if (! meta.beatTimes.empty())
        {
            auto grid = smoothBeatGrid (meta.beatTimes);
            const double gap = medianBeatGap (grid, 60.0 / meta.bpm);
            const double first = grid.front();
            const int lastIdx = (int) grid.size() - 1;

            for (int i = -(int) std::floor (first / gap);; ++i)
            {
                const double time = i < 0 ? first + i * gap
                                  : i > lastIdx ? grid.back() + (i - lastIdx) * gap
                                  : grid[(size_t) i];
                if (time > duration)
                    break;
                out.push_back ({ time, isGridAccent (i, meta.downbeatShift) });
            }
        }
        else
        {
            for (int b = 1;; ++b)
            {
                const double time = meta.firstBeatOffset + b * 60.0 / meta.bpm;
                if (time > duration)
                    break;
                out.push_back ({ time, isConstantAccent (b, meta.downbeatShift) });
            }
        }
        return out;
    }

    // Count-in before playing from `position` (port of planCountIn): bars*4 clicks on the song's own
    // grid, ending one beat before the first beat at/after `position`, accenting each bar's first click.
    inline std::vector<Click> planCountIn (const SongMeta& meta, double position, int bars)
    {
        std::vector<Click> out;
        if (bars <= 0 || ! (meta.bpm > 0))
            return out;

        const double interval = 60.0 / meta.bpm;
        std::function<double (int)> beatAt;
        int entry;

        if (! meta.beatTimes.empty())
        {
            auto grid = smoothBeatGrid (meta.beatTimes);
            const double first = grid.front(), last = grid.back();
            const int lastIdx = (int) grid.size() - 1;
            const double gap = medianBeatGap (grid, interval);
            beatAt = [=] (int i) { return i < 0 ? first + i * gap : i > lastIdx ? last + (i - lastIdx) * gap : grid[(size_t) i]; };

            if (position <= first)     entry = (int) std::ceil ((position - first) / gap - 1e-9);
            else if (position > last)  entry = lastIdx + (int) std::ceil ((position - last) / gap - 1e-9);
            else                       entry = (int) (std::lower_bound (grid.begin(), grid.end(), position) - grid.begin());
        }
        else
        {
            beatAt = [=] (int n) { return meta.firstBeatOffset + n * interval; };
            entry = (int) std::ceil ((position - meta.firstBeatOffset) / interval - 1e-9);
        }

        const int total = bars * beatsPerBar;
        for (int j = 0; j < total; ++j)
            out.push_back ({ beatAt (entry - total + j), j % beatsPerBar == 0 });
        return out;
    }

    enum class Sound { woodblock, beep, clave, cowbell };
    inline const StringArray soundNames { "Woodblock", "Beep", "Clave", "Cencerro" };
    constexpr double clickLength = 0.03; // every sound fits in 30 ms

    // One click sample, beatPos = seconds since the click (silence outside [0, 30 ms)).
    inline float clickSampleValue (double beatPos, bool accent, Random& rng, Sound sound = Sound::woodblock)
    {
        if (beatPos < 0 || beatPos >= clickLength)
            return 0;

        const double t = beatPos, fade = 1 - t / clickLength; // fade keeps the 30 ms cut silent
        auto sine = [t] (double f) { return std::sin (MathConstants<double>::twoPi * f * t); };

        switch (sound)
        {
            case Sound::beep: // classic digital metronome: pure tone, accent a fifth higher
                return (float) (sine (accent ? 1980 : 1320) * jmin (1.0, t / 0.001) * std::exp (-t / 0.008) * fade * (accent ? 0.6 : 0.45));

            case Sound::clave: // dry, bright wood hit
                return (float) ((sine (accent ? 3000 : 2500) * std::exp (-t / 0.006) + (rng.nextDouble() * 2 - 1) * std::exp (-t / 0.0015) * 0.3)
                                * fade * (accent ? 0.7 : 0.5));

            case Sound::cowbell: // two inharmonic square-ish partials
            {
                auto sq = [] (double v) { return std::tanh (4 * v); };
                const double k = accent ? 1.25 : 1.0;
                return (float) ((sq (sine (540 * k)) + sq (sine (800 * k))) * 0.5 * std::exp (-t / 0.012) * fade * (accent ? 0.55 : 0.4));
            }

            case Sound::woodblock: break;
        }

        // Woodblock (Electron's): a falling tone plus a short noise burst.
        constexpr double decay = clickLength, noiseDur = 0.005;
        double sample = 0;
        if (beatPos >= 0 && beatPos < decay)
        {
            const double start = accent ? 900 : 650, end = accent ? 500 : 400;
            const double freq = start + (end - start) * (beatPos / decay);
            sample = std::sin (MathConstants<double>::twoPi * freq * beatPos) * (1 - beatPos / decay) * (accent ? 0.7 : 0.55);
        }
        if (beatPos >= 0 && beatPos < noiseDur)
            sample += (rng.nextDouble() * 2 - 1) * (1 - beatPos / noiseDur) * (accent ? 0.3 : 0.2);
        return (float) sample;
    }

    // Count-in on the master bus: before `gateEnd` the song is silenced and only `cs` is heard.
    // `blockStart` is the edit time of buffer sample `start`.
    inline void renderCountIn (AudioBuffer<float>& buf, int start, int num, double blockStart, double sampleRate,
                               const std::vector<Click>& cs, double gateEnd, Random& rng, Sound sound = Sound::woodblock)
    {
        for (int i = 0; i < num; ++i)
        {
            const double t = blockStart + i / sampleRate;
            if (t >= gateEnd)
                break;

            float v = 0;
            for (auto& c : cs)
                v += clickSampleValue (t - c.time, c.accent, rng, sound);

            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                buf.setSample (ch, start + i, v);
        }
    }

    // Mono 16-bit WAV of the clicks, `duration` seconds long.
    inline bool renderWav (const std::vector<Click>& cs, double duration, const File& out,
                           Sound sound = Sound::woodblock, double sampleRate = 44100)
    {
        AudioBuffer<float> buf (1, jmax (1, (int) std::ceil (duration * sampleRate)));
        buf.clear();
        Random rng (1); // fixed seed: same file every time

        const int clickLen = (int) std::ceil (clickLength * sampleRate);
        for (auto& c : cs)
        {
            const int start = (int) std::round (c.time * sampleRate);
            for (int i = 0; i < clickLen && start + i < buf.getNumSamples(); ++i)
                if (start + i >= 0)
                    buf.addSample (0, start + i, clickSampleValue (i / sampleRate, c.accent, rng, sound));
        }

        out.deleteFile();
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (out), sampleRate, 1, 16, {}, 0));
        return w != nullptr && w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }
}
