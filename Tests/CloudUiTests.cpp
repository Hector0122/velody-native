#include <JuceHeader.h>
#include <sys/stat.h>
#include "../Source/CloudUi.h"
#include "../Source/PythonWorker.h"

// The cloud UI: menu and settings form (pure), then menu click / panel button -> controller (real
// threads) -> Tests/fake_cloud.py -> data on disk and in the cloud, as the user would see it.
struct CloudUiTests : UnitTest
{
    CloudUiTests() : UnitTest ("Cloud UI") {}

    File tmp;

    static void write (const File& f, const String& text) { f.getParentDirectory().createDirectory(); f.replaceWithText (text); }

    template <typename Cond>
    static bool waitFor (Cond cond, int timeoutMs = 20000)
    {
        const auto end = Time::getMillisecondCounter() + (uint32) timeoutMs;
        while (! cond())
        {
            if (Time::getMillisecondCounter() > end)
                return false;
            MessageManager::getInstance()->runDispatchLoopUntil (10);
        }
        return true;
    }

    // First item (submenus included) whose text starts with `prefix`.
    static std::optional<PopupMenu::Item> item (const PopupMenu& m, const String& prefix)
    {
        for (PopupMenu::MenuItemIterator it (m, true); it.next();)
            if (it.getItem().text.startsWith (prefix))
                return it.getItem();
        return {};
    }

    // Runs a menu item's action, failing the test (not crashing) when the item isn't there.
    void click (const PopupMenu& m, const String& prefix)
    {
        auto i = item (m, prefix);
        expect (i.has_value() && i->action != nullptr, "no menu item: " + prefix);
        if (i && i->action)
            i->action();
    }

    void runTest() override
    {
        tmp = File::createTempFile ("velody-cloud-ui");
        tmp.createDirectory();
        testPure();
        testMenu();
        testAgainstFakeWorker();
        tmp.deleteRecursively();
    }

    void testPure()
    {
        using namespace CloudUi;

        beginTest ("sizes and song labels");
        expectEquals (formatBytes (500), String ("500 B"));
        expectEquals (formatBytes (1536), String ("1.5 KB"));
        expectEquals (formatBytes (12 * 1024 * 1024), String ("12 MB"));
        expectEquals (songLabel ({ "A", Cloud::State::synced, {}, {}, 0 }), String (u8"✓ A"));
        expectEquals (songLabel ({ "B", Cloud::State::cloud, {}, {}, 2048 }), String (u8"☁ B  (2.0 KB)"));
        expectEquals (songLabel ({ "C", Cloud::State::local, {}, {}, 0 }), String ("C"));

        beginTest ("settings form: blank token keeps the saved one, URL checked, on needs URL + token");
        Cloud::Settings saved { "https://a.workers.dev", "old", true };
        auto r = applyForm (saved, "https://b.workers.dev/", "", true);
        expect (r.settings && r.settings->url == "https://b.workers.dev" && r.settings->token == "old");
        expectEquals (applyForm (saved, "https://b.workers.dev", " new ", true).settings->token, String ("new"));
        expect (! applyForm (saved, "http://b.workers.dev", "", true).settings);
        expect (! applyForm ({}, "https://b.workers.dev", "", true).settings);
        expect (applyForm ({}, "https://b.workers.dev", "", true).error.contains ("token"));
        expect (! applyForm ({}, "", "t", true).settings);
        auto off = applyForm ({}, "", "", false);
        expect (off.settings && ! off.settings->active());
    }

    void testMenu()
    {
        using namespace CloudUi;
        beginTest ("menu: states, what's enabled, and which action each item fires");

        Library::Song local { "Local", tmp.getChildFile ("Local"), {} }, synced { "Synced", tmp.getChildFile ("Synced"), {} };
        Listing l;
        l.enabled = true;
        l.songs = { { "Local", Cloud::State::local, {}, local, 0 },
                    { "Remote", Cloud::State::cloud, "11111111-1111-4111-8111-111111111111", std::nullopt, 3 * 1024 * 1024 },
                    { "Synced", Cloud::State::synced, "22222222-2222-4222-8222-222222222222", synced, 0 } };

        std::vector<Action> fired;
        auto act = [&] (const Action& a) { fired.push_back (a); };

        auto m = songsMenu (l, "Local", false, false, tmp, act);
        expect (item (m, String (u8"☁ Remote  (3.0 MB)")).has_value());
        expect (item (m, String (u8"✓ Synced")).has_value());
        expect (item (m, String (u8"Subir «Local» a la nube")).has_value());
        expect (item (m, String (u8"Nube… (activada)")).has_value());
        expect (! item (m, String (u8"Nube sin conexión")).has_value());

        click (m, String (u8"☁ Remote"));
        click (m, String (u8"✓ Synced"));
        click (m, String (u8"Subir a la nube la canción que solo está aquí"));
        click (m, "Remote" + String (u8"…")); // "Borrar de la nube" submenu
        click (m, String (u8"Quitar «Local»"));
        expectEquals ((int) fired.size(), 5);
        expect (fired[0].kind == Action::download && fired[0].entry.name == "Remote");
        expect (fired[1].kind == Action::open && fired[1].entry.local.has_value());
        expect (fired[2].kind == Action::uploadAll);
        expect (fired[3].kind == Action::deleteFromCloud && fired[3].entry.cloudId == l.songs[1].cloudId);
        expect (fired[4].kind == Action::removeCurrent);

        beginTest ("menu: while syncing, cloud actions are off but songs still open");
        auto busy = songsMenu (l, "Synced", true, false, tmp, act);
        expect (item (busy, String (u8"Sincronizando")).has_value());
        expect (! item (busy, String (u8"☁ Remote"))->isEnabled);
        expect (item (busy, String (u8"✓ Synced"))->isEnabled);
        expect (! item (busy, String (u8"Quitar «Synced»"))->isEnabled);
        expect (! item (busy, String (u8"Subir «")).has_value()); // already in the cloud

        beginTest ("menu: offline says so and offers no cloud actions");
        l.offline = true;
        auto offline = songsMenu (l, "Local", false, false, tmp, act);
        expect (item (offline, String (u8"Nube sin conexión")).has_value());
        expect (! item (offline, String (u8"Subir")).has_value());
        expect (! item (offline, "Borrar de la nube").has_value());
        expect (! item (offline, String (u8"☁ Remote"))->isEnabled);
        expect (item (offline, String (u8"Nube… (sin conexión)")).has_value());
    }

