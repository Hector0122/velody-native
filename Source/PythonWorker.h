#pragma once
#include <JuceHeader.h>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// Runs the python-worker scripts the same way the Electron app does: spawn, read newline-delimited
// JSON from stdout ({"type":"progress"|"complete"|"skipped"|"error", ...}), ignore anything else.
namespace PythonWorker
{
    // Same venv the Electron setup creates, so Demucs etc. are already installed.
    inline File pythonPath()
    {
        auto venv = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/share/velody/venv/bin/python3");
        return venv.existsAsFile() ? venv : File ("/usr/bin/python3");
    }

    // Installed (.deb): <prefix>/bin/velody-native -> <prefix>/share/velody-native/python-worker.
    // Built from source: the checkout's python-worker (path baked in by CMake).
    inline File scriptsDirFor (const File& executable)
    {
        auto installed = executable.getParentDirectory().getSiblingFile ("share/velody-native/python-worker");
        if (installed.getChildFile ("separate.py").existsAsFile())
            return installed;
       #ifdef VELODY_PYTHON_WORKER_DIR
        return File (VELODY_PYTHON_WORKER_DIR);
       #else
        return installed;
       #endif
    }

    inline File scriptsDir() { return scriptsDirFor (File::getSpecialLocation (File::currentExecutableFile)); }

    struct Message
    {
        String type, message, path, title;
        int percent = -1;
        StringArray stems;
    };

    // One stdout line -> message, or nothing for non-JSON lines (Demucs' own logging).
    inline std::optional<Message> parseLine (const String& line)
    {
        auto json = JSON::parse (line.trim());
        if (! json.isObject() || ! json["type"].isString())
            return {};

        Message m;
        m.type = json["type"].toString();
        m.message = json["message"].toString();
        m.path = json["path"].toString();
        m.title = json["title"].toString();
        if (json.hasProperty ("percent"))
            m.percent = jlimit (0, 100, roundToInt ((double) json["percent"]));
        if (auto* a = json["stems"].getArray())
            for (auto& s : *a)
                m.stems.add (s.toString());
        return m;
    }

    // A child process with its stdout piped to us; stderr goes to our terminal (like Electron's console.error).
    // Unlike juce::ChildProcess it can send SIGTERM, which separate.py handles by stopping Demucs and
    // deleting its temp dir (SIGKILL would orphan Demucs).
    class Process
    {
    public:
        ~Process()
        {
            if (pid > 0)
            {
                terminate();
                waitForExit();
            }
            if (fd >= 0)
                ::close (fd);
        }

