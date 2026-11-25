#include <JuceHeader.h>
#include "../Source/Zoom.h"

// Timeline zoom math: zooming keeps the time under the mouse in place, the view stays inside the song,
// the page turns with the playhead, heights stay in range.
struct ZoomTests : UnitTest
{
    ZoomTests() : UnitTest ("Zoom (timeline)") {}

    void runTest() override
    {
        using namespace Zoom;
        const double end = limit (238.0); // a 3:58 song

        beginTest ("the view can go up to the song's end plus a margin, at least a minute");
        expectWithinAbsoluteError (end, 242.76, 0.001);
        expectEquals (limit (10.0), 60.0);

        beginTest ("zooming in keeps the time under the mouse where it was on screen");
        View v { 0, 240 };
        auto z = around (v, 60.0, 0.5, end);              // mouse at 1/4 of the width
        expectWithinAbsoluteError (z.span(), 120.0, 1e-9);
        expectWithinAbsoluteError ((60.0 - z.start) / z.span(), 0.25, 1e-9);
        auto back = around (z, 60.0, 2.0, end);
        expectWithinAbsoluteError (back.start, 0.0, 1e-9);
        expectWithinAbsoluteError (back.end, 240.0, 1e-9);

        beginTest ("never before 0, past the end, closer than ¼ s or wider than the song");
        auto left = around ({ 0, 100 }, 0.0, 0.1, end);
        expect (left.start == 0.0 && left.span() > 9.99 && left.span() < 10.01);
        auto tiny = around ({ 100, 101 }, 100.5, 0.0001, end);
        expectWithinAbsoluteError (tiny.span(), minSpan, 1e-9);
        auto wide = around ({ 0, 240 }, 120.0, 10.0, end);
        expectWithinAbsoluteError (wide.start, 0.0, 1e-9);
        expectWithinAbsoluteError (wide.end, end, 1e-9);
        auto atEnd = scroll ({ 200, 240 }, 100.0, end);
        expectWithinAbsoluteError (atEnd.end, end, 1e-9);
        expectWithinAbsoluteError (atEnd.span(), 40.0, 1e-9);
        auto atStart = scroll ({ 10, 50 }, -100.0, end);
        expect (atStart == View { 0, 40 });

        beginTest ("while playing, the page turns when the playhead leaves the view");
        expect (! follow ({ 0, 30 }, 15.0, end).has_value());
        auto next = follow ({ 0, 30 }, 30.5, end);
        expect (next.has_value() && next->start < 30.5 && next->end > 30.5);
        expectWithinAbsoluteError (next->span(), 30.0, 1e-9);
        auto last = follow ({ 0, 30 }, 241.0, end);
        expect (last.has_value());
        expectWithinAbsoluteError (last->end, end, 1e-9);

        beginTest ("mouse wheel: up = closer, down = farther, one notch is a clear step");
        expect (wheelFactor (0.1f) < 0.85 && wheelFactor (0.1f) > 0.7);
        expectWithinAbsoluteError (wheelFactor (0.1f) * wheelFactor (-0.1f), 1.0, 1e-9);

        beginTest ("track heights: grow/shrink in range; a track's own height wins over everyone's");
        expectEquals (scaledHeight (100, 1.25), 125);
        expectEquals (scaledHeight (400, 2.0), maxHeight);
        expectEquals (scaledHeight (40, 0.5), minHeight);
        ValueTree t ("TRACK");
        expectEquals (heightOf (t, 100), 100);
        t.setProperty (trackHeightProp, 300, nullptr);
        expectEquals (heightOf (t, 100), 300);
        t.setProperty (trackHeightProp, 5000, nullptr);
        expectEquals (heightOf (t, 100), maxHeight);
    }
};

static ZoomTests zoomTests;
