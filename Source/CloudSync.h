#pragma once
#include <JuceHeader.h>
#include "CloudClient.h"
#include "Library.h"
#include "SongMeta.h"

// Cloud library sync, ported from velody/src/main/cloud/sync.ts. The cloud is a synced copy of the
// library, never the playback source: songs are downloaded whole before they open, and everything
// keeps working with the cloud off or unreachable. All calls block: run them off the message thread.
namespace Cloud
{
    inline const String downloadDirPrefix { ".velody_dl_" }; // dot: the library scan skips it

    enum class State { local, cloud, synced };

    struct Entry
    {
        String name;
        State state = State::local;
        String cloudId;
        std::optional<Library::Song> local; // set unless cloud-only
        int64 sizeBytes = 0;
    };

    // A song name/id from the cloud becomes a folder name here: plain names only.
    inline bool isSafeSongName (const String& n)
    {
        return n.isNotEmpty() && n != "." && n != ".." && ! n.startsWithChar ('.') && ! n.containsAnyOf ("/\\") && ! n.containsChar (0);
    }

    inline bool isSongId (const String& id) { return std::regex_match (id.toStdString(), songIdRe()); }

    // Audio files of a song folder that belong in the cloud (stems + saved recordings), sorted.
    inline StringArray songFiles (const File& dir)
    {
        StringArray names;
        for (auto& f : dir.findChildFiles (File::findFiles, false))
            if (isCloudFileName (f.getFileName()) && ! f.getFileName().startsWith ("_tmp"))
                names.add (f.getFileName());
        names.sort (false);
        return names;
    }

    inline String sha256Of (const File& f) { return SHA256 (f).toHexString(); }

    // Name/size/SHA-256 of every file to sync. Hashes are cached in .velody (`fileHashes`, keyed by
    // size + mtime) so each file is hashed once.
    inline std::vector<FileInfo> buildManifest (const File& dir)
    {
        auto meta = SongMeta::read (dir);
        auto cache = meta["fileHashes"];
        DynamicObject::Ptr next = new DynamicObject();
        bool changed = false;
        std::vector<FileInfo> files;

        for (auto& name : songFiles (dir))
        {
            auto f = dir.getChildFile (name);
            const auto size = f.getSize();
            const auto mtime = f.getLastModificationTime().toMilliseconds();
            auto hit = cache[Identifier (name)];
            String digest;
            // Electron stores fractional ms; within 1 ms is the same file.
            if (hit.isObject() && (int64) hit["size"] == size && std::abs ((double) hit["mtimeMs"] - (double) mtime) < 1.0
                && std::regex_match (hit["sha256"].toString().toStdString(), sha256Re()))
                digest = hit["sha256"].toString();
            else
            {
                digest = sha256Of (f);
                changed = true;
            }

            DynamicObject::Ptr e = new DynamicObject();
            e->setProperty ("size", size);
            e->setProperty ("mtimeMs", mtime);
            e->setProperty ("sha256", digest);
            next->setProperty (Identifier (name), var (e.get()));
            files.push_back ({ name, size, digest });
        }

        const int cached = cache.getDynamicObject() != nullptr ? cache.getDynamicObject()->getProperties().size() : 0;
        if (changed || cached != (int) files.size())
        {
            NamedValueSet p;
            p.set ("fileHashes", var (next.get()));
            SongMeta::update (dir, p, false);
        }
        return files;
    }

