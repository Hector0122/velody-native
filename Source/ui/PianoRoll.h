#pragma once
#include <JuceHeader.h>
#include "../Theme.h"

namespace te = tracktion;

// Piano roll for one MIDI clip: a keyboard on the left, bars/beats on top. Click empty space to add a note
// (drag to give it length), drag a note to move it (time and pitch), drag its right edge to resize,
// right-click or Delete to remove, mouse wheel for velocity, Ctrl+Z to undo. Grid 1/4…1/32 and Quantize.
namespace PianoRoll
{
    // Positions inside the clip, in beats (quarter notes), as MidiNote keeps them.
    struct Geometry
    {
        double pxPerBeat = 96.0;
        int rowHeight = 14, keyboardWidth = 56, rulerHeight = 22;
        static constexpr int highest = 127;

        float beatToX (double beat) const { return (float) (keyboardWidth + beat * pxPerBeat); }
        double xToBeat (float x) const     { return jmax (0.0, (x - keyboardWidth) / pxPerBeat); }
        int noteToY (int note) const       { return rulerHeight + (highest - note) * rowHeight; }
        int yToNote (float y) const        { return jlimit (0, highest, highest - (int) std::floor ((y - rulerHeight) / rowHeight)); }
    };

    inline double floorTo (double beat, double grid)   { return std::floor (beat / grid + 1e-9) * grid; }
    inline double nearestTo (double beat, double grid) { return std::round (beat / grid) * grid; }

    inline bool isBlackKey (int note) { const int n = note % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }

    inline String noteName (int note)
    {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return String (names[note % 12]) + String (note / 12 - 1);
    }

    // Starts and lengths to the grid (lengths at least one grid step).
    inline void quantize (te::MidiList& list, double grid, UndoManager* um)
    {
        const auto notes = list.getNotes(); // a copy: moving notes re-sorts the list
        for (auto* n : notes)
        {
            const double start = jmax (0.0, nearestTo (n->getStartBeat().inBeats(), grid));
            const double length = jmax (grid, nearestTo (n->getLengthBeats().inBeats(), grid));
            n->setStartAndLength (te::BeatPosition::fromBeats (start), te::BeatDuration::fromBeats (length), um);
        }
    }

    inline te::MidiNote* noteAt (te::MidiList& list, const Geometry& g, Point<float> p)
    {
        const int pitch = g.yToNote (p.y);
        const double beat = g.xToBeat (p.x);
        for (auto* n : list.getNotes())
            if (n->getNoteNumber() == pitch && beat >= n->getStartBeat().inBeats() && beat < n->getEndBeat().inBeats())
                return n;
        return nullptr;
    }

    class Editor : public Component
    {
    public:
        explicit Editor (te::MidiClip& c) : clip (&c)
        {
            setWantsKeyboardFocus (true);
            updateSize();
        }

        double grid = 0.25; // a 1/16 note in 4/4
        Geometry geometry;

        te::MidiList& list() { return clip->getSequence(); }
        UndoManager* undo() { return &clip->edit.getUndoManager(); }
        double lengthBeats() const { return clip->getLengthInBeats().inBeats(); }

        void updateSize()
        {
            setSize ((int) geometry.beatToX (jmax (4.0, lengthBeats())) + 40, geometry.noteToY (-1) + 1);
        }

