#pragma once
#include <JuceHeader.h>
#include "SongEdit.h"

namespace te = tracktion;

// One row per audio track: name, volume (dB) and pan, written straight into the track's
// VolumeAndPanPlugin so they're saved with the edit. Mute/solo stay in the track header.
// Last row: Master (the edit's master volume) and Mix Pan (the mix but not the metronome).
class MixerPanel : public Component
{
public:
    static constexpr int rowHeight = 28;

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
                    addAndMakeVisible (rows.add (new Row (t->getName(), *vp)));
            if (auto mv = edit->getMasterVolumePlugin())
            {
                master = std::make_unique<MasterRow> (*this, *mv);
                addAndMakeVisible (*master);
            }
        }
        resized();
    }

    int getIdealHeight() const { return (rows.size() + (master != nullptr ? 1 : 0)) * rowHeight; }

    // Mix Pan is a preference for every song (like Electron's velody:masterPan): the owner saves it.
    std::function<void (float)> onMixPanChanged;

    struct MasterRow : Component
    {
        MasterRow (MixerPanel& owner, te::VolumeAndPanPlugin& p) : plugin (&p)
        {
            label.setText ("Master", dontSendNotification);
            label.setFont (label.getFont().boldened());
            volume.setRange (-60.0, 6.0, 0.1);
            volume.setTextValueSuffix (" dB");
            volume.setDoubleClickReturnValue (true, 0.0);
            volume.setValue (p.getVolumeDb(), dontSendNotification);
            volume.onValueChange = [this] { if (plugin != nullptr) plugin->setVolumeDb ((float) volume.getValue()); };

            mixPan.setRange (-1.0, 1.0, 0.05);
            mixPan.setDoubleClickReturnValue (true, 0.0);
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

            for (auto* s : { &volume, &mixPan })
                s->setTextBoxStyle (Slider::TextBoxRight, false, 90, rowHeight - 6);
            addAndMakeVisible (label);
            addAndMakeVisible (volume);
            addAndMakeVisible (mixPan);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (2);
            label.setBounds (r.removeFromLeft (150));
            mixPan.setBounds (r.removeFromRight (220));
            volume.setBounds (r);
        }

        te::VolumeAndPanPlugin::Ptr plugin;
        Label label;
        Slider volume, mixPan;
    };

    std::unique_ptr<MasterRow> master;

    struct Row : Component
    {
        Row (const String& name, te::VolumeAndPanPlugin& p) : plugin (&p)
        {
            label.setText (name, dontSendNotification);

            volume.setRange (-60.0, 6.0, 0.1);
            volume.setTextValueSuffix (" dB");
            volume.setDoubleClickReturnValue (true, 0.0);
            volume.setValue (p.getVolumeDb(), dontSendNotification);
            volume.onValueChange = [this] { if (plugin != nullptr) plugin->setVolumeDb ((float) volume.getValue()); };

            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setValue (p.getPan(), dontSendNotification);
            pan.onValueChange = [this] { if (plugin != nullptr) plugin->setPan ((float) pan.getValue()); };

            for (auto* s : { &volume, &pan })
                s->setTextBoxStyle (Slider::TextBoxRight, false, 70, rowHeight - 6);

            addAndMakeVisible (label);
            addAndMakeVisible (volume);
            addAndMakeVisible (pan);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (2);
            label.setBounds (r.removeFromLeft (150));
            pan.setBounds (r.removeFromRight (220));
            volume.setBounds (r);
        }

        te::VolumeAndPanPlugin::Ptr plugin;
        Label label;
        Slider volume, pan;
    };

    OwnedArray<Row> rows;

    void resized() override
    {
        auto r = getLocalBounds();
        for (auto* row : rows)
            row->setBounds (r.removeFromTop (rowHeight));
        if (master != nullptr)
            master->setBounds (r.removeFromTop (rowHeight));
    }

private:
    te::Edit* edit = nullptr;
};
