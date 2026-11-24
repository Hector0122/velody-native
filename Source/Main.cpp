#include <JuceHeader.h>
#include "common/Utilities.h"
#include "common/Components.h"
#include "common/PluginWindow.h"

// One project file in ~/Documents/Velody; recordings land next to it as WAV.
static File projectFile()
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Velody");
    dir.createDirectory();
    return dir.getChildFile ("Proyecto.tracktionedit");
}

class MainComponent : public Component,
                      private ChangeListener
{
public:
    MainComponent()
    {
        Helpers::addAndMakeVisible (*this, { &audioButton, &pluginsButton, &playButton, &recordButton,
                                             &newTrackButton, &deleteButton, &folderButton, &status });

        audioButton.onClick = [this] { EngineHelpers::showAudioDeviceSettings (engine); };
        pluginsButton.onClick = [this] { showPluginScanner(); };
        playButton.onClick = [this] { stopOrPlay (false); };
        recordButton.onClick = [this] { stopOrPlay (true); };
        newTrackButton.onClick = [this] { edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 1); };
        deleteButton.onClick = [this] { deleteSelection(); };
        folderButton.onClick = [] { projectFile().revealToUser(); };

        auto f = projectFile();
        edit = f.existsAsFile() ? te::loadEditFromFile (engine, f) : te::createEmptyEdit (engine, f);
        edit->playInStopEnabled = true;
        edit->getTransport().addChangeListener (this);
        selectionManager.addChangeListener (this);
        engine.getDeviceManager().addChangeListener (this);

        assignInputsToTracks();
        save();

        editComponent = std::make_unique<EditComponent> (*edit, selectionManager);
        editComponent->getEditViewState().showFooters = true; // footer = plugin chain with "+" button
        editComponent->getEditViewState().showWaveDevices = true;
        addAndMakeVisible (*editComponent);

        refreshButtons();
        setSize (1000, 600);
    }

    ~MainComponent() override
    {
        engine.getDeviceManager().removeChangeListener (this);
        save();
        engine.getTemporaryFileManager().getTempDirectory().deleteRecursively();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (34);
        const int w = 110;
        for (auto* b : { &audioButton, &pluginsButton, &playButton, &recordButton, &newTrackButton, &deleteButton, &folderButton })
            b->setBounds (top.removeFromLeft (w).reduced (2));
        status.setBounds (top.reduced (4, 0));
        editComponent->setBounds (r);
    }

private:
    te::Engine engine { "Velody", std::make_unique<ExtendedUIBehaviour>(), nullptr };
    te::SelectionManager selectionManager { engine };
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<EditComponent> editComponent;

    TextButton audioButton { "Audio" }, pluginsButton { "Plugins" }, playButton { "Play" },
               recordButton { "Grabar" }, newTrackButton { "+ Pista" }, deleteButton { "Borrar" },
               folderButton { "Carpeta" };
    Label status;

    void save() { te::EditFileOperations (*edit).save (true, true, false); }

    void stopOrPlay (bool record)
    {
        auto& t = edit->getTransport();
        const bool wasRecording = t.isRecording();

        if (record) EngineHelpers::toggleRecord (*edit);
        else        EngineHelpers::togglePlay (*edit);

        if (wasRecording)
            save(); // keep the new take on disk right away
    }

    // One mono track per hardware input (Scarlett 2i2 -> input 1 and input 2), armed and monitored.
    void assignInputsToTracks()
    {
        auto& dm = engine.getDeviceManager();

        for (int i = 0; i < dm.getNumWaveInDevices(); ++i)
            if (auto in = dm.getWaveInDevice (i))
            {
                in->setChannelConfiguration (te::ChannelConfiguration::mono());
                in->setMonitorMode (te::InputDevice::MonitorMode::automatic);
                in->setEnabled (true);
            }

        edit->getTransport().ensureContextAllocated();

        int trackNum = 0;
        for (auto instance : edit->getAllInputDevices())
            if (instance->getInputDevice().getDeviceType() == te::InputDevice::waveDevice)
                if (auto t = EngineHelpers::getOrInsertAudioTrackAt (*edit, trackNum++))
                {
                    [[maybe_unused]] auto res = instance->setTarget (t->itemID, true, &edit->getUndoManager(), 0);
                    instance->setRecordingEnabled (t->itemID, true);
                }

        edit->restartPlayback();
        refreshButtons();
    }

    void showPluginScanner()
    {
        DialogWindow::LaunchOptions o;
        o.dialogTitle = "Plugins (VST3 / LV2)";
        o.dialogBackgroundColour = Colours::black;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;

        auto& pm = engine.getPluginManager();
        auto v = new PluginListComponent (pm.pluginFormatManager, pm.knownPluginList,
                                          engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
                                          std::addressof (engine.getPropertyStorage().getPropertiesFile()), true);
        v->setSize (800, 600);
        o.content.setOwned (v);
        o.launchAsync();
    }

    void deleteSelection()
    {
        auto sel = selectionManager.getSelectedObject (0);
        if (auto clip = dynamic_cast<te::Clip*> (sel))         clip->removeFromParent();
        else if (auto plugin = dynamic_cast<te::Plugin*> (sel)) plugin->deleteFromParent();
        else if (auto track = dynamic_cast<te::AudioTrack*> (sel)) edit->deleteTrack (track);
    }

    void refreshButtons()
    {
        auto& t = edit->getTransport();
        playButton.setButtonText (t.isPlaying() && ! t.isRecording() ? "Stop" : "Play");
        recordButton.setButtonText (t.isRecording() ? "Detener" : "Grabar");
        recordButton.setColour (TextButton::buttonColourId, t.isRecording() ? Colours::darkred : getLookAndFeel().findColour (TextButton::buttonColourId));

        auto* dev = engine.getDeviceManager().deviceManager.getCurrentAudioDevice();
        status.setText (dev == nullptr ? "Sin interfaz de audio"
                                       : dev->getName() + "  |  " + String (dev->getCurrentSampleRate(), 0) + " Hz  |  "
                                         + String (dev->getCurrentBufferSizeSamples()) + " muestras  |  latencia "
                                         + String (1000.0 * (dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples())
                                                   / jmax (1.0, dev->getCurrentSampleRate()), 1) + " ms",
                        dontSendNotification);

        deleteButton.setEnabled (selectionManager.getSelectedObject (0) != nullptr);
    }

    void changeListenerCallback (ChangeBroadcaster* source) override
    {
        if (source == &engine.getDeviceManager())
            assignInputsToTracks(); // interface or channel changed in the Audio dialog
        else
            refreshButtons();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class VelodyApp : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Velody"; }
    const String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override                 { window = nullptr; }

private:
    struct MainWindow : DocumentWindow
    {
        MainWindow() : DocumentWindow ("Velody", Colours::black, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (VelodyApp)
