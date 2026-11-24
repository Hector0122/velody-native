#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include "CloudSync.h"

// The cloud sync UI pieces, kept out of Main.cpp so tests can drive them: a controller that runs one
// cloud operation at a time off the message thread, the "Canciones" menu with sync states, and the
// settings panel (like velody's CloudSettingsModal + Home.tsx).
namespace CloudUi
{
    inline String formatBytes (int64 bytes)
    {
        if (bytes < 1024)
            return String (bytes) + " B";
        const char* units[] = { "KB", "MB", "GB", "TB" };
        double v = (double) bytes / 1024.0;
        int i = 0;
        while (v >= 1024.0 && i < 3)
        {
            v /= 1024.0;
            ++i;
        }
        return String (v, v >= 10.0 ? 0 : 1) + " " + units[i];
    }

    inline String songLabel (const Cloud::Entry& e)
    {
        switch (e.state)
        {
            case Cloud::State::synced: return String (u8"✓ ") + e.name;
            case Cloud::State::cloud:  return String (u8"☁ ") + e.name + (e.sizeBytes > 0 ? "  (" + formatBytes (e.sizeBytes) + ")" : String());
            case Cloud::State::local:  break;
        }
        return e.name;
    }

    // What the settings form saves: a blank token keeps the saved one (it's never shown back).
    struct FormResult { std::optional<Cloud::Settings> settings; String error; };

    inline FormResult applyForm (const Cloud::Settings& current, const String& url, const String& token, bool enabled)
    {
        Cloud::Settings s;
        s.enabled = enabled;
        s.token = token.trim().isNotEmpty() ? token.trim() : current.token;

        if (url.trim().isNotEmpty())
        {
            auto u = Cloud::normaliseUrl (url);
            if (! u)
                return { {}, String (u8"URL no válida: tiene que empezar con https://") };
            s.url = *u;
        }

        if (enabled && s.url.isEmpty())
            return { {}, String (u8"Para activar la nube falta la URL del Worker") };
        if (enabled && s.token.isEmpty())
            return { {}, String (u8"Para activar la nube falta el token") };
        return { s, {} };
    }

    // Runs a blocking job on its own thread and calls `done` (with the error message, empty when it
    // worked) on the message thread. Nothing is called back once the runner is gone; a job still in
    // flight then just finishes on its own (an interrupted download is cleaned up at the next start).
    class Runner
    {
    public:
        using Job = std::function<void (const Cloud::Progress&)>;

        ~Runner() { *alive = false; }

        void run (Job job, std::function<void (const String&)> done, std::function<void (int)> progress = {})
        {
            std::thread ([a = alive, job = std::move (job), done = std::move (done), progress = std::move (progress)]
            {
                auto post = [a] (std::function<void()> fn) { MessageManager::callAsync ([a, fn] { if (*a) fn(); }); };
                String error;
                try
                {
                    job ([&] (int p) { if (progress) post ([progress, p] { progress (p); }); });
                }
                catch (const Cloud::Error& e) { error = e.message(); }
                catch (const std::exception& e) { error = String::fromUTF8 (e.what()); }
                catch (...) { error = "Error inesperado"; }
                post ([done, error] { done (error); });
            }).detach();
        }

    private:
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    };

    using Listing = Cloud::Sync::Listing;

    // The library's cloud actions. Transfers (upload/download/remove/delete) run one at a time and
    // make busy() true; callbacks arrive on the message thread.
    class Controller
    {
    public:
        Controller (File libraryRoot, File settingsFile) : root (std::move (libraryRoot)), settingsPath (std::move (settingsFile)) {}

        std::function<void (const String&)> onStatus; // progress text while a transfer runs

        Cloud::Settings settings() const { return Cloud::Settings::load (settingsPath); }
        File settingsFile() const { return settingsPath; }
        bool active() const { return settings().active(); }
        bool busy() const { return running > 0; }

