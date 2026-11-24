#pragma once
#include <JuceHeader.h>
#include "Library.h"

namespace te = tracktion;

namespace SongEdit
{
    // One track per stem, named after the file, with the whole file as a clip at 0 s.
    inline void addStemTracks (te::Edit& edit, const Array<File>& stems)
    {
        edit.ensureNumberOfAudioTracks (stems.size());

        for (int i = 0; i < stems.size(); ++i)
        {
            auto t = te::getAudioTracks (edit)[i];
            t->setName (stems[i].getFileNameWithoutExtension());

            te::AudioFile audio (edit.engine, stems[i]);
            if (audio.isValid())
                t->insertWaveClip (t->getName(), stems[i],
                                   { { {}, te::TimeDuration::fromSeconds (audio.getLength()) }, {} }, false);
        }
    }

    // Input tracks go after the leading stem tracks so a stem is never recorded over.
    inline int firstInputTrack (te::Edit& edit)
    {
        int i = 0;
        while (auto t = te::getAudioTracks (edit)[i])
        {
            if (! Library::isStemName (t->getName()))
                break;
            ++i;
        }
        return i;
    }
}
