#pragma once
#include <JuceHeader.h>
#include "CloudUi.h"

// Song search (Electron's filterSongsByQuery, a bit friendlier): accents and case don't matter and every
// word must appear, in any order ("caifanes afuera"). A small window: type to filter, ↑/↓ to move,
// Enter or double-click to open (a cloud-only song downloads first), Esc to close.
namespace SongSearch
{
    // Lower case without accents, for comparing.
    inline String fold (const String& s)
    {
        static const String from = String::fromUTF8 (u8"áàäâãåéèëêíìïîóòöôõúùüûñçÁÀÄÂÃÅÉÈËÊÍÌÏÎÓÒÖÔÕÚÙÜÛÑÇ");
        static const String to   = "aaaaaaeeeeiiiiooooouuuuncaaaaaaeeeeiiiiooooouuuunc";
        return s.toLowerCase().replaceCharacters (from, to);
    }

    inline bool matches (const String& name, const String& query)
    {
        const auto n = fold (name);
        for (auto& word : StringArray::fromTokens (fold (query), " \t", ""))
            if (word.isNotEmpty() && ! n.contains (word))
                return false;
        return true;
    }

    inline std::vector<Cloud::Entry> filter (const std::vector<Cloud::Entry>& songs, const String& query)
    {
        std::vector<Cloud::Entry> out;
        for (auto& e : songs)
            if (matches (e.name, query))
                out.push_back (e);
        return out;
    }

    class Panel : public Component, private ListBoxModel, private TextEditor::Listener, private KeyListener
    {
    public:
        explicit Panel (std::function<void (const Cloud::Entry&)> choose) : onChoose (std::move (choose))
        {
            query.setTextToShowWhenEmpty (String (u8"Buscar canción…"), Colours::grey);
            query.addListener (this);
            query.addKeyListener (this);
            addAndMakeVisible (query);
            list.setModel (this);
            list.setRowHeight (26);
            addAndMakeVisible (list);
            addAndMakeVisible (count);
            setSize (420, 420);
        }

        ~Panel() override { query.removeKeyListener (this); }

        // The library (local songs at once; the cloud's list replaces it when it arrives).
        void setSongs (std::vector<Cloud::Entry> s)
        {
            songs = std::move (s);
            refilter();
        }

        void visibilityChanged() override
        {
            if (isShowing())
                query.grabKeyboardFocus();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            query.setBounds (r.removeFromTop (30));
            count.setBounds (r.removeFromBottom (22));
            list.setBounds (r.withTrimmedTop (6));
        }

        // Opens the selected result (Enter, double-click).
        void chooseSelected()
        {
            const int row = list.getSelectedRow();
            if (row >= 0 && row < (int) shown.size() && onChoose)
                onChoose (shown[(size_t) row]);
        }

        // ↑/↓ in the search box move through the results.
        bool navigate (const KeyPress& k)
        {
            const int row = list.getSelectedRow();
            if (k == KeyPress::downKey) { list.selectRow (jmin (row + 1, (int) shown.size() - 1)); return true; }
            if (k == KeyPress::upKey)   { list.selectRow (jmax (row - 1, 0)); return true; }
            return false;
        }

        TextEditor query;
        ListBox list;
        Label count;
        std::vector<Cloud::Entry> shown;

    private:
        std::vector<Cloud::Entry> songs;
        std::function<void (const Cloud::Entry&)> onChoose;

        void refilter()
        {
            shown = filter (songs, query.getText());
            list.updateContent();
            list.selectRow (shown.empty() ? -1 : 0);
            count.setText (shown.size() == 1 ? String (u8"1 canción") : String ((int) shown.size()) + String (u8" canciones"), dontSendNotification);
        }

        void textEditorTextChanged (TextEditor&) override { refilter(); }
        void textEditorReturnKeyPressed (TextEditor&) override { chooseSelected(); }
        void textEditorEscapeKeyPressed (TextEditor&) override
        {
            if (auto* w = findParentComponentOfClass<DialogWindow>())
                w->exitModalState (0);
        }

        bool keyPressed (const KeyPress& k, Component*) override { return navigate (k); }

        int getNumRows() override { return (int) shown.size(); }

        void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= (int) shown.size())
                return;
            if (selected)
                g.fillAll (getLookAndFeel().findColour (TextEditor::highlightColourId));
            g.setColour (getLookAndFeel().findColour (Label::textColourId));
            g.setFont (15.0f);
            g.drawText (CloudUi::songLabel (shown[(size_t) row]), 8, 0, w - 16, h, Justification::centredLeft, true);
        }

        void listBoxItemDoubleClicked (int row, const MouseEvent&) override
        {
            list.selectRow (row);
            chooseSelected();
        }

        void returnKeyPressed (int) override { chooseSelected(); }
    };
}
