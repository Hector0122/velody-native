#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "Library.h"
#include "PythonWorker.h"
#include "SongMeta.h"
#include "Setup.h"

// Import queue, like Electron's JobManager: songs (files or YouTube links) are imported one at a time
// on a background thread — (download →) separate.py → detect_bpm.py. Queued jobs can be cancelled,
// the running one too (its Python process gets SIGTERM); a failure doesn't stop the queue. Finished
// jobs stay listed (with their error) until cleared.
namespace Import
{
    inline const StringArray audioExtensions { ".wav", ".mp3", ".flac", ".m4a", ".ogg" };

    inline bool isImportableFile (const File& f)
    {
        return f.existsAsFile() && audioExtensions.contains (f.getFileExtension().toLowerCase());
    }

    struct Job
    {
        enum class Status { queued, running, done, failed, cancelled };

        int id = 0;
        File file;     // local audio (or the downloaded one, once a link job has it)
        String url;    // YouTube link; empty for a file
        String name;   // song name (the link until its title is known)
        Status status = Status::queued;
        int progress = 0;
        String stage;  // "Descargando", "Separando", "Analizando"
        String error;

        // A job from elsewhere (Vibes): `key` identifies it, `fetch` brings the audio into a
        // velody_dl_* temp folder (outcome.path), `afterImport` runs once it's in the library.
        String key;
        std::function<PythonWorker::Outcome (std::function<void (int)>, std::function<bool()>)> fetch;
        std::function<void()> afterImport;

        bool finished() const { return status == Status::done || status == Status::failed || status == Status::cancelled; }
    };

    using Stop = std::function<bool()>;

    // The work of one job, swappable for tests.
    struct Steps
    {
        std::function<PythonWorker::Outcome (const String& url, std::function<void (const String& title, int pct)>, std::function<void()> onUpdating, Stop)> download;
        std::function<PythonWorker::Outcome (const File& audio, std::function<void (int)>, Stop)> separate;
        std::function<void (const String& songName, Stop)> analyse;
    };

    // The real workers, writing into the library at `root`.
    inline PythonWorker::Outcome notInstalled()
    {
        PythonWorker::Outcome o;
        o.error = String (u8"Falta la instalación de Demucs: Importar → Instalación (Demucs)…");
        return o;
    }

    // `bitrate` is read when each song is separated, so a change applies to the next one.
    inline Steps pythonSteps (File root, std::function<int()> bitrate = {})
    {
        Steps s;
        s.download = [] (const String& url, auto onProgress, auto onUpdating, Stop stop)
        {
            if (! Setup::ready())
                return notInstalled();
            return PythonWorker::download (PythonWorker::downloadArgs (url), PythonWorker::updateYtDlpArgs(), onProgress, onUpdating, stop);
        };
        s.separate = [root, bitrate] (const File& audio, auto onProgress, Stop stop)
        {
            if (! Setup::ready())
                return notInstalled();
            return PythonWorker::run (PythonWorker::separateArgs (audio, root, bitrate ? bitrate() : 0), onProgress, stop);
        };
        // Like Electron after a separation: BPM/beat grid/downbeat/chords into .velody, so the new
        // song has a metronome and count-in. A failed detection isn't saved (it can be retried).
        s.analyse = [root] (const String& name, Stop stop)
        {
            for (auto& song : Library::scan (root))
                if (song.name == name)
                    if (auto stem = SongMeta::pickBeatTrackingStem (song.stems); stem.existsAsFile())
                        if (auto patch = SongMeta::analysisPatch (PythonWorker::lastLine (PythonWorker::detectBpmArgs (stem), stop)))
                            SongMeta::update (song.dir, *patch);
        };
        return s;
    }

    class Queue : private Thread
    {
    public:
        explicit Queue (Steps s) : Thread ("Importar"), steps (std::move (s)) {}

        ~Queue() override
        {
            *alive = false;
            signalThreadShouldExit();
            wake.signal();
            stopThread (15000); // stop() is true now: the running worker gets SIGTERM
        }

        // Called on the message thread whenever a job changes, and once per imported song.
        std::function<void()> onChanged;
        std::function<void (const String& songName)> onImported;

        int addFile (const File& f) { return add ({ 0, f, {}, f.getFileNameWithoutExtension() }); }
        int addUrl (const String& url) { return add ({ 0, {}, url.trim(), url.trim() }); }

        int addRemote (const String& key, const String& name, decltype (Job::fetch) fetch, std::function<void()> afterImport)
        {
            Job j { 0, {}, {}, name };
            j.key = key;
            j.fetch = std::move (fetch);
            j.afterImport = std::move (afterImport);
            return add (j);
        }

