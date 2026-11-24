#pragma once
#include <JuceHeader.h>
#include <regex>
#include <sys/stat.h>

// Client for the personal cloud Worker (velody/cloud-worker, contract in velody/src/shared/cloud.ts),
// ported from velody/src/main/cloud/client.ts: JSON API under /v1 plus raw transfers to presigned R2
// URLs. Retries only network failures and 5xx, never 4xx (they won't change on retry).
namespace Cloud
{
    struct FileInfo { String name; int64 size = 0; String sha256; };

    struct Song
    {
        String id, name;
        std::vector<FileInfo> files;
        int64 metaUpdatedAt = 0; // 0 = no metadata uploaded yet
        int64 updatedAt = 0;
    };

    struct Error : std::runtime_error
    {
        Error (const String& m, int s, var b = {}) : std::runtime_error (m.toStdString()), status (s), body (std::move (b)) {}
        String message() const { return String::fromUTF8 (what()); }
        int status; // 0 = network / timeout
        var body;
    };

    inline const std::regex& songIdRe()  { static const std::regex r ("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"); return r; }
    inline const std::regex& sha256Re()  { static const std::regex r ("^[0-9a-f]{64}$"); return r; }

    // CLOUD_FILE_NAME_RE: plain names only (no separators, no leading dot), mp3/wav.
    inline bool isCloudFileName (const String& n)
    {
        return n.isNotEmpty() && n.length() <= 204 && ! n.startsWithChar ('.') && ! n.containsAnyOf ("/\\")
               && ! n.containsChar (0) && (n.endsWithIgnoreCase (".mp3") || n.endsWithIgnoreCase (".wav"));
    }

    // https only, except plain http to this machine (wrangler dev / tests). Trailing slashes trimmed.
    inline std::optional<String> normaliseUrl (const String& raw)
    {
        auto u = raw.trim();
        while (u.endsWithChar ('/'))
            u = u.dropLastCharacters (1);
        const auto host = URL (u).getDomain().upToFirstOccurrenceOf (":", false, false);
        const bool local = host == "localhost" || host == "127.0.0.1";
        if (host.isEmpty() || ! (u.startsWith ("https://") || (local && u.startsWith ("http://"))) || u.containsAnyOf (" \t\n"))
            return {};
        return u;
    }

    // ~/.config/Velody/cloud.settings, readable only by the user (it holds the bearer token).
    struct Settings
    {
        String url, token;
        bool enabled = false;

        static juce::File file() { return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Velody/cloud.settings"); }

        bool active() const { return enabled && url.isNotEmpty() && token.isNotEmpty(); }

        static Settings load (const juce::File& f = file())
        {
            Settings s;
            auto j = JSON::parse (f);
            s.url = j["url"].toString();
            s.token = j["token"].toString();
            s.enabled = (bool) j["enabled"];
            return s;
        }

        bool save (const juce::File& f = file()) const
        {
            DynamicObject::Ptr o = new DynamicObject();
            o->setProperty ("url", url);
            o->setProperty ("token", token);
            o->setProperty ("enabled", enabled);
            f.getParentDirectory().createDirectory();
            // Written in place into a file that is 0600 before the token goes in (replaceWithText
            // would write a fresh file with default permissions), then renamed over the old one.
            auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
            tmp.deleteFile();
            if (! tmp.create() || ::chmod (tmp.getFullPathName().toRawUTF8(), 0600) != 0)
                return false;
            {
                FileOutputStream out (tmp);
                if (! out.openedOk() || ! out.writeText (JSON::toString (var (o.get())), false, false, nullptr))
                    return false;
                out.flush();
                if (out.getStatus().failed())
                    return false;
            }
            return tmp.moveFileTo (f);
        }
    };

    class Client
    {
    public:
        Client (String baseUrl, String bearer, int timeout = 15000, int retryCount = 2, int retryDelay = 500)
            : base (baseUrl + "/v1"), token (std::move (bearer)), timeoutMs (timeout), retries (retryCount), retryDelayMs (retryDelay) {}

        void health() { api ("GET", "/health"); }

        std::vector<Song> listSongs()
        {
            std::vector<Song> out;
            auto r = api ("GET", "/songs"); // kept alive: getArray() points into it
            if (auto* a = r["songs"].getArray())
                for (auto& s : *a)
                    out.push_back (parseSong (s));
            return out;
        }

        struct Presigned { String name, url; };

        // Only the files that still need uploading come back.
        std::vector<Presigned> beginUpload (const String& id, const String& name, const std::vector<FileInfo>& files)
        {
            Array<var> fs;
            for (auto& f : files)
            {
                DynamicObject::Ptr o = new DynamicObject();
                o->setProperty ("name", f.name);
                o->setProperty ("size", f.size);
                o->setProperty ("sha256", f.sha256);
                fs.add (var (o.get()));
            }
            DynamicObject::Ptr req = new DynamicObject();
            req->setProperty ("name", name);
            req->setProperty ("files", fs);

            std::vector<Presigned> out;
            auto r = api ("POST", "/songs/" + id + "/upload", var (req.get()));
            if (auto* a = r["uploads"].getArray())
                for (auto& u : *a)
                    out.push_back ({ u["name"].toString(), u["url"].toString() });
            return out;
        }

        Song commit (const String& id) { return parseSong (api ("POST", "/songs/" + id + "/commit")["song"]); }

