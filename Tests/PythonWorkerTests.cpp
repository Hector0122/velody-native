#include <JuceHeader.h>
#include "../Source/PythonWorker.h"
#include "../Source/SongMeta.h"

// The real spawn/read/SIGTERM path, against small scripts that speak separate.py's protocol.
struct PythonWorkerTests : UnitTest
{
    PythonWorkerTests() : UnitTest ("PythonWorker") {}

    File dir;

    StringArray script (const String& name, const String& code, StringArray extra = {})
    {
        auto f = dir.getChildFile (name + ".py");
        f.replaceWithText (code);
        StringArray a { "/usr/bin/python3", f.getFullPathName() };
        a.addArray (extra);
        return a;
    }

    void runTest() override
    {
        using namespace PythonWorker;

        beginTest ("parseLine reads the JSON protocol and ignores Demucs logging");
        expect (! parseLine ("Downloading model...").has_value());
        expect (! parseLine ("").has_value());
        auto p = parseLine (R"({"type":"progress","percent":42.6})");
        expect (p.has_value() && p->type == "progress" && p->percent == 43);
        auto c = parseLine (R"({"type":"complete","stems":["a/drums.mp3","a/bass.mp3"]})");
        expect (c.has_value() && c->stems.size() == 2);
        auto e = parseLine (R"({"type":"error","message":"boom"})");
        expect (e.has_value() && e->message == "boom");

        dir = File::createTempFile ("velody-worker");
        dir.createDirectory();
        auto never = [] { return false; };

        beginTest ("success: progress in order, stems reported, files written, non-JSON ignored");
        Array<int> progress;
        auto out = dir.getChildFile ("out");
        auto ok = run (script ("ok", R"(
import json, sys, os
out = sys.argv[1]
os.makedirs(os.path.join(out, "Song"), exist_ok=True)
print("Demucs says hello", flush=True)
for p in (10, 50, 100):
    print(json.dumps({"type": "progress", "percent": p}), flush=True)
stems = []
for s in ("drums", "vocals"):
    path = os.path.join(out, "Song", s + ".mp3")
    open(path, "w").write("x")
    stems.append(path)
print(json.dumps({"type": "complete", "stems": stems}))
)", { out.getFullPathName() }), [&] (int pct) { progress.add (pct); }, never);
        expect (ok.ok, ok.error);
        expect (progress == Array<int> { 10, 50, 100 });
        expectEquals (ok.stems.size(), 2);
        expect (out.getChildFile ("Song/drums.mp3").existsAsFile());

        beginTest ("error message from the script reaches the user");
        auto err = run (script ("err", R"(
import json, sys
print(json.dumps({"type": "error", "message": "Demucs is not installed"}), flush=True)
sys.exit(1)
)"), {}, never);
        expect (! err.ok && ! err.cancelled);
        expectEquals (err.error, String ("Demucs is not installed"));

        beginTest ("a crash without a message reports the exit code");
        auto crash = run (script ("crash", "import sys\nsys.exit(3)\n"), {}, never);
        expect (! crash.ok);
        expect (crash.error.contains ("3"));

        beginTest ("exit 0 without stems is not a success");
        expect (! run (script ("empty", "print('nothing')\n"), {}, never).ok);

        beginTest ("cancel sends SIGTERM (so separate.py can clean up), not SIGKILL");
        auto marker = dir.getChildFile ("got-sigterm");
        bool started = false;
        auto cancelled = run (script ("slow", R"(
import json, signal, sys, time
def bye(*_):
    open(sys.argv[1], "w").write("1")
    sys.exit(143)
signal.signal(signal.SIGTERM, bye)
print(json.dumps({"type": "progress", "percent": 5}), flush=True)
time.sleep(30)
)", { marker.getFullPathName() }), [&] (int) { started = true; }, [&] { return started; });
        expect (cancelled.cancelled && ! cancelled.ok);
        expect (marker.existsAsFile());

        beginTest ("timeout stops a hung worker");
        const auto t0 = Time::getMillisecondCounter();
        auto hung = run (script ("hung", "import time\ntime.sleep(30)\n"), {}, never, 300);
        expect (! hung.ok && ! hung.cancelled);
        expect (hung.error.contains ("10 min"));
        expectLessThan ((int) (Time::getMillisecondCounter() - t0), 5000);

        beginTest ("a missing interpreter fails cleanly");
        expect (! run ({ "/nonexistent/python" }, {}, never).ok);

        beginTest ("separateArgs calls the bundled separate.py like Electron's JobManager");
        auto args = separateArgs (File ("/music/Song.mp3"), File ("/lib"));
        expect (File (args[1]).existsAsFile(), args[1]);
        expectEquals (args[2], String ("/music/Song.mp3"));
        expect (args.contains ("--output-dir") && args[args.indexOf ("--output-dir") + 1] == "/lib");

        testDownload();
        dir.deleteRecursively();
    }

    void testDownload()
    {
        using namespace PythonWorker;
        auto never = [] { return false; };

        beginTest ("isDownloadableUrl: only http(s) web links");
        expect (isDownloadableUrl ("https://www.youtube.com/watch?v=abc"));
        expect (isDownloadableUrl ("  https://youtu.be/abc  "));
        expect (! isDownloadableUrl ("/home/me/song.mp3"));
        expect (! isDownloadableUrl ("file:///etc/passwd"));
        expect (! isDownloadableUrl ("--exec rm"));
        expect (! isDownloadableUrl ("https://"));
        expect (! isDownloadableUrl (""));

        beginTest ("downloadArgs calls the bundled download.py like Electron");
        auto a = downloadArgs ("https://youtu.be/abc");
        expect (File (a[1]).existsAsFile(), a[1]);
        expectEquals (a[2], String ("https://youtu.be/abc"));

        auto fakeDownload = script ("dl", R"(
import json, os, sys
marker = sys.argv[1]
if len(sys.argv) > 2 and sys.argv[2] == "blocked" and not os.path.exists(marker):
    print(json.dumps({"type": "error", "message": "[UPDATE_YTDLP] YouTube blocked the download."}), flush=True)
    sys.exit(1)
d = os.path.join(os.path.dirname(marker), "velody_dl_x")
os.makedirs(d, exist_ok=True)
path = os.path.join(d, "My Song.mp3")
open(path, "w").write("x")
print(json.dumps({"type": "metadata", "title": "My Song", "duration": 200}), flush=True)
print(json.dumps({"type": "progress", "percent": 50}), flush=True)
print(json.dumps({"type": "complete", "path": path, "title": "My Song"}), flush=True)
)", { dir.getChildFile ("updated").getFullPathName() });
        auto fakeUpdate = script ("update", "import sys\nopen(sys.argv[1], 'w').write('1')\n", { dir.getChildFile ("updated").getFullPathName() });
        auto failingUpdate = script ("update-fails", "import sys\nsys.exit(1)\n");

        beginTest ("download: progress carries the title, result is the audio path");
        StringArray seen;
        int updates = 0;
        auto o = download (fakeDownload, fakeUpdate, [&] (const String& t, int p) { seen.add (t + ":" + String (p)); },
                           [&] { ++updates; }, never);
        expect (o.ok, o.error);
        expectEquals (o.title, String ("My Song"));
        expect (File (o.path).existsAsFile());
        expect (seen.contains ("My Song:50"));
        expectEquals (updates, 0);

        beginTest ("download: YouTube blocks an old yt-dlp -> update once, retry, succeed");
        dir.getChildFile ("updated").deleteFile();
        auto blocked = fakeDownload;
        blocked.add ("blocked");
        updates = 0;
        o = download (blocked, fakeUpdate, [] (const String&, int) {}, [&] { ++updates; }, never);
        expect (o.ok, o.error);
        expectEquals (updates, 1);

        beginTest ("download: if the update fails, the user sees YouTube's message");
        dir.getChildFile ("updated").deleteFile();
        o = download (blocked, failingUpdate, [] (const String&, int) {}, [] {}, never);
        expect (! o.ok);
        expect (o.error.contains ("YouTube blocked"));

        beginTest ("download: other errors are not retried");
        updates = 0;
        auto priv = script ("private", "import json\nprint(json.dumps({'type':'error','message':'This video is private'}))\nraise SystemExit(1)\n");
        o = download (priv, fakeUpdate, [] (const String&, int) {}, [&] { ++updates; }, never);
        expectEquals (o.error, String ("This video is private"));
        expectEquals (updates, 0);

        beginTest ("download: complete without a file path is a failure");
        auto nopath = script ("nopath", "import json\nprint(json.dumps({'type':'complete','title':'x'}))\n");
        expect (! download (nopath, fakeUpdate, [] (const String&, int) {}, [] {}, never).ok);
    }
};

static PythonWorkerTests pythonWorkerTests;

// Real Demucs through the app's code path. Slow, so only with VELODY_E2E_SEPARATE=<audio file>;
// stems go to a temp folder, never the library.
struct RealSeparationTest : UnitTest
{
    RealSeparationTest() : UnitTest ("Real separation (opt-in)") {}

    void runTest() override
    {
        auto input = SystemStats::getEnvironmentVariable ("VELODY_E2E_SEPARATE", {});
        if (input.isEmpty())
            return;

        beginTest ("separate.py + Demucs from the Electron venv produce stems");
        auto out = File::createTempFile ("velody-e2e");
        Array<int> progress;
        auto o = PythonWorker::run (PythonWorker::separateArgs (File (input), out), [&] (int p) { progress.add (p); }, [] { return false; });
        expect (o.ok, o.error);
        expect (! progress.isEmpty());
        auto songDir = out.getChildFile (File (input).getFileNameWithoutExtension());
        logMessage ("stems: " + o.stems.joinIntoString (", ") + "  progress updates: " + String (progress.size()));
        expect (songDir.getChildFile ("drums.mp3").existsAsFile());
        expect (songDir.getChildFile ("vocals.mp3").existsAsFile());

        beginTest ("detect_bpm.py on the separated drums gives a cacheable analysis");
        auto last = PythonWorker::lastLine (PythonWorker::detectBpmArgs (songDir.getChildFile ("drums.mp3")), [] { return false; });
        auto patch = SongMeta::analysisPatch (last);
        expect (patch.has_value(), last.substring (0, 300));
        if (patch)
            logMessage ("bpm " + (*patch)["bpm"].toString() + ", tracker " + (*patch)["tracker"].toString()
                        + ", beats " + String ((*patch)["beatTimes"].size()) + ", chords " + String ((*patch)["chords"].size()));
        out.deleteRecursively();
    }
};

static RealSeparationTest realSeparationTest;
