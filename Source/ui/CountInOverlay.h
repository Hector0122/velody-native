#pragma once
#include <JuceHeader.h>
#include "../Metronome.h"
#include "../Theme.h"

// The count-in, big on screen (like Electron's recording countdown): the beats left before the song
// comes in, counting down (8, 7 … 1), the first beat of each bar in the accent colour, and which bar it is.
namespace CountInDisplay
{
    struct State { bool visible = false; int remaining = 0, bar = 0, bars = 0; bool downbeat = false; };

    // `clicks` and `position` in edit time; `entry` is where the song comes in.
    inline State at (const std::vector<Metronome::Click>& clicks, double position, double entry, int beatsPerBar = 4)
    {
        State s;
        if (clicks.empty() || position >= entry - 1e-3)
            return s;
        int passed = 0;
        for (auto& c : clicks)
            if (c.time <= position + 1e-3)
                ++passed;
        const int total = (int) clicks.size();
        s.visible = true;
        s.remaining = jlimit (1, total, total - jmax (1, passed) + 1);
        const int index = jmax (0, passed - 1); // the click now sounding
        s.bars = jmax (1, (total + beatsPerBar - 1) / beatsPerBar);
        s.bar = jmin (s.bars, index / beatsPerBar + 1);
        s.downbeat = clicks[(size_t) index].accent;
        return s;
    }

    class Overlay : public Component
    {
    public:
        Overlay() { setInterceptsMouseClicks (false, false); }

        void show (const State& s)
        {
            if (s.visible != state.visible || s.remaining != state.remaining || s.bar != state.bar)
            {
                state = s;
                setVisible (s.visible);
                repaint();
            }
        }

        void paint (Graphics& g) override
        {
            if (! state.visible)
                return;
            auto card = getLocalBounds().toFloat().withSizeKeepingCentre (jmin (360.0f, getWidth() - 40.0f), jmin (300.0f, getHeight() - 20.0f));
            g.setColour (Theme::bg.withAlpha (0.82f));
            g.fillRoundedRectangle (card, 24.0f);
            g.setColour ((state.downbeat ? Theme::accent : Theme::primary).withAlpha (0.8f));
            g.drawRoundedRectangle (card, 24.0f, 3.0f);
            auto r = card.reduced (16);
            auto caption = r.removeFromBottom (36);
            g.setColour (Theme::muted);
            g.setFont (Theme::body (16.0f, true));
            g.drawText (String (u8"Count-in  ·  compás ") + String (state.bar) + " de " + String (state.bars), caption, Justification::centred);
            g.setColour (state.downbeat ? Theme::accent.brighter (0.3f) : Colours::white);
            g.setFont (Theme::title (jmin (200.0f, r.getHeight())));
            g.drawText (String (state.remaining), r, Justification::centred);
        }

        State state;
    };
}