        // Any job (finished too) with this key: so a song from Vibes isn't queued twice, nor
        // retried every few minutes after it failed (until the finished jobs are cleared).
        bool has (const String& key) const
        {
            const ScopedLock sl (lock);
            return std::any_of (jobs.begin(), jobs.end(), [&] (auto& j) { return j.key == key; });
        }

        // A queued job is dropped right away; the running one is stopped and marked cancelled.
        bool cancel (int id)
        {
            const ScopedLock sl (lock);
            for (auto& j : jobs)
                if (j.id == id)
                {
                    if (j.status == Job::Status::queued)
                        j.status = Job::Status::cancelled;
                    else if (j.status == Job::Status::running)
                        cancelId = id;
                    else
                        return false;
                    changed();
                    return true;
                }
            return false;
        }

        void clearFinished()
        {
            const ScopedLock sl (lock);
            jobs.erase (std::remove_if (jobs.begin(), jobs.end(), [] (auto& j) { return j.finished(); }), jobs.end());
            changed();
        }

        std::vector<Job> snapshot() const
        {
            const ScopedLock sl (lock);
            return jobs;
        }

        int pending() const
        {
            const ScopedLock sl (lock);
            return (int) std::count_if (jobs.begin(), jobs.end(), [] (auto& j) { return ! j.finished(); });
        }

        // "Separando X… 40 %  (2 de 5)" for the status bar; empty when idle.
        String statusText() const
        {
            const ScopedLock sl (lock);
            const int total = (int) std::count_if (jobs.begin(), jobs.end(), [] (auto& j) { return j.status != Job::Status::cancelled; });
            int index = 0;
            for (auto& j : jobs)
            {
                if (j.status == Job::Status::cancelled)
                    continue;
                ++index;
                if (j.status == Job::Status::running)
                    return j.stage + " " + j.name + String (u8"…") + (j.progress > 0 ? "  " + String (j.progress) + " %" : String())
                           + (total > 1 ? "  (" + String (index) + " de " + String (total) + ")" : String());
            }
            return {};
        }

    private:
        Steps steps;
        CriticalSection lock;
        std::vector<Job> jobs;
        int nextId = 1;
        std::atomic<int> cancelId { 0 };
        WaitableEvent wake;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

        int add (Job j)
        {
            {
                const ScopedLock sl (lock);
                j.id = nextId++;
                jobs.push_back (j);
                changed();
            }
            if (! isThreadRunning())
                startThread();
            wake.signal();
            return j.id;
        }

        void post (std::function<void()> fn)
        {
            MessageManager::callAsync ([a = alive, fn] { if (*a) fn(); });
        }

        void changed() { post ([this] { if (onChanged) onChanged(); }); }

        // Runs fn on the job with this id, under the lock.
        void update (int id, std::function<void (Job&)> fn)
        {
            const ScopedLock sl (lock);
            for (auto& j : jobs)
                if (j.id == id)
                    fn (j);
            changed();
        }

        std::optional<Job> next()
        {
            const ScopedLock sl (lock);
            for (auto& j : jobs)
                if (j.status == Job::Status::queued)
                {
                    j.status = Job::Status::running;
                    changed();
                    return j;
                }
            return {};
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                auto job = next();
                if (! job)
                {
                    wake.wait (500);
                    continue;
                }
                runJob (*job);
            }
        }

