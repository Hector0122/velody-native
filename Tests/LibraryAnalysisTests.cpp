#include <JuceHeader.h>
#include "../Source/LibraryAnalysis.h"

// Background analysis with a fake detector (no Python): which songs get analysed, in what order,
// waiting while an import runs, failures not saved, and the result landing in .velody.
struct LibraryAnalysisTests : UnitTest
{
    LibraryAnalysisTests() : UnitTest ("Library analysis") {}

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

    static void song (const File& root, const String& name, const String& meta = {})
    {
        root.getChildFile (name).createDirectory();
        root.getChildFile (name + "/drums.mp3").replaceWithText ("d");
        if (meta.isNotEmpty())
            root.getChildFile (name + "/.velody").replaceWithText (meta);
    }

    static String current() { return String (R"({"bpm":120,"firstBeatOffset":0.1,"tracker":"beat_this","chords":[],"_version":)") + String (SongMeta::analysisCacheVersion) + "}"; }

    void runTest() override
    {
        using LibraryAnalysis::needsAnalysis;

        beginTest ("needsAnalysis: like Electron's (no bpm, old version, and fallbacks once Beat This! is there)");
        auto j = [] (const String& s) { return JSON::parse (s); };
        expect (needsAnalysis (var(), false));
        expect (needsAnalysis (j (R"({"bpm":0,"_version":22})"), false));
        expect (needsAnalysis (j (R"({"bpm":120,"_version":21})"), false));
        expect (! needsAnalysis (j (current()), true));
        auto fallback = j (current().replace ("beat_this", "fallback"));
        expect (! needsAnalysis (fallback, false) && needsAnalysis (fallback, true));
        auto noChords = j (current().replace (R"("chords":[],)", ""));
        expect (! needsAnalysis (noChords, false) && needsAnalysis (noChords, true));

        auto tmp = File::createTempFile ("velody-analysis");
        auto root = tmp.getChildFile ("lib");
        root.createDirectory();
        song (root, "A sin analisis");
        song (root, "B vieja", R"({"bpm":100,"_version":3,"practiceSpeed":0.8,"updatedAt":5})");
        song (root, "C lista", current());
        song (root, "D falla");
        song (root, "E abierta");
        root.getChildFile ("F sin stems").createDirectory();

        StringArray detected;
        CriticalSection lock;
        std::atomic<bool> importing { true }, ready { true };
        auto detect = [&] (const File& stem, std::function<bool()>) -> std::optional<NamedValueSet>
        {
            const auto name = stem.getParentDirectory().getFileName();
            {
                const ScopedLock sl (lock);
                detected.add (name);
            }
            if (name.startsWith ("D"))
                return std::nullopt;
            NamedValueSet p;
            p.set ("bpm", 97);
            p.set ("firstBeatOffset", 0.2);
            p.set ("tracker", "beat_this");
            p.set ("chords", Array<var>());
            p.set ("_version", SongMeta::analysisCacheVersion);
            return p;
        };

        beginTest ("waits while an import is separating; the opened song goes first; the rest in order; failures aren't saved");
        {
            LibraryAnalysis::Analyzer a (root, detect, [&] { return ready.load(); }, [&] { return importing.load(); }, [] { return false; });
            StringArray analysed, failed;
            a.onAnalysed = [&] (const String& n) { analysed.add (n); };
            a.onFailed = [&] (const String& n) { failed.add (n); };
            a.request();
            Thread::sleep (300);
            expect (detected.isEmpty()); // Demucs is busy
            a.prioritize ("E abierta");
            importing = false;
            expect (waitFor ([&] { return analysed.size() == 3 && failed.size() == 1; }));
            expectEquals (detected.joinIntoString ("|"), String ("E abierta|A sin analisis|B vieja|D falla"));
            expectEquals (failed.joinIntoString ("|"), String ("D falla"));
            auto b = SongMeta::read (root.getChildFile ("B vieja"));
            expect ((int) b["bpm"] == 97 && (int) b["_version"] == SongMeta::analysisCacheVersion);
            expect (std::abs ((double) b["practiceSpeed"] - 0.8) < 1e-9);  // the rest of .velody stays
            expect ((int64) b["updatedAt"] > 5);                             // stamped: it syncs with the cloud
            expect (! root.getChildFile ("D falla/.velody").exists());
            expect (! a.needs (root.getChildFile ("A sin analisis")));

            beginTest ("asking again doesn't loop over the one that failed (until next start)");
            detected.clear();
            a.request();
            Thread::sleep (300);
            expect (detected.isEmpty(), detected.joinIntoString ("|"));
        }

        beginTest ("without the setup (no Demucs/librosa) nothing runs");
        {
            song (root, "G nueva");
            detected.clear();
            ready = false;
            LibraryAnalysis::Analyzer a (root, detect, [&] { return ready.load(); }, [] { return false; }, [] { return false; });
            a.request();
            Thread::sleep (300);
            expect (detected.isEmpty());
        }

        beginTest ("once Beat This! is installed, fallback grids are redone; closing mid-detection doesn't hang");
        {
            ready = true;
            song (root, "H fallback", current().replace ("beat_this", "fallback"));
            detected.clear();
            std::atomic<bool> stopped { false };
            auto slow = [&] (const File& stem, std::function<bool()> stop) -> std::optional<NamedValueSet>
            {
                {
                    const ScopedLock sl (lock);
                    detected.add (stem.getParentDirectory().getFileName());
                }
                while (! stop())
                    Thread::sleep (5);
                stopped = true;
                return std::nullopt;
            };
            auto a = std::make_unique<LibraryAnalysis::Analyzer> (root, slow, [] { return true; }, [] { return false; }, [] { return true; });
            a->prioritize ("H fallback");
            expect (waitFor ([&] { const ScopedLock sl (lock); return detected.contains ("H fallback"); }));
            const auto t0 = Time::getMillisecondCounter();
            a = nullptr;
            expect (stopped && Time::getMillisecondCounter() - t0 < 5000);
        }

        tmp.deleteRecursively();
    }
};

static LibraryAnalysisTests libraryAnalysisTests;
