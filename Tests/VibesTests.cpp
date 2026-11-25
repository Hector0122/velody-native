#include <JuceHeader.h>
#include "../Source/Vibes.h"

// Vibes → Velody against Tests/fake_cloud.py (Vibes' routes): a song sent from Vibes is fetched,
// separated (fake step) into the library and taken off Vibes' list; failures stay there.
struct VibesTests : UnitTest
{
    VibesTests() : UnitTest ("Vibes to Velody") {}

    template <typename Cond>
    static bool waitFor (Cond cond, int timeoutMs = 15000)
    {
        const auto end = Time::getMillisecondCounter() + (uint32) timeoutMs;
        while (! cond())
        {
            if (Time::getMillisecondCounter() > end)
                return false;
            MessageManager::getInstance()->runDispatchLoopUntil (10);
        }
        return true;
    }

    void runTest() override
    {
        auto tmp = File::createTempFile ("velody-vibes");
        tmp.createDirectory();

        beginTest ("song names: 'Artista - Título', always a plain folder name");
        expectEquals (Vibes::songName ({ "dQw4w9WgXcQ", "Afuera", "Caifanes" }), String ("Caifanes - Afuera"));
        expectEquals (Vibes::songName ({ "dQw4w9WgXcQ", "AC/DC: Back", "" }), String ("ACDC Back"));
        expectEquals (Vibes::songName ({ "dQw4w9WgXcQ", "...", "" }), String ("dQw4w9WgXcQ"));
        expect (Vibes::isSongId ("dQw4w9WgXcQ") && ! Vibes::isSongId ("../../etc/x") && ! Vibes::isSongId ("short"));

        PythonWorker::Process server;
        expect (server.start ({ "/usr/bin/python3", File (__FILE__).getSiblingFile ("fake_cloud.py").getFullPathName(), "tok" }), "fake worker");
        String port;
        server.readLine (port, 5000);
        const auto url = "http://127.0.0.1:" + port.trim();
        auto post = [&] (const String& path) { URL::createWithoutParsing (url + path).withPOSTData ("").readEntireTextStream (true); };
        auto vibesState = [&] { return JSON::parse (URL (url + "/_vibes").readEntireTextStream()); };

        // Fake separation: the song folder gets a drums stem named after the file, like separate.py.
        auto lib = tmp.getChildFile ("lib");
        StringArray separated;
        Import::Steps steps;
        steps.download = [] (auto&&...) { return PythonWorker::Outcome(); };
        steps.separate = [&] (const File& audio, auto, Import::Stop)
        {
            PythonWorker::Outcome o;
            if (audio.getFileNameWithoutExtension().contains ("Rota"))
            {
                o.error = "Demucs falló";
                return o;
            }
            auto stem = lib.getChildFile (audio.getFileNameWithoutExtension() + "/drums.mp3");
            stem.getParentDirectory().createDirectory();
            stem.replaceWithText (audio.loadFileAsString());
            separated.add (audio.getFileName());
            o.ok = true;
            o.stems.add (stem.getFullPathName());
            return o;
        };
        steps.analyse = [] (auto&&...) {};

        Import::Queue queue (steps);
        auto settings = tmp.getChildFile ("cfg/vibes.settings");
        auto dl = tmp.getChildFile ("dl");
        dl.createDirectory();
        Vibes::Poller poller (queue, settings, dl);

        auto check = [&]
        {
            int added = -1;
            String error = "pending";
            poller.check ([&] (int a, const String& e) { added = a; error = e; });
            expect (waitFor ([&] { return added >= 0; }));
            return std::make_pair (added, error);
        };

        beginTest ("not configured: says so and checks nothing");
        auto r = check();
        expect (r.first == 0 && r.second.contains ("Configura Vibes"), r.second);

        beginTest ("a wrong token is reported");
        Cloud::Settings { url, "nope", true }.save (settings);
        r = check();
        expect (r.second.isNotEmpty() && r.first == 0, r.second);

        beginTest ("a sent song: downloaded, separated into the library as 'Artista - Título', taken off Vibes' list");
        Cloud::Settings { url, "tok", true }.save (settings);
        post ("/_vibes?id=aaaaaaaaaaa&title=Afuera&artists=Caifanes&size=300");
        post ("/_vibes?id=bbbbbbbbbbb&title=Rota&artists=X&size=10");
        StringArray imported;
        queue.onImported = [&] (const String& n) { imported.add (n); };
        r = check();
        expectEquals (r.first, 2);
        expect (r.second.isEmpty(), r.second);
        expect (waitFor ([&] { return queue.pending() == 0; }));
        expect (waitFor ([&] { return imported.size() == 1; }));
        expectEquals (imported[0], String ("Caifanes - Afuera"));
        expect (separated.contains ("Caifanes - Afuera.m4a"), separated.joinIntoString (","));
        expectEquals (lib.getChildFile ("Caifanes - Afuera/drums.mp3").loadFileAsString(), String::repeatedString ("m", 300));
        auto state = vibesState();
        expect (! (bool) state["aaaaaaaaaaa"]["sent"]);  // done: off the list
        expect ((bool) state["bbbbbbbbbbb"]["sent"]);    // failed: stays in Vibes for a retry
        expectEquals (dl.findChildFiles (File::findDirectories, false, "velody_dl_*").size(), 0);
        auto jobs = queue.snapshot();
        auto failed = std::find_if (jobs.begin(), jobs.end(), [] (auto& j) { return j.key == "vibes:bbbbbbbbbbb"; });
        expect (failed != jobs.end() && failed->status == Import::Job::Status::failed && failed->error.contains ("Demucs"));

        beginTest ("checking again doesn't queue the failed song again until the finished ones are cleared");
        r = check();
        expectEquals (r.first, 0);
        queue.clearFinished();
        r = check();
        expectEquals (r.first, 1);
        expect (waitFor ([&] { return queue.pending() == 0; }));

        beginTest ("a sent song already in the library isn't queued again, just taken off Vibes' list");
        {
            Vibes::Poller withLibrary (queue, settings, dl, lib);
            queue.clearFinished();
            post ("/_vibes?id=ccccccccccc&title=Afuera&artists=Caifanes&size=50"); // same name as one already separated
            int added = -1;
            withLibrary.check ([&] (int a, const String&) { added = a; });
            expect (waitFor ([&] { return added >= 0; }));
            expect (! queue.has ("vibes:ccccccccccc")); // (the failed "Rota" may be retried; this one isn't)
            expect (waitFor ([&] { return ! (bool) vibesState()["ccccccccccc"]["sent"]; }));
            expect (waitFor ([&] { return queue.pending() == 0; }));
        }

        beginTest ("closing Velody during the analysis still takes the separated song off Vibes' list");
        {
            Import::Steps slow = steps;
            std::atomic<bool> analysing { false };
            slow.analyse = [&] (const String&, Import::Stop stop) { analysing = true; while (! stop()) Thread::sleep (5); };
            auto q = std::make_unique<Import::Queue> (slow);
            Vibes::Poller p2 (*q, settings, dl);
            post ("/_vibes?id=ddddddddddd&title=Lenta&artists=Banda&size=40");
            int added = -1;
            p2.check ([&] (int a, const String&) { added = a; });
            expect (waitFor ([&] { return analysing.load(); }));
            q = nullptr; // like closing the app mid-analysis
            expect (! (bool) vibesState()["ddddddddddd"]["sent"]);
            expect (lib.getChildFile ("Banda - Lenta").isDirectory());
        }

        beginTest ("Vibes unreachable: an error, nothing queued");
        Cloud::Settings { "http://127.0.0.1:1", "tok", true }.save (settings);
        queue.clearFinished();
        r = check();
        expect (r.first == 0 && r.second.isNotEmpty());
        expect (queue.snapshot().empty());

        server.terminate();
        tmp.deleteRecursively();
    }
};

static VibesTests vibesTests;
