#pragma once
#include <JuceHeader.h>

namespace te = tracktion;

// Editing takes (what REAPER is used for): split at the cursor, move (also to another track), trim the
// edges keeping the audio in place, fades, duplicate, mute, loop a part (to practise it, or to record several
// takes in a row) and pick the take of a clip recorded in a loop. Everything goes through the edit's undo.
namespace ClipEdit
{
    inline te::Clip* split (te::Clip& clip, te::TimePosition at)
    {
        auto pos = clip.getPosition();
        if (at <= pos.getStart() || at >= pos.getEnd())
            return nullptr;
        if (auto* track = clip.getClipTrack())
            return track->splitClip (clip, at);
        return nullptr;
    }

    // Splits whatever is under the cursor: the selected clips, else every clip of the selected tracks.
    inline int splitAt (te::Edit& edit, const Array<te::Clip*>& clips, const Array<te::Track*>& tracks, te::TimePosition at)
    {
        edit.getUndoManager().beginNewTransaction();
        int n = 0;
        for (auto* c : clips)
            n += ClipEdit::split (*c, at) != nullptr ? 1 : 0;
        if (clips.isEmpty())
            for (auto* t : tracks)
                if (auto* ct = dynamic_cast<te::ClipTrack*> (t))
                    for (auto* c : ct->getClips())
                        if (c->getPosition().time.containsInclusive (at) && ClipEdit::split (*c, at) != nullptr)
                        {
                            ++n;
                            break;
                        }
        return n;
    }

    // To `start` (not before 0), and onto `track` when given.
    inline void move (te::Clip& clip, te::TimePosition start, te::ClipTrack* track = nullptr)
    {
        clip.setStart (te::TimePosition::fromSeconds (jmax (0.0, start.inSeconds())), false, true);
        if (track != nullptr && track != clip.getClipTrack())
            clip.moveTo (*track);
    }

    // Edges: the audio stays where it is in time, only what's heard changes.
    inline void trimStart (te::Clip& clip, te::TimePosition newStart)
    {
        const auto end = clip.getPosition().getEnd();
        const double s = jlimit (0.0, end.inSeconds() - 0.01, newStart.inSeconds());
        clip.setStart (te::TimePosition::fromSeconds (s), true, false);
    }

    inline void trimEnd (te::Clip& clip, te::TimePosition newEnd)
    {
        const auto start = clip.getPosition().getStart();
        clip.setLength (te::TimeDuration::fromSeconds (jmax (0.01, newEnd.inSeconds() - start.inSeconds())), true);
    }

    inline bool setFades (te::Clip& clip, double inSeconds, double outSeconds)
    {
        if (auto* a = dynamic_cast<te::AudioClipBase*> (&clip))
        {
            a->setFadeIn (te::TimeDuration::fromSeconds (jmax (0.0, inSeconds)));
            a->setFadeOut (te::TimeDuration::fromSeconds (jmax (0.0, outSeconds)));
            return true;
        }
        return false;
    }

    // A copy right after it.
    inline te::Clip::Ptr duplicate (te::Clip& clip)
    {
        auto copy = te::duplicateClip (clip);
        if (copy != nullptr)
            copy->setStart (clip.getPosition().getEnd(), false, true);
        return copy;
    }

    inline void toggleMute (te::Clip& clip) { clip.setMuted (! clip.isMuted()); }

    // Loop playback over the clip (to practise that part, or to record several takes of it).
    inline void loopOver (te::Edit& edit, const te::Clip& clip)
    {
        auto& t = edit.getTransport();
        t.setLoopRange (clip.getPosition().time);
        t.looping = true;
    }

    inline void loopBetween (te::Edit& edit, te::TimePosition a, te::TimePosition b)
    {
        if (b < a)
            std::swap (a, b);
        if (b.inSeconds() - a.inSeconds() < 0.05)
            return;
        auto& t = edit.getTransport();
        t.setLoopRange ({ a, b });
        t.looping = true;
    }

    // Takes of a clip recorded in a loop (each pass is one file). Tracktion's own take switching needs its
    // "projects" (which Velody doesn't use) and drops file-based takes, so the files are read and set here.
    inline Array<File> takeFiles (te::Clip& clip)
    {
        Array<File> out;
        if (auto* w = dynamic_cast<te::WaveAudioClip*> (&clip))
        {
            auto takesTree = w->state.getChildWithName (te::IDs::TAKES);
            for (int i = 0; i < takesTree.getNumChildren(); ++i)
            {
                auto take = takesTree.getChild (i);
                out.add (te::SourceFileReference::findFileFromString (clip.edit, take[te::IDs::source].toString()));
            }
        }
        return out;
    }

    inline StringArray takes (te::Clip& clip)
    {
        StringArray names;
        const auto files = takeFiles (clip);
        if (files.size() > 1)
            for (int i = 0; i < files.size(); ++i)
                names.add ("Toma " + String (i + 1));
        return names;
    }

    // The take whose file the clip plays now (-1 when none of them).
    inline int currentTake (te::Clip& clip)
    {
        return takeFiles (clip).indexOf (clip.getSourceFileReference().getFile());
    }

    inline void chooseTake (te::Clip& clip, int index)
    {
        const auto files = takeFiles (clip);
        if (! isPositiveAndBelow (index, files.size()) || ! files[index].existsAsFile())
            return;
        clip.getSourceFileReference().setToFile (files[index], te::SourceFileReference::PathStyle::alwaysAbsolute, false);
    }
}
