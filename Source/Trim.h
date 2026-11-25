#pragma once
#include <JuceHeader.h>
#include "SongMeta.h"

// A song's trim range (Electron's useTrim / add-song-trim-points): where the real song starts and,
// optionally, ends, to skip a long intro or outro. Kept in .velody (`trimStart`, `trimEnd`, song time in
// seconds), so it syncs with the cloud. The audio files are never touched: the song opens and ⏮ Inicio
// goes to the start, playback stops at the end, and exports cover just that range. Seeking anywhere is
// still allowed.
namespace Trim
{
    struct Range
    {
        double start = 0;
        std::optional<double> end;
        bool active() const { return start > 0 || end.has_value(); }
    };

    inline bool isTime (const var& v) { return (v.isDouble() || v.isInt() || v.isInt64()) && std::isfinite ((double) v) && (double) v >= 0; }

    inline Range load (const File& songDir)
    {
        auto m = SongMeta::read (songDir);
        Range r;
        if (isTime (m["trimStart"]))
            r.start = (double) m["trimStart"];
        if (isTime (m["trimEnd"]) && (double) m["trimEnd"] > r.start)
            r.end = (double) m["trimEnd"];
        return r;
    }

    inline bool save (const File& songDir, const Range& r)
    {
        NamedValueSet p;
        p.set ("trimStart", r.start);
        p.set ("trimEnd", r.end ? var (*r.end) : var()); // null, like Electron's "no end"
        return SongMeta::update (songDir, p);
    }

    // Start here. An end that would now be before the start is dropped.
    inline Range withStart (Range r, double t)
    {
        r.start = jmax (0.0, t);
        if (r.end && *r.end <= r.start)
            r.end.reset();
        return r;
    }

    // End here; nothing (empty) when it isn't after the start, like Electron's handleSetTrimEnd.
    inline std::optional<Range> withEnd (Range r, double t)
    {
        if (t <= r.start)
            return {};
        r.end = t;
        return r;
    }

    // Playback just crossed the end (between the last check and now): stop. Never while recording (the
    // take must not be cut), and not when playing from past the end (to listen there and move it).
    inline bool shouldStop (const Range& r, double lastPos, double songPos, bool playing, bool recording)
    {
        return playing && ! recording && r.end && lastPos < *r.end && songPos >= *r.end;
    }

    // The export range in edit time (the edit runs at song time / speed).
    inline std::pair<double, double> renderRange (const Range& r, double editLength, double speed)
    {
        const double s = jlimit (0.0, editLength, r.start / speed);
        const double e = r.end ? jlimit (s, editLength, *r.end / speed) : editLength;
        return { s, e };
    }

    inline String clock (double seconds)
    {
        const int t = roundToInt (seconds);
        return String (t / 60) + ":" + String (t % 60).paddedLeft ('0', 2);
    }

    inline String describe (const Range& r)
    {
        if (! r.active())
            return String (u8"Sin tramo: la canción completa");
        return "Tramo: " + clock (r.start) + String (u8" → ") + (r.end ? clock (*r.end) : String ("final"));
    }

    struct Action { enum Kind { setStart, setEnd, clear } kind; };

    // The "Recorte" menu; `songPos` is where the cursor is (song time).
    inline PopupMenu menu (const Range& r, double songPos, std::function<void (Action)> act)
    {
        PopupMenu m;
        m.addSectionHeader (describe (r));
        m.addItem (String (u8"La canción empieza aquí (") + clock (songPos) + ")", [act] { act ({ Action::setStart }); });
        m.addItem (String (u8"La canción termina aquí (") + clock (songPos) + ")", songPos > r.start, false, [act] { act ({ Action::setEnd }); });
        m.addItem (String (u8"Canción completa (quitar el tramo)"), r.active(), false, [act] { act ({ Action::clear }); });
        m.addSeparator();
        m.addItem (String (u8"No corta el audio: solo marca desde dónde y hasta dónde suena, y qué se exporta"), false, false, nullptr);
        return m;
    }
}