        void paint (Graphics& g) override
        {
            auto& G = geometry;
            g.fillAll (Theme::lane);
            // Rows (black keys darker), then beat/bar lines.
            for (int n = 0; n <= Geometry::highest; ++n)
            {
                g.setColour (isBlackKey (n) ? Theme::bg : Theme::lane.brighter (0.04f));
                g.fillRect (G.keyboardWidth, G.noteToY (n), getWidth(), G.rowHeight - 1);
            }
            const int beatsPerBar = clip->edit.tempoSequence.getTimeSig (0) != nullptr ? clip->edit.tempoSequence.getTimeSig (0)->numerator.get() : 4;
            for (double b = 0; b <= lengthBeats() + 1e-6; b += grid)
            {
                const bool bar = std::fmod (b, (double) beatsPerBar) < 1e-6, beat = std::fmod (b, 1.0) < 1e-6;
                g.setColour (bar ? Theme::outline.brighter (0.5f) : beat ? Theme::outline.brighter (0.1f) : Theme::outline.withAlpha (0.4f));
                g.fillRect ((int) G.beatToX (b), G.rulerHeight, 1, getHeight());
                if (bar)
                {
                    g.setColour (Theme::muted);
                    g.setFont (Theme::mono (11.0f));
                    g.drawText (String ((int) (b / beatsPerBar) + 1), (int) G.beatToX (b) + 4, 0, 40, G.rulerHeight, Justification::centredLeft);
                }
            }
            // Notes: colour by velocity.
            for (auto* n : list().getNotes())
            {
                auto r = noteBounds (*n);
                const float v = (float) n->getVelocity() / 127.0f;
                g.setColour (Theme::primary.interpolatedWith (Theme::accent, 1.0f - v).withAlpha (0.55f + 0.45f * v));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (n == selected ? Colours::white : Theme::primary.brighter (0.4f));
                g.drawRoundedRectangle (r, 3.0f, n == selected ? 2.0f : 1.0f);
            }
            // Keyboard.
            for (int n = 0; n <= Geometry::highest; ++n)
            {
                auto k = Rectangle<int> (0, G.noteToY (n), G.keyboardWidth, G.rowHeight - 1);
                g.setColour (isBlackKey (n) ? Colour (0xff2a2a30) : Colour (0xffd4d4d8));
                g.fillRect (k);
                if (n % 12 == 0)
                {
                    g.setColour (Theme::bg);
                    g.setFont (Theme::mono (10.0f));
                    g.drawText (noteName (n), k.reduced (3, 0), Justification::centredRight);
                }
            }
            g.setColour (Theme::panel);
            g.fillRect (0, 0, getWidth(), G.rulerHeight);
        }

        Rectangle<float> noteBounds (te::MidiNote& n) const
        {
            const float x1 = geometry.beatToX (n.getStartBeat().inBeats()), x2 = geometry.beatToX (n.getEndBeat().inBeats());
            return { x1, (float) geometry.noteToY (n.getNoteNumber()) + 1.0f, jmax (4.0f, x2 - x1), (float) geometry.rowHeight - 3.0f };
        }

        void mouseDown (const MouseEvent& e) override
        {
            grabKeyboardFocus();
            if (e.x < geometry.keyboardWidth || e.y < geometry.rulerHeight)
                return;
            undo()->beginNewTransaction();
            auto* hit = noteAt (list(), geometry, e.position);
            if (e.mods.isPopupMenu())
            {
                if (hit != nullptr)
                    removeNote (*hit);
                return;
            }
            if (hit == nullptr)
            {
                const double start = floorTo (geometry.xToBeat (e.position.x), grid);
                if (start >= lengthBeats())
                    return;
                hit = list().addNote (geometry.yToNote (e.position.y), te::BeatPosition::fromBeats (start),
                                      te::BeatDuration::fromBeats (grid), 100, 0, undo());
                dragMode = Drag::resize;
            }
            else
                dragMode = noteBounds (*hit).getRight() - e.position.x < 6.0f ? Drag::resize : Drag::move;
            selected = hit;
            dragStartBeat = hit->getStartBeat().inBeats();
            dragStartLength = hit->getLengthBeats().inBeats();
            dragStartNote = hit->getNoteNumber();
            repaint();
        }

