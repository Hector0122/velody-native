#include <JuceHeader.h>
#include "common/Utilities.h"
#include "../Source/ui/Components.h"
#include "../Source/PluginPicker.h"
#include "../Source/SongEdit.h"

// «+ Efecto»: one entry per effect (the best of its VST3/LV2/LADSPA builds), no instruments, a search
// box, the recent ones first; the button on the track opens it and the chosen effect lands in the track
// before its fader. The old menu's «Create New Empty Rack» is gone.
struct PluginPickerTests : UnitTest
{
    PluginPickerTests() : UnitTest ("Plugin picker (+ Efecto)") {}

    static PluginDescription desc (const String& name, const String& maker, const String& format, bool instrument = false)
    {
        PluginDescription d;
        d.name = name;
        d.manufacturerName = maker;
        d.pluginFormatName = format;
        d.fileOrIdentifier = "/plugins/" + format + "/" + name;
        d.isInstrument = instrument;
        return d;
    }

    static const PluginPicker::Choice* find (const std::vector<PluginPicker::Choice>& v, const String& name)
    {
        for (auto& c : v)
            if (c.name == name)
                return &c;
        return nullptr;
    }

    static int indexOf (te::Track& t, te::Plugin* p)
    {
        int i = 0;
        for (auto q : t.pluginList)
        {
            if (q == p)
                return i;
            ++i;
        }
        return -1;
    }

