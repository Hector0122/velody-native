#pragma once
#include <JuceHeader.h>
#include <regex>
#include "CloudClient.h"
#include "CloudUi.h"
#include "ImportQueue.h"

// Vibes → Velody: songs sent from Vibes (desktop or phone, "Mandar a Velody") wait in Vibes' Worker
// (GET /v1/velody). Velody lists them, downloads each one's audio (GET /v1/songs/{id}/stream, an .m4a
// in Vibes' R2) into the import queue, and once it is separated and analysed takes it off the list
// (DELETE /v1/velody/{id}). A failed one stays in Vibes, so it can be retried.
namespace Vibes
{
    // ~/.config/Velody/vibes.settings: Vibes' Worker URL + token (0600, like the cloud's).
    inline File settingsFile()
    {
        return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Velody/vibes.settings");
    }

    struct Send { String id, title, artists; };

    inline bool isSongId (const String& id)
    {
        static const std::regex re ("^[A-Za-z0-9_-]{11}$");
        return std::regex_match (id.toStdString(), re);
    }

    // "Artista - Título", as a plain folder name (the library folder is named after the file).
    inline String songName (const Send& s)
    {
        auto raw = (s.artists.trim().isNotEmpty() ? s.artists.trim() + " - " : String()) + s.title.trim();
        auto name = File::createLegalFileName (raw).trimCharactersAtStart (". ").substring (0, 120).trim();
        return name.isNotEmpty() ? name : s.id;
    }

    inline std::vector<Send> listSends (Cloud::Client& c)
    {
        std::vector<Send> out;
        auto r = c.api ("GET", "/velody");
        if (auto* a = r["sends"].getArray())
            for (auto& v : *a)
                if (isSongId (v["id"].toString()))
                    out.push_back ({ v["id"].toString(), v["title"].toString(), v["artists"].toString() });
        return out;
    }

    // The song's audio into <tmpRoot>/velody_dl_vibes_<id>/<name>.m4a (the queue deletes the folder).
    inline PythonWorker::Outcome fetch (Cloud::Client& c, const Send& s, const File& tmpRoot)
    {
        PythonWorker::Outcome o;
        try
        {
            auto url = c.api ("GET", "/songs/" + s.id + "/stream")["url"].toString();
            auto data = c.getFile (url);
            auto dir = tmpRoot.getChildFile ("velody_dl_vibes_" + s.id);
            dir.deleteRecursively();
            dir.createDirectory();
            auto f = dir.getChildFile (songName (s) + ".m4a");
            if (data.getSize() == 0 || ! f.replaceWithData (data.getData(), data.getSize()))
                throw Cloud::Error ("No se pudo guardar el audio de Vibes", 0);
            o.ok = true;
            o.path = f.getFullPathName();
        }
        catch (const Cloud::Error& e)
        {
            o.error = "Vibes: " + e.message();
        }
        return o;
    }

    inline void unsend (Cloud::Client& c, const String& id) { c.api ("DELETE", "/velody/" + id); }

    // Checks Vibes' list off the message thread and queues what's new. Main calls check() at start,
    // every few minutes and from "Traer de Vibes".
    class Poller
    {
    public:
        Poller (Import::Queue& q, File settings, File tmp) : queue (q), settingsPath (std::move (settings)), tmpRoot (std::move (tmp)) {}

        bool active() const { return Cloud::Settings::load (settingsPath).active(); }
        bool checking() const { return busy; }

        // done(added, error) on the message thread. Without Vibes configured: error, nothing checked.
        void check (std::function<void (int, const String&)> done)
        {
            auto s = Cloud::Settings::load (settingsPath);
            if (! s.active())
                return done (0, String (u8"Configura Vibes primero (Importar → Vibes…)"));
            if (busy)
                return done (0, {});

            busy = true;
            auto sends = std::make_shared<std::vector<Send>>();
            runner.run ([s, sends] (const Cloud::Progress&)
                        {
                            Cloud::Client c (s.url, s.token, 8000, 0);
                            *sends = listSends (c);
                        },
                        [this, s, sends, done] (const String& error)
                        {
                            busy = false;
                            if (error.isNotEmpty())
                                return done (0, error);
                            int added = 0;
                            for (auto& send : *sends)
                            {
                                const auto key = "vibes:" + send.id;
                                if (queue.has (key))
                                    continue;
                                ++added;
                                queue.addRemote (key, songName (send),
                                    [s, send, tmp = tmpRoot] (std::function<void (int)>, std::function<bool()>)
                                    {
                                        Cloud::Client c (s.url, s.token);
                                        return fetch (c, send, tmp);
                                    },
                                    [s, id = send.id]
                                    {
                                        // Off the list once it is in the library; if this fails, the next check
                                        // finds it already queued (done) and leaves it.
                                        try { Cloud::Client c (s.url, s.token); unsend (c, id); }
                                        catch (...) {}
                                    });
                            }
                            done (added, {});
                        });
        }

    private:
        Import::Queue& queue;
        File settingsPath, tmpRoot;
        CloudUi::Runner runner;
        bool busy = false;
    };
}
