#pragma once
#include <JuceHeader.h>
#include "SongSearch.h"

namespace te = tracktion;

// «+ Efecto»: a window with a search box instead of the old menu of ~1000 entries. The Linux plugin packs
// ship most effects in two or three formats (VST3, LV2, LADSPA), often with the maker spelled differently,
// so each effect is listed once by name, in the best format that's there. Instruments aren't listed (they
// go in «+ Pista → Pista de instrumento»). The effects used last come first.
namespace PluginPicker
{
    struct Choice
    {
        String name, detail;          // detail: maker · format, or «Incluido en Velody»
        String key;                   // what «recientes» remembers
        String xmlType;               // te::ExternalPlugin::xmlTypeName, or a built-in's type
        PluginDescription desc;       // for external plugins
    };

    // VST3 first (best hosted), then LV2, then LADSPA (no GUI, fewest features).
    inline int formatRank (const String& format)
    {
        if (format == "VST3")   return 0;
        if (format == "LV2")    return 1;
        if (format == "LADSPA") return 2;
        return 3;
    }

    inline String dedupeKey (const String& name) { return SongSearch::fold (name).trim(); }

    // "Steve Harris <steve@plugin.org.uk>" → "Steve Harris"; "LSP VST3" → "LSP".
    inline String cleanMaker (const String& maker)
    {
        auto m = maker.upToFirstOccurrenceOf ("<", false, false).trim();
        for (auto suffix : { " VST3", " LV2", " LADSPA" })
            if (m.endsWith (suffix))
                m = m.dropLastCharacters ((int) strlen (suffix)).trim();
        return m;
    }

    // Velody's own effects (no scan needed).
    inline std::vector<Choice> builtIns()
    {
        std::vector<Choice> out;
        auto add = [&] (const String& name, const String& xmlType) { out.push_back ({ name, String (u8"Incluido en Velody"), "builtin:" + xmlType, xmlType, {} }); };
        add ("Ecualizador (EQ)", te::EqualiserPlugin::xmlTypeName);
        add ("Compresor", te::CompressorPlugin::xmlTypeName);
        add ("Reverb", te::ReverbPlugin::xmlTypeName);
        add ("Delay", te::DelayPlugin::xmlTypeName);
        add ("Chorus", te::ChorusPlugin::xmlTypeName);
        add ("Phaser", te::PhaserPlugin::xmlTypeName);
        add ("Filtro pasa bajos", te::LowPassPlugin::xmlTypeName);
        add ("Cambio de tono", te::PitchShiftPlugin::xmlTypeName);
        return out;
    }

    // The scanned plugins as effects: one per name, best format; a name that's an instrument in any format
    // is left out. Sorted by name.
    inline std::vector<Choice> externalEffects (const Array<PluginDescription>& types)
    {
        std::map<String, PluginDescription> best;
        std::set<String> instruments;
        for (auto& d : types)
        {
            const auto k = dedupeKey (d.name);
            if (k.isEmpty())
                continue;
            if (d.isInstrument)
                instruments.insert (k);
            auto it = best.find (k);
            if (it == best.end() || formatRank (d.pluginFormatName) < formatRank (it->second.pluginFormatName))
                best[k] = d;
        }
        std::vector<Choice> out;
        for (auto& [k, d] : best)
        {
            if (instruments.count (k) > 0)
                continue;
            const auto maker = cleanMaker (d.manufacturerName);
            out.push_back ({ d.name.trim(), (maker.isNotEmpty() ? maker + String (u8" · ") : String()) + d.pluginFormatName,
                             k, te::ExternalPlugin::xmlTypeName, d });
        }
        std::sort (out.begin(), out.end(), [] (const Choice& a, const Choice& b) { return a.name.compareNatural (b.name) < 0; });
        return out;
    }

    // Everything «+ Efecto» offers: Velody's own first, then the scanned ones.
    inline std::vector<Choice> all (const Array<PluginDescription>& types)
    {
        auto out = builtIns();
        for (auto& c : externalEffects (types))
            out.push_back (c);
        return out;
    }