        // Reads the settings on every call, so a change in the settings panel applies right away.
        Cloud::MakeClient makeClient() const
        {
            return [f = settingsPath] (bool quick) -> std::unique_ptr<Cloud::Client>
            {
                auto s = Cloud::Settings::load (f);
                if (! s.active())
                    return nullptr;
                return quick ? std::make_unique<Cloud::Client> (s.url, s.token, 4000, 0)
                             : std::make_unique<Cloud::Client> (s.url, s.token);
            };
        }

        Cloud::Sync sync() const { return { root, makeClient() }; }

        // No network: cloudIds from .velody only (used while a transfer runs).
        Listing localListing() const { return Cloud::Sync (root, [] (bool) { return std::unique_ptr<Cloud::Client>(); }).list(); }

        void list (std::function<void (const Listing&)> done)
        {
            if (! active())
                return done (localListing());
            auto out = std::make_shared<Listing>();
            runner.run ([s = sync(), out] (const Cloud::Progress&) mutable { *out = s.list(); },
                        [this, out, done] (const String& error)
                        {
                            if (error.isNotEmpty()) // list() itself never throws for the network; just in case
                            {
                                *out = localListing();
                                out->enabled = out->offline = true;
                                out->error = error;
                            }
                            done (*out);
                        });
        }

        void upload (const String& name, std::function<void (const String&)> done)
        {
            transfer ("Subiendo " + name, [s = sync(), name] (const Cloud::Progress& p) mutable { s.upload (name, p); }, std::move (done));
        }

        // Every song that is only on this laptop, one after another; failures don't stop the rest.
        void uploadAll (const StringArray& names, std::function<void (int uploaded, const StringArray& failed)> done)
        {
            auto failed = std::make_shared<StringArray>();
            auto uploaded = std::make_shared<int> (0);
            transfer ("Subiendo " + String (names.size()) + " canciones",
                      [s = sync(), names, failed, uploaded] (const Cloud::Progress& p) mutable
                      {
                          for (int i = 0; i < names.size(); ++i)
                          {
                              try
                              {
                                  s.upload (names[i], [&] (int pct) { p ((i * 100 + pct) / jmax (1, names.size())); });
                                  ++*uploaded;
                              }
                              catch (const Cloud::Error& e) { failed->add (names[i] + ": " + e.message()); }
                          }
                      },
                      [failed, uploaded, done] (const String&) { done (*uploaded, *failed); });
        }

        void download (const Cloud::Entry& e, std::function<void (const File&, const String&)> done)
        {
            auto dir = std::make_shared<File>();
            transfer ("Descargando " + e.name, [s = sync(), e, dir] (const Cloud::Progress& p) mutable { *dir = s.download (e, p); },
                      [dir, done] (const String& error) { done (*dir, error); });
        }

        // Quick and never failing: offline just opens the local copy.
        void syncMetaOnOpen (const Cloud::Entry& e, std::function<void()> done)
        {
            if (e.state != Cloud::State::synced || ! active())
                return done();
            status (String (u8"Revisando la nube…"));
            runner.run ([s = sync(), e] (const Cloud::Progress&) mutable { s.syncMetaOnOpen (e); }, [done] (const String&) { done(); });
        }

        void removeLocal (const Cloud::Entry& e, std::function<bool (const File&)> trash, std::function<void (const String&)> done)
        {
            transfer ("Sincronizando " + e.name + " antes de quitarla",
                      [s = sync(), e, trash] (const Cloud::Progress&) mutable { s.removeLocal (e, trash); }, std::move (done));
        }

        void deleteFromCloud (const Cloud::Entry& e, std::function<void (const String&)> done)
        {
            transfer ("Borrando " + e.name + " de la nube", [s = sync(), e] (const Cloud::Progress&) mutable { s.deleteFromCloud (e); }, std::move (done));
        }

        void cleanupStaleDownloads() { sync().cleanupStaleDownloads(); }

    private:
        File root, settingsPath;
        Runner runner;
        int running = 0;

        void status (const String& s) { if (onStatus) onStatus (s); }

        void transfer (const String& what, Runner::Job job, std::function<void (const String&)> done)
        {
            ++running;
            status (what + String (u8"…"));
            runner.run (std::move (job),
                        [this, done] (const String& error) { --running; done (error); },
                        [this, what] (int pct) { status (what + String (u8"…  ") + String (pct) + " %"); });
        }
    };

