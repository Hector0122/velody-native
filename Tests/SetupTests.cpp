#include <JuceHeader.h>
#include "../Source/Setup.h"

// First-run setup against a fake system (no real pip/PyTorch download): which steps run, in what
// order, what is skipped when it's already there, and what the user reads when something fails.
struct SetupTests : UnitTest
{
    SetupTests() : UnitTest ("Setup (first run)") {}

    File tmp;

    // A pretend machine: python3.11 on PATH or not, pip that can fail per package, a downloadable
    // standalone Python or not. Every command run is recorded.
    struct FakeSystem
    {
        File root;
        bool systemPython = true, downloadWorks = true;
        String systemPythonVersion = "Python 3.11.9";
        StringArray pipFails, commands;
        File bin() const { return root.getChildFile ("bin"); }
        File ffmpeg() const { return root.getChildFile ("ffmpeg"); }

        Setup::Env env()
        {
            bin().createDirectory();
            if (systemPython)
                bin().getChildFile ("python3.11").replaceWithText ("#!");
            ffmpeg().replaceWithText ("#!");

            Setup::Env e;
            e.path = bin().getFullPathName() + ":/nonexistent";
            e.ffmpeg = ffmpeg();
            e.download = [this] (const String& url, const File& dest, std::function<void (int)> progress, auto)
            {
                commands.add ("download " + url.fromLastOccurrenceOf ("/", false, false));
                if (! downloadWorks)
                    return false;
                progress (50);
                progress (100);
                return dest.replaceWithText ("tarball");
            };
            e.run = [this] (const StringArray& args, std::function<void (const String&)> onLine, auto, int)
            {
                commands.add (File (args[0]).getFileName() + " " + args.joinIntoString (" ", 1));
                PythonWorker::Ran r;
                r.started = true;
                r.code = 0;
                if (args[1] == "--version")
                    onLine (args[0].contains ("velody/python/") ? "Python 3.11.15" : systemPythonVersion);
                else if (args[1] == "-m" && args[2] == "venv")
                {
                    File venv (args[3]);
                    venv.getChildFile ("bin").createDirectory();
                    venv.getChildFile ("bin/python3").replaceWithText ("#!");
                    venv.getChildFile ("pyvenv.cfg").replaceWithText ("home = /usr/bin\nversion = 3.11.9\n");
                    venv.getChildFile ("lib/python3.11/site-packages").createDirectory();
                }
                else if (args[1] == "-m" && args[2] == "pip")
                {
                    auto site = File (args[0]).getParentDirectory().getParentDirectory().getChildFile ("lib/python3.11/site-packages");
                    for (int i = 5; i < args.size(); ++i)
                    {
                        onLine ("Collecting " + args[i]);
                        if (pipFails.contains (args[i]))
                        {
                            onLine ("ERROR: No matching distribution found for " + args[i]);
                            r.code = 1;
                            return r;
                        }
                        site.getChildFile (Setup::distName (args[i]) + "-1.0.dist-info").createDirectory();
                    }
                    onLine ("Successfully installed");
                }
                else if (File (args[0]).getFileName() == "tar")
                {
                    File into (args[4]);
                    into.getChildFile ("python/bin").createDirectory();
                    into.getChildFile ("python/bin/python3").replaceWithText ("#!");
                }
                return r;
            };
            return e;
        }
    };

    Setup::Paths paths (const String& name) { return { tmp.getChildFile (name + "/share/velody") }; }

    struct Run { String error; Array<Setup::Event> events; };

    static Run install (const Setup::Paths& p, Setup::Env e)
    {
        Run r;
        Setup::Installer inst (p, std::move (e));
        inst.onEvent = [&] (const Setup::Event& ev) { r.events.add (ev); };
        r.error = inst.run ([] { return false; });
        return r;
    }

    static bool stepDone (const Run& r, Setup::Step s)
    {
        for (auto& e : r.events)
            if (e.step == s && e.state == Setup::Event::State::done)
                return true;
        return false;
    }

