#include <JuceHeader.h>
#include "../Source/Updater.h"

// Velody's own updater with a fake system: versions, finding the .deb in GitHub's answer, and the
// download → pkexec apt install flow with its failures (nothing is left behind).
struct UpdaterTests : UnitTest
{
    UpdaterTests() : UnitTest ("Updater") {}

    void runTest() override
    {
        using namespace Updater;

        beginTest ("versions: only a real x.y.z newer than this one");
        expect (isNewer ("v0.10.0", "0.9.2") && isNewer ("0.3.0", "0.2.9"));
        expect (! isNewer ("v0.2.0", "0.2.0") && ! isNewer ("v0.1.9", "0.2.0"));
        expect (! isNewer ("nightly", "0.1.0") && ! isNewer ("v1.2", "0.1.0"));

        beginTest ("GitHub's latest release: the amd64 .deb when it's newer");
        auto release = JSON::parse (R"({"tag_name":"v0.3.0","html_url":"https://github.com/x/releases/tag/v0.3.0",
            "assets":[{"name":"notes.txt","browser_download_url":"https://dl/notes.txt"},
                      {"name":"velody-native_0.3.0_amd64.deb","size":6,"browser_download_url":"https://dl/velody-native_0.3.0_amd64.deb"}]})");
        auto u = parseRelease (release, "0.2.0");
        expect (u && u->version == "0.3.0" && u->debUrl == "https://dl/velody-native_0.3.0_amd64.deb" && u->size == 6);
        expect (! parseRelease (release, "0.3.0"));
        expect (! parseRelease (JSON::parse (R"({"tag_name":"v0.3.0","assets":[]})"), "0.2.0")); // no .deb yet
        expect (! parseRelease (var(), "0.2.0"));

        beginTest ("only the installed .deb updates itself");
        expect (canSelfUpdate (File ("/usr/bin/velody-native")));
        expect (! canSelfUpdate (File ("/home/x/velody-native/build/Velody_artefacts/Release/Velody")));

        auto tmp = File::createTempFile ("velody-upd");
        tmp.createDirectory();
        StringArray commands;
        int aptCode = 0;
        String body = "abcdef";
        Setup::Env env;
        env.download = [&] (const String&, const File& dest, std::function<void (int)> progress, auto)
        {
            progress (100);
            return dest.replaceWithText (body);
        };
        env.run = [&] (const StringArray& args, std::function<void (const String&)> onLine, auto, int)
        {
            commands.add (args.joinIntoString (" "));
            expect (File (args[4]).existsAsFile()); // the .deb is there while apt reads it
            onLine ("E: Sub-process /usr/bin/dpkg returned an error code (1)");
            PythonWorker::Ran r;
            r.started = true;
            r.code = aptCode;
            return r;
        };
        Update upd { "0.3.0", "https://dl/x.deb", {}, 6 };
        Array<int> stages;
        auto progress = [&] (Stage s, int) { stages.addIfNotAlreadyThere ((int) s); };
        auto never = [] { return false; };

        beginTest ("install: download, then pkexec apt install; nothing left behind");
        expect (install (upd, env, tmp, progress, never).isEmpty());
        expectEquals (commands[0], "/usr/bin/pkexec apt install -y " + tmp.getChildFile ("velody-native_0.3.0_amd64.deb").getFullPathName());
        expect (stages == Array<int> { (int) Stage::download, (int) Stage::install });
        expect (tmp.findChildFiles (File::findFiles, false).isEmpty());

        beginTest ("closed password window, apt failing, a short download: each says so");
        aptCode = 126;
        expect (install (upd, env, tmp, progress, never).contains (String (u8"canceló la contraseña")));
        aptCode = 100;
        expect (install (upd, env, tmp, progress, never).contains ("dpkg returned an error"));
        aptCode = 0;
        body = "abc";
        commands.clear();
        expect (install (upd, env, tmp, progress, never).contains ("incompleta"));
        expect (commands.isEmpty());
        expect (tmp.findChildFiles (File::findFiles, false).isEmpty());

        tmp.deleteRecursively();
    }
};

static UpdaterTests updaterTests;
