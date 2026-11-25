#pragma once
#include <JuceHeader.h>
#include "../Theme.h"

// Round transport buttons with drawn icons (no font glyphs needed): Play/Stop, Record/Stop, ⏮ start.
class TransportButton : public Button
{
public:
    enum class Kind { play, record, start };

    TransportButton (Kind k, const String& name) : Button (name), kind (k) {}

    // Play: playing → shows Stop. Record: recording → red with a Stop square.
    void setActive (bool a)
    {
        if (active != a)
        {
            active = a;
            repaint();
        }
    }

    bool isActive() const { return active; }

    void paintButton (Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        const float d = jmin (r.getWidth(), r.getHeight()) - (kind == Kind::play ? 14.0f : 4.0f);
        auto c = r.withSizeKeepingCentre (d, d);

        Colour fill = Theme::raised;
        if (kind == Kind::play)   fill = Theme::primary;
        if (kind == Kind::record) fill = active ? Theme::record : Theme::raised;
        if (! isEnabled())        fill = fill.withMultipliedAlpha (0.4f);
        else if (down)            fill = fill.brighter (0.2f);
        else if (over)            fill = fill.brighter (0.1f);
        if (kind == Kind::play && isEnabled())
            for (int i = 3; i >= 1; --i) // soft glow
            {
                g.setColour (Theme::primary.withAlpha (0.07f * (float) (4 - i)));
                g.fillEllipse (c.expanded ((float) i * 3.0f));
            }
        if (kind == Kind::record && active)
        {
            g.setColour (Theme::record.withAlpha (0.25f));
            g.fillEllipse (c.expanded (6.0f));
        }
        g.setGradientFill (ColourGradient (fill.brighter (0.15f), c.getX(), c.getY(), fill.darker (0.15f), c.getX(), c.getBottom(), false));
        g.fillEllipse (c);
        if (kind != Kind::play)
        {
            g.setColour (Theme::outline);
            g.drawEllipse (c.reduced (0.5f), 1.0f);
        }

        const float s = d * 0.34f;
        auto icon = c.withSizeKeepingCentre (s, s);
        Path p;
        const bool showStop = active;
        if (showStop)
        {
            p.addRoundedRectangle (icon.reduced (s * 0.05f), s * 0.12f);
            g.setColour (Colours::white);
        }
        else if (kind == Kind::play)
        {
            p.addTriangle (icon.getX() + s * 0.12f, icon.getY(), icon.getX() + s * 0.12f, icon.getBottom(), icon.getRight() + s * 0.1f, icon.getCentreY());
            g.setColour (Colours::white);
        }
        else if (kind == Kind::record)
        {
            p.addEllipse (icon.reduced (s * 0.05f));
            g.setColour (isEnabled() ? Theme::record : Theme::muted);
        }
        else
        {
            const float bar = s * 0.16f;
            p.addRectangle (icon.getX(), icon.getY(), bar, s);
            p.addTriangle (icon.getRight(), icon.getY(), icon.getRight(), icon.getBottom(), icon.getX() + bar, icon.getCentreY());
            g.setColour (Theme::text);
        }
        g.fillPath (p);
    }

private:
    Kind kind;
    bool active = false;
};