    void runTest() override
    {
        using namespace Setup;
        tmp = File::createTempFile ("velody-setup");
        tmp.createDirectory();

        beginTest ("versions, pyvenv.cfg and installed packages are read from files");
        expectEquals (parseVersion ("Python 3.11.15"), String ("3.11"));
        expectEquals (parseVersion ("Python 3.9.1\n"), String ("3.9"));
        expectEquals (parseVersion ("bash: python3.11: not found"), String());
        expect (isCompatible ("3.10") && ! isCompatible ("3.12") && ! isCompatible ("3.8"));
        auto venv = tmp.getChildFile ("v");
        venv.getChildFile ("pyvenv.cfg").create();
        venv.getChildFile ("pyvenv.cfg").replaceWithText ("home = /x\nversion_info = 3.10.4.final.0\n");
        expectEquals (venvVersion (venv), String ("3.10"));
        for (auto d : { "torch-2.1.0.dist-info", "yt_dlp-2024.1.1.dist-info", "Beat_This-0.1.dist-info", "librosa-0.10" })
            venv.getChildFile (String ("lib/python3.10/site-packages/") + d).createDirectory();
        expect (hasPackage (venv, "torch") && hasPackage (venv, "yt-dlp") && hasPackage (venv, "beat-this"));
        expect (! hasPackage (venv, "librosa")); // a folder that isn't a dist-info doesn't count
        expectEquals (missingPackages (venv, requiredPackages).joinIntoString (","), String ("demucs,torchaudio,librosa"));

        beginTest ("check: nothing installed / all there");
        FakeSystem fresh { tmp.getChildFile ("fresh") };
        auto p = paths ("fresh");
        auto env = fresh.env();
        auto st = check (p, env.ffmpeg);
        expect (! st.ready() && ! st.venv && st.missing.size() == 5 && st.ffmpeg);

        beginTest ("fresh machine with python3.11: venv + every package + Beat This!, in that order");
        auto r = install (p, env);
        expect (r.error.isEmpty(), r.error);
        for (auto s : { Step::python, Step::venv, Step::packages, Step::ffmpeg })
            expect (stepDone (r, s));
        expect (check (p, env.ffmpeg).ready());
        expect (hasPackage (p.venv(), optionalPackage));
        expectEquals (fresh.commands.size(), 4);
        expect (fresh.commands[0] == "python3.11 --version");
        expect (fresh.commands[1].startsWith ("python3.11 -m venv "));
        expect (fresh.commands[2].endsWith ("install --upgrade demucs torch torchaudio librosa yt-dlp"), fresh.commands[2]);
        expect (fresh.commands[3].endsWith ("install --upgrade beat-this"));

        beginTest ("already installed: nothing runs");
        fresh.commands.clear();
        r = install (p, env);
        expect (r.error.isEmpty() && fresh.commands.isEmpty(), fresh.commands.joinIntoString ("|"));

        beginTest ("one package missing: pip installs only that one");
        p.venv().getChildFile ("lib/python3.11/site-packages/librosa-1.0.dist-info").deleteRecursively();
        r = install (p, env);
        expect (r.error.isEmpty());
        expectEquals (fresh.commands.joinIntoString ("|").fromFirstOccurrenceOf ("--upgrade ", false, false), String ("librosa"));

        beginTest ("a venv on the wrong Python (3.12) is recreated");
        p.venv().getChildFile ("pyvenv.cfg").replaceWithText ("version = 3.12.3\n");
        fresh.commands.clear();
        r = install (p, env);
        expect (r.error.isEmpty() && venvVersion (p.venv()) == "3.11" && check (p, env.ffmpeg).ready());
        expect (fresh.commands.joinIntoString ("|").contains ("-m venv"));

        beginTest ("no Python 3.9–3.11 (Ubuntu 24.04 has 3.12): a standalone 3.11 is downloaded and used");
        FakeSystem noble { tmp.getChildFile ("noble") };
        noble.systemPython = false;
        auto pn = paths ("noble");
        auto en = noble.env();
        r = install (pn, en);
        expect (r.error.isEmpty(), r.error);
        expect (pn.standalonePython().existsAsFile());
        expect (noble.commands[0].startsWith ("download cpython-3.11"));
        expect (noble.commands.joinIntoString ("|").contains ("python3 -m venv"));
        expect (! pn.data.getChildFile ("python3.11.tar.gz").exists() && ! pn.data.getChildFile (".python-extract").exists());
        bool sawPercent = false;
        for (auto& e : r.events)
            sawPercent |= e.step == Step::python && e.percent == 50;
        expect (sawPercent);

        beginTest ("a system python3.11 that answers another version isn't used");
        FakeSystem liar { tmp.getChildFile ("liar") };
        liar.systemPythonVersion = "Python 3.12.3";
        auto pl = paths ("liar");
        r = install (pl, liar.env());
        expect (r.error.isEmpty() && pl.standalonePython().existsAsFile());

        beginTest ("no internet for Python: says how to install it by hand, stops there");
        FakeSystem offline { tmp.getChildFile ("offline") };
        offline.systemPython = false;
        offline.downloadWorks = false;
        auto po = paths ("offline");
        r = install (po, offline.env());
        expect (r.error.contains ("sudo apt install python3.11"), r.error);
        expect (! po.venv().exists());
        expect (! stepDone (r, Step::venv));

        beginTest ("pip fails: the error shows pip's last lines and the command to run by hand");
        FakeSystem broken { tmp.getChildFile ("broken") };
        broken.pipFails.add ("torch");
        auto pb = paths ("broken");
        r = install (pb, broken.env());
        expect (r.error.contains ("No matching distribution found for torch"), r.error);
        expect (r.error.contains ("Prueba a mano") && r.error.contains ("-m pip install demucs torch"), r.error);
        expect (! check (pb, broken.ffmpeg()).ready());

        beginTest ("Beat This! failing doesn't fail the setup");
        FakeSystem nobeat { tmp.getChildFile ("nobeat") };
        nobeat.pipFails.add ("beat-this");
        auto pnb = paths ("nobeat");
        r = install (pnb, nobeat.env());
        expect (r.error.isEmpty(), r.error);
        expect (check (pnb, nobeat.ffmpeg()).ready());
        String packagesDone;
        for (auto& e : r.events)
            if (e.step == Step::packages && e.state == Event::State::done)
                packagesDone = e.message;
        expect (packagesDone.contains ("librosa"), packagesDone);

        beginTest ("no ffmpeg: says to apt install it");
        FakeSystem noff { tmp.getChildFile ("noff") };
        auto pf = paths ("noff");
        auto ef = noff.env();
        ef.ffmpeg = tmp.getChildFile ("missing/ffmpeg");
        r = install (pf, ef);
        expect (r.error.contains ("sudo apt install ffmpeg"));

        beginTest ("Instalación window: Instalar runs it and every step turns ✓; a failure offers Reintentar");
        {
            FakeSystem ui { tmp.getChildFile ("ui") };
            auto pu = paths ("ui");
            int finished = 0;
            Panel panel (pu, ui.env(), [&] { ++finished; });
            expect (panel.steps[2]->getText().contains ("falta"));
            expect (panel.installButton.isEnabled() && panel.installButton.getButtonText() == "Instalar");
            panel.installButton.triggerClick();
            expect (waitFor ([&] { return finished == 1; }));
            expect (panel.result.isEmpty(), panel.result);
            for (auto* l : panel.steps)
                expect (l->getText().startsWith (String (u8"✓")), l->getText());
            expect (! panel.installButton.isEnabled() && panel.installButton.getButtonText() == "Listo");
            expect (panel.log.getText().contains ("Listo"));
        }
        {
            FakeSystem ui { tmp.getChildFile ("ui2") };
            ui.pipFails.add ("demucs");
            int finished = 0;
            Panel panel (paths ("ui2"), ui.env(), [&] { ++finished; });
            panel.installButton.triggerClick();
            expect (waitFor ([&] { return finished == 1; }));
            expect (panel.steps[2]->getText().startsWith (String (u8"✕")), panel.steps[2]->getText());
            expect (panel.installButton.isEnabled() && panel.installButton.getButtonText() == "Reintentar");
            expect (panel.log.getText().contains ("demucs"));
        }

        tmp.deleteRecursively();
    }

    template <typename Cond>
    static bool waitFor (Cond cond, int timeoutMs = 10000)
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
};

static SetupTests setupTests;
