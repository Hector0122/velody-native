#pragma once
#include <JuceHeader.h>
#include "PythonWorker.h"

// First-run setup, ported from velody/src/main/setup (Linux only): a compatible Python (3.9–3.11,
// downloaded standalone if the system has none), the venv at ~/.local/share/velody/venv with Demucs,
// PyTorch, librosa and yt-dlp (+ Beat This!, optional), and ffmpeg (a dependency of the .deb).
// The check reads files only (pyvenv.cfg, *.dist-info), so it is instant at every start.
namespace Setup
{
    struct Paths
    {
        File data; // ~/.local/share/velody (shared with the Electron app)
        File venv() const { return data.getChildFile ("venv"); }
        File venvPython() const { return venv().getChildFile ("bin/python3"); }
        File standaloneDir() const { return data.getChildFile ("python"); }
        File standalonePython() const { return standaloneDir().getChildFile ("bin/python3"); }
    };

    inline Paths defaultPaths() { return { File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/share/velody") }; }

    inline const StringArray requiredPackages { "demucs", "torch", "torchaudio", "librosa", "yt-dlp" };
    inline const String optionalPackage { "beat-this" }; // better beat tracking; detection falls back without it
    inline const StringArray compatibleVersions { "3.11", "3.10", "3.9" };
    inline const String standalonePythonUrl {
        "https://github.com/astral-sh/python-build-standalone/releases/download/20260623/"
        "cpython-3.11.15%2B20260623-x86_64-unknown-linux-gnu-install_only.tar.gz" };

    // "Python 3.11.15" -> "3.11"
    inline String parseVersion (const String& versionOutput)
    {
        auto t = versionOutput.trim();
        if (! t.startsWith ("Python "))
            return {};
        auto parts = StringArray::fromTokens (t.fromFirstOccurrenceOf ("Python ", false, false), ".", "");
        return parts.size() >= 2 && parts[0].containsOnly ("0123456789") && parts[1].containsOnly ("0123456789")
                   ? parts[0] + "." + parts[1] : String();
    }

    inline bool isCompatible (const String& majorMinor) { return compatibleVersions.contains (majorMinor); }

    // The venv's Python ("3.11") from its pyvenv.cfg; empty when there is no venv.
    inline String venvVersion (const File& venv)
    {
        StringArray lines;
        venv.getChildFile ("pyvenv.cfg").readLines (lines);
        for (auto& l : lines)
        {
            auto key = l.upToFirstOccurrenceOf ("=", false, false).trim();
            if (key == "version" || key == "version_info")
                return parseVersion ("Python " + l.fromFirstOccurrenceOf ("=", false, false).trim());
        }
        return {};
    }

    // pip's normalised name, as in "<name>-<version>.dist-info".
    inline String distName (const String& pkg) { return pkg.toLowerCase().replaceCharacters ("-.", "__"); }

    inline bool hasPackage (const File& venv, const String& pkg)
    {
        for (auto& lib : venv.getChildFile ("lib").findChildFiles (File::findDirectories, false, "python3*"))
            for (auto& d : lib.getChildFile ("site-packages").findChildFiles (File::findDirectories, false, "*.dist-info"))
                if (distName (d.getFileName().upToFirstOccurrenceOf ("-", false, false)) == distName (pkg))
                    return true;
        return false;
    }

    inline StringArray missingPackages (const File& venv, const StringArray& pkgs)
    {
        StringArray missing;
        for (auto& p : pkgs)
            if (! hasPackage (venv, p))
                missing.add (p);
        return missing;
    }

    struct Status
    {
        bool venv = false, ffmpeg = false;
        StringArray missing;
        bool ready() const { return venv && missing.isEmpty() && ffmpeg; }
    };

    inline Status check (const Paths& p, const File& ffmpeg)
    {
        Status s;
        s.venv = p.venvPython().existsAsFile() && isCompatible (venvVersion (p.venv()));
        s.missing = s.venv ? missingPackages (p.venv(), requiredPackages) : requiredPackages;
        s.ffmpeg = ffmpeg.existsAsFile();
        return s;
    }

    inline bool ready() { return check (defaultPaths(), PythonWorker::ffmpegPath()).ready(); }

    inline File findInPath (const String& name, const String& pathEnv)
    {
        for (auto& dir : StringArray::fromTokens (pathEnv, ":", ""))
            if (dir.isNotEmpty())
                if (auto f = File (dir).getChildFile (name); f.existsAsFile())
                    return f;
        return {};
    }

    // How the installer touches the system; tests swap it.
    struct Env
    {
        std::function<PythonWorker::Ran (const StringArray& args, std::function<void (const String&)> onLine,
                                         std::function<bool()> stop, int timeoutMs)> run;
        std::function<bool (const String& url, const File& dest, std::function<void (int)> progress, std::function<bool()> stop)> download;
        String path; // $PATH
        File ffmpeg;
    };

    inline Env systemEnv()
    {
        Env e;
        e.run = [] (const StringArray& args, auto onLine, auto stop, int timeout) { return PythonWorker::drain (args, onLine, stop, timeout, true); };
        e.download = [] (const String& url, const File& dest, std::function<void (int)> progress, std::function<bool()> stop)
        {
            int status = 0;
            auto in = URL (url).createInputStream (URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                                                       .withConnectionTimeoutMs (30000).withNumRedirectsToFollow (5).withStatusCode (&status));
            if (in == nullptr || status != 200)
                return false;
            dest.deleteFile();
            FileOutputStream out (dest);
            if (! out.openedOk())
                return false;
            const auto total = in->getTotalLength();
            HeapBlock<char> buf (1 << 16);
            int64 done = 0;
            while (! in->isExhausted())
            {
                if (stop())
                    return false;
                const auto n = in->read (buf, 1 << 16);
                if (n <= 0)
                    break;
                out.write (buf, (size_t) n);
                done += n;
                if (total > 0 && progress)
                    progress ((int) (done * 100 / total));
            }
            out.flush();
            return total <= 0 || done == total;
        };
        e.path = SystemStats::getEnvironmentVariable ("PATH", "/usr/local/bin:/usr/bin:/bin");
        e.ffmpeg = PythonWorker::ffmpegPath();
        return e;
    }

    enum class Step { python, venv, packages, ffmpeg };
    inline const StringArray stepNames { String (u8"Python 3.9–3.11"), "Entorno de Python (venv)", "Demucs, PyTorch, librosa, yt-dlp", "ffmpeg" };

    struct Event
    {
        enum class State { working, done, failed };
        Step step;
        State state;
        int percent = -1; // -1: no measurable progress (pip)
        String message;
    };

    // Runs the steps that are missing, in order. Blocking: call it from a background thread.
    class Installer
    {
    public:
        Installer (Paths p, Env e) : paths (std::move (p)), env (std::move (e)) {}

        std::function<void (const Event&)> onEvent;

        // Empty when everything is ready; else what failed and how to fix it by hand.
        String run (std::function<bool()> stop)
        {
            for (auto step : { Step::python, Step::venv, Step::packages, Step::ffmpeg })
            {
                if (stop())
                    return String (u8"Instalación cancelada");
                emit (step, Event::State::working, -1, String (u8"Revisando…"));
                auto error = runStep (step, stop);
                if (error.isNotEmpty())
                {
                    emit (step, Event::State::failed, -1, error);
                    return error;
                }
            }
            return {};
        }

    private:
        Paths paths;
        Env env;
        File python; // the interpreter the venv is created from

        void emit (Step s, Event::State st, int pct, const String& msg) { if (onEvent) onEvent ({ s, st, pct, msg }); }
        void done (Step s, const String& msg) { emit (s, Event::State::done, 100, msg); }

        bool venvOk() const { return paths.venvPython().existsAsFile() && isCompatible (venvVersion (paths.venv())); }

        String versionOf (const File& exe, const std::function<bool()>& stop)
        {
            String out;
            env.run ({ exe.getFullPathName(), "--version" }, [&] (const String& l) { if (out.isEmpty()) out = l; }, stop, 15000);
            return parseVersion (out);
        }

        String runStep (Step step, const std::function<bool()>& stop)
        {
            switch (step)
            {
                case Step::python:   return findPython (stop);
                case Step::venv:     return makeVenv (stop);
                case Step::packages: return installPackages (stop);
                case Step::ffmpeg:
                    if (env.ffmpeg.existsAsFile())
                        return done (step, "ffmpeg listo"), String();
                    return String (u8"No se encontró ffmpeg. Instálalo con:\n  sudo apt install ffmpeg");
            }
            return {};
        }

        String findPython (const std::function<bool()>& stop)
        {
            if (venvOk())
                return done (Step::python, "Python " + venvVersion (paths.venv()) + " (ya en la venv)"), String();

            for (auto& v : compatibleVersions)
                if (auto f = findInPath ("python" + v, env.path); f != File() && versionOf (f, stop) == v)
                {
                    python = f;
                    return done (Step::python, "Python " + v + " del sistema"), String();
                }

            if (paths.standalonePython().existsAsFile() && isCompatible (versionOf (paths.standalonePython(), stop)))
            {
                python = paths.standalonePython();
                return done (Step::python, String (u8"Python 3.11 portátil")), String();
            }

            // None on the system (Ubuntu 24.04 ships 3.12): a standalone 3.11, like Electron.
            paths.data.createDirectory();
            auto tarball = paths.data.getChildFile ("python3.11.tar.gz");
            emit (Step::python, Event::State::working, 0, String (u8"Descargando Python 3.11…"));
            const bool got = env.download (standalonePythonUrl, tarball,
                                           [this] (int pct) { emit (Step::python, Event::State::working, pct, String (u8"Descargando Python 3.11…  ") + String (pct) + " %"); },
                                           stop);
            if (! got)
            {
                tarball.deleteFile();
                return String (u8"No se pudo descargar Python 3.11 (¿hay internet?). También puedes instalarlo:\n  sudo apt install python3.11 python3.11-venv");
            }

            emit (Step::python, Event::State::working, -1, String (u8"Descomprimiendo Python 3.11…"));
            auto extract = paths.data.getChildFile (".python-extract");
            extract.deleteRecursively();
            extract.createDirectory();
            auto r = env.run ({ "/usr/bin/tar", "-xzf", tarball.getFullPathName(), "-C", extract.getFullPathName() }, [] (const String&) {}, stop, 5 * 60 * 1000);
            tarball.deleteFile();
            auto root = extract.getChildFile ("python"); // the install_only tarball's top folder
            if (r.code == 0 && root.isDirectory())
            {
                paths.standaloneDir().deleteRecursively();
                root.moveFileTo (paths.standaloneDir());
            }
            extract.deleteRecursively();

            if (! paths.standalonePython().existsAsFile())
                return String (u8"No se pudo descomprimir Python 3.11.");
            python = paths.standalonePython();
            return done (Step::python, String (u8"Python 3.11 portátil instalado")), String();
        }

        String makeVenv (const std::function<bool()>& stop)
        {
            if (venvOk())
                return done (Step::venv, "Venv lista"), String();

            emit (Step::venv, Event::State::working, -1, "Creando la venv en " + paths.venv().getFullPathName() + String (u8"…"));
            paths.venv().deleteRecursively(); // an incompatible or broken one
            auto r = env.run ({ python.getFullPathName(), "-m", "venv", paths.venv().getFullPathName() }, [] (const String&) {}, stop, 2 * 60 * 1000);
            if (r.code != 0 || ! venvOk())
                return "No se pudo crear la venv.\nPrueba a mano:\n  " + python.getFullPathName() + " -m venv " + paths.venv().getFullPathName();
            return done (Step::venv, "Venv creada"), String();
        }

        // pip: only what is missing (an install lacking one package shouldn't re-download torch).
        String pip (const StringArray& pkgs, const std::function<bool()>& stop, StringArray& tail)
        {
            StringArray args { paths.venvPython().getFullPathName(), "-m", "pip", "install", "--upgrade" };
            args.addArray (pkgs);
            auto r = env.run (args, [&] (const String& line)
            {
                if (line.trim().isEmpty())
                    return;
                tail.add (line);
                if (tail.size() > 10)
                    tail.remove (0);
                emit (Step::packages, Event::State::working, -1, line.trim().substring (0, 140));
            }, stop, 45 * 60 * 1000);
            return r.code == 0 ? String() : (r.timedOut ? String (u8"tardó demasiado") : r.stopped ? String ("cancelada") : String (u8"pip falló"));
        }

        String installPackages (const std::function<bool()>& stop)
        {
            auto missing = missingPackages (paths.venv(), requiredPackages);
            if (! missing.isEmpty())
            {
                emit (Step::packages, Event::State::working, -1, "Instalando " + missing.joinIntoString (", ") + String (u8"… (PyTorch pesa ~2 GB, puede tardar)"));
                StringArray tail;
                auto why = pip (missing, stop, tail);
                missing = missingPackages (paths.venv(), requiredPackages);
                if (why.isNotEmpty() || ! missing.isEmpty())
                    return "No se pudieron instalar los paquetes (" + (why.isNotEmpty() ? why : "faltan " + missing.joinIntoString (", ")) + ").\n"
                           + tail.joinIntoString ("\n") + "\n\nPrueba a mano:\n  " + paths.venvPython().getFullPathName() + " -m pip install "
                           + requiredPackages.joinIntoString (" ") + " " + optionalPackage;
            }

            if (! hasPackage (paths.venv(), optionalPackage))
            {
                emit (Step::packages, Event::State::working, -1, String (u8"Instalando Beat This! (opcional)…"));
                StringArray tail;
                if (pip ({ optionalPackage }, stop, tail).isNotEmpty() || ! hasPackage (paths.venv(), optionalPackage))
                    return done (Step::packages, String (u8"Paquetes listos (Beat This! no se instaló: se usa el detector de librosa)")), String();
            }
            return done (Step::packages, "Paquetes listos"), String();
        }
    };

    // The "Instalación" window: the four steps with their state, a progress bar and the last line
    // of output; "Instalar" runs what's missing. Closing it cancels (pip gets SIGTERM).
    class Panel : public Component
    {
    public:
        Panel (Paths p, Env e, std::function<void()> finished = {})
            : paths (std::move (p)), env (std::move (e)), onFinished (std::move (finished))
        {
            intro.setText (String (u8"Para separar canciones Velody necesita Python con Demucs y PyTorch. Se instala una sola vez en ")
                               + paths.data.getFullPathName() + String (u8" (la misma carpeta que usaba la app anterior)."),
                           dontSendNotification);
            intro.setJustificationType (Justification::topLeft);
            addAndMakeVisible (intro);
            for (int i = 0; i < 4; ++i)
            {
                auto* l = steps.add (new Label());
                addAndMakeVisible (l);
            }
            addAndMakeVisible (bar);
            addAndMakeVisible (log);
            log.setMultiLine (true);
            log.setReadOnly (true);
            log.setScrollbarsShown (true);
            addAndMakeVisible (installButton);
            installButton.onClick = [this] { install(); };
            refresh();
            setSize (560, 360);
        }

        ~Panel() override
        {
            worker = nullptr; // stops the install: its process gets SIGTERM
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            intro.setBounds (r.removeFromTop (44));
            for (auto* l : steps)
                l->setBounds (r.removeFromTop (24));
            r.removeFromTop (6);
            bar.setBounds (r.removeFromTop (20));
            r.removeFromTop (6);
            auto bottom = r.removeFromBottom (30);
            installButton.setBounds (bottom.removeFromRight (140));
            log.setBounds (r.reduced (0, 4));
        }

        // Shows what's there now (no processes, instant).
        void refresh()
        {
            auto s = check (paths, env.ffmpeg);
            const bool ok[] { s.venv, s.venv, s.missing.isEmpty(), s.ffmpeg };
            for (int i = 0; i < 4; ++i)
                setStep (i, ok[i] ? Event::State::done : Event::State::working, ok[i] ? String() : String ("falta"));
            installButton.setButtonText (s.ready() ? "Listo" : "Instalar");
            installButton.setEnabled (! s.ready());
            progress = s.ready() ? 1.0 : 0.0;
        }

        bool running() const { return worker != nullptr; }
        String result; // after a run: empty = ready

        void install()
        {
            if (worker != nullptr)
                return;
            installButton.setEnabled (false);
            installButton.setButtonText (String (u8"Instalando…"));
            log.clear();
            worker = std::make_unique<Worker> (*this);
            worker->startThread();
        }

        OwnedArray<Label> steps;
        TextEditor log;
        TextButton installButton { "Instalar" };

    private:
        Paths paths;
        Env env;
        std::function<void()> onFinished;
        Label intro;
        double progress = 0;
        ProgressBar bar { progress };

        void setStep (int i, Event::State st, const String& msg)
        {
            const String mark = st == Event::State::done ? String (u8"✓ ") : st == Event::State::failed ? String (u8"✕ ") : String (u8"… ");
            steps[i]->setText (mark + stepNames[i] + (msg.isNotEmpty() ? String (u8"  — ") + msg : String()), dontSendNotification);
            steps[i]->setColour (Label::textColourId, st == Event::State::done ? Colours::lightgreen : st == Event::State::failed ? Colours::salmon : Colours::white);
        }

        void event (const Event& e)
        {
            const int i = (int) e.step;
            setStep (i, e.state, e.state == Event::State::failed ? String ("ver abajo") : e.message.upToFirstOccurrenceOf ("\n", false, false).substring (0, 60));
            progress = e.percent >= 0 ? e.percent / 100.0 : -1.0; // -1: JUCE shows it as busy
            if (e.state == Event::State::failed || (e.state == Event::State::working && e.message.isNotEmpty()))
                log.moveCaretToEnd(), log.insertTextAtCaret (e.message + "\n");
        }

        void finished (const String& error)
        {
            worker = nullptr;
            result = error;
            if (error.isEmpty())
            {
                refresh();
                log.moveCaretToEnd();
                log.insertTextAtCaret (String (u8"\nListo: ya puedes importar canciones.\n"));
            }
            else
            {
                installButton.setEnabled (true);
                installButton.setButtonText ("Reintentar");
                progress = 0;
            }
            if (onFinished)
                onFinished();
        }

        struct Worker : Thread
        {
            explicit Worker (Panel& p) : Thread (String (u8"Instalación")), panel (&p), installer (p.paths, p.env) {}
            ~Worker() override { stopThread (20000); }

            void run() override
            {
                installer.onEvent = [this] (const Event& e) { post ([e] (Panel& p) { p.event (e); }); };
                auto error = installer.run ([this] { return threadShouldExit(); });
                if (! threadShouldExit())
                    post ([error] (Panel& p) { p.finished (error); });
            }

            void post (std::function<void (Panel&)> fn)
            {
                MessageManager::callAsync ([sp = panel, fn] { if (sp != nullptr) fn (*sp); });
            }

            SafePointer<Panel> panel;
            Installer installer;
        };

        std::unique_ptr<Worker> worker;
    };
}