        void mouseDrag (const MouseEvent& e) override
        {
            if (selected == nullptr || dragMode == Drag::none)
                return;
            const double deltaBeats = nearestTo ((e.position.x - e.mouseDownPosition.x) / geometry.pxPerBeat, grid);
            if (dragMode == Drag::move)
            {
                const double start = jlimit (0.0, jmax (0.0, lengthBeats() - grid), dragStartBeat + deltaBeats);
                const int note = jlimit (0, Geometry::highest, dragStartNote - (int) std::round ((e.position.y - e.mouseDownPosition.y) / geometry.rowHeight));
                selected->setStartAndLength (te::BeatPosition::fromBeats (start), te::BeatDuration::fromBeats (dragStartLength), undo());
                selected->setNoteNumber (note, undo());
            }
            else
            {
                const double length = jmax (grid, dragStartLength + deltaBeats);
                selected->setStartAndLength (te::BeatPosition::fromBeats (dragStartBeat), te::BeatDuration::fromBeats (length), undo());
            }
            repaint();
        }

        void mouseUp (const MouseEvent&) override { dragMode = Drag::none; }

        void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
        {
            if (auto* n = noteAt (list(), geometry, e.position))
            {
                n->setVelocity (jlimit (1, 127, n->getVelocity() + (w.deltaY > 0 ? 8 : -8)), undo());
                repaint();
                return;
            }
            Component::mouseWheelMove (e, w); // scroll the viewport
        }

        bool keyPressed (const KeyPress& k) override
        {
            if ((k == KeyPress::deleteKey || k == KeyPress::backspaceKey) && selected != nullptr)
            {
                undo()->beginNewTransaction();
                removeNote (*selected);
                return true;
            }
            if (k == KeyPress ('z', ModifierKeys::commandModifier, 0))
            {
                selected = nullptr;
                undo()->undo();
                repaint();
                return true;
            }
            return false;
        }

        void removeNote (te::MidiNote& n)
        {
            if (selected == &n)
                selected = nullptr;
            list().removeNote (n, undo());
            repaint();
        }

        te::MidiNote* selected = nullptr;

    private:
        te::MidiClip::Ptr clip;
        enum class Drag { none, move, resize } dragMode = Drag::none;
        double dragStartBeat = 0, dragStartLength = 0;
        int dragStartNote = 60;
    };

    // The window's content: grid + Quantize on top, the editor in a scrolling view.
    class Panel : public Component
    {
    public:
        explicit Panel (te::MidiClip& c) : editor (c)
        {
            gridBox.addItemList ({ String (u8"Cuadrícula 1/4"), String (u8"Cuadrícula 1/8"), String (u8"Cuadrícula 1/16"), String (u8"Cuadrícula 1/32") }, 1);
            gridBox.setSelectedItemIndex (2, dontSendNotification);
            gridBox.onChange = [this]
            {
                static const double grids[] { 1.0, 0.5, 0.25, 0.125 };
                editor.grid = grids[jlimit (0, 3, gridBox.getSelectedItemIndex())];
                editor.repaint();
            };
            quantizeButton.onClick = [this]
            {
                editor.undo()->beginNewTransaction();
                quantize (editor.list(), editor.grid, editor.undo());
                editor.repaint();
            };
            help.setText (String (u8"Clic: nota · arrastra: mover / borde: duración · clic derecho o Supr: borrar · rueda: intensidad · Ctrl+Z"),
                          dontSendNotification);
            help.setFont (Theme::body (12.0f));
            help.setColour (Label::textColourId, Theme::muted);
            for (auto* c2 : std::initializer_list<Component*> { &gridBox, &quantizeButton, &help, &view })
                addAndMakeVisible (c2);
            view.setViewedComponent (&editor, false);
            setSize (900, 520);
            // Start around the notes there are, else around middle C.
            int focus = 60;
            if (! editor.list().getNotes().isEmpty())
                focus = editor.list().getNotes().getFirst()->getNoteNumber();
            view.setViewPosition (0, jmax (0, editor.geometry.noteToY (focus) - 200));
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto top = r.removeFromTop (32);
            gridBox.setBounds (top.removeFromLeft (170).reduced (2));
            quantizeButton.setBounds (top.removeFromLeft (110).reduced (2));
            help.setBounds (top.reduced (8, 0));
            view.setBounds (r.withTrimmedTop (6));
        }

        Editor editor;
        ComboBox gridBox;
        TextButton quantizeButton { "Cuantizar" };

    private:
        Label help;
        Viewport view;
    };
}
