#include <JuceHeader.h>
#include "common/Utilities.h"
#include "common/Components.h"
#include "common/PluginWindow.h"
#include "SongEdit.h"

// Project files live in ~/Documents/Velody and recordings land next to them as WAV,
// so the Electron library is only ever read, never written.
static File projectFile (const String& name = "Proyecto")
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Velody");
    dir.createDirectory();
    return dir.getChildFile (File::createLegalFileName (name) + ".tracktionedit");
}

class MainComponent : public Component,
                      private ChangeListener
{
public:
    MainComponent()
    {
        Helpers::addAndMakeVisible (*this, { &songsButton, &audioButton, &pluginsButton, &playButton, &recordButton,
                                             &newTrackButton, &deleteButton, &folderButton, &status });

        songsButton.onClick = [this] { showSongsMenu(); };
        audioButton.onClick = [this] { EngineHelpers::showAudioDeviceSettings (engine); };
        pluginsButton.onClick = [this] { showPluginScanner(); };
        playButton.onClick = [this] { stopOrPlay (false); };
        recordButton.onClick = [this] { stopOrPlay (true); };
        newTrackButton.onClick = [this] { edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 1); };
        deleteButton.onClick = [this] { deleteSelection(); };
        folderButton.onClick = [] { projectFile().revealToUser(); };

        selectionManager.addChangeListener (this);
        engine.getDeviceManager().addChangeListener (this);

        openEdit (projectFile(), {});
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
        for (auto* b : { &songsButton, &audioButton, &pluginsButton, &playButton, &recordButton, &newTrackButton, &deleteButton, &folderButton })
            b->setBounds (top.removeFromLeft (w).reduced (2));
        status.setBounds (top.reduced (4, 0));
        if (editComponent != nullptr)
            editComponent->setBounds (r);
    }

private:
    te::Engine engine { "Velody", std::make_unique<ExtendedUIBehaviour>(), nullptr };
    te::SelectionManager selectionManager { engine };
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<EditComponent> editComponent;

    TextButton songsButton { "Canciones" }, audioButton { "Audio" }, pluginsButton { "Plugins" }, playButton { "Play" },
               recordButton { "Grabar" }, newTrackButton { "+ Pista" }, deleteButton { "Borrar" },
               folderButton { "Carpeta" };
    Label status;

    Array<Library::Song> songs;

    void save() { te::EditFileOperations (*edit).save (true, true, false); }

    // A new song edit gets one track per stem; reopening keeps the user's edit (mutes, plugins, takes).
    void openEdit (const File& f, const Array<File>& stems)
    {
        if (edit != nullptr)
        {
            edit->getTransport().stop (false, false);
            save();
            selectionManager.deselectAll();
            editComponent = nullptr;
            edit = nullptr;
        }

        if (f.existsAsFile())
        {
            edit = te::loadEditFromFile (engine, f);
        }
        else
        {
            edit = te::createEmptyEdit (engine, f);
            SongEdit::addStemTracks (*edit, stems);
        }

        edit->playInStopEnabled = true;
        edit->getTransport().addChangeListener (this);

        assignInputsToTracks();
        save();

        editComponent = std::make_unique<EditComponent> (*edit, selectionManager);
        editComponent->getEditViewState().showFooters = true; // footer = plugin chain with "+" button
        editComponent->getEditViewState().showWaveDevices = true;
        addAndMakeVisible (*editComponent);
        resized();
        refreshButtons();
    }

    void showSongsMenu()
    {
        songs = Library::scan (Library::defaultRoot());

        PopupMenu m;
        m.addItem (-1, "Proyecto vacío");
        m.addSeparator();
        for (int i = 0; i < songs.size(); ++i)
            m.addItem (i + 1, songs[i].name);
        if (songs.isEmpty())
            m.addItem (-2, "No hay canciones en " + Library::defaultRoot().getFullPathName(), false);

        m.showMenuAsync (PopupMenu::Options().withTargetComponent (songsButton), [this] (int id)
        {
            if (id == -1)
                openEdit (projectFile(), {});
            else if (id > 0)
                openEdit (projectFile (songs[id - 1].name), songs[id - 1].stems);
        });
    }

    void stopOrPlay (bool record)
    {
        auto& t = edit->getTransport();
        const bool wasRecording = t.isRecording();

        if (record) EngineHelpers::toggleRecord (*edit);
        else        EngineHelpers::togglePlay (*edit);

        if (wasRecording)
            save(); // keep the new take on disk right away
    }

    // One mono track per hardware input (Scarlett 2i2 -> input 1 and input 2), armed and monitored,
    // placed after the song's stem tracks so a stem is never recorded over.
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

        int trackNum = SongEdit::firstInputTrack (*edit);

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
