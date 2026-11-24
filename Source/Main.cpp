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
                                             &newTrackButton, &deleteButton, &folderButton, &countInBox, &soundBox, &speedLabel, &speedSlider, &pitchLabel, &pitchSlider,
                                             &status, &mixer });

        speedLabel.setText ("Velocidad", dontSendNotification);
        pitchLabel.setText ("Tono", dontSendNotification);
        speedSlider.setRange (50, 150, 5);
        speedSlider.setTextValueSuffix (" %");
        speedSlider.setDoubleClickReturnValue (true, 100);
        pitchSlider.setRange (-12, 12, 1);
        pitchSlider.setTextValueSuffix (" st");
        pitchSlider.setDoubleClickReturnValue (true, 0);
        for (auto* sl : { &speedSlider, &pitchSlider })
        {
            sl->setTextBoxStyle (Slider::TextBoxRight, false, 60, 22);
            sl->onValueChange = [this] { applyPractice(); };
        }

        countInBox.addItemList ({ "Sin count-in", "Count-in 1 compás", "Count-in 2 compases" }, 1);

        soundBox.addItemList (Metronome::soundNames, 1);
        soundBox.setTooltip ("Sonido del metrónomo y del count-in");
        soundBox.setSelectedItemIndex (settings.getIntValue ("clickSound", 0), dontSendNotification);
        soundBox.onChange = [this]
        {
            settings.setValue ("clickSound", soundBox.getSelectedItemIndex());
            refreshClickTrack();
        };
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
        auto row = r.removeFromTop (30);
        countInBox.setBounds (row.removeFromLeft (180).reduced (2));
        soundBox.setBounds (row.removeFromLeft (120).reduced (2));
        speedLabel.setBounds (row.removeFromLeft (70));
        speedSlider.setBounds (row.removeFromLeft (260));
        pitchLabel.setBounds (row.removeFromLeft (40));
        pitchSlider.setBounds (row.removeFromLeft (260));
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
    ComboBox countInBox, soundBox;
    std::optional<Library::Song> currentSong;
    File currentEditFile;

    // App-wide preferences (~/.config/Velody/Velody.settings).
    PropertiesFile settings { []
    {
        PropertiesFile::Options o;
        o.applicationName = "Velody";
        o.filenameSuffix = "settings";
        o.folderName = "Velody";
        return o;
    }() };

    Metronome::Sound clickSound() const { return (Metronome::Sound) jlimit (0, Metronome::soundNames.size() - 1, soundBox.getSelectedItemIndex()); }

    // Re-rendered on every open (and sound change) so a downbeat fixed in the Electron app shows up here too.
    void refreshClickTrack()
    {
        if (edit == nullptr || ! currentSong.has_value())
            return;

        auto clickWav = currentEditFile.getSiblingFile (currentEditFile.getFileNameWithoutExtension() + " - metronomo.wav");
        edit->getTransport().stop (false, false); // the WAV is about to be rewritten
        if (SongEdit::writeClickWav (engine, *currentSong, clickWav, clickSound()))
        {
            SongEdit::setClickTrack (*edit, clickWav);
            SongEdit::setPractice (*edit, speed(), semitones()); // the new clip starts at 100 %
        }
    }
    Label speedLabel, pitchLabel;
    Slider speedSlider, pitchSlider;
    double appliedSpeed = 1;
    MixerPanel mixer;

    static inline const Identifier countInProp { "velodyCountInBars" }, // saved in the song's edit
                                   speedProp { "velodySpeed" }, pitchProp { "velodySemitones" };
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

        const bool isNew = ! f.existsAsFile();
        edit = SongEdit::openOrCreate (engine, f);
        if (isNew && song != nullptr)
            SongEdit::addStemTracks (*edit, song->stems);

        currentEditFile = f;
        currentSong = song != nullptr ? std::optional<Library::Song> (*song) : std::nullopt;

        songMeta = song != nullptr ? Metronome::readMeta (song->dir) : Metronome::SongMeta();
        countIn = CountInPlugin::findOrInsert (*edit);
        countInBox.setSelectedId ((int) edit->state.getProperty (countInProp, songMeta.countInBars) + 1, dontSendNotification);
        countInBox.setEnabled (songMeta.bpm > 0);

        // Speed/pitch start from Electron's saved practice values; changes here stay in this edit.
        appliedSpeed = 1;
        speedSlider.setValue (100 * Metronome::clampSpeed (edit->state.getProperty (speedProp, songMeta.practiceSpeed)), dontSendNotification);
        pitchSlider.setValue (Metronome::clampSemitones (edit->state.getProperty (pitchProp, songMeta.transposeSemitones)), dontSendNotification);
        speedSlider.setEnabled (song != nullptr);
        pitchSlider.setEnabled (song != nullptr);
        refreshClickTrack(); // also applies the speed/pitch to the new click clip
        applyPractice();

        edit->playInStopEnabled = true;
        edit->getTransport().addChangeListener (this);

        assignInputsToTracks();
        save();

        editComponent = std::make_unique<EditComponent> (*edit, selectionManager);
        editComponent->getEditViewState().showFooters = true; // footer = plugin chain with "+" button
        editComponent->getEditViewState().showWaveDevices = true;
        addAndMakeVisible (*editComponent);
        fitViewToSong();
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
        // The plan is in song time; the edit runs at song time / speed.
        auto clicks = Metronome::planCountIn (songMeta, t.getPosition().inSeconds() * speed(), bars);
        for (auto& c : clicks)
            c.time /= speed();

        if (countIn != nullptr && ! clicks.empty())
        {
            countInFrom = t.getPosition().inSeconds();
            countIn->arm (clicks, countInFrom, clickSound());

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

    double speed() const { return Metronome::clampSpeed (speedSlider.getValue() / 100.0); }
    int semitones() const { return Metronome::clampSemitones (pitchSlider.getValue()); }

    void applyPractice()
    {
        if (edit == nullptr)
            return;

        edit->state.setProperty (speedProp, speed(), nullptr);
        edit->state.setProperty (pitchProp, semitones(), nullptr);

        // Keep the same spot in the song when the speed (and so the edit timeline) changes.
        auto& t = edit->getTransport();
        const double songPos = t.getPosition().inSeconds() * appliedSpeed;
        SongEdit::setPractice (*edit, speed(), semitones());
        t.setPosition (te::TimePosition::fromSeconds (songPos / speed()));
        appliedSpeed = speed();
        fitViewToSong(); // the song got longer/shorter
        refreshButtons();
    }

    // The example timeline shows a fixed 0–60 s and never scrolls, so the playhead vanished past
    // the first minute. Show the whole song instead (empty project keeps 60 s).
    void fitViewToSong()
    {
        if (editComponent == nullptr)
            return;

        auto& v = editComponent->getEditViewState();
        v.viewX1 = te::TimePosition();
        v.viewX2 = te::TimePosition::fromSeconds (jmax (60.0, edit->getLength().inSeconds() * 1.02));
    }

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

        // Like Electron: no recording over a stretched/transposed song (the take wouldn't line up).
        recordButton.setEnabled (t.isRecording() || (speed() == 1.0 && semitones() == 0));
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