        struct Download { FileInfo file; String url; };

        std::vector<Download> download (const String& id)
        {
            std::vector<Download> out;
            auto r = api ("GET", "/songs/" + id + "/download");
            if (auto* a = r["files"].getArray())
                for (auto& f : *a)
                    out.push_back ({ parseFile (f), f["url"].toString() });
            return out;
        }

        struct Meta { var meta; int64 updatedAt = 0; };

        std::optional<Meta> getMeta (const String& id)
        {
            try
            {
                auto r = api ("GET", "/songs/" + id + "/meta");
                return Meta { r["meta"], (int64) r["updatedAt"] };
            }
            catch (const Error& e)
            {
                if (e.status == 404)
                    return {};
                throw;
            }
        }

        // ok, or (on 409) the newer version the cloud already has.
        struct PutMetaResult { bool ok = false; int64 updatedAt = 0; std::optional<Meta> current; };

        PutMetaResult putMeta (const String& id, const var& meta, int64 updatedAt)
        {
            DynamicObject::Ptr req = new DynamicObject();
            req->setProperty ("meta", meta);
            req->setProperty ("updatedAt", updatedAt);
            try
            {
                return { true, (int64) api ("PUT", "/songs/" + id + "/meta", var (req.get()))["updatedAt"], {} };
            }
            catch (const Error& e)
            {
                if (e.status != 409)
                    throw;
                PutMetaResult r;
                if (e.body["meta"].isObject() && (e.body["updatedAt"].isInt() || e.body["updatedAt"].isInt64() || e.body["updatedAt"].isDouble()))
                    r.current = Meta { e.body["meta"], (int64) e.body["updatedAt"] };
                return r;
            }
        }

        void deleteSong (const String& id) { api ("DELETE", "/songs/" + id); }

        void putFile (const String& url, const MemoryBlock& data)
        {
            auto r = send ("PUT", url, &data, {}, transferTimeoutMs);
            if (r.status < 200 || r.status >= 300)
                throw Error ("La subida al almacenamiento falló (" + String (r.status) + ")", r.status);
        }

        MemoryBlock getFile (const String& url)
        {
            auto r = send ("GET", url, nullptr, {}, transferTimeoutMs);
            if (r.status < 200 || r.status >= 300)
                throw Error ("La descarga del almacenamiento falló (" + String (r.status) + ")", r.status);
            return r.body;
        }

        int transferTimeoutMs = 120000; // one stem is a few MB

        // One JSON call to the Worker under /v1 (errors as Cloud::Error). Also used for Vibes' Worker.
        var api (const String& method, const String& path, const var& body = {})
        {
            String headers = "Authorization: Bearer " + token;
            MemoryBlock data;
            if (! body.isVoid())
            {
                headers << "\r\nContent-Type: application/json";
                auto json = JSON::toString (body, true);
                data.append (json.toRawUTF8(), json.getNumBytesAsUTF8());
            }

            auto r = send (method, base + path, body.isVoid() ? nullptr : &data, headers, timeoutMs);
            auto parsed = JSON::parse (r.body.toString());
            if (r.status < 200 || r.status >= 300)
                throw Error (r.status == 401 ? String (u8"Token de la nube inválido")
                                             : parsed["error"].isString() ? parsed["error"].toString() : "Error de la nube " + String (r.status),
                             r.status, parsed);
            return parsed;
        }

    private:
        String base, token;
        int timeoutMs, retries, retryDelayMs;

        struct Response { int status = 0; MemoryBlock body; };

        // One HTTP request with timeout; retried on network errors and 5xx.
        Response send (const String& method, const String& url, const MemoryBlock* body, const String& headers, int timeout)
        {
            Response last;
            for (int attempt = 0; attempt <= retries; ++attempt)
            {
                if (attempt > 0)
                    Thread::sleep (retryDelayMs * (1 << (attempt - 1)));

                // Without parsing: presigned URLs must keep their exact query string (the signature).
                auto u = URL::createWithoutParsing (url);
                if (body != nullptr)
                    u = u.withPOSTData (*body);

                int status = 0;
                auto opts = URL::InputStreamOptions (body != nullptr ? URL::ParameterHandling::inPostData : URL::ParameterHandling::inAddress)
                                .withHttpRequestCmd (method)
                                .withExtraHeaders (headers)
                                .withConnectionTimeoutMs (timeout)
                                .withNumRedirectsToFollow (0)
                                .withStatusCode (&status);

                last = {};
                if (auto in = u.createInputStream (opts))
                {
                    in->readIntoMemoryBlock (last.body);
                    last.status = status;
                }

                if (last.status != 0 && last.status < 500)
                    return last;
            }
            if (last.status == 0)
                throw Error ("No se pudo conectar con la nube", 0);
            return last;
        }

        static FileInfo parseFile (const var& f) { return { f["name"].toString(), (int64) f["size"], f["sha256"].toString() }; }

        static Song parseSong (const var& s)
        {
            Song song { s["id"].toString(), s["name"].toString(), {}, s["metaUpdatedAt"].isVoid() ? 0 : (int64) s["metaUpdatedAt"], (int64) s["updatedAt"] };
            if (auto* fs = s["files"].getArray())
                for (auto& f : *fs)
                    song.files.push_back (parseFile (f));
            return song;
        }
    };
}
