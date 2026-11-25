#include <JuceHeader.h>
#include "../Source/PluginPaths.h"

// REAPER's Linux plugins (Flathub LinuxAudio extensions) found by Velody: the newest branch that loads,
// merged into the scan paths without losing the user's own, and LV2_PATH/LADSPA_PATH for loading.
struct PluginPathsTests : UnitTest
{
    PluginPathsTests() : UnitTest ("Plugin paths (REAPER's)") {}

    static void lib (const File& f) { f.getParentDirectory().createDirectory(); f.replaceWithText ("ELF"); }

    void runTest() override
    {
        using namespace PluginPaths;
        auto root = File::createTempFile ("velody-flatpak");
        auto ext = [&] (const String& name, const String& branch) { return root.getChildFile ("org.freedesktop.LinuxAudio.Plugins." + name + "/x86_64/" + branch + "/active/files"); };

        // Surge: 26.08 needs a newer glibc, 25.08 loads. Ratatouille: both load. TAP: LADSPA + LV2.
        lib (ext ("Surge-XT", "25.08").getChildFile ("vst3/Surge XT.vst3/Contents/x86_64-linux/Surge XT.so"));
        lib (ext ("Surge-XT", "26.08").getChildFile ("vst3/Surge XT.vst3/Contents/x86_64-linux/Surge XT.so"));
        lib (ext ("Ratatouille", "25.08").getChildFile ("lv2/Ratatouille.lv2/Ratatouille.so"));
        lib (ext ("Ratatouille", "26.08").getChildFile ("lv2/Ratatouille.lv2/Ratatouille_ui.so"));
        lib (ext ("Ratatouille", "26.08").getChildFile ("lv2/Ratatouille.lv2/Ratatouille.so"));
        lib (ext ("TAP", "25.08").getChildFile ("ladspa/tap_eq.so"));
        lib (ext ("TAP", "25.08").getChildFile ("lv2/tap-eq.lv2/tap_eq.so"));
        root.getChildFile ("org.gnome.Platform/x86_64/48").createDirectory(); // not a plugin extension

        StringArray tried;
        auto loads = [&] (const File& f) { tried.add (f.getFullPathName()); return ! f.getFullPathName().contains ("Surge-XT/x86_64/26.08"); };

        beginTest ("the newest branch that loads on this system, per extension and format");
        auto found = flatpakPlugins ({ root, root.getChildFile ("missing") }, loads);
        expectEquals (found.vst3.joinIntoString ("|"), ext ("Surge-XT", "25.08").getChildFile ("vst3").getFullPathName());
        expectEquals (found.lv2.size(), 2);
        expect (found.lv2.contains (ext ("Ratatouille", "26.08").getChildFile ("lv2").getFullPathName()));
        expect (found.lv2.contains (ext ("TAP", "25.08").getChildFile ("lv2").getFullPathName()));
        expectEquals (found.ladspa.joinIntoString ("|"), ext ("TAP", "25.08").getChildFile ("ladspa").getFullPathName());
        for (auto& t : tried)
            expect (! t.endsWith ("_ui.so"), t); // the GUI library isn't the one tested

        beginTest ("scan paths: the user's own folders stay, ours are added once");
        PropertiesFile::Options o;
        o.applicationName = "velody-pluginpaths-test";
        o.filenameSuffix = "settings";
        o.folderName = root.getChildFile ("cfg").getFullPathName();
        PropertiesFile props (root.getChildFile ("cfg/plugins.settings"), o);
        props.setValue ("lastPluginScanPath_VST3", "/home/me/my-vst3");
        addToScanPaths (props, found);
        addToScanPaths (props, found);
        auto vst3 = FileSearchPath (props.getValue ("lastPluginScanPath_VST3"));
        expectEquals (vst3.getNumPaths(), 2);
        expect (vst3[0] == File ("/home/me/my-vst3"));
        expect (FileSearchPath (props.getValue ("lastPluginScanPath_LV2")).toString().contains ("/usr/lib/lv2")); // defaults kept
        expect (props.getValue ("lastPluginScanPath_LADSPA").contains ("TAP"));

        beginTest ("LV2_PATH / LADSPA_PATH: what was there, plus ours");
        ::setenv ("LV2_PATH", "/opt/mine/lv2", 1);
        ::unsetenv ("LADSPA_PATH");
        setEnvironment (found);
        auto lv2 = String (::getenv ("LV2_PATH"));
        expect (lv2.startsWith ("/opt/mine/lv2:") && lv2.contains ("Ratatouille") && ! lv2.contains (";"), lv2);
        auto ladspa = String (::getenv ("LADSPA_PATH"));
        expect (ladspa.contains ("/usr/lib/ladspa") && ladspa.contains ("TAP"), ladspa);
        ::unsetenv ("LV2_PATH");
        ::unsetenv ("LADSPA_PATH");

        beginTest ("the load check is remembered per library (path + date) across starts");
        {
            auto cacheFile = root.getChildFile ("cache/plugin-paths.cache");
            auto someLib = ext ("TAP", "25.08").getChildFile ("ladspa/tap_eq.so"); // not a real library: doesn't load
            expect (! cachedLoads (cacheFile) (someLib));
            expect (cacheFile.loadFileAsString().contains ("tap_eq.so"));
            cacheFile.replaceWithText (cacheFile.loadFileAsString().replace ("\t0", "\t1")); // pretend it loaded last time
            expect (cachedLoads (cacheFile) (someLib));                                     // a new start reads the cache
            someLib.setLastModificationTime (Time::getCurrentTime() + RelativeTime::seconds (5));
            expect (! cachedLoads (cacheFile) (someLib));                                   // changed: checked again
        }

        beginTest ("this machine (informative): what REAPER's extensions give Velody");
        const auto t0 = Time::getMillisecondCounterHiRes();
        auto real = flatpakPlugins (flatpakRuntimeRoots());
        logMessage ("  VST3: " + real.vst3.joinIntoString (", ") + "\n  LV2: " + real.lv2.joinIntoString (", ")
                    + "\n  LADSPA: " + real.ladspa.joinIntoString (", ") + "\n  " + String (Time::getMillisecondCounterHiRes() - t0, 0) + " ms");

        root.deleteRecursively();
    }
};

static PluginPathsTests pluginPathsTests;