    // Local scan + cloud index, matched by cloudId (in .velody), or by name for a local song without
    // one (reinstall / lost .velody) so it isn't uploaded twice. `cloud` empty = offline: local only.
    inline std::vector<Entry> merge (const Array<Library::Song>& local, const StringArray& localCloudIds,
                                     const std::optional<std::vector<Song>>& cloud)
    {
        std::vector<Entry> out;
        std::set<String> matched, taken;

        for (int i = 0; i < local.size(); ++i)
        {
            Entry e { local[i].name, State::local, localCloudIds[i], local[i], 0 };
            if (cloud)
            {
                String id = e.cloudId;
                if (id.isEmpty())
                    for (auto& s : *cloud)
                        if (s.name == e.name) { id = s.id; break; }

                const bool inCloud = std::any_of (cloud->begin(), cloud->end(), [&] (auto& s) { return s.id == id; });
                if (id.isNotEmpty() && inCloud && matched.count (id) == 0)
                {
                    matched.insert (id);
                    e.state = State::synced;
                    e.cloudId = id;
                }
            }
            else if (e.cloudId.isNotEmpty())
            {
                e.state = State::synced;
            }
            taken.insert (e.name);
            out.push_back (e);
        }

        if (cloud)
            for (auto& s : *cloud)
            {
                // shortcut: a cloud-only song whose name is taken by a different local song is hidden (no rename), like Electron.
                if (matched.count (s.id) != 0 || taken.count (s.name) != 0 || ! isSafeSongName (s.name) || ! isSongId (s.id))
                    continue;
                int64 size = 0;
                for (auto& f : s.files)
                    size += f.size;
                out.push_back ({ s.name, State::cloud, s.id, std::nullopt, size });
            }

        std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.name.compareNatural (b.name) < 0; });
        return out;
    }

    using Progress = std::function<void (int percent)>;
    using MakeClient = std::function<std::unique_ptr<Client> (bool quick)>; // nullptr while cloud sync is off

    class Sync
    {
    public:
        Sync (File libraryRoot, MakeClient make) : root (std::move (libraryRoot)), makeClient (std::move (make)) {}

        struct Listing { std::vector<Entry> songs; bool enabled = false, offline = false; String error; };

        // The library with sync states (plain local scan when the cloud is off or unreachable).
        Listing list()
        {
            Listing l;
            auto local = Library::scan (root);
            StringArray ids;
            for (auto& s : local)
                ids.add (SongMeta::read (s.dir)["cloudId"].toString());

            std::optional<std::vector<Song>> cloud;
            if (auto c = makeClient (true)) // quick: the song menu waits on it, offline falls back fast
            {
                l.enabled = true;
                try { cloud = c->listSongs(); }
                catch (const Error& e) { l.offline = true; l.error = e.message(); }
            }

            l.songs = merge (local, ids, cloud);

            // Songs matched by name get their cloudId saved, so later actions use the existing copy.
            for (int i = 0; i < local.size(); ++i)
                if (ids[i].isEmpty())
                    for (auto& e : l.songs)
                        if (e.name == local[i].name && e.state == State::synced)
                        {
                            NamedValueSet p;
                            p.set ("cloudId", e.cloudId);
                            SongMeta::update (local[i].dir, p, false);
                        }
            return l;
        }

        // begin → PUT changed files → commit → sync metadata. Never touches local audio.
        String upload (const String& name, Progress progress = {})
        {
            auto c = requireClient();
            auto dir = root.getChildFile (name);
            if (! isSafeSongName (name) || ! dir.isDirectory())
                throw Error (String (u8"La canción no está en esta laptop: ") + name, 0);

            auto meta = SongMeta::read (dir);
            String id = meta["cloudId"].toString();
            if (! isSongId (id))
                id = Uuid().toDashedString();

            auto files = buildManifest (dir);
            if (files.empty())
                throw Error ("No hay audio para subir", 0);

            auto uploads = c->beginUpload (id, name, files);
            if (meta["cloudId"].toString() != id) // persist right away so an interrupted upload resumes on the same id
            {
                NamedValueSet p;
                p.set ("cloudId", id);
                SongMeta::update (dir, p, false);
            }

            int64 total = 0, done = 0;
            for (auto& u : uploads)
                total += dir.getChildFile (u.name).getSize();
            for (auto& u : uploads)
            {
                if (! isCloudFileName (u.name))
                    throw Error ("Nombre de archivo inesperado: " + u.name, 0);
                MemoryBlock data;
                if (! dir.getChildFile (u.name).loadFileAsData (data))
                    throw Error ("No se pudo leer " + u.name, 0);
                c->putFile (u.url, data);
                done += (int64) data.getSize();
                if (progress)
                    progress (total > 0 ? (int) (done * 95 / total) : 95);
            }

            c->commit (id);
            syncMeta (*c, id, dir);
            return id;
        }

        enum class MetaSync { pushed, pulled, same, skipped };

        // Last-write-wins on updatedAt: push when local is newer, pull (replacing synced keys) when the
        // cloud's is.
        static MetaSync syncMeta (Client& c, const String& id, const File& dir)
        {
            auto local = SongMeta::read (dir);
            if (SongMeta::updatedAt (local) == 0) // legacy sidecar without a timestamp: stamp it
            {
                SongMeta::update (dir, {});
                local = SongMeta::read (dir);
            }
            const auto localAt = SongMeta::updatedAt (local);
            auto remote = c.getMeta (id);
            const auto remoteAt = remote ? remote->updatedAt : 0;

            auto pull = [&] (const Client::Meta& m)
            {
                NamedValueSet p;
                if (auto* o = m.meta.getDynamicObject())
                    for (auto& kv : o->getProperties())
                        if (! SongMeta::localOnlyKeys.contains (kv.name.toString()))
                            p.set (kv.name, kv.value);
                p.set ("updatedAt", m.updatedAt);
                SongMeta::update (dir, p, false, true);
            };

            if (remote && remoteAt > localAt)
            {
                pull (*remote);
                return MetaSync::pulled;
            }
            if (localAt > remoteAt)
            {
                auto r = c.putMeta (id, SongMeta::syncedPart (local), localAt);
                if (! r.ok && r.current)
                {
                    pull (*r.current);
                    return MetaSync::pulled;
                }
                if (! r.ok)
                    throw Error (String (u8"No se pudo guardar el análisis en la nube"), 0);
                return MetaSync::pushed;
            }
            return MetaSync::same;
        }

        // Right before a synced song opens: fail-fast (quick client), never throws — offline just
        // opens the local copy.
        MetaSync syncMetaOnOpen (const Entry& e)
        {
            try
            {
                auto c = makeClient (true);
                if (c == nullptr || e.state != State::synced || ! isSongId (e.cloudId) || ! e.local)
                    return MetaSync::skipped;
                return syncMeta (*c, e.cloudId, e.local->dir);
            }
            catch (...)
            {
                return MetaSync::skipped;
            }
        }

        // Into a hidden temp folder, every file's size + hash verified, then renamed into the library
        // in one step: a failure never leaves a partial song behind.
        File download (const Entry& e, Progress progress = {})
        {
            auto c = requireClient();
            if (e.state != State::cloud || ! isSongId (e.cloudId) || ! isSafeSongName (e.name))
                throw Error (String (u8"La canción ya está en esta laptop"), 0);

            auto tmp = root.getChildFile (downloadDirPrefix + e.cloudId);
            tmp.deleteRecursively();
            tmp.createDirectory();
            struct Cleanup { File f; bool keep = false; ~Cleanup() { if (! keep) f.deleteRecursively(); } } cleanup { tmp };

            auto files = c->download (e.cloudId);
            int64 total = 0, done = 0;
            for (auto& d : files)
                total += d.file.size;

            DynamicObject::Ptr hashes = new DynamicObject();
            for (auto& d : files)
            {
                if (! isCloudFileName (d.file.name))
                    throw Error ("Nombre de archivo inesperado: " + d.file.name, 0);
                auto data = c->getFile (d.url);
                auto dest = tmp.getChildFile (d.file.name);
                if ((int64) data.getSize() != d.file.size || SHA256 (data).toHexString() != d.file.sha256)
                    throw Error (String (u8"El archivo descargado está dañado: ") + d.file.name, 0);
                if (! dest.replaceWithData (data.getData(), data.getSize()))
                    throw Error ("No se pudo guardar " + d.file.name, 0);

                DynamicObject::Ptr h = new DynamicObject();
                h->setProperty ("size", d.file.size);
                h->setProperty ("mtimeMs", dest.getLastModificationTime().toMilliseconds());
                h->setProperty ("sha256", d.file.sha256);
                hashes->setProperty (Identifier (d.file.name), var (h.get()));
                done += d.file.size;
                if (progress)
                    progress (total > 0 ? (int) (done * 95 / total) : 95);
            }

            NamedValueSet p;
            if (auto remote = c->getMeta (e.cloudId))
            {
                if (auto* o = remote->meta.getDynamicObject())
                    for (auto& kv : o->getProperties())
                        if (! SongMeta::localOnlyKeys.contains (kv.name.toString()))
                            p.set (kv.name, kv.value);
                p.set ("updatedAt", remote->updatedAt);
            }
            p.set ("cloudId", e.cloudId);
            p.set ("fileHashes", var (hashes.get()));
            SongMeta::update (tmp, p, false);

            auto dest = root.getChildFile (e.name);
            if (dest.exists())
                throw Error (String (u8"Ya existe una carpeta llamada «") + e.name + String (u8"»"), 0);
            if (! tmp.moveFileTo (dest))
                throw Error ("No se pudo mover la descarga a la biblioteca", 0);
            cleanup.keep = true;
            return dest;
        }

        // Frees disk space: re-uploads first (files + metadata) so nothing exists only here, and only
        // then does the local folder go (`trash`).
        void removeLocal (const Entry& e, const std::function<bool (const File&)>& trash)
        {
            if (e.state != State::synced || ! e.local)
                throw Error (String (u8"Solo se puede quitar de la laptop una canción que ya está en la nube"), 0);
            try { upload (e.name); }
            catch (const Error& err) { throw Error (String (u8"No se pudo sincronizar antes de quitarla; no se borró nada: ") + err.message(), err.status); }
            if (! trash (e.local->dir))
                throw Error ("No se pudo mandar a la papelera " + e.local->dir.getFullPathName(), 0);
        }

        // Deletes from the cloud first; the caller removes the local copy only once this succeeded.
        void deleteFromCloud (const Entry& e)
        {
            if (e.state == State::local || ! isSongId (e.cloudId))
                return;
            auto c = requireClient();
            try { c->deleteSong (e.cloudId); }
            catch (const Error& err) { throw Error (String (u8"No se pudo borrar de la nube: ") + err.message(), err.status); }
        }

        // Download folders left behind by a crash or quit mid-download.
        void cleanupStaleDownloads()
        {
            for (auto& d : root.findChildFiles (File::findDirectories, false, downloadDirPrefix + "*"))
                d.deleteRecursively();
        }

    private:
        File root;
        MakeClient makeClient;

        std::unique_ptr<Client> requireClient()
        {
            auto c = makeClient (false);
            if (c == nullptr)
                throw Error (String (u8"La sincronización con la nube no está activada"), 0);
            return c;
        }
    };
}