    struct Action
    {
        enum Kind { openEmpty, open, download, upload, uploadAll, removeCurrent, deleteFromCloud, settings } kind;
        Cloud::Entry entry {};
    };

    // The "Canciones" menu. Item actions call `act`; show it with showMenuAsync and no callback.
    inline PopupMenu songsMenu (const Listing& l, const String& currentSong, bool busy, bool importing, const File& root,
                                std::function<void (const Action&)> act)
    {
        const bool online = l.enabled && ! l.offline;
        PopupMenu m;
        m.addItem (String (u8"Proyecto vacío"), [act] { act ({ Action::openEmpty }); });
        m.addSeparator();

        if (l.enabled && l.offline)
            m.addItem (String (u8"Nube sin conexión: solo se ven las canciones de esta laptop"), false, false, nullptr);
        if (busy)
            m.addItem (String (u8"Sincronizando con la nube…"), false, false, nullptr);
        if (online && std::any_of (l.songs.begin(), l.songs.end(), [] (auto& e) { return e.state != Cloud::State::local; }))
            m.addSectionHeader (String (u8"✓ aquí y en la nube   ☁ solo en la nube (se descarga al abrirla)"));

        for (auto& e : l.songs)
        {
            if (e.state == Cloud::State::cloud)
                m.addItem (songLabel (e), online && ! busy, false, [act, e] { act ({ Action::download, e }); });
            else
                m.addItem (songLabel (e), [act, e] { act ({ Action::open, e }); });
        }
        if (l.songs.empty())
            m.addItem ("No hay canciones en " + root.getFullPathName(), false, false, nullptr);

        auto current = std::find_if (l.songs.begin(), l.songs.end(), [&] (auto& e) { return e.local && e.name == currentSong; });
        StringArray localOnly;
        PopupMenu cloudOnly;
        for (auto& e : l.songs)
        {
            if (e.state == Cloud::State::local)
                localOnly.add (e.name);
            if (e.state == Cloud::State::cloud)
                cloudOnly.addItem (e.name + String (u8"…"), ! busy, false, [act, e] { act ({ Action::deleteFromCloud, e }); });
        }

        if (currentSong.isNotEmpty() && current != l.songs.end())
        {
            m.addSeparator();
            if (online && current->state == Cloud::State::local)
                m.addItem (String (u8"Subir «") + currentSong + String (u8"» a la nube"), ! busy, false, [act, e = *current] { act ({ Action::upload, e }); });
            m.addItem (String (u8"Quitar «") + currentSong + String (u8"»…"), ! importing && ! busy, false, [act, e = *current] { act ({ Action::removeCurrent, e }); });
        }

        if (online && (! localOnly.isEmpty() || cloudOnly.getNumItems() > 0))
        {
            m.addSeparator();
            if (! localOnly.isEmpty())
                m.addItem (localOnly.size() == 1 ? String (u8"Subir a la nube la canción que solo está aquí")
                                                 : "Subir a la nube las " + String (localOnly.size()) + String (u8" canciones que solo están aquí"),
                           ! busy, false, [act] { act ({ Action::uploadAll }); });
            if (cloudOnly.getNumItems() > 0)
                m.addSubMenu ("Borrar de la nube", cloudOnly, ! busy);
        }

        m.addSeparator();
        m.addItem (String (u8"Nube… ") + (! l.enabled ? "(apagada)" : l.offline ? String (u8"(sin conexión)") : "(activada)"),
                   [act] { act ({ Action::settings }); });
        return m;
    }

