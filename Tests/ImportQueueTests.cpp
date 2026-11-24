#include <JuceHeader.h>
#include "../Source/ImportQueue.h"

// The import queue with fake steps (no Python): order, one at a time, cancel waiting/running jobs,
// failures don't stop the queue, links, temp cleanup, and the Importar menu driving it.
struct ImportQueueTests : UnitTest
{
    ImportQueueTests() : UnitTest ("Import queue") {}

    template <typename Cond>
    static bool waitFor (Cond cond, int timeoutMs = 10000)
    {
        const auto end = Time::getMillisecondCounter() + (uint32) timeoutMs;
        while (! cond())
        {
            if (Time::getMillisecondCounter() > end)
                return false;
            MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
        return true;
    }

    static std::optional<PopupMenu::Item> item (const PopupMenu& m, const String& prefix)
    {
        for (PopupMenu::MenuItemIterator it (m, true); it.next();)
            if (it.getItem().text.startsWith (prefix))
                return it.getItem();
        return {};
    }

    // Fake separation: "fail*" fails, "block*" runs until stopped, the rest finish at once. Stems
    // go to <root>/<name>/drums.mp3 like separate.py.
    struct Fake
    {
        File root;
        CriticalSection lock;
        StringArray separated, analysed;
        std::atomic<int> running { 0 }, maxRunning { 0 };
        std::atomic<bool> blockedStarted { false };

        Import::Steps steps()
        {
            Import::Steps s;
            s.download = [this] (const String& url, auto onProgress, auto, Import::Stop)
            {
                PythonWorker::Outcome o;
                if (url.contains ("bad"))
                {
                    o.error = "Video no disponible";
                    return o;
                }
                auto dir = root.getChildFile ("velody_dl_" + String (Random::getSystemRandom().nextInt (1000000)));
                dir.createDirectory();
                auto f = dir.getChildFile ("Titulo Real.mp3");
                f.replaceWithText ("x");
                onProgress ("Titulo Real", 50);
                o.ok = true;
                o.path = f.getFullPathName();
                return o;
            };
            s.separate = [this] (const File& audio, auto onProgress, Import::Stop stop)
            {
                const int now = ++running;
                maxRunning = jmax (maxRunning.load(), now);
                PythonWorker::Outcome o;
                const auto name = audio.getFileNameWithoutExtension();
                if (name.startsWith ("block"))
                {
                    blockedStarted = true;
                    while (! stop())
                        Thread::sleep (5);
                    o.cancelled = true;
                    --running;
                    return o;
                }
                Thread::sleep (20);
                onProgress (40);
                --running;
                if (name.startsWith ("fail"))
                {
                    o.error = "Demucs se quedó sin memoria\ndetalle";
                    return o;
                }
                auto stem = root.getChildFile (name + "/drums.mp3");
                stem.getParentDirectory().createDirectory();
                stem.replaceWithText ("d");
                o.ok = true;
                o.stems.add (stem.getFullPathName());
                const ScopedLock sl (lock);
                separated.add (name);
                return o;
            };
            s.analyse = [this] (const String& name, Import::Stop)
            {
                const ScopedLock sl (lock);
                analysed.add (name);
            };
            return s;
        }
    };

    static int count (const std::vector<Import::Job>& jobs, Import::Job::Status st)
    {
        return (int) std::count_if (jobs.begin(), jobs.end(), [st] (auto& j) { return j.status == st; });
    }

    void runTest() override
    {
        auto tmp = File::createTempFile ("velody-import");
        tmp.createDirectory();
        auto audio = [&] (const String& name) { auto f = tmp.getChildFile ("in/" + name + ".mp3"); f.getParentDirectory().createDirectory(); f.replaceWithText ("a"); return f; };
        using St = Import::Job::Status;

        beginTest ("only audio files are importable");
        expect (Import::isImportableFile (audio ("a")));
        tmp.getChildFile ("in/notes.txt").replaceWithText ("x");
        expect (! Import::isImportableFile (tmp.getChildFile ("in/notes.txt")));
        expect (! Import::isImportableFile (tmp.getChildFile ("in/missing.mp3")));

        beginTest ("several files: one at a time, in order, each separated then analysed; a failure doesn't stop the rest");
        {
            Fake fake { tmp.getChildFile ("lib1") };
            Import::Queue q (fake.steps());
            StringArray imported;
            int changes = 0;
            q.onImported = [&] (const String& n) { imported.add (n); };
            q.onChanged = [&] { ++changes; };
            for (auto n : { "uno", "fail-dos", "tres" })
                q.addFile (audio (n));
            expect (q.pending() == 3);
            expect (waitFor ([&] { return q.pending() == 0 && imported.size() == 2; }));
            expectEquals (fake.maxRunning.load(), 1);
            expectEquals (imported.joinIntoString (","), String ("uno,tres"));
            expectEquals (fake.analysed.joinIntoString (","), String ("uno,tres"));
            auto jobs = q.snapshot();
            expect (jobs[0].status == St::done && jobs[1].status == St::failed && jobs[2].status == St::done);
            expect (jobs[1].error.contains ("memoria"));
            expect (Import::statusLabel (jobs[1]).startsWith (String (u8"✕ fail-dos: Demucs")));
            expect (! Import::statusLabel (jobs[1]).contains ("detalle")); // first line only in the menu
            expect (fake.root.getChildFile ("uno/drums.mp3").existsAsFile());
            expect (changes > 0);

            q.clearFinished();
            expect (q.snapshot().empty());
        }

        beginTest ("cancel: a waiting job never runs; the running one is stopped and the next one goes");
        {
            Fake fake { tmp.getChildFile ("lib2") };
            Import::Queue q (fake.steps());
            const int blocked = q.addFile (audio ("block"));
            const int waiting = q.addFile (audio ("espera"));
            q.addFile (audio ("despues"));
            expect (waitFor ([&] { return fake.blockedStarted.load(); }));
            expect (q.statusText().startsWith ("Separando block"), q.statusText());
            expect (q.statusText().contains ("(1 de 3)"), q.statusText());
            expect (q.cancel (waiting));
            expect (q.cancel (blocked));
            expect (waitFor ([&] { return q.pending() == 0; }));
            auto jobs = q.snapshot();
            expect (jobs[0].status == St::cancelled && jobs[1].status == St::cancelled && jobs[2].status == St::done);
            expectEquals (fake.separated.joinIntoString (","), String ("despues"));
            expect (! q.cancel (jobs[2].id)); // finished: nothing to cancel
        }

        beginTest ("a link: downloaded, named after the video title, its temp folder removed; a bad link fails alone");
        {
            Fake fake { tmp.getChildFile ("lib3") };
            fake.root.createDirectory();
            Import::Queue q (fake.steps());
            StringArray imported;
            q.onImported = [&] (const String& n) { imported.add (n); };
            q.addUrl ("https://youtu.be/bad");
            q.addUrl ("  https://youtu.be/ok ");
            expect (waitFor ([&] { return q.pending() == 0; }));
            auto jobs = q.snapshot();
            expect (jobs[0].status == St::failed && jobs[0].error == "Video no disponible");
            expect (jobs[1].status == St::done && jobs[1].name == "Titulo Real" && jobs[1].url == "https://youtu.be/ok");
            expect (waitFor ([&] { return imported.size() == 1; }));
            expectEquals (imported[0], String ("Titulo Real"));
            expectEquals (fake.root.findChildFiles (File::findDirectories, false, "velody_dl_*").size(), 0);
        }

        beginTest ("closing the app mid-import stops the running job instead of hanging");
        {
            Fake fake { tmp.getChildFile ("lib4") };
            auto q = std::make_unique<Import::Queue> (fake.steps());
            q->addFile (audio ("block2"));
            expect (waitFor ([&] { return fake.blockedStarted.load(); }));
            const auto t0 = Time::getMillisecondCounter();
            q = nullptr;
            expect (Time::getMillisecondCounter() - t0 < 5000);
        }

        beginTest ("Importar menu: add actions, click a job to cancel it, read a failure, clear finished");
        {
            Fake fake { tmp.getChildFile ("lib5") };
            Import::Queue q (fake.steps());
            std::vector<Import::MenuAction> fired;
            auto act = [&] (const Import::MenuAction& a)
            {
                fired.push_back (a);
                if (a.kind == Import::MenuAction::cancel) q.cancel (a.job.id);
                if (a.kind == Import::MenuAction::clearFinished) q.clearFinished();
            };

            auto empty = Import::menu (q.snapshot(), act);
            expect (item (empty, String (u8"Archivos…")).has_value() && item (empty, String (u8"Desde YouTube…")).has_value());
            expect (! item (empty, "Cola").has_value());

            q.addFile (audio ("block3"));
            q.addFile (audio ("fail-x"));
            expect (waitFor ([&] { return fake.blockedStarted.load(); }));
            auto m = Import::menu (q.snapshot(), act);
            auto running = item (m, "block3  (separando");
            expect (running.has_value() && running->isEnabled);
            expect (item (m, "fail-x  (en espera)").has_value());
            running->action();
            expect (waitFor ([&] { return q.pending() == 0; }));

            m = Import::menu (q.snapshot(), act);
            expect (item (m, "block3  (cancelada)").has_value() && ! item (m, "block3  (cancelada)")->isEnabled);
            item (m, String (u8"✕ fail-x"))->action();
            expect (fired.back().kind == Import::MenuAction::showError && fired.back().job.error.contains ("detalle"));
            item (m, "Limpiar las terminadas")->action();
            expect (q.snapshot().empty());
        }

        beginTest ("Importar menu: stem quality shows the current one and picking another fires it");
        {
            std::vector<Import::MenuAction> fired;
            auto m = Import::menu ({}, [&] (const Import::MenuAction& a) { fired.push_back (a); }, 192);
            expect (item (m, "Calidad de los stems: 192 kbps").has_value());
            expect (item (m, "192 kbps")->isTicked && ! item (m, "128 kbps")->isTicked);
            item (m, "320 kbps")->action();
            expect (fired.size() == 1 && fired[0].kind == Import::MenuAction::setBitrate && fired[0].bitrate == 320);
        }

        tmp.deleteRecursively();
    }
};

static ImportQueueTests importQueueTests;
