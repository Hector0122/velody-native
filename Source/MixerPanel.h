#pragma once
#include <JuceHeader.h>

namespace te = tracktion;

// One row per audio track: name, volume (dB) and pan, written straight into the track's
// VolumeAndPanPlugin so they're saved with the edit. Mute/solo stay in the track header.
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
        if (edit != nullptr)
            for (auto t : te::getAudioTracks (*edit))
                if (auto vp = t->getVolumePlugin())
                    addAndMakeVisible (rows.add (new Row (t->getName(), *vp)));
        resized();
    }

    int getIdealHeight() const { return rows.size() * rowHeight; }

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
    }

private:
    te::Edit* edit = nullptr;
};