    static int volumeIndex (te::Track& t)
    {
        int i = 0;
        for (auto q : t.pluginList)
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (q) != nullptr)
                return i;
            ++i;
        }
        return -1;
    }

    void runTest() override
    {
        using namespace PluginPicker;

        // What the scan found on this laptop, in small: LSP in VST3 + LADSPA, SWH in LV2 + LADSPA with the
        // maker spelled differently, NAM only in LV2, Surge (an instrument), an oscillator that LV2 calls an
        // instrument and LADSPA an effect.
        Array<PluginDescription> scanned {
            desc ("Compressor Stereo", "LSP LADSPA", "LADSPA"),
            desc ("Compressor Stereo", "LSP VST3", "VST3"),
            desc ("Aliasing", "Steve Harris <steve@plugin.org.uk>", "LADSPA"),
            desc ("Aliasing", "Steve Harris", "LV2"),
            desc ("Neural Amp Modeler", "", "LV2"),
            desc ("Surge XT", "Surge Synth Team", "VST3", true),
            desc ("Analogue Oscillator", "Steve Harris", "LV2", true),
            desc ("Analogue Oscillator", "Steve Harris <steve@plugin.org.uk>", "LADSPA"),
        };

        beginTest ("each effect once, in its best format; instruments left out");
        auto ext = externalEffects (scanned);
        expectEquals ((int) ext.size(), 3);
        expectEquals (find (ext, "Compressor Stereo")->desc.pluginFormatName, String ("VST3"));
        expectEquals (find (ext, "Compressor Stereo")->detail, String (u8"LSP · VST3"));
        expectEquals (find (ext, "Aliasing")->desc.pluginFormatName, String ("LV2"));
        expectEquals (find (ext, "Aliasing")->detail, String (u8"Steve Harris · LV2"));
        expectEquals (find (ext, "Neural Amp Modeler")->detail, String ("LV2"));
        expect (find (ext, "Surge XT") == nullptr);
        expect (find (ext, "Analogue Oscillator") == nullptr);
        expectEquals (ext[0].name, String ("Aliasing")); // sorted by name

        beginTest ("Velody's own effects come too, and no rack");
        auto choices = all (scanned);
        expect (find (choices, "Ecualizador (EQ)") != nullptr && find (choices, "Reverb") != nullptr);
        for (auto& c : choices)
            expect (! c.name.containsIgnoreCase ("rack") && c.xmlType != te::RackInstance::xmlTypeName);

        beginTest ("search: accents/case don't matter, maker and format count too");
        expectEquals ((int) filter (choices, {}, "neural").size(), 1);
        expectEquals ((int) filter (choices, {}, "NEURAL amp").size(), 1);
        expectEquals ((int) filter (choices, {}, "lsp").size(), 1);
        expect (find (filter (choices, {}, "ecualizador"), "Ecualizador (EQ)") != nullptr);
        expect (filter (choices, {}, "nada parecido").empty());

        beginTest ("recents: most recent first, at most 8, saved in the settings");
        StringArray recents;
        recents = remember (recents, find (choices, "Reverb")->key);
        recents = remember (recents, find (choices, "Neural Amp Modeler")->key);
        auto shown = filter (choices, recents, {});
        expectEquals (shown[0].name, String ("Neural Amp Modeler"));
        expectEquals (shown[1].name, String ("Reverb"));
        expectEquals ((int) shown.size(), (int) choices.size());
        recents = remember (recents, find (choices, "Reverb")->key); // used again: back on top, not twice
        expectEquals (recents.size(), 2);
        expectEquals (filter (choices, recents, {})[0].name, String ("Reverb"));
        for (int i = 0; i < 20; ++i)
            recents = remember (recents, "x" + String (i));
        expectEquals (recents.size(), maxRecents);

        auto settingsFile = File::createTempFile ("velody-recents.settings");
        {
            PropertiesFile props (settingsFile, {});
            saveRecents (props, { "builtin:reverb", "neural amp modeler" });
            props.saveIfNeeded();
        }
        {
            PropertiesFile props (settingsFile, {});
            expect (loadRecents (props) == StringArray { "builtin:reverb", "neural amp modeler" });
        }
        settingsFile.deleteFile();

        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-picker");
        dir.createDirectory();
        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("p.tracktionedit"));
        edit->ensureNumberOfAudioTracks (1);
        auto* track = te::getAudioTracks (*edit)[0];

        beginTest ("the chosen effect goes into the track, before its fader, in the order added; Ctrl+Z takes it out");
        const int before = track->pluginList.size();
        auto eq = addTo (*track, *find (choices, "Ecualizador (EQ)"));
        expect (dynamic_cast<te::EqualiserPlugin*> (eq.get()) != nullptr);
        expectEquals (track->pluginList.size(), before + 1);
        expectLessThan (indexOf (*track, eq.get()), volumeIndex (*track));
        auto reverb = addTo (*track, *find (choices, "Reverb"));
        expectEquals (indexOf (*track, reverb.get()), indexOf (*track, eq.get()) + 1);
        expectLessThan (indexOf (*track, reverb.get()), volumeIndex (*track));
        edit->getUndoManager().undo();
        expectEquals (indexOf (*track, reverb.get()), -1);
        expectEquals (track->pluginList.size(), before + 1);

        beginTest ("flow: «+ Efecto» on the track → search window → type → Enter → effect in the track");
        {
        te::SelectionManager selection (engine);
        EditViewState evs (*edit, selection);
        TrackFooterComponent footer (evs, track);
        TextButton* addButton = nullptr;
        for (auto* c : footer.getChildren())
            if (auto* b = dynamic_cast<TextButton*> (c); b != nullptr && b->getButtonText() == "+ Efecto")
                addButton = b;
        expect (addButton != nullptr);

        std::unique_ptr<Panel> panel;
        te::Plugin::Ptr added;
        VelodyUi::onAddEffect = [&] (te::Track::Ptr t)
        {
            panel = std::make_unique<Panel> (choices, StringArray(), [&, t] (const Choice& c) { added = addTo (*t, c); });
        };
        addButton->triggerClick();
        MessageManager::getInstance()->runDispatchLoopUntil (50);
        expect (panel != nullptr);
        if (panel != nullptr)
        {
            expectEquals ((int) panel->shown.size(), (int) choices.size());
            panel->query.setText ("compres", true);
            MessageManager::getInstance()->runDispatchLoopUntil (50);
            expectEquals (panel->count.getText(), String ("2 efectos")); // Velody's Compresor + LSP's
            panel->query.setText ("compresor", true);
            MessageManager::getInstance()->runDispatchLoopUntil (50);
            expectEquals (panel->count.getText(), String ("1 efecto"));
            panel->chooseSelected();
            expect (dynamic_cast<te::CompressorPlugin*> (added.get()) != nullptr);
            expect (indexOf (*track, added.get()) >= 0 && indexOf (*track, added.get()) < volumeIndex (*track));
        }
        VelodyUi::onAddEffect = nullptr;
        }

        eq = nullptr; // plugins must go before their edit
        reverb = nullptr;
        edit = nullptr;
        dir.deleteRecursively();
    }
};

static PluginPickerTests pluginPickerTests;

// The zoom on the real timeline: Ctrl+wheel brings the song closer (around the mouse), Alt+wheel makes
// every track taller, dragging a track's bottom edge makes just that one taller (double-click: back),
// and none of it lands in Ctrl+Z.
struct TimelineZoomUiTests : UnitTest
{
    TimelineZoomUiTests() : UnitTest ("Timeline zoom (on screen)") {}

    static MouseEvent event (Component& c, Point<float> pos, ModifierKeys mods, Point<float> down)
    {
        auto now = Time::getCurrentTime();
        return MouseEvent (Desktop::getInstance().getMainMouseSource(), pos, mods, MouseInputSource::defaultPressure,
                           MouseInputSource::defaultOrientation, MouseInputSource::defaultRotation,
                           MouseInputSource::defaultTiltX, MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, false);
    }

    template <class T>
    static Array<T*> childrenOf (Component& parent)
    {
        Array<T*> out;
        for (auto* c : parent.getChildren())
            if (auto* t = dynamic_cast<T*> (c))
                out.add (t);
        return out;
    }

