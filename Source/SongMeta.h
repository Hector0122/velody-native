#pragma once
#include <JuceHeader.h>
#include "Library.h"

// Writing a song's .velody sidecar exactly like Electron's updateMeta()/analysis cache, so both apps
// (and cloud sync, last-write-wins on updatedAt) keep reading the same file.
namespace SongMeta
{
    constexpr int analysisCacheVersion = 22; // velody/src/shared/analysis.ts ANALYSIS_CACHE_VERSION

    // Keys describing this device only, never uploaded (shared/cloud.ts LOCAL_ONLY_META_KEYS).
    inline const StringArray localOnlyKeys { "cloudId", "fileHashes" };

    // Merges `patch` into <songDir>/.velody and writes via temp file + rename so a reader never sees a
    // half-written file. Unknown keys (cloudId, fileHashes, …) are kept. Like Electron's updateMeta:
    // `touch` stamps updatedAt (cloud sync is last-write-wins on it; device bookkeeping passes false),
    // `replace` drops every synced key first (applying a newer cloud version).
    inline bool update (const File& songDir, const NamedValueSet& patch, bool touch = true, bool replace = false)
    {
        auto file = songDir.getChildFile (".velody");
        auto existing = JSON::parse (file);
        DynamicObject::Ptr obj = existing.isObject() ? existing.getDynamicObject() : new DynamicObject();

        if (replace)
        {
            DynamicObject::Ptr kept = new DynamicObject();
            for (auto& k : localOnlyKeys)
                if (obj->hasProperty (k))
                    kept->setProperty (k, obj->getProperty (k));
            obj = kept;
        }

        for (auto& p : patch)
            obj->setProperty (p.name, p.value);
        if (touch)
            obj->setProperty ("updatedAt", Time::currentTimeMillis());

        auto tmp = file.getSiblingFile (".velody." + Uuid().toString() + ".tmp");
        return tmp.replaceWithText (JSON::toString (var (obj.get()), true)) && tmp.moveFileTo (file);
    }

    inline var read (const File& songDir)
    {
        auto v = JSON::parse (songDir.getChildFile (".velody"));
        return v.isObject() ? v : var();
    }

    inline int64 updatedAt (const var& meta)
    {
        auto v = meta["updatedAt"];
        return v.isInt() || v.isInt64() || v.isDouble() ? (int64) v : 0;
    }

    // The synced part of the metadata: local-only keys and updatedAt removed.
    inline var syncedPart (const var& meta)
    {
        DynamicObject::Ptr out = new DynamicObject();
        if (auto* o = meta.getDynamicObject())
            for (auto& p : o->getProperties())
                if (! localOnlyKeys.contains (p.name.toString()) && p.name.toString() != "updatedAt")
                    out->setProperty (p.name, p.value);
        return var (out.get());
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
