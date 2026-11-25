#include <JuceHeader.h>
#include "../Source/ClipEdit.h"
#include "../Source/SongEdit.h"
#include "../Source/ui/CountInOverlay.h"
#include "TestAudio.h"

// Editing takes on a real edit: split, move (also to another track), trim keeping the audio in place,
// fades, duplicate, mute, loop a part, choose a take, undo. Plus the big count-in numbers.
struct ClipEditTests : UnitTest
{
    ClipEditTests() : UnitTest ("Clip editing (takes)") {}

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-clipedit");
        dir.createDirectory();
        TestAudio::writeSine (dir.getChildFile ("take1.wav"), 4.0, 220.0);
        TestAudio::writeSine (dir.getChildFile ("take2.wav"), 4.0, 440.0);
        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("p.tracktionedit")); // like the app
        edit->ensureNumberOfAudioTracks (2);
        auto* a = te::getAudioTracks (*edit)[0];
        auto* b = te::getAudioTracks (*edit)[1];
        SongEdit::insertWholeFile (*a, dir.getChildFile ("take1.wav"));
        auto clip = a->getClips()[0];
        auto& um = edit->getUndoManager();
        um.beginNewTransaction(); // each user action starts one (the app does it on each click/menu)
        auto secs = [] (te::TimePosition t) { return t.inSeconds(); };

        beginTest ("split at the cursor: two clips that meet there; outside the clip nothing happens");
        expect (ClipEdit::split (*clip, te::TimePosition::fromSeconds (9.0)) == nullptr);
        um.beginNewTransaction();
        auto* right = ClipEdit::split (*clip, te::TimePosition::fromSeconds (1.5));
        expect (right != nullptr);
        expectEquals (a->getClips().size(), 2);
        expectWithinAbsoluteError (secs (clip->getPosition().getEnd()), 1.5, 1e-6);
        expectWithinAbsoluteError (secs (right->getPosition().getStart()), 1.5, 1e-6);
        expectWithinAbsoluteError (right->getPosition().getOffset().inSeconds(), 1.5, 1e-6); // still the same audio there

        beginTest ("undo puts it back as one clip");
        edit->getUndoManager().undo();
        expectEquals (a->getClips().size(), 1);
        clip = a->getClips()[0];

        beginTest ("S with nothing selected but the track: splits what's under the cursor");
        expectEquals (ClipEdit::splitAt (*edit, {}, { a }, te::TimePosition::fromSeconds (2.0)), 1);
        expectEquals (a->getClips().size(), 2);
        edit->getUndoManager().undo();
        clip = a->getClips()[0];

        beginTest ("trim edges: what's heard changes, the audio stays where it was in time");
        um.beginNewTransaction();
        ClipEdit::trimStart (*clip, te::TimePosition::fromSeconds (1.0));
        expectWithinAbsoluteError (secs (clip->getPosition().getStart()), 1.0, 1e-6);
        expectWithinAbsoluteError (clip->getPosition().getOffset().inSeconds(), 1.0, 1e-6);
        ClipEdit::trimEnd (*clip, te::TimePosition::fromSeconds (3.0));
        expectWithinAbsoluteError (secs (clip->getPosition().getEnd()), 3.0, 1e-6);
        ClipEdit::trimStart (*clip, te::TimePosition::fromSeconds (10.0)); // past the end: kept tiny, not inverted
        expect (clip->getPosition().getLength().inSeconds() > 0);
        edit->getUndoManager().undo();

        beginTest ("move: later in time, and onto another track");
        clip = a->getClips()[0];
        um.beginNewTransaction();
        ClipEdit::move (*clip, te::TimePosition::fromSeconds (5.0));
        expectWithinAbsoluteError (secs (clip->getPosition().getStart()), 5.0, 1e-6);
        ClipEdit::move (*clip, te::TimePosition::fromSeconds (-2.0)); // never before 0
        expectWithinAbsoluteError (secs (clip->getPosition().getStart()), 0.0, 1e-6);
        ClipEdit::move (*clip, te::TimePosition::fromSeconds (1.0), b);
        expect (a->getClips().isEmpty() && b->getClips().size() == 1);
        clip = b->getClips()[0];

        beginTest ("fades, mute and duplicate");
        expect (ClipEdit::setFades (*clip, 0.25, 1.0));
        auto* audio = dynamic_cast<te::AudioClipBase*> (clip);
        expectWithinAbsoluteError (audio->getFadeIn().inSeconds(), 0.25, 1e-6);
        expectWithinAbsoluteError (audio->getFadeOut().inSeconds(), 1.0, 1e-6);
        ClipEdit::toggleMute (*clip);
        expect (clip->isMuted());
        ClipEdit::toggleMute (*clip);
        expect (! clip->isMuted());
        auto copy = ClipEdit::duplicate (*clip);
        expect (copy != nullptr && b->getClips().size() == 2);
        expectWithinAbsoluteError (secs (copy->getPosition().getStart()), secs (clip->getPosition().getEnd()), 1e-6);

        beginTest ("loop: over a clip, or between two points (in either order)");
        ClipEdit::loopOver (*edit, *clip);
        expect (edit->getTransport().looping.get());
        expect (edit->getTransport().getLoopRange() == clip->getPosition().time);
        ClipEdit::loopBetween (*edit, te::TimePosition::fromSeconds (6.0), te::TimePosition::fromSeconds (2.0));
        expectWithinAbsoluteError (secs (edit->getTransport().getLoopRange().getStart()), 2.0, 1e-6);
        expectWithinAbsoluteError (secs (edit->getTransport().getLoopRange().getEnd()), 6.0, 1e-6);

        beginTest ("takes: a clip recorded in a loop keeps each pass; picking one switches the audio");
        auto* wave = dynamic_cast<te::WaveAudioClip*> (clip);
        expect (ClipEdit::takes (*clip).isEmpty());
        wave->addTake (dir.getChildFile ("take1.wav"));
        wave->addTake (dir.getChildFile ("take2.wav"));
        expectEquals (ClipEdit::takes (*clip).size(), 2);
        ClipEdit::chooseTake (*clip, 1);
        expectEquals (ClipEdit::currentTake (*clip), 1);
        expect (wave->getOriginalFile().getFileName() == "take2.wav", wave->getOriginalFile().getFileName());
        ClipEdit::chooseTake (*clip, 0);
        expect (wave->getOriginalFile().getFileName() == "take1.wav");

        beginTest ("count-in on screen: counts down the beats left, bar by bar, gone when the song comes in");
        std::vector<Metronome::Click> clicks;
        for (int i = 0; i < 8; ++i)
            clicks.push_back ({ 1.0 + i * 0.5, i % 4 == 0 });
        auto s = CountInDisplay::at (clicks, 0.9, 5.0);       // before the first click
        expect (s.visible && s.remaining == 8 && s.bar == 1 && s.bars == 2);
        s = CountInDisplay::at (clicks, 1.0, 5.0);            // first click
        expect (s.remaining == 8 && s.downbeat);
        s = CountInDisplay::at (clicks, 1.6, 5.0);            // second
        expect (s.remaining == 7 && ! s.downbeat);
        s = CountInDisplay::at (clicks, 3.05, 5.0);           // fifth: bar 2
        expect (s.remaining == 4 && s.bar == 2 && s.downbeat);
        s = CountInDisplay::at (clicks, 4.6, 5.0);            // last
        expectEquals (s.remaining, 1);
        expect (! CountInDisplay::at (clicks, 5.0, 5.0).visible);
        expect (! CountInDisplay::at ({}, 1.0, 5.0).visible);

        copy = nullptr; // clips before the edit that owns them
        edit = nullptr;
        dir.deleteRecursively();
    }
};

static ClipEditTests clipEditTests;
