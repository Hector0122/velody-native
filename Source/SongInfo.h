#pragma once
#include <JuceHeader.h>
#include "Metronome.h"
#include "Theme.h"

// Ports of velody/src/renderer/src/utils.ts: where the music starts, and the chord "now → next".
namespace SongInfo
{
    constexpr float musicStartThreshold = 0.008f; // ~-42 dBFS
    constexpr double musicStartMaxScan = 30, musicStartPreroll = 0.03;

    // Earliest sample above the threshold across all stems/channels (first `maxScan` s), minus a short
    // pre-roll so the first attack isn't clipped. 0 = starts right away or nothing found.
    inline double findMusicStart (const std::vector<std::pair<AudioBuffer<float>, double>>& stems,
                                  float threshold = musicStartThreshold, double maxScan = musicStartMaxScan)
    {
        double earliest = std::numeric_limits<double>::infinity();
        for (auto& [buf, sr] : stems)
        {
            const int limit = (int) std::floor (jmin (maxScan, earliest) * sr);
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                auto* d = buf.getReadPointer (ch);
                const int end = jmin (limit, buf.getNumSamples());
                for (int i = 0; i < end; ++i)
                    if (std::abs (d[i]) > threshold)
                    {
                        earliest = jmin (earliest, i / sr);
                        break;
                    }
            }
        }
        return std::isfinite (earliest) ? jmax (0.0, earliest - musicStartPreroll) : 0.0;
    }

    // Decodes the first 30 s of each stem file.
    inline double findMusicStart (const Array<File>& files, AudioFormatManager& formats)
    {
        std::vector<std::pair<AudioBuffer<float>, double>> stems;
        for (auto& f : files)
            if (std::unique_ptr<AudioFormatReader> r (formats.createReaderFor (f)); r != nullptr)
            {
                const int n = (int) std::min (r->lengthInSamples, (juce::int64) (musicStartMaxScan * r->sampleRate));
                AudioBuffer<float> b ((int) r->numChannels, n);
                r->read (&b, 0, n, 0, true, true);
                stems.emplace_back (std::move (b), r->sampleRate);
            }
        return findMusicStart (stems);
    }

    // Index of the chord sounding at `time` in a sorted change list (-1 before the first).
    inline int chordIndexAt (const std::vector<Metronome::Chord>& chords, double time)
    {
        auto it = std::upper_bound (chords.begin(), chords.end(), time, [] (double t, auto& c) { return t < c.time; });
        return (int) (it - chords.begin()) - 1;
    }

    inline String chordLabel (const std::vector<Metronome::Chord>& chords, int idx)
    {
        return idx < 0 || idx >= (int) chords.size() || chords[(size_t) idx].chord == "N" ? String (u8"–") : chords[(size_t) idx].chord;
    }

    // Big "Em  →  Bm" for the song time given by `songTime`; repaints only when the chord changes.
    class ChordDisplay : public Component, private Timer
    {
    public:
        std::function<double()> songTime; // seconds in song time (edit time × speed)

        void setChords (std::vector<Metronome::Chord> c)
        {
            chords = std::move (c);
            index = -2;
            if (chords.empty()) stopTimer(); else startTimerHz (30);
            update();
        }

        String getText() const { return chords.empty() ? String() : now + String (u8"  →  ") + next; }

        void update()
        {
            const int i = songTime && ! chords.empty() ? chordIndexAt (chords, songTime()) : -1;
            if (i == index)
                return;
            index = i;
            now = chordLabel (chords, i);
            next = chordLabel (chords, i + 1);
            repaint();
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            if (chords.empty())
            {
                g.setColour (Theme::muted.withAlpha (0.6f));
                g.setFont (Theme::body (13.0f));
                g.drawText (String (u8"Aquí van los acordes (al abrir una canción analizada)"), r, Justification::centred);
                return;
            }
            // The chord now, big (brand title font), and the next one, smaller and muted.
            const auto nowFont = Theme::title (jmin (r.getHeight() * 0.8f, 46.0f));
            const auto nextFont = Theme::mono (jmin (r.getHeight() * 0.4f, 20.0f));
            const float nowW = GlyphArrangement::getStringWidth (nowFont, now) + 12;
            const float nextW = GlyphArrangement::getStringWidth (nextFont, String (u8"  →  ") + next);
            auto line = r.withSizeKeepingCentre (jmin (r.getWidth(), nowW + nextW), r.getHeight());
            g.setColour (Theme::text);
            g.setFont (nowFont);
            g.drawText (now, line.removeFromLeft (nowW), Justification::centredRight);
            g.setColour (Theme::muted);
            g.setFont (nextFont);
            g.drawText (String (u8"  →  ") + next, line, Justification::centredLeft);
        }

    private:
        std::vector<Metronome::Chord> chords;
        int index = -2;
        String now, next;

        void timerCallback() override { update(); }
    };
}