        // `mergeStderr`: stderr comes through the same pipe (pip writes its progress and errors there).
        bool start (const StringArray& args, bool mergeStderr = false)
        {
            int fds[2];
            if (::pipe (fds) != 0)
                return false;

            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init (&fa);
            posix_spawn_file_actions_adddup2 (&fa, fds[1], STDOUT_FILENO);
            if (mergeStderr)
                posix_spawn_file_actions_adddup2 (&fa, fds[1], STDERR_FILENO);
            posix_spawn_file_actions_addclose (&fa, fds[0]);
            posix_spawn_file_actions_addclose (&fa, fds[1]);

            std::vector<std::string> storage;
            for (auto& a : args)
                storage.push_back (a.toStdString());
            std::vector<char*> argv;
            for (auto& s : storage)
                argv.push_back (s.data());
            argv.push_back (nullptr);

            pid_t child = 0;
            const int err = posix_spawn (&child, argv[0], &fa, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy (&fa);
            ::close (fds[1]);

            if (err != 0)
            {
                ::close (fds[0]);
                return false;
            }

            pid = child;
            fd = fds[0];
            return true;
        }

        enum class Read { line, timeout, eof };

        // Waits up to `timeoutMs` for a full line.
        Read readLine (String& out, int timeoutMs)
        {
            for (;;)
            {
                auto nl = pending.find ('\n');
                if (nl != std::string::npos)
                {
                    out = String::fromUTF8 (pending.data(), (int) nl);
                    pending.erase (0, nl + 1);
                    return Read::line;
                }

                pollfd p { fd, POLLIN, 0 };
                const int r = ::poll (&p, 1, timeoutMs);
                if (r == 0)
                    return Read::timeout;
                if (r < 0)
                    return errno == EINTR ? Read::timeout : Read::eof;

                char buf[4096];
                const auto n = ::read (fd, buf, sizeof (buf));
                if (n <= 0)
                {
                    if (pending.empty())
                        return Read::eof;
                    out = String::fromUTF8 (pending.data(), (int) pending.size()); // last line without '\n'
                    pending.clear();
                    return Read::line;
                }
                pending.append (buf, (size_t) n);
            }
        }

        void terminate() { if (pid > 0) ::kill (pid, SIGTERM); }

        // Exit code, or -1 if it didn't exit normally.
        int waitForExit()
        {
            if (pid <= 0)
                return -1;
            int status = 0;
            while (::waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
            pid = 0;
            return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
        }

    private:
        pid_t pid = 0;
        int fd = -1;
        std::string pending;
    };

    // Runs a process to the end (blocking), handing each stdout line to onLine. `shouldStop` sends
    // SIGTERM; so does the timeout (Electron: 10 min for Demucs and downloads).
    struct Ran { bool started = false, stopped = false, timedOut = false; int code = -1; };

    inline Ran drain (const StringArray& args, std::function<void (const String&)> onLine,
                      std::function<bool()> shouldStop, int timeoutMs, bool mergeStderr = false)
    {
        Ran r;
        Process p;
        if (! (r.started = p.start (args, mergeStderr)))
            return r;

        const auto deadline = Time::getMillisecondCounter() + (uint32) timeoutMs;
        String line;
        for (;;)
        {
            if (! r.stopped && (shouldStop() || Time::getMillisecondCounter() > deadline))
            {
                r.timedOut = ! shouldStop();
                r.stopped = true;
                p.terminate();
            }

            auto read = p.readLine (line, 200);
            if (read == Process::Read::eof)
                break;
            if (read == Process::Read::line)
                onLine (line);
        }
        r.code = p.waitForExit();
        return r;
    }

    struct Outcome
    {
        bool ok = false, cancelled = false;
        String error;
        StringArray stems;  // separate.py
        String path, title; // download.py
    };

    // A JSON-lines worker (separate.py, download.py): progress to onProgress, success = exit 0 plus a
    // "complete" message; the first "error" message is the one shown.
    inline Outcome run (const StringArray& args, std::function<void (int)> onProgress,
                        std::function<bool()> shouldStop, int timeoutMs = 10 * 60 * 1000,
                        std::function<void (const String&)> onTitle = {})
    {
        Outcome o;
        bool complete = false;
        auto ran = drain (args, [&] (const String& line)
        {
            auto m = parseLine (line);
            if (! m)
                return;
            if (m->type == "progress" && m->percent >= 0 && onProgress)
                onProgress (m->percent);
            else if (m->type == "metadata" && m->title.isNotEmpty())
            {
                o.title = m->title;
                if (onTitle)
                    onTitle (o.title);
            }
            else if (m->type == "complete")
            {
                complete = true;
                o.stems = m->stems;
                o.path = m->path;
                if (m->title.isNotEmpty())
                    o.title = m->title;
            }
            else if (m->type == "error" && o.error.isEmpty())
                o.error = m->message;
        }, shouldStop, timeoutMs);

        if (! ran.started)
            o.error = "No se pudo iniciar " + args[0];
        else if (ran.stopped)
        {
            o.cancelled = ! ran.timedOut;
            o.error = ran.timedOut ? String (u8"Tardó demasiado (límite de 10 min)") : String ("Cancelado");
        }
        else if (o.error.isEmpty() && (ran.code != 0 || ! complete))
            o.error = String (u8"El proceso de Python terminó con código ") + String (ran.code);

        o.ok = o.error.isEmpty();
        return o;
    }

    // The last non-empty stdout line (detect_bpm.py prints one JSON result at the end), or "" on
    // failure/cancel/timeout.
    inline String lastLine (const StringArray& args, std::function<bool()> shouldStop, int timeoutMs = 10 * 60 * 1000)
    {
        String last;
        auto ran = drain (args, [&] (const String& l) { if (l.trim().isNotEmpty()) last = l; }, shouldStop, timeoutMs);
        return ran.started && ! ran.stopped && ran.code == 0 ? last : String();
    }

    // Only web links go to yt-dlp (it would also take local paths and other schemes).
    inline bool isDownloadableUrl (const String& s)
    {
        const auto t = s.trim();
        return (t.startsWithIgnoreCase ("https://") || t.startsWithIgnoreCase ("http://"))
               && URL (t).getDomain().isNotEmpty() && ! t.containsAnyOf (" \t\n");
    }

    // download.py with Electron's retry: when YouTube blocks an outdated yt-dlp ("[UPDATE_YTDLP]"),
    // update yt-dlp in the venv and try once more. The audio lands in a velody_dl_* temp dir.
    inline Outcome download (const StringArray& downloadArgs, const StringArray& updateArgs,
                             std::function<void (const String& title, int pct)> onProgress,
                             std::function<void()> onUpdating, std::function<bool()> shouldStop)
    {
        auto attempt = [&]
        {
            String title;
            auto o = run (downloadArgs, [&] (int pct) { onProgress (title, pct); }, shouldStop, 10 * 60 * 1000,
                          [&] (const String& t) { title = t; });
            if (o.ok && ! File::isAbsolutePath (o.path))
            {
                o.ok = false;
                o.error = String (u8"La descarga terminó sin archivo de audio");
            }
            return o;
        };

        auto o = attempt();
        if (o.ok || ! o.error.contains ("[UPDATE_YTDLP]"))
            return o;

        onUpdating();
        auto updated = drain (updateArgs, [] (const String&) {}, shouldStop, 2 * 60 * 1000);
        if (! updated.started || updated.stopped || updated.code != 0)
            return o; // keep YouTube's message
        return attempt();
    }

    inline File ffmpegPath()
    {
        auto f = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/share/velody/ffmpeg/bin/ffmpeg");
        return f.existsAsFile() ? f : File ("/usr/bin/ffmpeg");
    }

    // detect_bpm.py as handlers.ts runDetectBpm calls it.
    inline StringArray detectBpmArgs (const File& stem)
    {
        return { pythonPath().getFullPathName(), scriptsDir().getChildFile ("detect_bpm.py").getFullPathName(),
                 stem.getFullPathName(), "--ffmpeg-path", ffmpegPath().getFullPathName() };
    }

    // download.py as handlers.ts url:import calls it.
    inline StringArray downloadArgs (const String& url)
    {
        return { pythonPath().getFullPathName(), scriptsDir().getChildFile ("download.py").getFullPathName(),
                 url.trim(), "--ffmpeg-path", ffmpegPath().getFullPathName() };
    }

    inline StringArray updateYtDlpArgs() { return { pythonPath().getFullPathName(), "-m", "pip", "install", "-U", "yt-dlp" }; }

    // separate.py exactly as JobManager.ts calls it; stems land in <outputDir>/<file name>/.
    inline StringArray separateArgs (const File& audio, const File& outputDir)
    {
        return { pythonPath().getFullPathName(), scriptsDir().getChildFile ("separate.py").getFullPathName(),
                 audio.getFullPathName(), "--job-id", Uuid().toDashedString(), "--output-dir", outputDir.getFullPathName() };
    }
}