    void testAgainstFakeWorker()
    {
        using namespace CloudUi;

        PythonWorker::Process server;
        expect (server.start ({ "/usr/bin/python3", File (__FILE__).getSiblingFile ("fake_cloud.py").getFullPathName(), "tok" }), "fake worker");
        String port;
        server.readLine (port, 5000);
        const auto url = "http://127.0.0.1:" + port.trim();
        auto cloudState = [&] { return JSON::parse (URL (url + "/_state").readEntireTextStream()); };

        auto settingsFile = tmp.getChildFile ("cfg/cloud.settings");

        beginTest ("settings panel: test connection says if the token is wrong, then works");
        {
            SettingsPanel panel (settingsFile, {});
            panel.url.setText (url);
            panel.token.setText ("nope");
            panel.testButton.triggerClick();
            expect (waitFor ([&] { return panel.result.getText().startsWith (String (u8"✕")); }), panel.result.getText());
            expect (panel.result.getText().contains (String (u8"inválido")), panel.result.getText());

            panel.token.setText ("tok");
            panel.testButton.triggerClick();
            expect (waitFor ([&] { return panel.result.getText() == String (u8"✓ Conectado"); }), panel.result.getText());
            expect (! settingsFile.exists()); // testing doesn't save
        }

        beginTest ("settings panel: Guardar writes the settings (token private), and the token isn't shown back");
        Cloud::Settings saved;
        {
            SettingsPanel panel (settingsFile, [&] (const Cloud::Settings& s) { saved = s; });
            panel.url.setText (url + "/");
            panel.token.setText ("tok");
            panel.enabled.setToggleState (true, sendNotificationSync);
            panel.saveButton.triggerClick(); // async, like a real click
            expect (waitFor ([&] { return saved.active(); }, 5000), panel.result.getText());
            expect (saved.url == url);
            expect (panel.token.getText().isEmpty());
        }
        auto onDisk = Cloud::Settings::load (settingsFile);
        expect (onDisk.active() && onDisk.url == url && onDisk.token == "tok");
        struct stat st {};
        ::stat (settingsFile.getFullPathName().toRawUTF8(), &st);
        expectEquals ((int) (st.st_mode & 0777), 0600);
        {
            SettingsPanel reopened (settingsFile, {});
            expect (reopened.token.getText().isEmpty() && reopened.url.getText() == url && reopened.enabled.getToggleState());
        }

        // Laptop 1 with one song; the menu's actions go to the controller like Main.cpp does.
        auto lib1 = tmp.getChildFile ("lib1");
        write (lib1.getChildFile ("Song/drums.mp3"), String::repeatedString ("d", 4000));
        write (lib1.getChildFile ("Song/.velody"), R"({"bpm":120,"updatedAt":1000})");
        Controller c1 (lib1, settingsFile);
        StringArray statuses;
        c1.onStatus = [&] (const String& s) { statuses.add (s); };

        auto listNow = [&] (Controller& c)
        {
            std::optional<Listing> got;
            c.list ([&] (const Listing& l) { got = l; });
            expect (waitFor ([&] { return got.has_value(); }));
            return got.value_or (Listing {});
        };

        String lastError = "pending";
        bool done = false;
        auto onDone = [&] (const String& e) { lastError = e; done = true; };
        auto handle = [&] (Controller& c, const Action& a)
        {
            done = false;
            switch (a.kind)
            {
                case Action::uploadAll:
                {
                    StringArray names;
                    for (auto& e : listNow (c).songs)
                        if (e.state == Cloud::State::local)
                            names.add (e.name);
                    c.uploadAll (names, [&] (int, const StringArray& failed) { onDone (failed.joinIntoString (",")); });
                    break;
                }
                case Action::download:        c.download (a.entry, [&] (const File&, const String& e) { onDone (e); }); break;
                // A "trash" folder of the test's own: the system trash must not fill up with test songs.
                case Action::removeCurrent:   c.removeLocal (a.entry, [&] (const File& f) { tmp.getChildFile ("trash").createDirectory();
                                                                                             return f.moveFileTo (tmp.getChildFile ("trash").getNonexistentChildFile (f.getFileName(), {})); }, onDone); break;
                case Action::deleteFromCloud: c.deleteFromCloud (a.entry, onDone); break;
                case Action::upload:          c.upload (a.entry.name, onDone); break;
                default:                      break;
            }
            expect (c.busy() || done);
            expect (waitFor ([&] { return done; }, 30000));
            expect (! c.busy());
            expectEquals (lastError, String());
        };

        beginTest ("menu -> upload all: the song reaches the cloud and shows as synced, with progress");
        auto l = listNow (c1);
        expect (l.enabled && ! l.offline && l.songs.size() == 1 && l.songs[0].state == Cloud::State::local);
        auto m = songsMenu (l, "", false, false, lib1, [&] (const Action& a) { handle (c1, a); });
        click (m, String (u8"Subir a la nube la canción"));
        const auto id = SongMeta::read (lib1.getChildFile ("Song"))["cloudId"].toString();
        expect (Cloud::isSongId (id));
        expectEquals (cloudState()[Identifier (id)]["name"].toString(), String ("Song"));
        expect (statuses.joinIntoString ("|").contains ("Subiendo 1 canciones"), statuses.joinIntoString ("|"));
        expect (statuses.joinIntoString ("|").contains ("%"));
        expect (item (songsMenu (listNow (c1), "", false, false, lib1, {}), String (u8"✓ Song")).has_value());

        beginTest ("menu on laptop 2 -> click a cloud song: it is downloaded into the library");
        auto lib2 = tmp.getChildFile ("lib2");
        lib2.createDirectory();
        Controller c2 (lib2, settingsFile);
        m = songsMenu (listNow (c2), "", false, false, lib2, [&] (const Action& a) { handle (c2, a); });
        click (m, String (u8"☁ Song"));
        expectEquals (lib2.getChildFile ("Song/drums.mp3").loadFileAsString(), String::repeatedString ("d", 4000));
        expectEquals ((int) SongMeta::read (lib2.getChildFile ("Song"))["bpm"], 120);
        expect (item (songsMenu (listNow (c2), "", false, false, lib2, {}), String (u8"✓ Song")).has_value());

        beginTest ("open a synced song: the newer analysis from the other laptop comes first");
        NamedValueSet edit;
        edit.set ("bpm", 97);
        SongMeta::update (lib1.getChildFile ("Song"), edit); // edited on laptop 1, newer
        bool pushed = false;
        auto e1 = listNow (c1).songs[0];
        c1.syncMetaOnOpen (e1, [&] { pushed = true; });
        expect (waitFor ([&] { return pushed; }));
        bool opened = false;
        auto e2 = listNow (c2).songs[0];
        c2.syncMetaOnOpen (e2, [&] { opened = true; });
        expect (waitFor ([&] { return opened; }));
        expectEquals ((int) SongMeta::read (lib2.getChildFile ("Song"))["bpm"], 97);

        beginTest ("menu -> Quitar (de esta laptop): local folder goes, the cloud copy stays");
        m = songsMenu (listNow (c2), "Song", false, false, lib2, [&] (const Action& a) { handle (c2, a); });
        click (m, String (u8"Quitar «Song»"));
        expect (! lib2.getChildFile ("Song").exists());
        expect (cloudState().hasProperty (Identifier (id)));
        expect (item (songsMenu (listNow (c2), "", false, false, lib2, {}), String (u8"☁ Song")).has_value());

        beginTest ("menu -> Borrar de la nube: gone from the cloud; laptop 1 keeps its copy as local");
        m = songsMenu (listNow (c2), "", false, false, lib2, [&] (const Action& a) { handle (c2, a); });
        click (m, "Song" + String (u8"…"));
        expect (! cloudState().hasProperty (Identifier (id)));
        expect (listNow (c2).songs.empty());
        auto after1 = listNow (c1);
        expect (after1.songs.size() == 1 && after1.songs[0].state == Cloud::State::local);

        beginTest ("cloud unreachable: the menu still lists local songs and says it's offline");
        Cloud::Settings { "http://127.0.0.1:1", "tok", true }.save (settingsFile);
        auto lo = listNow (c1);
        expect (lo.offline && lo.songs.size() == 1);
        expect (item (songsMenu (lo, "", false, false, lib1, {}), String (u8"Nube sin conexión")).has_value());

        beginTest ("cloud off: plain local list, no network");
        Cloud::Settings {}.save (settingsFile);
        auto offList = listNow (c1);
        expect (! offList.enabled && offList.songs.size() == 1);
        expect (item (songsMenu (offList, "", false, false, lib1, {}), String (u8"Nube… (apagada)")).has_value());

        server.terminate();
    }
};

static CloudUiTests cloudUiTests;