    static void pump() { MessageManager::getInstance()->runDispatchLoopUntil (50); }

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-zoom");
        dir.createDirectory();
        {
            auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("p.tracktionedit"));
            edit->ensureNumberOfAudioTracks (2);
            te::SelectionManager selection (engine);
            {
                EditComponent ec (*edit, selection);
                ec.setSize (1170, 500); // lanes: 1000 px wide (headers 170)
                pump();
                auto& v = ec.getEditViewState();
                auto tracks = childrenOf<TrackComponent> (ec);
                auto headers = childrenOf<TrackHeaderComponent> (ec);
                expectEquals (tracks.size(), 2);
                expectEquals (tracks[0]->getHeight(), Zoom::defaultHeight);

                beginTest ("Ctrl+wheel up over the lanes: closer in time, the time under the mouse stays put");
                v.setView ({ 0, 60 });
                const float mouseX = 170 + 250; // a quarter of the way: 15 s
                ec.mouseWheelMove (event (ec, { mouseX, 100 }, ModifierKeys::commandModifier, { mouseX, 100 }), { 0, 0.1f, false, false, false });
                pump();
                expectLessThan (v.view().span(), 60.0);
                expectWithinAbsoluteError (v.view().start + v.view().span() * 0.25, 15.0, 0.01);
                ec.mouseWheelMove (event (ec, { mouseX, 100 }, ModifierKeys::commandModifier, { mouseX, 100 }), { 0, -0.1f, false, false, false });
                pump();
                expectWithinAbsoluteError (v.view().span(), 60.0, 0.01);

                beginTest ("Shift+wheel moves along the song");
                v.setView ({ 0, 20 });
                ec.mouseWheelMove (event (ec, { mouseX, 100 }, ModifierKeys::shiftModifier, { mouseX, 100 }), { 0, -0.2f, false, false, false });
                pump();
                expectGreaterThan (v.view().start, 0.0);
                expectWithinAbsoluteError (v.view().span(), 20.0, 0.01);

                beginTest ("Alt+wheel up: every track taller, so the waveforms are bigger");
                ec.mouseWheelMove (event (ec, { mouseX, 100 }, ModifierKeys::altModifier, { mouseX, 100 }), { 0, 0.1f, false, false, false });
                pump();
                expectGreaterThan (v.trackHeight.get(), Zoom::defaultHeight);
                expectEquals (tracks[0]->getHeight(), v.trackHeight.get());
                expectEquals (tracks[1]->getHeight(), v.trackHeight.get());
                expectEquals (tracks[1]->getY(), tracks[0]->getBottom() + 1);
                ec.scaleTrackHeight (100.0);
                pump();
                expectEquals (tracks[0]->getHeight(), Zoom::maxHeight);
                ec.scaleTrackHeight (0.0001);
                pump();
                expectEquals (tracks[0]->getHeight(), Zoom::minHeight);
                v.trackHeight = Zoom::defaultHeight;
                pump();

                beginTest ("dragging a track's bottom edge: just that track taller; double-click the edge: back");
                auto* h = headers[0];
                const Point<float> edge { 50.0f, (float) h->getHeight() - 2.0f };
                h->mouseDown (event (*h, edge, {}, edge));
                h->mouseDrag (event (*h, edge.translated (0, 150), {}, edge));
                pump();
                expectEquals (tracks[0]->getHeight(), Zoom::defaultHeight + 150);
                expectEquals (tracks[1]->getHeight(), Zoom::defaultHeight);
                expectEquals (h->getHeight(), Zoom::defaultHeight + 150);
                const Point<float> newEdge { 50.0f, (float) h->getHeight() - 2.0f };
                h->mouseDoubleClick (event (*h, newEdge, {}, newEdge));
                pump();
                expectEquals (tracks[0]->getHeight(), Zoom::defaultHeight);

                beginTest ("a click in the middle of the header still selects the track (not a resize)");
                const Point<float> middle { 50.0f, 10.0f };
                h->mouseDown (event (*h, middle, {}, middle));
                h->mouseDrag (event (*h, middle.translated (0, 80), {}, middle));
                pump();
                expect (selection.isSelected (tracks[0]->getTrack()));
                expectEquals (tracks[0]->getHeight(), Zoom::defaultHeight);

                beginTest ("zooming isn't an edit: Ctrl+Z doesn't bring the old zoom back");
                edit->getUndoManager().beginNewTransaction();
                v.setView ({ 5, 25 });
                v.trackHeight = 120;
                edit->getUndoManager().undo();
                pump();
                expect (v.view() == Zoom::View { 5, 25 });
                expectEquals (v.trackHeight.get(), 120);
            }
        }
        dir.deleteRecursively();
    }
};

static TimelineZoomUiTests timelineZoomUiTests;
