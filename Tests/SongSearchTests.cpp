#include <JuceHeader.h>
#include "../Source/SongSearch.h"

// Song search: accent/case-insensitive, every word in any order; the window filters as you type,
// ↑/↓ move, Enter opens the selected one (a cloud-only song comes back as such, to download).
struct SongSearchTests : UnitTest
{
    SongSearchTests() : UnitTest ("Song search") {}

    static std::optional<PopupMenu::Item> item (const PopupMenu& m, const String& prefix)
    {
        for (PopupMenu::MenuItemIterator it (m, true); it.next();)
            if (it.getItem().text.startsWith (prefix))
                return it.getItem();
        return {};
    }

    void runTest() override
    {
        using namespace SongSearch;

        beginTest ("accents and case don't matter; every word must be there, in any order");
        expectEquals (fold (String (u8"Canción Ñandú ÁRBOL")), String ("cancion nandu arbol"));
        expect (matches (String (u8"Caifanes - Afuera"), "afuera caifanes"));
        expect (matches (String (u8"Café Tacvba - Eres"), "cafe"));
        expect (matches (String (u8"Maná - Rayando el sol"), String (u8"MANÁ sol")));
        expect (! matches ("Caifanes - Afuera", "caifanes viento"));
        expect (matches ("Lo que sea", "   "));

        std::vector<Cloud::Entry> songs {
            { "Caifanes - Afuera", Cloud::State::synced, "11111111-1111-4111-8111-111111111111", Library::Song { "Caifanes - Afuera", {}, {} }, 0 },
            { String (u8"Café Tacvba - Eres"), Cloud::State::local, {}, Library::Song { String (u8"Café Tacvba - Eres"), {}, {} }, 0 },
            { "Caifanes - La Negra Tomasa", Cloud::State::cloud, "22222222-2222-4222-8222-222222222222", std::nullopt, 2048 }
        };
        expectEquals ((int) filter (songs, "caifanes").size(), 2);
        expectEquals ((int) filter (songs, "").size(), 3);

        beginTest ("the window: typing filters, the count follows, ↑/↓ move, Enter opens the selected song");
        std::vector<Cloud::Entry> chosen;
        Panel panel ([&] (const Cloud::Entry& e) { chosen.push_back (e); });
        panel.setSongs (songs);
        // Like typing: TextEditor tells its listeners asynchronously.
        auto type = [&] (const String& t) { panel.query.setText (t, true); MessageManager::getInstance()->runDispatchLoopUntil (50); };
        expectEquals ((int) panel.shown.size(), 3);
        expectEquals (panel.count.getText(), String ("3 canciones"));

        type ("caifanes");
        expectEquals ((int) panel.shown.size(), 2);
        expectEquals (panel.list.getSelectedRow(), 0);
        expect (panel.navigate (KeyPress (KeyPress::downKey)));
        expectEquals (panel.list.getSelectedRow(), 1);
        panel.navigate (KeyPress (KeyPress::downKey)); // already the last one
        expectEquals (panel.list.getSelectedRow(), 1);
        panel.navigate (KeyPress (KeyPress::upKey));
        expectEquals (panel.list.getSelectedRow(), 0);

        type ("tomasa");
        expectEquals (panel.count.getText(), String (u8"1 canción"));
        panel.chooseSelected();
        expect (chosen.size() == 1 && chosen[0].state == Cloud::State::cloud && chosen[0].name.contains ("Tomasa"));

        type ("nada que coincida");
        expect (panel.shown.empty() && panel.list.getSelectedRow() < 0);
        panel.chooseSelected(); // nothing selected: nothing happens
        expectEquals ((int) chosen.size(), 1);

        beginTest ("Canciones menu: Buscar canción… is the first item");
        CloudUi::Listing l;
        l.songs = songs;
        std::vector<CloudUi::Action> fired;
        auto m = CloudUi::songsMenu (l, {}, false, false, File(), [&] (const CloudUi::Action& a) { fired.push_back (a); });
        PopupMenu::MenuItemIterator it (m, false);
        expect (it.next() && it.getItem().text.startsWith (String (u8"Buscar canción…")));
        item (m, String (u8"Buscar canción"))->action();
        expect (fired.size() == 1 && fired[0].kind == CloudUi::Action::search);
    }
};

static SongSearchTests songSearchTests;
