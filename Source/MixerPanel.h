#pragma once
#include <JuceHeader.h>
#include "SongEdit.h"
#include "Theme.h"
#include "Mixing.h"

namespace te = tracktion;

// Mixer as vertical channel strips (like REAPER's): per track a pan knob, a vertical fader with its level
// meter, mute/solo and the name; volume and pan are written straight into the track's VolumeAndPanPlugin
// so they're saved with the edit. The Master strip (the edit's master volume + Mix Pan: the mix but not the
// metronome) stays on the right; many tracks scroll sideways.
class MixerPanel : public Component
{
public:
    static constexpr int stripWidth = 86, idealHeight = 270;

    MixerPanel()
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&strips, false);
        viewport.setScrollBarsShown (false, true);
    }

    ~MixerPanel() override { setEdit (nullptr); }

    void setEdit (te::Edit* e)
    {
        edit = e;
        rebuild();
    }

    // Call after tracks are added or removed.
    void rebuild()
    {
        rows.clear();
        master = nullptr;
        if (edit != nullptr)
        {
            for (auto t : te::getAudioTracks (*edit))
                if (auto vp = t->getVolumePlugin())
                    strips.addAndMakeVisible (rows.add (new Row (*t, *vp)));
            if (auto mv = edit->getMasterVolumePlugin())
            {
                master = std::make_unique<MasterRow> (*this, *mv);
                addAndMakeVisible (*master);
            }
        }
        resized();
    }

    int getIdealHeight() const { return edit != nullptr ? idealHeight : 0; }

    // Mix Pan is a preference for every song (like Electron's velody:masterPan): the owner saves it.
    std::function<void (float)> onMixPanChanged;

    // Rotary pan knob with a centre detent on double-click.
    static void stylePan (Slider& s)
    {
        s.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        s.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        s.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setPopupDisplayEnabled (true, true, nullptr);
    }

    static void styleFader (Slider& s)
    {
        s.setSliderStyle (Slider::LinearVertical);
        s.setRange (-60.0, 6.0, 0.1);
        s.setSkewFactorFromMidPoint (-12.0);
        s.setTextValueSuffix (" dB");
        s.setDoubleClickReturnValue (true, 0.0);
        s.setTextBoxStyle (Slider::TextBoxBelow, false, stripWidth - 12, 18);
    }

    // Peak meter fed by the track's LevelMeterPlugin.
    struct Meter : Component, private Timer
    {
        explicit Meter (te::LevelMeasurer* m) : measurer (m)
        {
            if (measurer != nullptr)
                measurer->addClient (client);
            startTimerHz (30);
        }
        ~Meter() override { if (measurer != nullptr) measurer->removeClient (client); }

        void timerCallback() override
        {
            for (int c = 0; c < 2; ++c)
            {
                const float db = client.getAndClearAudioLevel (c).dB;
                level[c] = jmax (db, level[c] - 1.5f); // falls back smoothly
            }
            repaint();
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const float w = (r.getWidth() - 2) / 2;
            for (int c = 0; c < 2; ++c)
            {
                auto bar = Rectangle<float> (r.getX() + c * (w + 2), r.getY(), w, r.getHeight());
                g.setColour (Theme::lane);
                g.fillRoundedRectangle (bar, 2);
                const float norm = jlimit (0.0f, 1.0f, (level[c] + 60.0f) / 66.0f);
                auto lit = bar.withTrimmedTop (bar.getHeight() * (1 - norm));
                ColourGradient grad (Theme::success, 0, bar.getBottom(), Theme::record, 0, bar.getY(), false);
                grad.addColour (0.75, Theme::mute);
                g.setGradientFill (grad);
                g.fillRoundedRectangle (lit, 2);
            }
        }

        te::LevelMeasurer* measurer;
        te::LevelMeasurer::Client client;
        float level[2] { -100, -100 };
    };

    struct Row : Component, private ValueTree::Listener
    {
        Row (te::AudioTrack& t, te::VolumeAndPanPlugin& p)
            : track (&t), plugin (&p), meter (t.getLevelMeterPlugin() != nullptr ? &t.getLevelMeterPlugin()->measurer : nullptr)
        {
            label.setText (t.getName(), dontSendNotification);
            label.setJustificationType (Justification::centred);
            label.setFont (Theme::body (12.0f, true));
            label.setTooltip ("Doble clic para cambiar el nombre");
            label.setEditable (false, true, false);
            label.onTextChange = [this]
            {
                auto n = label.getText().trim();
                if (n.isNotEmpty() && n != track->getName())
                    track->setName (n);
                else
                    label.setText (track->getName(), dontSendNotification);
            };
            trackState = t.state;
            trackState.addListener (this);

            styleFader (volume);
            volume.setValue (p.getVolumeDb(), dontSendNotification);
            volume.onValueChange = [this] { if (plugin != nullptr) plugin->setVolumeDb ((float) volume.getValue()); };

            pan.setRange (-1.0, 1.0, 0.01);
            stylePan (pan);
            pan.setValue (p.getPan(), dontSendNotification);
            pan.onValueChange = [this] { if (plugin != nullptr) plugin->setPan ((float) pan.getValue()); };

            for (auto* b : { &mute, &solo })
            {
                b->setClickingTogglesState (true);
                addAndMakeVisible (b);
            }
            mute.setColour (TextButton::buttonOnColourId, Theme::mute);
            solo.setColour (TextButton::buttonOnColourId, Theme::solo);
            mute.setToggleState (t.isMuted (false), dontSendNotification);
            solo.setToggleState (t.isSolo (false), dontSendNotification);
            mute.onClick = [this] { track->setMute (mute.getToggleState()); };
            solo.onClick = [this] { track->setSolo (solo.getToggleState()); };

            // With Automatizar on, a fader/knob move while playing is written as automation.
            volume.onDragStart = [this] { if (plugin != nullptr) plugin->volParam->parameterChangeGestureBegin(); };
            volume.onDragEnd   = [this] { if (plugin != nullptr) plugin->volParam->parameterChangeGestureEnd(); };
            pan.onDragStart    = [this] { if (plugin != nullptr) plugin->panParam->parameterChangeGestureBegin(); };
            pan.onDragEnd      = [this] { if (plugin != nullptr) plugin->panParam->parameterChangeGestureEnd(); };

            sends.setTooltip (String (u8"Envíos a los buses de efectos (una reverb para varias pistas)"));
            sends.onClick = [this] { showSends(); };
            addAndMakeVisible (sends);

            addAndMakeVisible (label);
            addAndMakeVisible (volume);
            addAndMakeVisible (pan);
            addAndMakeVisible (meter);
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (3, 2);
            g.setGradientFill (ColourGradient (Theme::raised.brighter (0.06f), 0, r.getY(), Theme::raised, 0, r.getBottom(), false));
            g.fillRoundedRectangle (r, Theme::radius);
            g.setColour (Theme::trackColour (SongEdit::audioTrackIndex (*track)));
            g.fillRoundedRectangle (r.removeFromTop (4).reduced (10, 0), 2.0f);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (7, 6);
            pan.setBounds (r.removeFromTop (36).withSizeKeepingCentre (36, 36));
            auto buttons = r.removeFromTop (24);
            mute.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 1));
            solo.setBounds (buttons.reduced (2, 1));
            sends.setBounds (r.removeFromTop (24).reduced (2, 1));
            label.setBounds (r.removeFromBottom (20));
            r.removeFromTop (4);
            meter.setBounds (r.removeFromRight (10).withTrimmedBottom (22));
            volume.setBounds (r);
        }

        ~Row() override { trackState.removeListener (this); }

        // Each bus: its send level; picking one shows a knob for it.
        void showSends()
        {
            PopupMenu m;
            auto list = Mixing::buses (track->edit);
            for (auto& b : list)
            {
                if (b.track == track.get())
                    continue;
                const float db = Mixing::sendLevelDb (*track, b.number);
                m.addItem (String (u8"→ ") + b.name + "   (" + (db <= -99.0f ? String (u8"apagado") : String (db, 1) + " dB") + ")",
                           [this, n = b.number, name = b.name] { showSendKnob (n, name); });
            }
            if (m.getNumItems() == 0)
                m.addItem (String (u8"No hay buses: créalo en «+ Pista → Bus de efectos»"), false, false, nullptr);
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (sends));
        }

        void showSendKnob (int bus, const String& name)
        {
            auto knob = std::make_unique<Slider> (Slider::RotaryHorizontalVerticalDrag, Slider::TextBoxBelow);
            knob->setRange (-60.0, 6.0, 0.1);
            knob->setSkewFactorFromMidPoint (-12.0);
            knob->setTextValueSuffix (" dB");
            knob->setValue (jmax (-60.0f, Mixing::sendLevelDb (*track, bus)), dontSendNotification);
            knob->setDoubleClickReturnValue (true, -60.0);
            knob->setTooltip (String (u8"Envío a ") + name + String (u8" (todo a la izquierda = apagado)"));
            knob->onValueChange = [t = track, bus, k = knob.get()] { Mixing::setSendLevelDb (*t, bus, k->getValue() <= -59.9 ? -100.0f : (float) k->getValue()); };
            knob->setSize (110, 120);
            CallOutBox::launchAsynchronously (std::move (knob), sends.getScreenBounds(), nullptr);
        }

        // Renamed (here or in the track header), muted or soloed elsewhere: follow it.
        void valueTreePropertyChanged (ValueTree& v, const Identifier& id) override
        {
            if (v != trackState)
                return;
            if (id == te::IDs::name)      label.setText (track->getName(), dontSendNotification);
            else if (id == te::IDs::mute) mute.setToggleState (track->isMuted (false), dontSendNotification);
            else if (id == te::IDs::solo) solo.setToggleState (track->isSolo (false), dontSendNotification);
        }

        te::AudioTrack::Ptr track;
        te::VolumeAndPanPlugin::Ptr plugin;
        Label label;
        Slider volume, pan;
        TextButton mute { "M" }, solo { "S" }, sends { String (u8"Envíos") };
        Meter meter;
        ValueTree trackState;
    };

    struct MasterRow : Component
    {
        MasterRow (MixerPanel& owner, te::VolumeAndPanPlugin& p) : plugin (&p), meter (masterMeasurer (p.edit))
        {
            label.setText ("Master", dontSendNotification);
            label.setJustificationType (Justification::centred);
            label.setFont (Theme::title (13.0f));
            label.setColour (Label::textColourId, Theme::primary);

            styleFader (volume);
            volume.setValue (p.getVolumeDb(), dontSendNotification);
            volume.onValueChange = [this] { if (plugin != nullptr) plugin->setVolumeDb ((float) volume.getValue()); };

            mixPan.setRange (-1.0, 1.0, 0.05);
            stylePan (mixPan);
            mixPan.setTooltip (String (u8"Mix Pan: toda la mezcla a un lado; el metrónomo queda con su propio pan (p. ej. mezcla a la bocina, click a los audífonos)"));
            mixPan.textFromValueFunction = [] (double v)
            {
                return std::abs (v) < 0.001 ? String ("Mix C") : "Mix " + String (roundToInt (std::abs (v) * 100)) + (v < 0 ? " % L" : " % R");
            };
            mixPan.setValue (SongEdit::mixPan (p.edit), dontSendNotification);
            mixPan.onValueChange = [this, &owner]
            {
                SongEdit::setMixPan (plugin->edit, (float) mixPan.getValue());
                if (owner.onMixPanChanged)
                    owner.onMixPanChanged ((float) mixPan.getValue());
            };
            mixPanLabel.setText ("Mix Pan", dontSendNotification);
            mixPanLabel.setJustificationType (Justification::centred);
            mixPanLabel.setFont (Theme::body (10.0f));
            mixPanLabel.setColour (Label::textColourId, Theme::muted);

            for (auto* c : std::initializer_list<Component*> { &label, &volume, &mixPan, &mixPanLabel, &meter })
                addAndMakeVisible (c);
        }

        // The master bus level comes from the edit's own master LevelMeterPlugin when there is one.
        static te::LevelMeasurer* masterMeasurer (te::Edit& e)
        {
            for (auto p : e.getMasterPluginList())
                if (auto lm = dynamic_cast<te::LevelMeterPlugin*> (p))
                    return &lm->measurer;
            return nullptr;
        }

        void paint (Graphics& g) override
        {
            g.setColour (Theme::raised.brighter (0.04f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (3, 2), Theme::radius);
            g.setColour (Theme::primary.withAlpha (0.5f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3, 2), Theme::radius, 1.0f);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (7, 6);
            mixPan.setBounds (r.removeFromTop (36).withSizeKeepingCentre (36, 36));
            mixPanLabel.setBounds (r.removeFromTop (24));
            label.setBounds (r.removeFromBottom (20));
            r.removeFromTop (4);
            meter.setBounds (r.removeFromRight (10).withTrimmedBottom (22));
            volume.setBounds (r);
        }

        te::VolumeAndPanPlugin::Ptr plugin;
        Label label, mixPanLabel;
        Slider volume, mixPan;
        Meter meter;
    };

    OwnedArray<Row> rows;
    std::unique_ptr<MasterRow> master;

    void resized() override
    {
        auto r = getLocalBounds();
        if (master != nullptr)
            master->setBounds (r.removeFromRight (stripWidth + 10).withTrimmedLeft (10));
        viewport.setBounds (r);
        const int h = r.getHeight() - (rows.size() * stripWidth > r.getWidth() ? viewport.getScrollBarThickness() : 0);
        strips.setSize (jmax (r.getWidth(), rows.size() * stripWidth), h);
        int x = 0;
        for (auto* row : rows)
        {
            row->setBounds (x, 0, stripWidth, h);
            x += stripWidth;
        }
    }

private:
    te::Edit* edit = nullptr;
    Viewport viewport;
    Component strips;
};
