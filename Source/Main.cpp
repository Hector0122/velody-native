#include <JuceHeader.h>
#include "common/Utilities.h"
#include "common/Components.h"
#include "common/PluginWindow.h"
#include "SongEdit.h"
#include "MixerPanel.h"
#include "CountInPlugin.h"

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
                                             &newTrackButton, &deleteButton, &folderButton, &countInBox, &status, &mixer });

        countInBox.addItemList ({ "Sin count-in", "Count-in 1 compás", "Count-in 2 compases" }, 1);
        countInBox.onChange = [this] { if (edit != nullptr) edit->state.setProperty (countInProp, countInBox.getSelectedId() - 1, nullptr); };

        songsButton.onClick = [this] { showSongsMenu(); };
        audioButton.onClick = [this] { EngineHelpers::showAudioDeviceSettings (engine); };
        pluginsButton.onClick = [this] { showPluginScanner(); };
        playButton.onClick = [this] { stopOrPlay (false); };
        recordButton.onClick = [this] { stopOrPlay (true); };
        newTrackButton.onClick = [this] { edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 1); mixer.rebuild(); resized(); };
        deleteButton.onClick = [this] { deleteSelection(); };
        folderButton.onClick = [] { projectFile().revealToUser(); };

        engine.getPluginManager().createBuiltInType<CountInPlugin>(); // before any edit that has one loads
        selectionManager.addChangeListener (this);
        engine.getDeviceManager().addChangeListener (this);

        openEdit (projectFile(), nullptr);
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
        countInBox.setBounds (top.removeFromLeft (170).reduced (2));
        status.setBounds (top.reduced (4, 0));
        mixer.setBounds (r.removeFromBottom (jmin (mixer.getIdealHeight(), r.getHeight() / 3)));
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
    ComboBox countInBox;
    MixerPanel mixer;

    static inline const Identifier countInProp { "velodyCountInBars" }; // saved in the song's edit
    Metronome::SongMeta songMeta; // bpm 0 when the open edit isn't an analyzed song
    CountInPlugin* countIn = nullptr;

    Array<Library::Song> songs;

    void save() { te::EditFileOperations (*edit).save (true, true, false); }

    // A new song edit gets one track per stem; reopening keeps the user's edit (mutes, plugins, takes).
    void openEdit (const File& f, const Library::Song* song)
    {
        if (edit != nullptr)
        {
            edit->getTransport().stop (false, false);
            save();
            selectionManager.deselectAll();
            mixer.setEdit (nullptr);
            countIn = nullptr;
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
            if (song != nullptr)
                SongEdit::addStemTracks (*edit, song->stems);
        }

        // Re-rendered on every open so a downbeat fixed in the Electron app shows up here too.
        auto clickWav = f.getSiblingFile (f.getFileNameWithoutExtension() + " - metronomo.wav");
        if (song != nullptr && SongEdit::writeClickWav (engine, *song, clickWav))
            SongEdit::setClickTrack (*edit, clickWav);

        songMeta = song != nullptr ? Metronome::readMeta (song->dir) : Metronome::SongMeta();
        countIn = CountInPlugin::findOrInsert (*edit);
        countInBox.setSelectedId ((int) edit->state.getProperty (countInProp, songMeta.countInBars) + 1, dontSendNotification);
        countInBox.setEnabled (songMeta.bpm > 0);

        edit->playInStopEnabled = true;
        edit->getTransport().addChangeListener (this);

        assignInputsToTracks();
        save();

        editComponent = std::make_unique<EditComponent> (*edit, selectionManager);
        editComponent->getEditViewState().showFooters = true; // footer = plugin chain with "+" button
        editComponent->getEditViewState().showWaveDevices = true;
        addAndMakeVisible (*editComponent);
        mixer.setEdit (edit.get());
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
                openEdit (projectFile(), nullptr);
            else if (id > 0)
                openEdit (projectFile (songs[id - 1].name), &songs.getReference (id - 1));
        });
    }

    void stopOrPlay (bool record)
    {
        auto& t = edit->getTransport();
        const bool wasRecording = t.isRecording();

        if (record)
            EngineHelpers::toggleRecord (*edit);
        else if (t.isPlaying())
            stopPlayback();
        else
            playWithCountIn();

        if (wasRecording)
            save(); // keep the new take on disk right away
    }

    void stopPlayback()
    {
        auto& t = edit->getTransport();
        const bool inCountIn = countIn != nullptr && countIn->isArmed();
        t.stop (false, false);

        if (inCountIn)
        {
            countIn->disarm();
            t.setPosition (te::TimePosition::fromSeconds (countInFrom)); // back to where Play was pressed
        }
    }

    // Clicks on the song's grid, then the song enters exactly at the current position.
    void playWithCountIn()
    {
        auto& t = edit->getTransport();
        const int bars = countInBox.getSelectedId() - 1;
        auto clicks = Metronome::planCountIn (songMeta, t.getPosition().inSeconds(), bars);

        if (countIn != nullptr && ! clicks.empty())
        {
            countInFrom = t.getPosition().inSeconds();
            countIn->arm (clicks, countInFrom);

            // The transport only allows negative positions down to its own count-in length (2 bars at
            // the edit's 120 bpm + 0.5 s = -4.5 s), so set that just for this jump.
            // shortcut: count-in clicks earlier than -4.5 s are skipped (2 slow bars at the song start); upgrade by offsetting the song in the edit.
            const auto oldMode = edit->getCountInMode();
            edit->setCountInMode (te::Edit::CountIn::twoBar);
            t.setPosition (te::TimePosition::fromSeconds (clicks.front().time));
            edit->setCountInMode (oldMode);
        }

        t.play (false);
    }

    double countInFrom = 0;

    // One mono track per hardware input (Scarlett 2i2 -> input 1 and input 2), armed (not monitored),
    // placed after the song's stem tracks so a stem is never recorded over.
    void assignInputsToTracks()
    {
        auto& dm = engine.getDeviceManager();

        for (int i = 0; i < dm.getNumWaveInDevices(); ++i)
            if (auto in = dm.getWaveInDevice (i))
            {
                in->setChannelConfiguration (te::ChannelConfiguration::mono());
                // Off by default: with speakers, monitoring an armed input feeds back. Turn it on per track
                // with "I" -> Input Monitoring, or use the Scarlett's Direct Monitor (no latency).
                in->setMonitorMode (te::InputDevice::MonitorMode::off);
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
        else if (auto track = dynamic_cast<te::AudioTrack*> (sel)) { edit->deleteTrack (track); mixer.rebuild(); resized(); }
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
