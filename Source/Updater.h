#pragma once
#include <JuceHeader.h>
#include "Setup.h"

// "Hay una versión nueva" (like Vibes' desktop updater): at start, ask GitHub for the latest release;
// installed from the .deb (/usr/bin/velody-native), "Actualizar" downloads the new .deb, installs it with
// pkexec (the system's password window) and restarts Velody on it. The .deb doesn't come from an apt
// repository, so nothing else would ever update it.
namespace Updater
{
    inline const String latestRelease { "https://api.github.com/repos/Hector0122/velody-native/releases/latest" };
    inline const File installedExe { "/usr/bin/velody-native" };

    struct Update { String version, debUrl, pageUrl; int64 size = 0; };

    // "v0.10.0" vs "0.9.2" -> true. Anything that isn't x.y.z counts as not newer.
    inline bool isNewer (const String& tag, const String& current)
    {
        auto parse = [] (String v) -> std::optional<std::array<int, 3>>
        {
            v = v.trim().trimCharactersAtStart ("v");
            auto p = StringArray::fromTokens (v, ".", "");
            if (p.size() != 3)
                return {};
            std::array<int, 3> out {};
            for (int i = 0; i < 3; ++i)
            {
                if (p[i].isEmpty() || ! p[i].containsOnly ("0123456789"))
                    return {};
                out[(size_t) i] = p[i].getIntValue();
            }
            return out;
        };
        auto a = parse (tag), b = parse (current);
        return a && b && *a > *b;
    }

    // The newer release with its amd64 .deb, from GitHub's JSON; empty when up to date or no .deb yet.
    inline std::optional<Update> parseRelease (const var& release, const String& current)
    {
        const auto tag = release["tag_name"].toString();
        if (tag.isEmpty() || ! isNewer (tag, current))
            return {};
        if (auto* assets = release["assets"].getArray())
            for (auto& a : *assets)
            {
                const auto name = a["name"].toString();
                if (name.startsWith ("velody-native_") && name.endsWith ("_amd64.deb") && a["browser_download_url"].isString())
                    return Update { tag.trimCharactersAtStart ("v"), a["browser_download_url"].toString(),
                                    release["html_url"].toString(), (int64) a["size"] };
            }
        return {};
    }

    // Only the installed .deb updates itself (not a build from source).
    inline bool canSelfUpdate (const File& exe) { return exe == installedExe; }

    inline File runningExe() { return PythonWorker::runningExe(); }

    // Ajustes → Buscar actualizaciones: what to tell when there's nothing to install.
    inline String manualCheckMessage (bool installed, bool reached, const String& current)
    {
        if (! installed)
            return String (u8"Esta copia de Velody no es la instalada (.deb): se actualiza compilando de nuevo.");
        if (! reached)
            return String (u8"No se pudo revisar (¿sin internet?). Intenta otra vez en un rato.");
        return String (u8"Ya tienes la versión más nueva (") + current + ").";
    }

    // Blocking (background thread): empty when offline, rate-limited or up to date.
    // `reached`: whether GitHub answered (so «no update» means up to date, not offline).
    inline std::optional<Update> check (const String& current, bool* reached = nullptr)
    {
        if (reached != nullptr)
            *reached = false;
        int status = 0;
        auto in = URL (latestRelease).createInputStream (URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                                                             .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: Velody")
                                                             .withConnectionTimeoutMs (10000).withStatusCode (&status));
        if (in == nullptr || status != 200)
            return {};
        if (reached != nullptr)
            *reached = true;
        return parseRelease (JSON::parse (in->readEntireStreamAsString()), current);
    }

    enum class Stage { download, install };

    // Downloads the .deb into `tmp` and installs it with pkexec apt; empty when it worked, else what to
    // tell the user. The downloaded file is always removed.
    inline String install (const Update& u, const Setup::Env& env, const File& tmp,
                           std::function<void (Stage, int)> progress, std::function<bool()> stop)
    {
        auto deb = tmp.getChildFile ("velody-native_" + u.version + "_amd64.deb");
        struct Cleanup { File f; ~Cleanup() { f.deleteFile(); } } cleanup { deb };

        progress (Stage::download, 0);
        if (! env.download (u.debUrl, deb, [&] (int pct) { progress (Stage::download, pct); }, stop))
            return String (u8"No se pudo descargar la actualización (¿hay internet?)");
        if (u.size > 0 && deb.getSize() != u.size)
            return String (u8"La descarga de la actualización llegó incompleta");

        progress (Stage::install, -1);
        StringArray out;
        auto r = env.run ({ "/usr/bin/pkexec", "apt", "install", "-y", deb.getFullPathName() },
                          [&] (const String& l) { out.add (l); if (out.size() > 4) out.remove (0); }, stop, 10 * 60 * 1000);
        // pkexec: 126 = the password window was closed or the password refused, 127 = not authorized.
        if (r.code == 126 || r.code == 127)
            return String (u8"Se canceló la contraseña: no se actualizó");
        if (r.code != 0)
            return String (u8"No se pudo instalar la actualización:\n") + out.joinIntoString ("\n");
        return {};
    }
}
