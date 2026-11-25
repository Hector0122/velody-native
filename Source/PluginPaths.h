#pragma once
#include <JuceHeader.h>
#include <dlfcn.h>
#include <cstdlib>

// The same Linux plugins REAPER uses: REAPER (flatpak) gets them from the Flathub
// org.freedesktop.LinuxAudio.Plugins.* extensions (Surge XT, Ratatouille, sfizz, AVLDrums, TAP, SWH, CMT…).
// Velody isn't a flatpak, so it looks in those extensions' folders directly. Each extension has one build
// per runtime branch (25.08, 26.08…); the newest one that loads on this system is used (a newer branch
// can need a newer glibc than the system's).
namespace PluginPaths
{
    struct Found { StringArray vst3, lv2, ladspa; };

    inline Array<File> flatpakRuntimeRoots()
    {
        return { File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/share/flatpak/runtime"),
                 File ("/var/lib/flatpak/runtime") };
    }

    // dlopen check: a library built for a newer runtime fails here (missing GLIBC_x.y), harmlessly.
    inline bool loadsHere (const File& lib)
    {
        if (auto* h = ::dlopen (lib.getFullPathName().toRawUTF8(), RTLD_LAZY | RTLD_LOCAL))
        {
            ::dlclose (h);
            return true;
        }
        return false;
    }

    // First plugin binary of a branch, to test whether that build loads here.
    inline File firstLibrary (const File& files)
    {
        for (auto sub : { "vst3", "lv2", "ladspa" })
            for (auto& f : files.getChildFile (sub).findChildFiles (File::findFiles, true, "*.so"))
                if (! f.getFileNameWithoutExtension().endsWithIgnoreCase ("_ui") && ! f.getFileName().containsIgnoreCase ("UI_"))
                    return f;
        return {};
    }

    inline Found flatpakPlugins (const Array<File>& roots, std::function<bool (const File&)> loads = loadsHere)
    {
        Found out;
        for (auto& root : roots)
            for (auto& ext : root.findChildFiles (File::findDirectories, false, "org.freedesktop.LinuxAudio.Plugins.*"))
            {
                auto branches = ext.getChildFile ("x86_64").findChildFiles (File::findDirectories, false);
                std::sort (branches.begin(), branches.end(), [] (const File& a, const File& b)
                           { return a.getFileName().compareNatural (b.getFileName()) > 0; }); // newest first
                for (auto& branch : branches)
                {
                    auto files = branch.getChildFile ("active/files");
                    auto lib = firstLibrary (files);
                    if (! files.isDirectory() || (lib.existsAsFile() && ! loads (lib)))
                        continue;
                    if (files.getChildFile ("vst3").isDirectory())   out.vst3.add (files.getChildFile ("vst3").getFullPathName());
                    if (files.getChildFile ("lv2").isDirectory())    out.lv2.add (files.getChildFile ("lv2").getFullPathName());
                    if (files.getChildFile ("ladspa").isDirectory()) out.ladspa.add (files.getChildFile ("ladspa").getFullPathName());
                    break;
                }
            }
        return out;
    }

    // `existing` (";"-separated, like FileSearchPath) plus `extra`, without duplicates.
    inline String merged (const String& existing, const StringArray& extra)
    {
        FileSearchPath p (existing);
        for (auto& d : extra)
            if (! p.toString().contains (d))
                p.add (File (d));
        return p.toString();
    }

    inline const String lv2Defaults { "~/.lv2;/usr/local/lib/lv2;/usr/lib/lv2;/usr/lib/x86_64-linux-gnu/lv2" };
    inline const String ladspaDefaults { "~/.ladspa;/usr/local/lib/ladspa;/usr/lib/ladspa" };

    // LV2/LADSPA plugins are found through LV2_PATH/LADSPA_PATH, also when a saved edit loads them, so
    // these must be set before the engine starts.
    inline void setEnvironment (const Found& f)
    {
        auto env = [] (const char* name, const String& defaults, const StringArray& extra)
        {
            auto current = SystemStats::getEnvironmentVariable (name, {}).replace (":", ";");
            auto all = merged (current.isNotEmpty() ? current : defaults, extra).replace (";", ":");
            ::setenv (name, all.toRawUTF8(), 1);
        };
        if (! f.lv2.isEmpty())    env ("LV2_PATH", lv2Defaults, f.lv2);
        if (! f.ladspa.isEmpty()) env ("LADSPA_PATH", ladspaDefaults, f.ladspa);
    }

    // The Plugins window scans the folders it remembers per format (JUCE's lastPluginScanPath_<format>).
    inline void addToScanPaths (PropertiesFile& props, const Found& f)
    {
        auto add = [&] (const String& format, const String& defaults, const StringArray& extra)
        {
            if (extra.isEmpty())
                return;
            const auto key = "lastPluginScanPath_" + format;
            props.setValue (key, merged (props.getValue (key, defaults), extra));
        };
        add ("VST3", "~/.vst3;/usr/lib/vst3;/usr/local/lib/vst3", f.vst3);
        add ("LV2", lv2Defaults, f.lv2);
        add ("LADSPA", ladspaDefaults, f.ladspa);
        props.saveIfNeeded();
    }

    // loadsHere, remembered per library (path + modification time) in `cacheFile`: loading big plugins
    // to check them costs ~1 s, so it's only done again when a plugin extension changes.
    inline std::function<bool (const File&)> cachedLoads (const File& cacheFile)
    {
        auto cache = std::make_shared<StringPairArray>();
        StringArray lines;
        cacheFile.readLines (lines);
        for (auto& l : lines)
            if (l.contains ("\t"))
                cache->set (l.upToLastOccurrenceOf ("\t", false, false), l.fromLastOccurrenceOf ("\t", false, false));

        return [cache, cacheFile] (const File& lib)
        {
            const auto key = lib.getFullPathName() + "|" + String (lib.getLastModificationTime().toMilliseconds());
            if (cache->containsKey (key))
                return (*cache)[key] == "1";
            const bool ok = loadsHere (lib);
            cache->set (key, ok ? "1" : "0");
            StringArray out;
            for (auto& k : cache->getAllKeys())
                out.add (k + "\t" + (*cache)[k]);
            cacheFile.getParentDirectory().createDirectory();
            cacheFile.replaceWithText (out.joinIntoString ("\n"));
            return ok;
        };
    }

    // Found once at start, before the engine exists.
    inline Found& atStart()
    {
        static Found f = flatpakPlugins (flatpakRuntimeRoots(),
            cachedLoads (File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Velody/plugin-paths.cache")));
        return f;
    }
}
