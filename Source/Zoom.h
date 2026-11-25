#pragma once
#include <JuceHeader.h>

// Timeline zoom, like REAPER: wider (more pixels per second) and taller tracks, so the waveforms get big
// enough to line takes up. Times are edit seconds; the view never goes before 0 or past `end`
// (where the audio ends, with a margin). Heights are pixels per track lane.
namespace Zoom
{
    inline constexpr double minSpan = 0.25;   // at most ~¼ s across the whole width
    inline constexpr int minHeight = 36, defaultHeight = 58, maxHeight = 480;

    struct View
    {
        double start = 0, end = 60;
        double span() const { return end - start; }
        bool operator== (const View& o) const { return start == o.start && end == o.end; }
    };

    // How far the view may go: the audio plus a little room, at least a minute.
    inline double limit (double contentLength) { return jmax (60.0, contentLength * 1.02); }

    // The same span moved so it fits in [0, limit].
    inline View clamp (View v, double limitEnd)
    {
        const double span = jlimit (minSpan, jmax (minSpan, limitEnd), v.span());
        const double start = jlimit (0.0, jmax (0.0, limitEnd - span), v.start);
        return { start, start + span };
    }

    // Zoom by `factor` (< 1 = closer) keeping `anchor` (the time under the mouse) where it is on screen.
    inline View around (View v, double anchor, double factor, double limitEnd)
    {
        const double frac = v.span() > 0 ? jlimit (0.0, 1.0, (anchor - v.start) / v.span()) : 0.0;
        const double span = jlimit (minSpan, jmax (minSpan, limitEnd), v.span() * factor);
        return clamp ({ anchor - frac * span, anchor - frac * span + span }, limitEnd);
    }

    inline View scroll (View v, double seconds, double limitEnd)
    {
        return clamp ({ v.start + seconds, v.end + seconds }, limitEnd);
    }

    // While playing, when the playhead leaves the view: the next page (it starts a bit before the playhead).
    inline std::optional<View> follow (View v, double playhead, double limitEnd)
    {
        if (playhead >= v.start && playhead <= v.end)
            return {};
        return clamp ({ playhead - v.span() * 0.05, playhead - v.span() * 0.05 + v.span() }, limitEnd);
    }

    // A mouse-wheel move as a zoom factor: up = closer. JUCE's deltaY is about ±0.1 per notch.
    inline double wheelFactor (float deltaY) { return std::exp (-(double) deltaY * 2.5); }

    inline int clampHeight (int h) { return jlimit (minHeight, maxHeight, h); }
    inline int scaledHeight (int h, double factor) { return clampHeight (roundToInt (h * factor)); }

    // One track's lane height: its own (dragged on the header's bottom edge) or everyone's.
    inline const Identifier trackHeightProp { "velodyHeight" };
    inline int heightOf (const ValueTree& trackState, int global)
    {
        const int own = trackState.getProperty (trackHeightProp, 0);
        return own > 0 ? clampHeight (own) : clampHeight (global);
    }
}
