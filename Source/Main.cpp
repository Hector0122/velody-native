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
#include "SongDelete.h"
#include "PracticeMeta.h"
#include "Trim.h"
#include "CloudUi.h"
#include "ImportQueue.h"
#include "Vibes.h"
#include "LibraryAnalysis.h"

// Project files live in ~/Documents/Velody and recordings land next to them as WAV,
// so the Electron library is only ever read, never written.
static File projectsDir() { return File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Velody"); }

static File projectFile (const String& name = "Proyecto")
{
    projectsDir().createDirectory();
    return projectsDir().getChildFile (File::createLegalFileName (name) + ".tracktionedit");
}

class MainComponent : public Component,
                      public FileDragAndDropTarget,
                      private ChangeListener
{
public:
    MainComponent()
    {
        Helpers::addAndMakeVisible (*this, { &songsButton, &importButton, &exportButton, &audioButton, &pluginsButton, &playButton, &recordButton,
                                             &newTrackButton, &deleteButton, &folderButton, &startButton, &trimButton, &chordDisplay, &countInBox, &soundBox, &speedLabel, &speedSlider, &pitchLabel, &pitchSlider,
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
            sl->onValueChange = [this] // only a user change gets here (opening sets them silently)
            {
                applyPractice();
                if (currentSong.has_value())
                    PracticeMeta::saveSpeedPitch (currentSong->dir, speed(), semitones());
            };
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
        countInBox.onChange = [this] { if (currentSong.has_value()) PracticeMeta::saveCountIn (currentSong->dir, countInBox.getSelectedId() - 1); };

        songsButton.onClick = [this] { showSongsMenu(); };
        importButton.onClick = [this] { showImportMenu(); };
        imports->onChanged = [this] { importsChanged(); };
        imports->onImported = [this] (const String& name) { songImported (name); };
        // Songs sent from Vibes: a first look shortly after start, then every 5 minutes.
        vibesTimer.fn = [this] { checkVibes (false); };
        vibesTimer.startTimer (5 * 60 * 1000);
        Timer::callAfterDelay (3000, [sp = SafePointer<MainComponent> (this)] { if (sp != nullptr) sp->checkVibes (false); });
        analysis->onAnalysed = [this] (const String& name) { songAnalysed (name); };
        analysis->onFailed = [this] (const String& name)
        {
            if (! currentSong.has_value() || currentSong->name != name)
                return;
            analysingOpenSong = false;
            status.setText (String (u8"No se pudo analizar el tempo de esta canción (sin metrónomo); se reintenta la próxima vez que abras Velody"), dontSendNotification);
        };
        Timer::callAfterDelay (5000, [sp = SafePointer<MainComponent> (this)] { if (sp != nullptr) sp->analysis->request(); });
        // First run (or a broken install): the setup window opens by itself, like Electron's SetupScreen.
        if (! Setup::ready())
            Timer::callAfterDelay (500, [sp = SafePointer<MainComponent> (this)] { if (sp != nullptr) sp->showSetup(); });
        exportButton.onClick = [this] { showExportMenu(); };
        startButton.setTooltip (String (u8"Ir a donde empieza la música"));
        startButton.onClick = [this] { goToMusicStart(); };
        trimButton.setTooltip (String (u8"Recorte: dónde empieza y termina la canción para ti (se salta una intro larga); se guarda en la canción"));
        trimButton.onClick = [this] { showTrimMenu(); };
        // The trim's end: playback stops there.
        trimTimer.fn = [this]
        {
            auto& t = edit->getTransport();
            const double pos = t.getPosition().inSeconds() * speed();
            const bool stop = Trim::shouldStop (trim, lastTrimCheck, pos, t.isPlaying(), t.isRecording());
            lastTrimCheck = pos;
            if (stop)
                stopPlayback();
        };
        trimTimer.startTimer (30);
        chordDisplay.songTime = [this] { return edit != nullptr ? edit->getTransport().getPosition().inSeconds() * speed() : 0.0; };
        audioButton.onClick = [this] { EngineHelpers::showAudioDeviceSettings (engine); };
        pluginsButton.onClick = [this] { showPluginScanner(); };
        playButton.onClick = [this] { stopOrPlay (false); };
        recordButton.onClick = [this] { stopOrPlay (true); };
        newTrackButton.onClick = [this]
        {
            edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 1);
            SongEdit::setMixPan (*edit, mixPanPreference());
            mixer.rebuild();
            resized();
        };
        mixer.onMixPanChanged = [this] (float pan) { settings.setValue ("mixPan", pan); };
        deleteButton.onClick = [this] { deleteSelection(); };
        folderButton.onClick = [] { projectFile().revealToUser(); };

        cloud.onStatus = [this] (const String& text) { status.setText (text, dontSendNotification); };
        cloud.cleanupStaleDownloads(); // a download cut off by a crash or quit

        engine.getPluginManager().createBuiltInType<CountInPlugin>(); // before any edit that has one loads
        selectionManager.addChangeListener (this);
        engine.getDeviceManager().addChangeListener (this);

        openEdit (projectFile(), nullptr);
        setSize (1000, 600);
    }

    ~MainComponent() override
    {
        exporter = nullptr;
        analysis = nullptr; // before the queue: it asks the queue whether it's busy
        imports = nullptr; // SIGTERMs separate.py so Demucs and its temp files don't outlive the app
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
        trimButton.setBounds (row.removeFromLeft (80).reduced (2));
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

    // The trim's start when there is one, else where the music starts (leading silence skipped).
    void goToMusicStart() { edit->getTransport().setPosition (te::TimePosition::fromSeconds ((trim.start > 0 ? trim.start : musicStart) / speed())); }

    TextButton trimButton { "Recorte" };
    double lastTrimCheck = 0;
    Trim::Range trim;

    void showTrimMenu()
    {
        if (! currentSong.has_value())
            return;
        const double pos = edit->getTransport().getPosition().inSeconds() * speed();
        Trim::menu (trim, pos, [this, pos] (Trim::Action a)
        {
            std::optional<Trim::Range> next;
            if (a.kind == Trim::Action::setStart) next = Trim::withStart (trim, pos);
            if (a.kind == Trim::Action::setEnd)   next = Trim::withEnd (trim, pos);
            if (a.kind == Trim::Action::clear)    next = Trim::Range();
            if (! next || ! currentSong.has_value())
                return;
            trim = *next;
            Trim::save (currentSong->dir, trim);
            refreshTrimButton();
            status.setText (Trim::describe (trim), dontSendNotification);
        }).showMenuAsync (PopupMenu::Options().withTargetComponent (trimButton));
    }

    void refreshTrimButton()
    {
        trimButton.setButtonText (trim.active() ? String (u8"Recorte ✓") : String ("Recorte"));
        trimButton.setEnabled (currentSong.has_value());
    }
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

    // Mix Pan, for every song (Electron: velody:masterPan).
    float mixPanPreference() { return jlimit (-1.0f, 1.0f, (float) settings.getDoubleValue ("mixPan", 0.0)); }

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

    // Speed, transpose and count-in live in the song's .velody (PracticeMeta): they sync with the cloud.
    Metronome::SongMeta songMeta; // bpm 0 when the open edit isn't an analyzed song
    CountInPlugin* countIn = nullptr;

    Array<Library::Song> songs;

    // Import queue (Electron's JobManager): one song at a time on a background thread.
    // shortcut: no bitrate choice (separate.py's default), unlike Electron's import screen.
    std::unique_ptr<Import::Queue> imports = std::make_unique<Import::Queue> (Import::pythonSteps (Library::defaultRoot(), [this] { return stemBitrate(); }));

    // MP3 quality of the stems, for every import (Electron: velody:stemBitrate). Read on the import thread.
    int stemBitrate() const
    {
        const int b = settings.getIntValue ("stemBitrate", 128);
        return PythonWorker::stemBitrates.contains (b) ? b : 128;
    }
    std::unique_ptr<FileChooser> chooser;

    void showImportMenu()
    {
        Import::menu (imports->snapshot(), [this] (const Import::MenuAction& a)
        {
            using A = Import::MenuAction;
            switch (a.kind)
            {
                case A::addFiles:      return chooseFilesToImport();
                case A::addUrl:        return askForUrl();
                case A::fetchVibes:    return checkVibes (true);
                case A::vibesSettings: return showVibesSettings();
                case A::setup:         return showSetup();
                case A::setBitrate:    settings.setValue ("stemBitrate", a.bitrate); return;
                case A::cancel:        imports->cancel (a.job.id); return;
                case A::clearFinished: imports->clearFinished(); return;
                case A::showError:
                    AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "No se pudo importar " + a.job.name, a.job.error);
                    return;
            }
        }, stemBitrate()).showMenuAsync (PopupMenu::Options().withTargetComponent (importButton));
    }

    Vibes::Poller vibes { *imports, Vibes::settingsFile(), File::getSpecialLocation (File::tempDirectory) };

    // Songs without a current analysis get one in the background (metronome, count-in, chords).
    std::unique_ptr<LibraryAnalysis::Analyzer> analysis = std::make_unique<LibraryAnalysis::Analyzer> (
        Library::defaultRoot(), LibraryAnalysis::pythonDetect(),
        [] { return Setup::ready(); },
        [q = imports.get()] { return q->pending() > 0; },
        [] { return Setup::hasPackage (Setup::defaultPaths().venv(), Setup::optionalPackage); });

    // The open song just got its analysis: metronome, count-in and chords without reopening it.
    bool analysingOpenSong = false; // the status bar says so until it's done

    void songAnalysed (const String& name)
    {
        if (! currentSong.has_value() || currentSong->name != name)
            return;
        analysingOpenSong = false;
        if (edit->getTransport().isPlaying() || edit->getTransport().isRecording())
            return status.setText (String (u8"Análisis listo: el metrónomo aparece al detener y volver a abrir la canción"), dontSendNotification);

        songMeta = Metronome::readMeta (currentSong->dir);
        countInBox.setEnabled (songMeta.bpm > 0);
        chordDisplay.setChords (songMeta.chords);
        refreshClickTrack();
        status.setText (String (u8"Análisis listo: metrónomo, count-in y acordes"), dontSendNotification);
    }
    struct FnTimer : Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } vibesTimer, trimTimer;

    // `manual` ("Traer de Vibes"): says what happened; the automatic checks stay quiet when offline.
    void checkVibes (bool manual)
    {
        if (! manual && ! vibes.active())
            return;
        vibes.check ([this, manual] (int added, const String& error)
        {
            if (error.isNotEmpty())
            {
                if (manual)
                    AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Vibes", error);
                return;
            }
            if (added > 0)
                status.setText (added == 1 ? String (u8"1 canción de Vibes en la cola") : String (added) + String (u8" canciones de Vibes en la cola"), dontSendNotification);
            else if (manual)
                status.setText ("Nada nuevo desde Vibes", dontSendNotification);
        });
    }

    void showSetup()
    {
        DialogWindow::LaunchOptions o;
        o.dialogTitle = String (u8"Instalación");
        o.dialogBackgroundColour = getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.content.setOwned (new Setup::Panel (Setup::defaultPaths(), Setup::systemEnv(),
                                              [sp = SafePointer<MainComponent> (this)] { if (sp != nullptr && Setup::ready()) sp->analysis->request(); }));
        o.launchAsync();
    }

    void showVibesSettings()
    {
        DialogWindow::LaunchOptions o;
        o.dialogTitle = "Vibes";
        o.dialogBackgroundColour = getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        CloudUi::SettingsTexts t;
        t.intro = String (u8"Las canciones que mandes desde Vibes («Mandar a Velody», en la compu o el celular) se separan aquí solas. "
                          u8"Usa la misma URL y token que en Vibes.");
        t.enable = "Traer canciones de Vibes";
        t.placeholder = "https://vibes.<tu-usuario>.workers.dev";
        o.content.setOwned (new CloudUi::SettingsPanel (Vibes::settingsFile(),
                                                        [sp = SafePointer<MainComponent> (this)] (const Cloud::Settings& s)
                                                        {
                                                            if (sp != nullptr && s.active())
                                                                sp->checkVibes (false);
                                                        }, t));
        o.launchAsync();
    }

    void chooseFilesToImport()
    {
        chooser = std::make_unique<FileChooser> (String (u8"Canciones para separar"), File::getSpecialLocation (File::userMusicDirectory),
                                                 "*" + Import::audioExtensions.joinIntoString (";*"));
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                              [this] (const FileChooser& fc)
        {
            for (auto& f : fc.getResults())
                if (Import::isImportableFile (f))
                    imports->addFile (f);
        });
    }

    // Audio files dropped on the window join the queue.
    bool isInterestedInFileDrag (const StringArray& files) override
    {
        for (auto& f : files)
            if (Import::audioExtensions.contains (File (f).getFileExtension().toLowerCase()))
                return true;
        return false;
    }

    void filesDropped (const StringArray& files, int, int) override
    {
        for (auto& f : files)
            if (Import::isImportableFile (File (f)))
                imports->addFile (File (f));
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
                imports->addUrl (url);
            else
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, String (u8"Enlace no válido"), "Pega un enlace que empiece con https://");
        }), false);
    }

    void importsChanged()
    {
        const int n = imports->pending();
        importButton.setButtonText (n > 0 ? "Importar (" + String (n) + ")" : String ("Importar"));
        if (auto text = imports->statusText(); text.isNotEmpty())
            status.setText (text, dontSendNotification);
        else
            refreshButtons();
    }

    // Stems are in the shared library now (Electron sees the song too). It opens here only when
    // nothing else is open, so a queue finishing doesn't pull the user out of the song they practise.
    void songImported (const String& name)
    {
        if (currentSong.has_value() || edit->getTransport().isPlaying() || edit->getTransport().isRecording())
            return;
        for (auto& song : Library::scan (Library::defaultRoot()))
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
            },
            // The song's trim: the export covers what plays.
            trim.active() ? std::optional (Trim::renderRange (trim, SongEdit::contentLength (*edit), speed())) : std::nullopt);
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
        // Practice values from the .velody (an older edit-only value is moved there once).
        const auto practice = song != nullptr ? PracticeMeta::load (song->dir, edit->state) : PracticeMeta::Values();
        // shortcut: the selector offers 0–2 bars; a longer count-in saved by Electron shows as 2 (kept in .velody until changed here).
        countInBox.setSelectedId (jlimit (0, 2, practice.countInBars) + 1, dontSendNotification);
        countInBox.setEnabled (songMeta.bpm > 0);

        appliedSpeed = 1;
        speedSlider.setValue (100 * practice.speed, dontSendNotification);
        pitchSlider.setValue (practice.semitones, dontSendNotification);
        speedSlider.setEnabled (song != nullptr);
        pitchSlider.setEnabled (song != nullptr);
        refreshClickTrack(); // also applies the speed/pitch to the new click clip
        applyPractice();

        // Like Electron: start where the music starts (leading silence skipped), or at the trim's start,
        // unless the cursor was moved.
        trim = song != nullptr ? Trim::load (song->dir) : Trim::Range();
        refreshTrimButton();
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
        analysingOpenSong = song != nullptr && Setup::ready() && analysis->needs (song->dir);
        if (analysingOpenSong)
            analysis->prioritize (song->name);
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
        SongEdit::setMixPan (*edit, mixPanPreference());
        mixer.setEdit (edit.get());
        resized();
        refreshButtons();
    }

    CloudUi::Controller cloud { Library::defaultRoot(), Cloud::Settings::file() };
    bool listingSongs = false;

    // With the cloud on, the list comes from the cloud first (quick timeout; offline shows local songs).
    // During a transfer it's the local list only, so other songs can still be opened.
    void showSongsMenu()
    {
        if (listingSongs)
            return;
        if (cloud.busy() || ! cloud.active())
            return showSongsMenu (cloud.localListing());

        listingSongs = true;
        status.setText (String (u8"Consultando la nube…"), dontSendNotification);
        cloud.list ([this] (const CloudUi::Listing& l)
        {
            listingSongs = false;
            refreshButtons();
            showSongsMenu (l);
        });
    }

    void showSongsMenu (const CloudUi::Listing& l)
    {
        listing = l;
        auto m = CloudUi::songsMenu (l, currentSong.has_value() ? currentSong->name : String(), cloud.busy(), imports->pending() > 0,
                                     Library::defaultRoot(), [this] (const CloudUi::Action& a) { songAction (a); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (songsButton));
    }

    CloudUi::Listing listing;

    void songAction (const CloudUi::Action& a)
    {
        using A = CloudUi::Action;
        switch (a.kind)
        {
            case A::openEmpty:       return openEdit (projectFile(), nullptr);
            case A::open:            return openSong (a.entry);
            case A::download:        return downloadAndOpen (a.entry);
            case A::upload:          return uploadSong (a.entry.name);
            case A::uploadAll:       return uploadAllLocal();
            case A::removeCurrent:   return confirmRemoveCurrentSong (a.entry);
            case A::deleteFromCloud: return confirmDeleteCloudOnly (a.entry);
            case A::settings:        return showCloudSettings();
        }
    }

    // A synced song gets the newer analysis (bpm, downbeat, chords) from the cloud before it opens.
    void openSong (const Cloud::Entry& e)
    {
        if (! e.local)
            return;
        cloud.syncMetaOnOpen (e, [this, song = *e.local]
        {
            refreshButtons();
            openEdit (projectFile (song.name), &song);
        });
    }

    void cloudFailed (const String& title, const String& error)
    {
        refreshButtons();
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, title, error);
    }

    void downloadAndOpen (const Cloud::Entry& e)
    {
        cloud.download (e, [this, name = e.name] (const File&, const String& error)
        {
            if (error.isNotEmpty())
                return cloudFailed ("No se pudo descargar " + name, error);
            refreshButtons();
            for (auto& song : Library::scan (Library::defaultRoot()))
                if (song.name == name)
                    return openEdit (projectFile (song.name), &song);
        });
    }

    void uploadSong (const String& name)
    {
        cloud.upload (name, [this, name] (const String& error)
        {
            if (error.isNotEmpty())
                return cloudFailed ("No se pudo subir " + name, error);
            refreshButtons();
            status.setText (String (u8"Subida a la nube: ") + name, dontSendNotification);
        });
    }

    void uploadAllLocal()
    {
        StringArray names;
        for (auto& e : listing.songs)
            if (e.state == Cloud::State::local)
                names.add (e.name);
        cloud.uploadAll (names, [this] (int uploaded, const StringArray& failed)
        {
            refreshButtons();
            if (! failed.isEmpty())
                return AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Se subieron " + String (uploaded) + String (u8"; fallaron:"),
                                                         failed.joinIntoString ("\n"));
            status.setText ("Subidas a la nube: " + String (uploaded) + " canciones", dontSendNotification);
        });
    }

    // In the cloud too: free space here (it stays in the cloud) or delete everywhere.
    void confirmRemoveCurrentSong (const Cloud::Entry& e)
    {
        if (e.state != Cloud::State::synced || ! listing.enabled || listing.offline)
            return confirmDeleteCurrentSong (false);

        AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::QuestionIcon)
                                    .withTitle (String (u8"¿Quitar «") + e.name + String (u8"»?"))
                                    .withMessage (String (u8"Esta canción está en esta laptop y en la nube.\n\n"
                                                          u8"Quitar de esta laptop: libera espacio; sigue en la nube y se descarga otra vez al abrirla "
                                                          u8"(tu proyecto aquí se conserva).\n\nBorrar en todas partes: de esta laptop y de la nube."))
                                    .withButton ("Quitar de esta laptop", 1)
                                    .withButton ("Borrar en todas partes", 2)
                                    .withButton ("Cancelar", 0),
                                [this, e] (int r)
                                {
                                    if (r == 1)
                                        removeLocalCopy (e);
                                    else if (r == 2)
                                        confirmDeleteCurrentSong (true, e);
                                });
    }

    void removeLocalCopy (const Cloud::Entry& e)
    {
        openEdit (projectFile(), nullptr); // close the song first: its folder is about to move
        cloud.removeLocal (e, [] (const File& f) { return f.moveToTrash(); }, [this, name = e.name] (const String& error)
        {
            if (error.isNotEmpty())
                return cloudFailed ("No se pudo quitar " + name, error);
            refreshButtons();
            status.setText (String (u8"Quitada de esta laptop (sigue en la nube): ") + name, dontSendNotification);
        });
    }

    void confirmDeleteCloudOnly (const Cloud::Entry& e)
    {
        AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::WarningIcon)
                                    .withTitle (String (u8"¿Borrar «") + e.name + String (u8"» de la nube?"))
                                    .withMessage (String (u8"Solo está en la nube: se borra para siempre, no se puede deshacer."))
                                    .withButton ("Borrar", 1)
                                    .withButton ("Cancelar", 0),
                                [this, e] (int r)
                                {
                                    if (r != 1)
                                        return;
                                    cloud.deleteFromCloud (e, [this, name = e.name] (const String& error)
                                    {
                                        if (error.isNotEmpty())
                                            return cloudFailed ("No se pudo borrar " + name, error);
                                        refreshButtons();
                                        status.setText ("Borrada de la nube: " + name, dontSendNotification);
                                    });
                                });
    }

    void showCloudSettings()
    {
        DialogWindow::LaunchOptions o;
        o.dialogTitle = "Nube";
        o.dialogBackgroundColour = getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.content.setOwned (new CloudUi::SettingsPanel (Cloud::Settings::file(), [] (const Cloud::Settings&) {}));
        o.launchAsync();
    }

    // `everywhere`: the cloud copy goes first; the local one only once that worked.
    void confirmDeleteCurrentSong (bool everywhere, const Cloud::Entry& entry = {})
    {
        if (! currentSong.has_value())
            return;

        const auto plan = SongDelete::plan (currentSong->dir, currentEditFile, SongDelete::takesIn (*edit, projectsDir()));
        const auto n = plan.takes.size();

        auto opts = MessageBoxOptions().withIconType (MessageBoxIconType::QuestionIcon)
                                       .withTitle ((everywhere ? String (u8"¿Borrar «") : String (u8"¿Quitar «")) + currentSong->name
                                                   + (everywhere ? String (u8"» de la nube y de esta laptop?") : String (u8"»?")))
                                       .withMessage ((everywhere ? String (u8"Se borra de la nube para siempre. ") : String())
                                                     + String (u8"Se manda a la papelera: los stems y el análisis de la canción, y su proyecto aquí "
                                                             u8"(volúmenes, plugins, metrónomo). Puedes recuperarlos desde la papelera.")
                                                     + (n > 0 ? "\n\nTienes " + String (n) + (n == 1 ? String (u8" grabación") : String (u8" grabaciones")) + " en esta canción." : String()));
        if (n > 0)
            opts = opts.withButton ("Quitar y conservar grabaciones", 1).withButton ("Quitar con mis grabaciones", 2).withButton ("Cancelar", 0);
        else
            opts = opts.withButton ("Quitar", 1).withButton ("Cancelar", 0);

        AlertWindow::showAsync (opts, [this, plan, n, everywhere, entry] (int result)
        {
            if (result == 0)
                return;
            const bool includeTakes = n > 0 && result == 2;
            if (! everywhere)
                return deleteSong (plan, includeTakes);

            cloud.deleteFromCloud (entry, [this, plan, includeTakes, name = entry.name] (const String& error)
            {
                if (error.isNotEmpty())
                    return cloudFailed ("No se pudo borrar " + name, error + String (u8"\n\nNo se borró nada de esta laptop."));
                refreshButtons();
                deleteSong (plan, includeTakes);
            });
        });
    }

    void deleteSong (const SongDelete::Plan& plan, bool includeTakes)
    {
        openEdit (projectFile(), nullptr); // close the song first: its files are about to move

        auto r = SongDelete::execute (plan, includeTakes, Library::defaultRoot(), projectsDir());
        if (r.failed())
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "No se pudo quitar la canción", r.getErrorMessage());
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

        SongEdit::setMixPan (*edit, mixPanPreference()); // input tracks created just now get it too
        mixer.rebuild();
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
        if (analysingOpenSong && imports->pending() == 0 && exporter == nullptr && ! cloud.busy())
            status.setText (String (u8"Analizando tempo y acordes de esta canción…"), dontSendNotification);
        else if (imports->pending() == 0 && exporter == nullptr && ! cloud.busy()) // while importing/exporting/syncing, the status shows the progress instead
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