    // URL + token + on/off, "Probar conexión" and "Guardar". The token is never shown back.
    class SettingsPanel : public Component
    {
    public:
        SettingsPanel (File settingsFile, std::function<void (const Cloud::Settings&)> saved)
            : file (std::move (settingsFile)), onSaved (std::move (saved))
        {
            for (auto* c : std::initializer_list<Component*> { &intro, &urlLabel, &url, &tokenLabel, &token, &enabled, &testButton, &result, &saveButton })
                addAndMakeVisible (c);

            intro.setText (String (u8"Guarda una copia de tus canciones en tu propio Cloudflare R2: puedes liberar espacio aquí "
                                   u8"y descargarlas cuando quieras. Sin conexión todo sigue funcionando."),
                           dontSendNotification);
            intro.setJustificationType (Justification::topLeft);
            urlLabel.setText ("URL del Worker", dontSendNotification);
            tokenLabel.setText ("Token", dontSendNotification);

            auto s = Cloud::Settings::load (file);
            current = s;
            url.setText (s.url, false);
            url.setTextToShowWhenEmpty ("https://velody-cloud.<tu-usuario>.workers.dev", Colours::grey);
            token.setPasswordCharacter ((juce_wchar) 0x2022);
            token.setTextToShowWhenEmpty (s.token.isNotEmpty() ? String (u8"guardado: escribe para cambiarlo") : String ("pega tu token"), Colours::grey);
            enabled.setButtonText ("Activar la nube");
            enabled.setToggleState (s.enabled, dontSendNotification);

            testButton.onClick = [this] { test(); };
            saveButton.onClick = [this] { save(); };
            setSize (460, 250);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            intro.setBounds (r.removeFromTop (48));
            urlLabel.setBounds (r.removeFromTop (20));
            url.setBounds (r.removeFromTop (26));
            r.removeFromTop (6);
            tokenLabel.setBounds (r.removeFromTop (20));
            token.setBounds (r.removeFromTop (26));
            r.removeFromTop (6);
            enabled.setBounds (r.removeFromTop (26));
            r.removeFromTop (8);
            auto row = r.removeFromTop (28);
            testButton.setBounds (row.removeFromLeft (130));
            saveButton.setBounds (row.removeFromRight (100));
            result.setBounds (row.reduced (8, 0));
        }

        void test()
        {
            auto form = applyForm (current, url.getText(), token.getText(), false);
            if (! form.settings || form.settings->url.isEmpty() || form.settings->token.isEmpty())
                return showResult (form.error.isNotEmpty() ? form.error : String ("Falta la URL o el token"), false);

            testButton.setEnabled (false);
            result.setText (String (u8"Probando…"), dontSendNotification);
            runner.run ([s = *form.settings] (const Cloud::Progress&) { Cloud::Client (s.url, s.token, 8000, 0).health(); },
                        [sp = SafePointer<SettingsPanel> (this)] (const String& error)
                        {
                            if (sp == nullptr) return;
                            sp->testButton.setEnabled (true);
                            sp->showResult (error.isEmpty() ? String ("Conectado") : error, error.isEmpty());
                        });
        }

        void save()
        {
            auto form = applyForm (current, url.getText(), token.getText(), enabled.getToggleState());
            if (! form.settings)
                return showResult (form.error, false);
            if (! form.settings->save (file))
                return showResult ("No se pudo guardar en " + file.getFullPathName(), false);

            current = *form.settings;
            token.clear();
            token.setTextToShowWhenEmpty (current.token.isNotEmpty() ? String (u8"guardado: escribe para cambiarlo") : String ("pega tu token"), Colours::grey);
            url.setText (current.url, false);
            showResult (current.enabled ? "Nube activada" : "Guardado", true);
            if (onSaved)
                onSaved (current);
        }

        TextEditor url, token;
        ToggleButton enabled;
        TextButton testButton { String (u8"Probar conexión") }, saveButton { "Guardar" };
        Label result;

    private:
        File file;
        std::function<void (const Cloud::Settings&)> onSaved;
        Cloud::Settings current;
        Label intro, urlLabel, tokenLabel;
        Runner runner;

        void showResult (const String& text, bool ok)
        {
            result.setText ((ok ? String (u8"✓ ") : String (u8"✕ ")) + text, dontSendNotification);
            result.setTooltip (text);
            result.setColour (Label::textColourId, ok ? Colours::lightgreen : Colours::salmon);
        }
    };
}