        void runJob (Job job)
        {
            const int id = job.id;
            Stop stop = [this, id] { return threadShouldExit() || cancelId == id; };
            auto stage = [this, id] (const String& s) { update (id, [s] (Job& j) { j.stage = s; j.progress = 0; }); };
            auto percent = [this, id] (int p)
            {
                const ScopedLock sl (lock);
                for (auto& j : jobs)
                    if (j.id == id && p != j.progress)
                    {
                        j.progress = p;
                        changed();
                    }
            };

            File audio = job.file, downloadDir;
            PythonWorker::Outcome outcome;
            outcome.ok = true;

            if (job.fetch)
            {
                stage ("Descargando");
                outcome = job.fetch (percent, stop);
                if (outcome.ok)
                {
                    audio = File (outcome.path);
                    downloadDir = audio.getParentDirectory();
                }
            }
            else if (job.url.isNotEmpty())
            {
                stage ("Descargando");
                outcome = steps.download (job.url,
                                          [&] (const String& title, int p)
                                          {
                                              if (title.isNotEmpty())
                                                  update (id, [title] (Job& j) { j.name = title; });
                                              percent (p);
                                          },
                                          [&] { stage (String (u8"YouTube bloqueó la descarga; actualizando yt-dlp:")); }, stop);
                if (outcome.ok)
                {
                    audio = File (outcome.path);
                    downloadDir = audio.getParentDirectory(); // download.py's velody_dl_* temp dir
                    update (id, [audio] (Job& j) { j.file = audio; j.name = audio.getFileNameWithoutExtension(); });
                }
            }

            String name = audio.getFileNameWithoutExtension();
            bool separated = false;
            if (outcome.ok && ! stop())
            {
                stage ("Separando");
                outcome = steps.separate (audio, percent, stop);
                separated = outcome.ok;
                // The song folder is the stems' folder (separate.py names it after the file).
                if (outcome.ok && ! outcome.stems.isEmpty())
                    name = File (outcome.stems[0]).getParentDirectory().getFileName();
            }

            // The song is in the library now: tell its source right away (Vibes takes it off its list).
            // Not after the analysis, which can be cut short by closing Velody (the background analysis
            // finishes it later); then the song would come back on every start.
            if (separated && job.afterImport)
                job.afterImport();

            if (outcome.ok && ! stop())
            {
                stage ("Analizando");
                steps.analyse (name, stop);
            }

            if (downloadDir.getFileName().startsWith ("velody_dl_"))
                downloadDir.deleteRecursively();

            const bool cancelled = stop() || outcome.cancelled;
            if (cancelId == id)
                cancelId = 0;
            update (id, [&] (Job& j)
            {
                j.status = cancelled ? Job::Status::cancelled : outcome.ok ? Job::Status::done : Job::Status::failed;
                j.error = cancelled || outcome.ok ? String() : outcome.error;
                j.name = outcome.ok ? name : j.name;
                j.progress = outcome.ok ? 100 : j.progress;
            });
            if (outcome.ok && ! cancelled)
                post ([this, name] { if (onImported) onImported (name); });
        }
    };

    inline String statusLabel (const Job& j)
    {
        switch (j.status)
        {
            case Job::Status::queued:    return j.name + "  (en espera)";
            case Job::Status::running:   return j.name + "  (" + j.stage.toLowerCase() + (j.progress > 0 ? " " + String (j.progress) + " %" : String()) + ")";
            case Job::Status::done:      return String (u8"✓ ") + j.name;
            case Job::Status::failed:    return String (u8"✕ ") + j.name + ": " + j.error.upToFirstOccurrenceOf ("\n", false, false);
            case Job::Status::cancelled: return j.name + "  (cancelada)";
        }
        return j.name;
    }

    struct MenuAction
    {
        enum Kind { addFiles, addUrl, fetchVibes, vibesSettings, setup, setBitrate, cancel, showError, clearFinished } kind;
        Job job {};
        int bitrate = 0;
    };

    // The "Importar" menu: add files / a link, then the queue (click a waiting or running job to
    // cancel it, a failed one to read the whole error).
    inline PopupMenu menu (const std::vector<Job>& jobs, std::function<void (const MenuAction&)> act, int bitrate = 128)
    {
        PopupMenu m;
        m.addItem (String (u8"Archivos…"), [act] { act ({ MenuAction::addFiles }); });
        m.addItem (String (u8"Desde YouTube…"), [act] { act ({ MenuAction::addUrl }); });
        m.addItem ("Traer lo que mandaste desde Vibes", [act] { act ({ MenuAction::fetchVibes }); });
        m.addItem (String (u8"Vibes…"), [act] { act ({ MenuAction::vibesSettings }); });
        m.addItem (String (u8"Instalación (Demucs)…"), [act] { act ({ MenuAction::setup }); });

        // Like Electron's import screen: size per song vs quality.
        PopupMenu quality;
        const std::pair<int, String> options[] { { 128, String (u8"128 kbps (~23 MB por canción, suficiente para practicar)") },
                                                 { 192, String (u8"192 kbps (~35 MB, equilibrado)") },
                                                 { 320, String (u8"320 kbps (~58 MB, máxima calidad)") } };
        for (auto& [kbps, text] : options)
            quality.addItem (text, true, bitrate == kbps, [act, k = kbps] { MenuAction a { MenuAction::setBitrate }; a.bitrate = k; act (a); });
        m.addSubMenu ("Calidad de los stems: " + String (bitrate) + " kbps", quality);
        if (jobs.empty())
            return m;

        m.addSeparator();
        m.addSectionHeader ("Cola (clic en una para cancelarla)");
        bool anyFinished = false;
        for (auto& j : jobs)
        {
            anyFinished |= j.finished();
            if (j.status == Job::Status::queued || j.status == Job::Status::running)
                m.addItem (statusLabel (j), [act, j] { act ({ MenuAction::cancel, j }); });
            else if (j.status == Job::Status::failed)
                m.addItem (statusLabel (j), [act, j] { act ({ MenuAction::showError, j }); });
            else
                m.addItem (statusLabel (j), false, false, nullptr);
        }
        if (anyFinished)
        {
            m.addSeparator();
            m.addItem ("Limpiar las terminadas", [act] { act ({ MenuAction::clearFinished }); });
        }
        return m;
    }
}
