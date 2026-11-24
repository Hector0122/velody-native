#pragma once
#include <JuceHeader.h>
#include "Library.h"

// Writing a song's .velody sidecar exactly like Electron's updateMeta()/analysis cache, so both apps
// (and cloud sync, last-write-wins on updatedAt) keep reading the same file.
namespace SongMeta
{
    constexpr int analysisCacheVersion = 22; // velody/src/shared/analysis.ts ANALYSIS_CACHE_VERSION

    // Merges `patch` into <songDir>/.velody, stamps updatedAt, and writes via temp file + rename
    // so a reader never sees a half-written file. Unknown keys (cloudId, fileHashes, …) are kept.
    inline bool update (const File& songDir, const NamedValueSet& patch)
    {
        auto file = songDir.getChildFile (".velody");
        auto existing = JSON::parse (file);
        DynamicObject::Ptr obj = existing.isObject() ? existing.getDynamicObject() : new DynamicObject();

        for (auto& p : patch)
            obj->setProperty (p.name, p.value);
        obj->setProperty ("updatedAt", Time::currentTimeMillis());

        auto tmp = file.getSiblingFile (".velody." + Uuid().toString() + ".tmp");
        return tmp.replaceWithText (JSON::toString (var (obj.get()), true)) && tmp.moveFileTo (file);
    }

    // Beat tracking runs on the drums stem (exact name), else the next most rhythmic one (pickBeatTrackingStem).
    inline File pickBeatTrackingStem (const Array<File>& stems)
    {
        for (auto& name : Library::stemNames)
            for (auto& f : stems)
                if (f.getFileNameWithoutExtension() == name)
                    return f;
        return stems.isEmpty() ? File() : stems[0];
    }

    // detect_bpm.py's last stdout line -> the .velody patch Electron caches, or nothing when the
    // detection failed (failures are never cached, so the next run retries).
    inline std::optional<NamedValueSet> analysisPatch (const String& lastLine)
    {
        auto r = JSON::parse (lastLine.trim());
        if (! r.isObject() || ! r["bpm"].isDouble() && ! r["bpm"].isInt() || (double) r["bpm"] <= 0
            || ! (r["firstBeatOffset"].isDouble() || r["firstBeatOffset"].isInt()))
            return {};

        NamedValueSet p;
        p.set ("bpm", r["bpm"]);
        p.set ("firstBeatOffset", r["firstBeatOffset"]);
        p.set ("beatTimes", r["beatTimes"].isArray() ? r["beatTimes"] : var (Array<var>()));
        p.set ("tracker", r["tracker"].toString() == "beat_this" ? "beat_this" : "fallback");
        if (r["downbeatShift"].isInt() || r["downbeatShift"].isInt64())
            p.set ("detectedDownbeatShift", r["downbeatShift"]);

        if (auto* chords = r["chords"].getArray()) // validChords: finite time, short name
        {
            Array<var> ok;
            for (auto& c : *chords)
                if ((c["time"].isDouble() || c["time"].isInt()) && c["chord"].isString() && c["chord"].toString().length() <= 12)
                    ok.add (c);
            p.set ("chords", ok);
        }

        p.set ("_version", analysisCacheVersion);
        return p;
    }
}
