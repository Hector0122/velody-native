#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "Library.h"
#include "PythonWorker.h"
#include "SongMeta.h"

// Background analysis of the library, ported from Electron's analyzeLibrary (handlers.ts): every song
// without a current analysis (BPM, beat grid, downbeat, chords) is analysed one at a time, so it has a
// metronome, count-in and chords. Waits while an import is separating (don't compete with Demucs);
// the song the user opens goes first. Only successful detections are saved.
namespace LibraryAnalysis
{
    // shared/analysis.ts hasCurrentAnalysis + needsAnalysis. `redoFallbacks` (Beat This! installed)
    // also redoes grids made without it and songs whose chords couldn't be detected.
    inline bool needsAnalysis (const var& meta, bool redoFallbacks)
    {
        const auto bpm = meta["bpm"];
        const bool current = (bpm.isInt() || bpm.isInt64() || bpm.isDouble()) && (double) bpm > 0
                             && (int) meta["_version"] == SongMeta::analysisCacheVersion;
        if (! current)
            return true;
        return redoFallbacks && (meta["tracker"].toString() == "fallback" || ! meta["chords"].isArray());
    }

    using Detect = std::function<std::optional<NamedValueSet> (const File& stem, std::function<bool()> stop)>;

    inline Detect pythonDetect()
    {
        return [] (const File& stem, std::function<bool()> stop)
        {
            return SongMeta::analysisPatch (PythonWorker::lastLine (PythonWorker::detectBpmArgs (stem), stop));
        };
    }

    class Analyzer : private Thread
    {
    public:
        // `canRun`: setup is ready (Python + packages); `busy`: an import is separating right now;
        // `redoFallbacks`: Beat This! is installed.
        Analyzer (File libraryRoot, Detect d, std::function<bool()> canRunFn, std::function<bool()> busyFn, std::function<bool()> redoFn)
            : Thread ("Analizar biblioteca"), root (std::move (libraryRoot)), detect (std::move (d)),
              canRun (std::move (canRunFn)), busy (std::move (busyFn)), redo (std::move (redoFn)) {}

        ~Analyzer() override
        {
            *alive = false;
            signalThreadShouldExit();
            wake.signal();
            stopThread (15000); // detect_bpm.py gets SIGTERM through stop()
        }

        // Called on the message thread for each song analysed (to refresh it if it's open).
        std::function<void (const String& songName)> onAnalysed;
        std::function<void (const String& songName)> onStarted;
        std::function<void (const String& songName)> onFailed;

        // (Re)scan the library: at start, after setup, after a download from the cloud.
        void request()
        {
            requested = true;
            if (! isThreadRunning())
                startThread (Thread::Priority::low);
            wake.signal();
        }

        // The user opened this song: it goes first, if it needs analysing at all.
        void prioritize (const String& songName)
        {
            {
                const ScopedLock sl (lock);
                first = songName;
            }
            request();
        }

        bool needs (const File& songDir) const { return needsAnalysis (SongMeta::read (songDir), redo()); }
        String current() const { const ScopedLock sl (lock); return running; }

    private:
        File root;
        Detect detect;
        std::function<bool()> canRun, busy, redo;
        CriticalSection lock;
        String first, running;
        std::atomic<bool> requested { false };
        WaitableEvent wake;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

        void post (std::function<void()> fn) { MessageManager::callAsync ([a = alive, fn] { if (*a) fn(); }); }

        // Next song to analyse: the prioritized one if it still needs it, else the first in the library.
        std::optional<Library::Song> next()
        {
            auto songs = Library::scan (root);
            String want;
            {
                const ScopedLock sl (lock);
                want = first;
                first = {};
            }
            if (want.isNotEmpty())
                for (auto& s : songs)
                    if (s.name == want && needs (s.dir))
                        return s;
            for (auto& s : songs)
                if (needs (s.dir) && SongMeta::pickBeatTrackingStem (s.stems).existsAsFile())
                    if (failed.count (s.name) == 0)
                        return s;
            return {};
        }

        std::set<String> failed; // detection failed this session: not retried in a loop (next start retries)

        void run() override
        {
            while (! threadShouldExit())
            {
                if (! requested.exchange (false))
                {
                    wake.wait (-1);
                    continue;
                }
                while (! threadShouldExit() && canRun())
                {
                    // Don't compete with Demucs for the CPU/GPU/RAM.
                    while (! threadShouldExit() && busy())
                        wake.wait (2000);
                    auto song = next();
                    if (! song || threadShouldExit())
                        break;
                    {
                        const ScopedLock sl (lock);
                        running = song->name;
                    }
                    post ([this, n = song->name] { if (onStarted) onStarted (n); });
                    auto stem = SongMeta::pickBeatTrackingStem (song->stems);
                    auto patch = detect (stem, [this] { return threadShouldExit(); });
                    {
                        const ScopedLock sl (lock);
                        running = {};
                    }
                    if (threadShouldExit())
                        break;
                    // Re-check: an import may have analysed it meanwhile.
                    if (patch && needs (song->dir))
                    {
                        SongMeta::update (song->dir, *patch); // stamped: the new analysis syncs with the cloud
                        post ([this, n = song->name] { if (onAnalysed) onAnalysed (n); });
                        if (needs (song->dir)) // still a fallback grid (Beat This! couldn't run): not again this session
                            failed.insert (song->name);
                    }
                    else if (! patch)
                    {
                        failed.insert (song->name);
                        post ([this, n = song->name] { if (onFailed) onFailed (n); });
                    }
                }
            }
        }
    };
}
