#include <JuceHeader.h>
#include "../Source/Trim.h"
#include "../Source/Exporter.h"
#include "TestAudio.h"

// Trim points (Electron's add-song-trim-points): .velody keys, the rules, when playback stops, the
// "Recorte" menu down to the file, and an export that covers just the trimmed range.
struct TrimTests : UnitTest
{
    TrimTests() : UnitTest ("Trim points") {}

    static std::optional<PopupMenu::Item> item (const PopupMenu& m, const String& prefix)
    {
        for (PopupMenu::MenuItemIterator it (m, true); it.next();)
            if (it.getItem().text.startsWith (prefix))
                return it.getItem();
        return {};
    }

    void runTest() override
    {
        using namespace Trim;
        auto dir = File::createTempFile ("velody-trim");
        dir.createDirectory();
        auto song = dir.getChildFile ("Song");
        song.createDirectory();
        song.getChildFile (".velody").replaceWithText (R"({"bpm":120,"updatedAt":5})");

        beginTest ("nothing saved: the whole song");
        auto r = load (song);
        expect (! r.active() && r.start == 0 && ! r.end);
        expectEquals (describe (r), String (u8"Sin tramo: la canción completa"));

        beginTest ("start/end rules: the end must be after the start; a new start past the end drops it");
        r = withStart (r, 12.4);
        expect (r.start == 12.4 && ! r.end);
        expect (! withEnd (r, 10.0).has_value());
        expect (! withEnd (r, 12.4).has_value());
        r = *withEnd (r, 220.0);
        expectEquals (describe (r), String (u8"Tramo: 0:12 → 3:40"));
        expect (! withStart (r, 230.0).end.has_value());
        expectEquals (withStart (r, -3).start, 0.0);

        beginTest ("saved in .velody (Electron's keys, null = no end), stamped so it syncs");
        expect (save (song, r));
        auto m = SongMeta::read (song);
        expect ((double) m["trimStart"] == 12.4 && (double) m["trimEnd"] == 220.0 && (int) m["bpm"] == 120);
        expect ((int64) m["updatedAt"] > 5);
        auto back = load (song);
        expect (back.start == 12.4 && back.end && *back.end == 220.0);
        save (song, Trim::Range());
        expect (song.getChildFile (".velody").loadFileAsString().contains ("\"trimEnd\": null"));
        expect (! load (song).active());
        song.getChildFile (".velody").replaceWithText (R"({"trimStart":30,"trimEnd":10})"); // end before start: ignored
        back = load (song);
        expect (back.start == 30 && ! back.end);

        beginTest ("playback stops when it crosses the end; not while recording; not when started past it");
        Trim::Range t { 10, 100.0 };
        expect (shouldStop (t, 99.9, 100.02, true, false));
        expect (! shouldStop (t, 99.9, 100.02, true, true));   // recording
        expect (! shouldStop (t, 99.0, 99.5, true, false));    // not there yet
        expect (! shouldStop (t, 120.0, 120.03, true, false)); // playing after the end on purpose
        expect (! shouldStop (t, 99.9, 100.02, false, false)); // stopped
        expect (! shouldStop ({ 10, std::nullopt }, 99.9, 1000.0, true, false));

        beginTest ("with no trim end, playback stops where the audio ends (3:58 song, not playing on forever)");
        const double songEnd = 238.0;
        expect (playbackEnd ({}, songEnd) == songEnd);
        expect (playbackEnd ({ 0, 100.0 }, songEnd) == 100.0);   // the trim's end wins
        expect (! playbackEnd ({}, 0.0).has_value());              // empty project: never stops
        expect (shouldStopAtEnd ({}, songEnd, 237.98, 238.01, true, false, false));
        expect (! shouldStopAtEnd ({}, songEnd, 200.0, 200.03, true, false, false)); // not there yet
        expect (! shouldStopAtEnd ({}, songEnd, 237.98, 238.01, true, true, false)); // recording past the end
        expect (! shouldStopAtEnd ({}, songEnd, 237.98, 238.01, true, false, true)); // loop on
        expect (! shouldStopAtEnd ({}, songEnd, 300.0, 300.03, true, false, false)); // started past the end
        expect (! shouldStopAtEnd ({}, 0.0, 10.0, 10.03, true, false, false));
        expect (shouldStopAtEnd ({ 10, 100.0 }, songEnd, 99.9, 100.02, true, false, false));

        beginTest ("export range in edit time (the edit runs at song time / speed)");
        auto [s, e] = renderRange ({ 10, 100.0 }, 400.0, 0.5);
        expect (s == 20.0 && e == 200.0);
        std::tie (s, e) = renderRange ({ 10, std::nullopt }, 400.0, 1.0);
        expect (s == 10.0 && e == 400.0);
        std::tie (s, e) = renderRange ({ 500, std::nullopt }, 400.0, 1.0);
        expect (s == 400.0 && e == 400.0);

        beginTest ("Recorte menu: shows the range, sets start/end at the cursor, clears");
        Trim::Range cur;
        Array<int> fired;
        auto act = [&] (Action a) { fired.add ((int) a.kind); };
        auto menu0 = menu (cur, 75.0, act);
        expect (item (menu0, "Sin tramo").has_value());
        expect (item (menu0, String (u8"La canción empieza aquí (1:15)")).has_value());
        expect (! item (menu0, String (u8"Canción completa"))->isEnabled);
        item (menu0, String (u8"La canción empieza aquí"))->action();
        item (menu0, String (u8"La canción termina aquí"))->action();
        expect (fired == Array<int> { Action::setStart, Action::setEnd });
        auto menu1 = menu ({ 80, std::nullopt }, 75.0, act);
        expect (! item (menu1, String (u8"La canción termina aquí"))->isEnabled); // the cursor is before the start
        expect (item (menu1, String (u8"Canción completa"))->isEnabled);

        beginTest ("export with the trim: only that range is rendered");
        {
            te::Engine engine { "VelodyTests" };
            TestAudio::writeSine (dir.getChildFile ("bass.wav"), 4.0, 220);
            auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("song.tracktionedit"));
            SongEdit::addStemTracks (*edit, { dir.getChildFile ("bass.wav") });
            auto out = dir.getChildFile ("trimmed.wav");
            String error = "timeout";
            bool done = false;
            Array<Exporter::Item> items;
            items.add ({ {}, out });
            Exporter ex (*edit, items, Exporter::Format::wav, [] (int, int) {}, [&] (const String& err) { error = err; done = true; },
                         renderRange ({ 1.0, 2.5 }, SongEdit::contentLength (*edit), 1.0));
            ex.start();
            for (int i = 0; i < 3000 && ! done; ++i)
                MessageManager::getInstance()->runDispatchLoopUntil (10);
            expect (error.isEmpty(), error);
            auto [len, freq] = TestAudio::analyse (out);
            expectWithinAbsoluteError (len, 1.5, 0.1);
            expectWithinAbsoluteError (freq, 220.0, 10.0);
        }

        dir.deleteRecursively();
    }
};

static TrimTests trimTests;
