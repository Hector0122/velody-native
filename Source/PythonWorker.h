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

    // shortcut: points at the source checkout (set by CMake); a packaged build needs to ship python-worker next to the binary.
    inline File scriptsDir()
    {
       #ifdef VELODY_PYTHON_WORKER_DIR
        return File (VELODY_PYTHON_WORKER_DIR);
       #else
        return File::getSpecialLocation (File::currentExecutableFile).getSiblingFile ("python-worker");
       #endif
    }

    struct Message
    {
        String type, message;
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

        bool start (const StringArray& args)
        {
            int fds[2];
            if (::pipe (fds) != 0)
                return false;

            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init (&fa);
            posix_spawn_file_actions_adddup2 (&fa, fds[1], STDOUT_FILENO);
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

    struct Outcome
    {
        bool ok = false, cancelled = false;
        String error;
        StringArray stems;
    };

    // Runs one worker script to the end (blocking): progress goes to onProgress, `shouldStop` cancels
    // with SIGTERM, and it's killed after `timeoutMs` (Electron: 10 min for Demucs).
    inline Outcome run (const StringArray& args, std::function<void (int)> onProgress,
                        std::function<bool()> shouldStop, int timeoutMs = 10 * 60 * 1000)
    {
        Outcome o;
        Process p;
        if (! p.start (args))
        {
            o.error = "No se pudo iniciar " + args[0];
            return o;
        }

        const auto deadline = Time::getMillisecondCounter() + (uint32) timeoutMs;
        bool stopping = false, timedOut = false;
        String line;

        for (;;)
        {
            if (! stopping && (shouldStop() || Time::getMillisecondCounter() > deadline))
            {
                timedOut = ! shouldStop();
                stopping = true;
                p.terminate();
            }

            auto r = p.readLine (line, 200);
            if (r == Process::Read::eof)
                break;
            if (r != Process::Read::line)
                continue;

            if (auto m = parseLine (line))
            {
                if (m->type == "progress" && m->percent >= 0 && onProgress)
                    onProgress (m->percent);
                else if (m->type == "complete")
                    o.stems = m->stems;
                else if (m->type == "error" && o.error.isEmpty())
                    o.error = m->message;
            }
        }

        const int code = p.waitForExit();

        if (stopping)
        {
            o.cancelled = ! timedOut;
            o.error = timedOut ? "La separación tardó demasiado (límite de 10 min)" : "Cancelado";
        }
        else if (o.error.isEmpty() && (code != 0 || o.stems.isEmpty()))
        {
            o.error = "El proceso de Python terminó con código " + String (code);
        }

        o.ok = o.error.isEmpty();
        return o;
    }

    // The last non-empty stdout line (detect_bpm.py prints one JSON result at the end), or "" on
    // failure/cancel/timeout.
    inline String lastLine (const StringArray& args, std::function<bool()> shouldStop, int timeoutMs = 10 * 60 * 1000)
    {
        Process p;
        if (! p.start (args))
            return {};

        const auto deadline = Time::getMillisecondCounter() + (uint32) timeoutMs;
        bool stopped = false;
        String line, last;
        for (;;)
        {
            if (! stopped && (shouldStop() || Time::getMillisecondCounter() > deadline))
            {
                stopped = true;
                p.terminate();
            }
            auto r = p.readLine (line, 200);
            if (r == Process::Read::eof)
                break;
            if (r == Process::Read::line && line.trim().isNotEmpty())
                last = line;
        }
        return p.waitForExit() == 0 && ! stopped ? last : String();
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

    // separate.py exactly as JobManager.ts calls it; stems land in <outputDir>/<file name>/.
    inline StringArray separateArgs (const File& audio, const File& outputDir)
    {
        return { pythonPath().getFullPathName(), scriptsDir().getChildFile ("separate.py").getFullPathName(),
                 audio.getFullPathName(), "--job-id", Uuid().toDashedString(), "--output-dir", outputDir.getFullPathName() };
    }
}
