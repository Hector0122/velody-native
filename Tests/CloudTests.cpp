#include <JuceHeader.h>
#include "../Source/CloudSync.h"
#include "../Source/PythonWorker.h"

// Pure parts, then the real client + sync engine over HTTP against Tests/fake_cloud.py (same rules as
// the real Worker; the user's cloud is never touched).
struct CloudTests : UnitTest
{
    CloudTests() : UnitTest ("Cloud sync") {}

    File tmp;

    static void write (const File& f, const String& text) { f.getParentDirectory().createDirectory(); f.replaceWithText (text); }

    void runTest() override
    {
        tmp = File::createTempFile ("velody-cloud");
        tmp.createDirectory();
        testPure();
        testAgainstFakeWorker();
        tmp.deleteRecursively();
    }

    void testPure()
    {
        using namespace Cloud;

        beginTest ("normaliseUrl: https only (http just for localhost), trailing slashes trimmed");
        expect (normaliseUrl ("https://velody-cloud.x.workers.dev/") == String ("https://velody-cloud.x.workers.dev"));
        expect (normaliseUrl ("http://127.0.0.1:8787") == String ("http://127.0.0.1:8787"));
        expect (! normaliseUrl ("http://velody-cloud.x.workers.dev").has_value());
        expect (! normaliseUrl ("ftp://x").has_value());
        expect (! normaliseUrl ("").has_value());

        beginTest ("names from the cloud: audio files and folder names must be plain");
        expect (isCloudFileName ("vocals.mp3") && isCloudFileName ("Take 1.WAV"));
        expect (! isCloudFileName (".velody") && ! isCloudFileName ("../x.mp3") && ! isCloudFileName ("a/b.mp3") && ! isCloudFileName ("notes.txt"));
        expect (isSafeSongName ("Caifanes - Afuera"));
        expect (! isSafeSongName ("..") && ! isSafeSongName (".velody_dl_x") && ! isSafeSongName ("a/../../etc") && ! isSafeSongName (""));
        expect (isSongId ("be467b66-73cb-4add-bf65-9df012eb1c10") && ! isSongId ("../../x"));

        beginTest ("merge: by cloudId, by name when the id was lost, cloud-only, offline");
        auto lib = tmp.getChildFile ("merge");
        Array<Library::Song> local;
        for (auto n : { "A", "B", "C" })
            local.add (Library::Song { n, lib.getChildFile (n), {} });
        const String idA = "11111111-1111-4111-8111-111111111111", idB = "22222222-2222-4222-8222-222222222222",
                     idX = "33333333-3333-4333-8333-333333333333", idC = "44444444-4444-4444-8444-444444444444";
        std::vector<Song> cloud { { idA, "A", {}, 0, 0 }, { idB, "B", {}, 0, 0 }, { idX, "X", { { "drums.mp3", 10, "" } }, 0, 0 },
                                  { idC, "C", {}, 0, 0 }, { "55555555-5555-4555-8555-555555555555", "../evil", {}, 0, 0 } };
        // A linked by id, B matched by name, C local-only but a different cloud song is named C (hidden).
        auto m = merge (local, { idA, "", "66666666-6666-4666-8666-666666666666" }, cloud);
        auto find = [&] (const String& n) { return std::find_if (m.begin(), m.end(), [&] (auto& e) { return e.name == n; }); };
        expect (find ("A")->state == State::synced && find ("A")->cloudId == idA);
        expect (find ("B")->state == State::synced && find ("B")->cloudId == idB);
        expect (find ("C")->state == State::local);
        expect (find ("X")->state == State::cloud && find ("X")->sizeBytes == 10 && ! find ("X")->local);
        expect (find ("../evil") == m.end());
        expectEquals ((int) m.size(), 4);
        auto off = merge (local, { idA, "", "" }, std::nullopt);
        expect (off[0].state == State::synced && off[1].state == State::local && off.size() == 3);

        beginTest ("buildManifest: audio only, SHA-256, hashes cached in .velody by size + mtime");
        auto dir = tmp.getChildFile ("manifest");
        write (dir.getChildFile ("drums.mp3"), "abc");
        write (dir.getChildFile ("vocals.wav"), "hello");
        write (dir.getChildFile ("_tmp_rec_1.wav"), "x");
        write (dir.getChildFile ("notes.txt"), "x");
        write (dir.getChildFile (".velody"), R"({"bpm":100,"updatedAt":5})");
        auto files = buildManifest (dir);
        expectEquals ((int) files.size(), 2);
        expectEquals (files[0].name, String ("drums.mp3"));
        expectEquals (files[0].sha256, String ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        auto meta = SongMeta::read (dir);
        expectEquals ((int) meta["updatedAt"], 5); // bookkeeping doesn't count as an edit
        // Prove the cache is used: a fake digest with the same size/mtime comes back unchanged.
        auto fh = meta["fileHashes"];
        fh["drums.mp3"].getDynamicObject()->setProperty ("sha256", String::repeatedString ("0", 64));
        NamedValueSet p;
        p.set ("fileHashes", fh);
        SongMeta::update (dir, p, false);
        expectEquals (buildManifest (dir)[0].sha256, String::repeatedString ("0", 64));

        beginTest ("settings: saved readable only by the user");
        auto sf = tmp.getChildFile ("cfg/cloud.settings");
        Settings s { "https://x.workers.dev", "tok", true };
        expect (s.save (sf));
        auto back = Settings::load (sf);
        expect (back.url == s.url && back.token == "tok" && back.enabled && back.active());
        struct stat st {};
        ::stat (sf.getFullPathName().toRawUTF8(), &st);
        expectEquals ((int) (st.st_mode & 0777), 0600);
    }

    void testAgainstFakeWorker()
    {
        using namespace Cloud;

        PythonWorker::Process server;
        auto script = File (__FILE__).getSiblingFile ("fake_cloud.py");
        expect (server.start ({ "/usr/bin/python3", script.getFullPathName(), "tok" }), "fake worker");
        String port;
        server.readLine (port, 5000);
        const auto url = "http://127.0.0.1:" + port.trim();

        auto client = [&] (String token = "tok", int retries = 2) { return std::make_unique<Client> (url, token, 5000, retries, 50); };
        auto get = [&] (const String& path) { return JSON::parse (URL (url + path).readEntireTextStream()); };
        // Without parsing: URL would move the query into the (empty) POST body.
        auto post = [&] (const String& path) { URL::createWithoutParsing (url + path).withPOSTData ("").readEntireTextStream (true); };

        beginTest ("health; a wrong token says so");
        client()->health();
        try { client ("nope")->health(); expect (false); }
        catch (const Error& e) { expectEquals (e.status, 401); expect (e.message().contains (String (u8"inválido"))); }

        beginTest ("5xx is retried, but not forever");
        post ("/_fail?n=1");
        client()->health();
        post ("/_fail?n=5");
        try { client ("tok", 2)->health(); expect (false); }
        catch (const Error& e) { expectEquals (e.status, 503); }
        post ("/_fail?n=0");

        // Laptop 1 with one song.
        auto lib1 = tmp.getChildFile ("lib1");
        auto song = lib1.getChildFile ("Song");
        write (song.getChildFile ("drums.mp3"), String::repeatedString ("d", 3000));
        write (song.getChildFile ("vocals.mp3"), String::repeatedString ("v", 2000));
        write (song.getChildFile (".velody"), R"({"bpm":120,"downbeatShift":1,"updatedAt":1000})");
        bool on = true;
        Sync sync1 (lib1, [&] (bool) { return on ? client() : nullptr; });

        beginTest ("cloud off: plain local list, actions refuse politely");
        on = false;
        auto l = sync1.list();
        expect (! l.enabled && l.songs.size() == 1 && l.songs[0].state == State::local);
        try { sync1.upload ("Song"); expect (false); } catch (const Error&) {}
        on = true;

        beginTest ("upload: files + metadata reach the cloud, cloudId saved locally");
        Array<int> progress;
        auto id = sync1.upload ("Song", [&] (int p) { progress.add (p); });
        expect (isSongId (id));
        expectEquals (SongMeta::read (song)["cloudId"].toString(), id);
        expectEquals (progress.getLast(), 95);
        auto state = get ("/_state");
        expectEquals (state[Identifier (id)]["name"].toString(), String ("Song"));
        expectEquals ((int) state[Identifier (id)]["meta"]["bpm"], 120);
        expect (! state[Identifier (id)]["meta"].hasProperty ("cloudId")); // device-only keys stay home
        l = sync1.list();
        expect (l.songs.size() == 1 && l.songs[0].state == State::synced);

        beginTest ("re-upload sends only what changed");
        expect (client()->beginUpload (id, "Song", buildManifest (song)).empty());
        write (song.getChildFile ("vocals.mp3"), String::repeatedString ("w", 2000));
        auto ups = client()->beginUpload (id, "Song", buildManifest (song));
        expect (ups.size() == 1 && ups[0].name == "vocals.mp3");
        sync1.upload ("Song");

        beginTest ("metadata, last write wins: a newer cloud version is pulled, device keys kept");
        DynamicObject::Ptr newer = new DynamicObject();
        newer->setProperty ("bpm", 90);
        auto put = client()->putMeta (id, var (newer.get()), 9'000'000'000'000);
        expect (put.ok);
        Entry e1 { "Song", State::synced, id, Library::scan (lib1)[0], 0 };
        expect (sync1.syncMetaOnOpen (e1) == Sync::MetaSync::pulled);
        auto local = SongMeta::read (song);
        expectEquals ((int) local["bpm"], 90);
        expect (! local.hasProperty ("downbeatShift")); // replaced by the cloud version
        expectEquals (local["cloudId"].toString(), id);
        expect (local["fileHashes"].isObject());

        beginTest ("metadata: a local edit is pushed; an older write gets the stored version back");
        NamedValueSet edit;
        edit.set ("bpm", 95);
        SongMeta::update (song, edit); // stamps now (older than the cloud's far-future stamp)
        expect (sync1.syncMetaOnOpen (e1) == Sync::MetaSync::pulled);
        expectEquals ((int) SongMeta::read (song)["bpm"], 90);
        auto conflict = client()->putMeta (id, var (newer.get()), 1);
        expect (! conflict.ok && conflict.current.has_value() && conflict.current->updatedAt == 9'000'000'000'000);

        beginTest ("syncMetaOnOpen never throws when the cloud is down");
        Sync broken (lib1, [&] (bool) { return std::make_unique<Client> ("http://127.0.0.1:1", "tok", 300, 0, 10); });
        expect (broken.syncMetaOnOpen (e1) == Sync::MetaSync::skipped);

        // Laptop 2: empty library.
        auto lib2 = tmp.getChildFile ("lib2");
        lib2.createDirectory();
        Sync sync2 (lib2, [&] (bool) { return client(); });

        beginTest ("download: cloud-only song lands whole, verified, with its metadata");
        l = sync2.list();
        expect (l.songs.size() == 1 && l.songs[0].state == State::cloud);
        auto got = sync2.download (l.songs[0]);
        expectEquals (got.getFullPathName(), lib2.getChildFile ("Song").getFullPathName());
        expectEquals (got.getChildFile ("drums.mp3").loadFileAsString(), String::repeatedString ("d", 3000));
        auto m2 = SongMeta::read (got);
        expect (m2["cloudId"].toString() == id && (int) m2["bpm"] == 90 && m2["fileHashes"]["drums.mp3"].isObject());
        expectEquals (lib2.findChildFiles (File::findDirectories, false, ".velody_dl_*").size(), 0);
        expect (sync2.list().songs[0].state == State::synced);

        beginTest ("download: a corrupted file aborts and leaves nothing behind");
        got.deleteRecursively();
        URL::createWithoutParsing (url + "/r2/" + id + "/drums.mp3?sig=a%2Fb%3Dc").withPOSTData (String::repeatedString ("X", 3000))
            .createInputStream (URL::InputStreamOptions (URL::ParameterHandling::inPostData).withHttpRequestCmd ("PUT"));
        l = sync2.list();
        try { sync2.download (l.songs[0]); expect (false); }
        catch (const Error& e) { expect (e.message().contains (String (u8"dañado")), e.message()); }
        expect (! lib2.getChildFile ("Song").exists());
        expectEquals (lib2.findChildFiles (File::findDirectories, false, ".velody_dl_*").size(), 0);
        sync1.upload ("Song"); // repair the cloud copy from laptop 1

        beginTest ("a reinstalled laptop (no cloudId) is matched by name, not uploaded twice");
        auto lib3 = tmp.getChildFile ("lib3");
        write (lib3.getChildFile ("Song/drums.mp3"), "x");
        Sync sync3 (lib3, [&] (bool) { return client(); });
        l = sync3.list();
        expect (l.songs.size() == 1 && l.songs[0].state == State::synced);
        expectEquals (SongMeta::read (lib3.getChildFile ("Song"))["cloudId"].toString(), id);

        beginTest ("remove from this laptop: re-synced first, then only the local copy goes");
        auto trashDir = tmp.getChildFile ("trash");
        trashDir.createDirectory();
        auto trash = [&] (const File& f) { return f.moveFileTo (trashDir.getChildFile (f.getFileName())); };
        write (song.getChildFile ("guitar.mp3"), "new take");
        e1 = sync1.list().songs[0];
        sync1.removeLocal (e1, trash);
        expect (! song.exists());
        auto cloudFiles = get ("/_state")[Identifier (id)]["files"];
        expect (cloudFiles.hasProperty ("guitar.mp3")); // nothing existed only here when it left
        expect (sync1.list().songs[0].state == State::cloud);

        beginTest ("remove from this laptop refuses a song that isn't in the cloud");
        write (lib1.getChildFile ("Local/drums.mp3"), "x");
        auto onlyLocal = sync1.list();
        auto it = std::find_if (onlyLocal.songs.begin(), onlyLocal.songs.end(), [] (auto& e) { return e.name == "Local"; });
        try { sync1.removeLocal (*it, trash); expect (false); } catch (const Error&) {}
        expect (lib1.getChildFile ("Local").exists());

        beginTest ("delete everywhere: gone from the cloud");
        sync1.deleteFromCloud (sync1.list().songs[1]);
        expect (! get ("/_state").hasProperty (Identifier (id)));
        auto after = sync1.list();
        expect (after.songs.size() == 1 && after.songs[0].name == "Local");

        beginTest ("offline: the list still shows local songs and says why");
        Sync offline (lib1, [&] (bool) { return std::make_unique<Client> ("http://127.0.0.1:1", "tok", 300, 0, 10); });
        auto lo = offline.list();
        expect (lo.enabled && lo.offline && lo.error.isNotEmpty() && lo.songs.size() == 1);

        beginTest ("stale download folders are cleaned up");
        lib1.getChildFile (".velody_dl_" + id).createDirectory();
        sync1.cleanupStaleDownloads();
        expectEquals (lib1.findChildFiles (File::findDirectories, false, ".velody_dl_*").size(), 0);

        server.terminate();
    }
};

static CloudTests cloudTests;
