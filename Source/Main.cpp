#include <JuceHeader.h>
#include "common/Utilities.h"
#include "common/Components.h"
#include "common/PluginWindow.h"
#include "SongEdit.h"
#include "MixerPanel.h"
#include "CountInPlugin.h"
#include "PythonWorker.h"
#include "SongMeta.h"
#include "Exporter.h"
#include "SongInfo.h"

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
        Helpers::addAndMakeVisible (*this, { &songsButton, &importButton, &exportButton, &audioButton, &pluginsButton, &playButton, &recordButton,
                                             &newTrackButton, &deleteButton, &folderButton, &startButton, &chordDisplay, &countInBox, &soundBox, &speedLabel, &speedSlider, &pitchLabel, &pitchSlider,
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

        countInBox.addItemList (StringArray (String ("Sin count-in"), String (u8"Count-in 1 compás"), String ("Count-in 2 compases")), 1);

        soundBox.addItemList (Metronome::soundNames, 1);
        soundBox.setTooltip (String (u8"Sonido del metrónomo y del count-in"));
        soundBox.setSelectedItemIndex (settings.getIntValue ("clickSound", 0), dontSendNotification);
        soundBox.onChange = [this]
        {
            settings.setValue ("clickSound", soundBox.getSelectedItemIndex());
            refreshClickTrack();
        };
        countInBox.onChange = [this] { if (edit != nullptr) edit->state.setProperty (countInProp, countInBox.getSelectedId() - 1, nullptr); };

        songsButton.onClick = [this] { showSongsMenu(); };
        importButton.onClick = [this] { if (importing != nullptr) importing->signalThreadShouldExit(); else showImportMenu(); };
        exportButton.onClick = [this] { showExportMenu(); };
        startButton.setTooltip (String (u8"Ir a donde empieza la música"));
        startButton.onClick = [this] { goToMusicStart(); };
        chordDisplay.songTime = [this] { return edit != nullptr ? edit->getTransport().getPosition().inSeconds() * speed() : 0.0; };
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
        exporter = nullptr;
        importing = nullptr; // SIGTERMs separate.py so Demucs and its temp files don't outlive the app
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
        const int w = 92;
        for (auto* b : { &songsButton, &importButton, &exportButton, &audioButton, &pluginsButton, &playButton, &recordButton, &newTrackButton, &deleteButton, &folderButton })
            b->setBounds (top.removeFromLeft (w).reduced (2));
        auto row = r.removeFromTop (30);
        startButton.setBounds (row.removeFromLeft (70).reduced (2));
        countInBox.setBounds (row.removeFromLeft (180).reduced (2));
        soundBox.setBounds (row.removeFromLeft (120).reduced (2));
        speedLabel.setBounds (row.removeFromLeft (70));
        speedSlider.setBounds (row.removeFromLeft (260));
        pitchLabel.setBounds (row.removeFromLeft (40));
        pitchSlider.setBounds (row.removeFromLeft (260));
        chordDisplay.setBounds (row.reduced (8, 1));
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

    TextButton songsButton { "Canciones" }, importButton { "Importar" }, exportButton { "Exportar" }, audioButton { "Audio" }, pluginsButton { "Plugins" }, playButton { "Play" },
               recordButton { "Grabar" }, newTrackButton { "+ Pista" }, deleteButton { "Borrar" },
               folderButton { "Carpeta" };
    Label status;
    ComboBox countInBox, soundBox;
    TextButton startButton { String (u8"⏮ Inicio") };
    SongInfo::ChordDisplay chordDisplay;
    double musicStart = 0; // song time where the first sound is

    static inline const Identifier musicStartProp { "velodyMusicStart" }; // cached: decoding stems takes a moment

    void goToMusicStart() { edit->getTransport().setPosition (te::TimePosition::fromSeconds (musicStart / speed())); }
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

    // Import runs on a background thread, one song at a time: (YouTube download →) separate.py →
    // detect_bpm.py, like Electron's url:import + JobManager + analysis.
    // shortcut: no queue and no bitrate choice (separate.py's default), unlike Electron's import screen.
    struct ImportThread : Thread
    {
        ImportThread (MainComponent& o, File f, String link)
            : Thread ("Importar"), owner (&o), audio (std::move (f)), url (std::move (link)) {}
        ~ImportThread() override { stopThread (15000); }

        void run() override
        {
            auto stop = [this] { return threadShouldExit(); };
            File downloadDir;

            if (url.isNotEmpty())
            {
                status (String (u8"Descargando…"));
                auto dl = PythonWorker::download (PythonWorker::downloadArgs (url), PythonWorker::updateYtDlpArgs(),
                                                  [this] (const String& title, int pct) { status ("Descargando " + title + String (u8"…  ") + String (pct) + " %"); },
                                                  [this] { status (String (u8"YouTube bloqueó la descarga; actualizando yt-dlp…")); }, stop);
                if (! dl.ok)
                    return finish (dl, url);

                audio = File (dl.path);
                downloadDir = audio.getParentDirectory(); // download.py's velody_dl_* temp dir
            }

            const auto name = audio.getFileNameWithoutExtension();
            status ("Separando " + name + String (u8"…"));
            auto outcome = PythonWorker::run (PythonWorker::separateArgs (audio, Library::defaultRoot()),
                                              [this, name] (int pct) { status ("Separando " + name + String (u8"…  ") + String (pct) + " %"); }, stop);
            if (outcome.ok && ! threadShouldExit())
                analyse (name);

            if (downloadDir.getFileName().startsWith ("velody_dl_"))
                downloadDir.deleteRecursively();

            finish (outcome, name);
        }

        // Like Electron after a separation: BPM/beat grid/downbeat/chords into .velody, so the new
        // song has a metronome and count-in. A failed detection isn't saved (it can be retried).
        void analyse (const String& name)
        {
            status (String (u8"Analizando tempo y acordes…"));
            for (auto& song : Library::scan (Library::defaultRoot()))
                if (song.name == name)
                    if (auto stem = SongMeta::pickBeatTrackingStem (song.stems); stem.existsAsFile())
                        if (auto patch = SongMeta::analysisPatch (PythonWorker::lastLine (PythonWorker::detectBpmArgs (stem),
                                                                                         [this] { return threadShouldExit(); })))
                            SongMeta::update (song.dir, *patch);
        }

        void status (const String& text) { post ([text] (MainComponent& m) { m.status.setText (text, dontSendNotification); }); }
        void finish (const PythonWorker::Outcome& o, const String& name) { post ([o, name] (MainComponent& m) { m.importFinished (o, name); }); }

        void post (std::function<void (MainComponent&)> fn)
        {
            MessageManager::callAsync ([o = owner, fn] { if (o != nullptr) fn (*o); });
        }

        Component::SafePointer<MainComponent> owner;
        File audio;
        String url;
    };

    std::unique_ptr<ImportThread> importing;
    std::unique_ptr<FileChooser> chooser;

    void startImport (const File& f, const String& url)
    {
        importing = std::make_unique<ImportThread> (*this, f, url);
        importing->startThread();
        importButton.setButtonText ("Cancelar");
    }

    void showImportMenu()
    {
        PopupMenu m;
        m.addItem (String (u8"Archivo…"), [this] { chooseFileToImport(); });
        m.addItem (String (u8"Desde YouTube…"), [this] { askForUrl(); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (importButton));
    }

    void chooseFileToImport()
    {
        chooser = std::make_unique<FileChooser> (String (u8"Canción para separar"), File::getSpecialLocation (File::userMusicDirectory),
                                                 "*.wav;*.mp3;*.flac;*.m4a;*.ogg");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            if (auto f = fc.getResult(); f.existsAsFile())
                startImport (f, {});
        });
    }

    std::unique_ptr<AlertWindow> urlDialog;

    void askForUrl()
    {
        urlDialog = std::make_unique<AlertWindow> ("Importar desde YouTube", "Pega el enlace del video:", MessageBoxIconType::NoIcon);
        urlDialog->addTextEditor ("url", SystemClipboard::getTextFromClipboard().trim().startsWith ("http")
                                             ? SystemClipboard::getTextFromClipboard().trim() : String(), {});
        urlDialog->addButton ("Importar", 1, KeyPress (KeyPress::returnKey));
        urlDialog->addButton ("Cancelar", 0, KeyPress (KeyPress::escapeKey));
        urlDialog->enterModalState (true, ModalCallbackFunction::create ([this] (int result)
        {
            const auto url = urlDialog->getTextEditorContents ("url").trim();
            urlDialog = nullptr;
            if (result != 1)
                return;

            if (PythonWorker::isDownloadableUrl (url))
                startImport ({}, url);
            else
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, String (u8"Enlace no válido"), "Pega un enlace que empiece con https://");
        }), false);
    }

    // Stems are in the shared library now (Electron sees the song too); open it here.
    void importFinished (const PythonWorker::Outcome& o, const String& name)
    {
        importing = nullptr;
        importButton.setButtonText ("Importar");
        refreshButtons();

        if (! o.ok)
        {
            if (! o.cancelled)
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "No se pudo importar " + name, o.error);
            return;
        }

        songs = Library::scan (Library::defaultRoot());
        for (auto& song : songs)
            if (song.name == name)
                return openEdit (projectFile (song.name), &song);
    }

    std::unique_ptr<Exporter> exporter;

    String songTitle() const { return currentSong.has_value() ? currentSong->name : currentEditFile.getFileNameWithoutExtension(); }

    void showExportMenu()
    {
        PopupMenu m;
        m.addSectionHeader (String (u8"Se exporta lo que escuchas (volumen, mute, velocidad, metrónomo si no está en mute)"));
        m.addItem ("Mezcla (WAV)", [this] { chooseMixFile (Exporter::Format::wav); });
        m.addItem ("Mezcla (MP3)", [this] { chooseMixFile (Exporter::Format::mp3); });
        m.addItem ("Cada pista por separado (WAV)", [this] { chooseStemsFolder (Exporter::Format::wav); });
        m.addItem ("Cada pista por separado (MP3)", [this] { chooseStemsFolder (Exporter::Format::mp3); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (exportButton));
    }

    static String ext (Exporter::Format f) { return f == Exporter::Format::mp3 ? ".mp3" : ".wav"; }

    void chooseMixFile (Exporter::Format f)
    {
        auto suggested = File::getSpecialLocation (File::userMusicDirectory).getChildFile (File::createLegalFileName (songTitle()) + ext (f));
        chooser = std::make_unique<FileChooser> ("Guardar mezcla", suggested, "*" + ext (f));
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                              [this, f] (const FileChooser& fc)
        {
            if (auto out = fc.getResult(); out != File())
            {
                Array<Exporter::Item> items;
                items.add ({ {}, out.withFileExtension (ext (f)) }); // empty track set = the whole mix
                startExport (items, f);
            }
        });
    }

    void chooseStemsFolder (Exporter::Format f)
    {
        chooser = std::make_unique<FileChooser> ("Carpeta para las pistas", File::getSpecialLocation (File::userMusicDirectory));
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories, [this, f] (const FileChooser& fc)
        {
            auto dir = fc.getResult();
            if (! dir.isDirectory())
                return;

            Array<Exporter::Item> items;
            for (auto t : Exporter::exportableTracks (*edit))
                items.add (Exporter::stemItem (*edit, *t, dir.getChildFile (File::createLegalFileName (songTitle() + " - " + t->getName()) + ext (f))));
            startExport (items, f);
        });
    }

    void startExport (Array<Exporter::Item> items, Exporter::Format f)
    {
        if (items.isEmpty())
            return;

        edit->getTransport().stop (false, false);
        exportButton.setEnabled (false);
        const auto first = items.getFirst().out;
        exporter = std::make_unique<Exporter> (*edit, items, f,
            [this] (int done, int total) { status.setText ("Exportando " + String (done + 1) + " de " + String (total) + String (u8"…"), dontSendNotification); },
            [this, first] (const String& error)
            {
                // Destroy the exporter after its callback has returned.
                MessageManager::callAsync ([sp = Component::SafePointer<MainComponent> (this), error, first]
                {
                    if (sp == nullptr) return;
                    sp->exporter = nullptr;
                    sp->exportButton.setEnabled (true);
                    sp->refreshButtons();
                    if (error.isNotEmpty())
                        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "No se pudo exportar", error);
                    else
                        first.revealToUser();
                });
            });
        exporter->start();
    }

    void save() { te::EditFileOperations (*edit).save (true, true, false); }

    // A new song edit gets one track per stem; reopening keeps the user's edit (mutes, plugins, takes).
    void openEdit (const File& f, const Library::Song* song)
    {
        exporter = nullptr; // cancels a render of the edit we're about to close
        exportButton.setEnabled (true);
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

        // Like Electron: start where the music starts (leading silence skipped) unless the cursor was moved.
        musicStart = 0;
        if (song != nullptr)
        {
            if (! edit->state.hasProperty (musicStartProp))
                edit->state.setProperty (musicStartProp, SongInfo::findMusicStart (song->stems, engine.getAudioFileFormatManager().readFormatManager), nullptr);
            musicStart = edit->state[musicStartProp];
            if (edit->getTransport().getPosition() == te::TimePosition())
                goToMusicStart();
        }
        startButton.setEnabled (song != nullptr);
        chordDisplay.setChords (songMeta.chords);

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
        m.addItem (-1, String (u8"Proyecto vacío"));
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
        v.viewX2 = te::TimePosition::fromSeconds (jmax (60.0, SongEdit::contentLength (*edit) * 1.02));
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
        if (importing == nullptr && exporter == nullptr) // while importing/exporting, the status shows the progress instead
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
    const String getApplicationVersion() override { return ProjectInfo::versionString; }

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