    // What the list shows for a search: the recent ones first (most recent on top), then the rest in order.
    inline std::vector<Choice> filter (const std::vector<Choice>& choices, const StringArray& recents, const String& query)
    {
        std::vector<Choice> recent, rest;
        for (auto& c : choices)
            if (SongSearch::matches (c.name + " " + c.detail, query))
                (recents.contains (c.key) ? recent : rest).push_back (c);
        std::stable_sort (recent.begin(), recent.end(), [&] (const Choice& a, const Choice& b) { return recents.indexOf (a.key) < recents.indexOf (b.key); });
        recent.insert (recent.end(), rest.begin(), rest.end());
        return recent;
    }

    inline constexpr int maxRecents = 8;

    inline StringArray remember (StringArray recents, const String& key)
    {
        recents.removeString (key);
        recents.insert (0, key);
        while (recents.size() > maxRecents)
            recents.remove (recents.size() - 1);
        return recents;
    }

    inline StringArray loadRecents (PropertiesFile& p)                       { return StringArray::fromLines (p.getValue ("recentEffects")); }
    inline void saveRecents (PropertiesFile& p, const StringArray& recents)  { p.setValue ("recentEffects", recents.joinIntoString ("\n")); }

    // Where a new effect goes: after the instrument and the effects already there, before the track's own
    // volume/pan and meter (so the fader still comes last).
    inline int insertIndex (te::Track& track)
    {
        int index = 0;
        for (auto p : track.pluginList)
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr)
                break;
            ++index;
        }
        return index;
    }

    inline te::Plugin::Ptr create (te::Edit& edit, const Choice& c)
    {
        return edit.getPluginCache().createNewPlugin (c.xmlType, c.desc);
    }

    // The chosen effect into the track (undoable). Returns the plugin, or nullptr when it couldn't be created.
    inline te::Plugin::Ptr addTo (te::Track& track, const Choice& c, te::SelectionManager* selection = nullptr)
    {
        auto plugin = create (track.edit, c);
        if (plugin == nullptr)
            return {};
        track.edit.getUndoManager().beginNewTransaction();
        track.pluginList.insertPlugin (plugin, insertIndex (track), selection);
        return plugin;
    }

    class Panel : public Component, private ListBoxModel, private TextEditor::Listener, private KeyListener
    {
    public:
        Panel (std::vector<Choice> choicesIn, StringArray recentsIn, std::function<void (const Choice&)> choose)
            : choices (std::move (choicesIn)), recents (std::move (recentsIn)), onChoose (std::move (choose))
        {
            query.setTextToShowWhenEmpty (String (u8"Buscar efecto… (ej. neural, reverb, eq)"), Colours::grey);
            query.addListener (this);
            query.addKeyListener (this);
            addAndMakeVisible (query);
            list.setModel (this);
            list.setRowHeight (38);
            addAndMakeVisible (list);
            addAndMakeVisible (count);
            refilter();
            setSize (460, 520);
        }

        ~Panel() override { query.removeKeyListener (this); }

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

        void chooseSelected()
        {
            const int row = list.getSelectedRow();
            if (row >= 0 && row < (int) shown.size() && onChoose)
                onChoose (shown[(size_t) row]);
        }

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
        std::vector<Choice> shown;

    private:
        std::vector<Choice> choices;
        StringArray recents;
        std::function<void (const Choice&)> onChoose;

        void refilter()
        {
            shown = filter (choices, recents, query.getText());
            list.updateContent();
            list.selectRow (shown.empty() ? -1 : 0);
            count.setText (shown.size() == 1 ? String ("1 efecto") : String ((int) shown.size()) + " efectos", dontSendNotification);
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
            const auto& c = shown[(size_t) row];
            if (selected)
                g.fillAll (getLookAndFeel().findColour (TextEditor::highlightColourId));
            auto text = getLookAndFeel().findColour (Label::textColourId);
            g.setColour (text);
            g.setFont (FontOptions (15.0f));
            g.drawText (c.name, 8, 2, w - 16, h / 2, Justification::bottomLeft, true);
            g.setColour (text.withAlpha (0.55f));
            g.setFont (FontOptions (12.0f));
            const auto detail = recents.contains (c.key) ? String (u8"Reciente · ") + c.detail : c.detail;
            g.drawText (detail, 8, h / 2, w - 16, h / 2 - 2, Justification::topLeft, true);
        }

        void listBoxItemDoubleClicked (int row, const MouseEvent&) override
        {
            list.selectRow (row);
            chooseSelected();
        }

        void returnKeyPressed (int) override { chooseSelected(); }
    };
}
