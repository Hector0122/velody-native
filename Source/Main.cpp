#include <JuceHeader.h>
#include "common/Utilities.h"
#include "Theme.h"
#include "ui/Components.h"
#include "ui/TransportButton.h"
#include "ui/Brand.h"
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
#include "RecordCountIn.h"
#include "SongSearch.h"
#include "Updater.h"
#include "PluginPaths.h"
#include "Projects.h"
#include "LiveMonitoring.h"
#include "Instruments.h"
#include "Mixing.h"
#include "PluginPicker.h"
#include "AppSettings.h"
#include "ui/PianoRoll.h"
#include "ui/CountInOverlay.h"
#include "SongKey.h"
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
        Helpers::addAndMakeVisible (*this, { &wordmark, &songTitleLabel, &songInfo, &songsButton, &projectsButton, &importButton, &exportButton, &settingsButton,
                                             &startButton, &playButton, &recordButton, &timeLabel, &chordDisplay, &trimButton, &loopButton, &clickButton, &newTrackButton, &deleteButton,
                                             &countInBox, &recordCountInBox, &soundBox, &speedLabel, &speedDown, &speedValue, &speedUp, &pitchLabel, &pitchDown, &pitchValue, &pitchUp,
                                             &mixerToggle, &automateButton, &clearAutomationButton, &status, &mixer });

        // Header: wordmark, the open song (title + BPM/trim/speed), the menus.
        songTitleLabel.setFont (Theme::title (17.0f));
        songInfo.setFont (Theme::mono (12.0f));
        songInfo.setColour (Label::textColourId, Theme::muted);
        settingsButton.onClick = [this] { showSettingsMenu(); };
        projectsButton.onClick = [this] { showProjectsMenu(); };
        automateButton.setClickingTogglesState (true);
        automateButton.setColour (TextButton::buttonOnColourId, Theme::record.darker (0.2f));
        automateButton.setTooltip (String (u8"Con esto prendido, lo que muevas en el mezclador mientras suena se graba como automatización"));
        automateButton.onClick = [this] { Mixing::setWriting (*edit, automateButton.getToggleState()); };
        clearAutomationButton.setTooltip (String (u8"Quita la automatización de la pista seleccionada"));
        clearAutomationButton.onClick = [this]
        {
            auto* t = dynamic_cast<te::AudioTrack*> (selectionManager.getSelectedObject (0));
            if (t == nullptr)
                return status.setText (String (u8"Selecciona una pista (clic en su nombre) para borrar su automatización"), dontSendNotification);
            edit->getUndoManager().beginNewTransaction();
            const int n = Mixing::clearAutomation (*t);
            status.setText (n > 0 ? String (u8"Automatización borrada: ") + t->getName() : t->getName() + String (u8" no tiene automatización"), dontSendNotification);
            if (editComponent != nullptr) editComponent->repaint();
        };
        loopButton.setTooltip (String (u8"Repetir una parte: para practicarla, o para grabar varias tomas seguidas (L)"));
        loopButton.setColour (TextButton::buttonOnColourId, Theme::accent);
        loopButton.onClick = [this] { showLoopMenu(); };
        clickButton.setClickingTogglesState (true);
        clickButton.setTooltip (String (u8"Metrónomo del proyecto (al tempo del proyecto)"));
        clickButton.onClick = [this] { if (edit != nullptr) edit->clickTrackEnabled = clickButton.getToggleState(); };
        timeLabel.setFont (Theme::mono (22.0f));
        timeLabel.setJustificationType (Justification::centred);
        status.setFont (Theme::mono (12.0f));
        status.setColour (Label::textColourId, Theme::muted);
        for (auto* l : { &speedLabel, &pitchLabel })
        {
            l->setFont (Theme::body (12.0f));
            l->setColour (Label::textColourId, Theme::muted);
            l->setJustificationType (Justification::centredRight);
        }
        playButton.setTooltip ("Play / Stop (barra espaciadora)");
        recordButton.setTooltip ("Grabar / Detener");
        mixerOpen = settings.getBoolValue ("mixerOpen", true);
        mixerToggle.onClick = [this]
        {
            mixerOpen = ! mixerOpen;
            settings.setValue ("mixerOpen", mixerOpen);
            refreshMixerToggle();
            // Folds/unfolds smoothly: the panes slide to their new place.
            auto oldEdit = editComponent != nullptr ? editComponent->getBounds() : Rectangle<int>();
            auto oldMixer = mixer.getBounds();
            resized();
            auto& anim = Desktop::getInstance().getAnimator();
            if (editComponent != nullptr)
            {
                auto target = editComponent->getBounds();
                editComponent->setBounds (oldEdit);
                anim.animateComponent (editComponent.get(), target, 1.0f, 200, false, 0.4, 0.4);
            }
            if (mixerOpen)
            {
                auto target = mixer.getBounds();
                mixer.setBounds (oldMixer.withY (getHeight()));
                mixer.setAlpha (0.0f);
                anim.animateComponent (&mixer, target, 1.0f, 220, false, 0.4, 0.4);
            }
        };
        refreshMixerToggle();

        speedLabel.setText ("Velocidad", dontSendNotification);
        pitchLabel.setText ("Tono", dontSendNotification);
        speedSlider.setRange (50, 150, 0.1);
        speedSlider.setTextValueSuffix (" %");
        speedSlider.setDoubleClickReturnValue (true, 100);
        pitchSlider.setRange (-12, 12, 1);
        pitchSlider.setTextValueSuffix (" st");
        pitchSlider.setDoubleClickReturnValue (true, 0);
        // Speed as the song's BPM and pitch as its key, with −/+ (the sliders hold the values, unseen).
        // −/+ move 1 BPM (1 % without a tempo); the value: original tempo, or type one (e.g. 120 → 110).
        speedDown.onClick = [this] { stepTempo (-1); };
        speedUp.onClick   = [this] { stepTempo (+1); };
        speedValue.onClick = [this] { showTempoMenu(); };
        pitchDown.onClick = [this] { pitchSlider.setValue (pitchSlider.getValue() - 1, sendNotificationSync); };
        pitchUp.onClick   = [this] { pitchSlider.setValue (pitchSlider.getValue() + 1, sendNotificationSync); };
        pitchValue.onClick = [this] { pitchSlider.setValue (0, sendNotificationSync); };
        speedValue.setTooltip (String (u8"Tempo de práctica: − / + de 1 en 1 BPM; clic para escribir un tempo o volver al original"));
        pitchValue.setTooltip (String (u8"Tonalidad (estimada de los acordes). Clic: tono original"));
        for (auto* b : { &speedValue, &pitchValue })
            b->setColour (TextButton::buttonColourId, Theme::lane);
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
        // Recording's own count-in, for every song (Electron: separate from the one on Play).
        recordCountInBox.addItemList (RecordCountIn::names, 1);
        recordCountInBox.setTooltip (String (u8"Clicks antes de grabar, sobre el tempo de la canción; la toma empieza donde está el cursor"));
        recordCountInBox.setSelectedItemIndex (RecordCountIn::barsOptions.indexOf (RecordCountIn::clampBars (settings.getIntValue ("recordCountInBars", RecordCountIn::defaultBars))),
                                               dontSendNotification);
        recordCountInBox.onChange = [this] { settings.setValue ("recordCountInBars", RecordCountIn::barsOptions[recordCountInBox.getSelectedItemIndex()]); };

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
        startButton.setTooltip (String (u8"Ir a donde empieza la música (o el inicio del recorte)"));
        startButton.onClick = [this] { goToMusicStart(); };
        trimButton.setTooltip (String (u8"Tramo: marca dónde empieza y dónde termina la canción para ti (para saltarte una intro larga). No corta el audio; se guarda en la canción"));
        trimButton.onClick = [this] { showTrimMenu(); };
        // The trim's end, or where the audio ends: playback stops there.
        trimTimer.fn = [this]
        {
            auto& t = edit->getTransport();
            const double pos = t.getPosition().inSeconds() * speed();
            const bool stop = Trim::shouldStopAtEnd (trim, SongEdit::contentLength (*edit) * speed(), lastTrimCheck, pos,
                                                     t.isPlaying(), t.isRecording(), t.looping.get());
            lastTrimCheck = pos;
            if (stop)
                stopPlayback();
            else if (t.isPlaying() && editComponent != nullptr)
                editComponent->followPlayhead(); // zoomed in: the view turns the page with the playhead
            // The count-in, big on screen while it plays.
            const bool counting = countIn != nullptr && countIn->isArmed() && t.isPlaying();
            countInOverlay.show (counting ? CountInDisplay::at (countInClicks, t.getPosition().inSeconds(), countInFrom) : CountInDisplay::State());
            timeLabel.setText (Trim::clock (jmax (0.0, pos)) + "." + String ((int) (jmax (0.0, pos) * 10) % 10)
                                   + (songLength > 0 ? "  /  " + Trim::clock (songLength) : String()), dontSendNotification);
        };
        trimTimer.startTimer (30);
        // Autosave: what you change (mix, plugins, edits) is on disk within 30 s, even if Velody is killed.
        autosaveTimer.fn = [this]
        {
            if (edit != nullptr && edit->hasChangedSinceSaved() && ! edit->getTransport().isRecording())
                save();
        };
        autosaveTimer.startTimer (30 * 1000);
        chordDisplay.songTime = [this] { return edit != nullptr ? edit->getTransport().getPosition().inSeconds() * speed() : 0.0; };
        playButton.onClick = [this] { stopOrPlay (false); };
        recordButton.onClick = [this] { stopOrPlay (true); };
        newTrackButton.onClick = [this] { showNewTrackMenu(); };
        VelodyUi::onEditMidiClip = [this] (te::MidiClip& c) { showPianoRoll (c); };
        VelodyUi::onAddEffect = [this] (te::Track::Ptr t) { showEffectPicker (t); };
        mixer.onMixPanChanged = [this] (float pan) { settings.setValue ("mixPan", pan); };
        deleteButton.onClick = [this] { deleteSelection(); };

        cloud.onStatus = [this] (const String& text) { status.setText (text, dontSendNotification); };
        cloud.cleanupStaleDownloads(); // a download cut off by a crash or quit

        PluginPaths::addToScanPaths (engine.getPropertyStorage().getPropertiesFile(), PluginPaths::atStart());
        // First run: JACK (PipeWire) when it's there, for low latency; the choice is then the user's.
        if (! settings.getBoolValue ("audioTypeChosen", false)
            && LiveMonitoring::preferJack (engine.getDeviceManager().deviceManager))
            settings.setValue ("audioTypeChosen", true); // until JACK could be chosen, it's tried at each start
        VelodyUi::onMonitorOn = [this]
        {
            if (settings.getBoolValue ("monitorWarned", false))
                return;
            settings.setValue ("monitorWarned", true);
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, String (u8"Monitoreo en vivo"),
                String (u8"Ahora te escuchas a través de los plugins de esta pista (pon ahí tu amp sim con el «+» del final de la pista).\n\n"
                        u8"Usa audífonos: con bocinas el micrófono o la guitarra se retroalimentan.\n"
                        u8"Para tocar cómodo baja el buffer en Ajustes → Latencia (128 o 64)."));
        };
        engine.getPluginManager().createBuiltInType<CountInPlugin>(); // before any edit that has one loads
        selectionManager.addChangeListener (this);
        engine.getDeviceManager().addChangeListener (this);

        Projects::migrateLegacy (projectsDir());
        openLast();
        setWantsKeyboardFocus (true); // Ctrl+F
        addChildComponent (countInOverlay);

        addChildComponent (updateButton);
        updateButton.setColour (TextButton::buttonColourId, Theme::success.darker (0.3f));
        updateButton.onClick = [this] { confirmUpdate(); };
        Timer::callAfterDelay (4000, [sp = SafePointer<MainComponent> (this)] { if (sp != nullptr) sp->checkForUpdate(); });
        setSize (1280, 800);
    }

    ~MainComponent() override
    {
        exporter = nullptr;
        analysis = nullptr; // before the queue: it asks the queue whether it's busy
        imports = nullptr; // SIGTERMs separate.py so Demucs and its temp files don't outlive the app
        engine.getDeviceManager().removeChangeListener (this);
        VelodyUi::onMonitorOn = nullptr;
        VelodyUi::onAddEffect = nullptr;
        VelodyUi::onEditMidiClip = nullptr;
        if (forcedQuantum)
            LiveMonitoring::releaseBuffer(); // PipeWire's buffer back to automatic for the other apps
        save();
        engine.getTemporaryFileManager().getTempDirectory().deleteRecursively();
    }

    static constexpr int headerH = 60, transportH = 76, practiceH = 44, statusH = 26, mixerBarH = 30;

    void paint (Graphics& g) override
    {
        g.fillAll (Theme::bg);
        auto r = getLocalBounds();
        auto header = r.removeFromTop (headerH);
        auto transport = r.removeFromTop (transportH + practiceH);
        auto statusBar = r.removeFromBottom (statusH);
        g.setColour (Theme::panel);
        g.fillRect (header);
        g.fillRect (transport);
        g.fillRect (statusBar);
        g.fillRect (mixerBarArea);
        g.setColour (Theme::outline);
        g.fillRect (header.removeFromBottom (1));
        g.fillRect (transport.removeFromBottom (1));
        g.fillRect (statusBar.removeFromTop (1));
        g.fillRect (mixerBarArea.withHeight (1));

        // The clock in a pill, the chords on a card.
        for (auto* c : std::initializer_list<Component*> { &timeLabel, &chordDisplay })
        {
            auto b = c->getBounds().toFloat().expanded (c == &timeLabel ? 4.0f : 0.0f, 2.0f);
            g.setColour (Theme::lane);
            g.fillRoundedRectangle (b, Theme::radius + 2);
            g.setColour (Theme::outline);
            g.drawRoundedRectangle (b, Theme::radius + 2, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();

        // Header: Velody · song · menus (· update).
        auto header = r.removeFromTop (headerH).reduced (16, 10);
        wordmark.setBounds (header.removeFromLeft (150));
        for (auto* b : { &settingsButton, &exportButton, &importButton, &projectsButton, &songsButton })
            b->setBounds (header.removeFromRight (b == &songsButton || b == &projectsButton ? 132 : 120).reduced (3, 2));
        if (updateButton.isVisible())
            updateButton.setBounds (header.removeFromRight (180).reduced (3, 2));
        auto titleArea = header.withTrimmedLeft (12);
        songTitleLabel.setBounds (titleArea.removeFromTop (titleArea.getHeight() * 11 / 20));
        songInfo.setBounds (titleArea);

        // Transport: ⏮ ▶ ● time · chords · Recorte, + Pista, Borrar.
        auto transport = r.removeFromTop (transportH).reduced (16, 8);
        startButton.setBounds (transport.removeFromLeft (44).withSizeKeepingCentre (44, 44));
        transport.removeFromLeft (8);
        playButton.setBounds (transport.removeFromLeft (60).withSizeKeepingCentre (60, 60));
        transport.removeFromLeft (8);
        recordButton.setBounds (transport.removeFromLeft (52).withSizeKeepingCentre (52, 52));
        transport.removeFromLeft (16);
        timeLabel.setBounds (transport.removeFromLeft (210).reduced (0, 6));
        transport.removeFromLeft (12);
        for (auto* b : { &deleteButton, &newTrackButton, &trimButton, &loopButton, &clickButton })
            if (b->isVisible())
                b->setBounds (transport.removeFromRight (104).withSizeKeepingCentre (98, 34));
        chordDisplay.setBounds (transport.reduced (16, 4));

        // Practice: count-ins, click sound, speed, pitch.
        auto practice = r.removeFromTop (practiceH).reduced (16, 6);
        countInBox.setBounds (practice.removeFromLeft (170).reduced (3, 0));
        recordCountInBox.setBounds (practice.removeFromLeft (220).reduced (3, 0));
        soundBox.setBounds (practice.removeFromLeft (130).reduced (3, 0));
        auto stepper = [&] (Label& label, TextButton& down, TextButton& value, TextButton& up, int valueW)
        {
            label.setBounds (practice.removeFromLeft (84));
            down.setBounds (practice.removeFromLeft (32).reduced (2, 0));
            value.setBounds (practice.removeFromLeft (valueW).reduced (2, 0));
            up.setBounds (practice.removeFromLeft (32).reduced (2, 0));
        };
        stepper (speedLabel, speedDown, speedValue, speedUp, 170);
        stepper (pitchLabel, pitchDown, pitchValue, pitchUp, 170);

        // Bottom: status bar, then the mixer (folds away) with its bar.
        status.setBounds (r.removeFromBottom (statusH).reduced (16, 0));
        const int mixerH = mixerOpen ? jmin (mixer.getIdealHeight(), r.getHeight() * 45 / 100) : 0;
        mixer.setVisible (mixerOpen);
        mixer.setBounds (r.removeFromBottom (mixerH).reduced (12, 0));
        mixerBarArea = r.removeFromBottom (mixerBarH);
        auto bar = mixerBarArea.reduced (12, 3);
        mixerToggle.setBounds (bar.removeFromLeft (120));
        bar.removeFromLeft (12);
        automateButton.setBounds (bar.removeFromLeft (130));
        bar.removeFromLeft (6);
        clearAutomationButton.setBounds (bar.removeFromLeft (190));
        if (editComponent != nullptr)
            editComponent->setBounds (r);
        countInOverlay.setBounds (r);
        countInOverlay.toFront (false);
    }

private:
    te::Engine engine { "Velody", std::make_unique<ExtendedUIBehaviour>(), nullptr };
    te::SelectionManager selectionManager { engine };
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<EditComponent> editComponent;

    Theme::IconButton songsButton { "Canciones", Theme::Icon::songs }, projectsButton { "Proyectos", Theme::Icon::projects },
                      importButton { "Importar", Theme::Icon::import },
                      exportButton { "Exportar", Theme::Icon::exportIcon }, settingsButton { "Ajustes", Theme::Icon::settings };
    TextButton newTrackButton { "+ Pista" }, deleteButton { "Borrar" }, mixerToggle, clickButton { "Click" }, loopButton { "Bucle" },
               automateButton { "Automatizar" }, clearAutomationButton { String (u8"Borrar automatización") };
    TransportButton playButton { TransportButton::Kind::play, "Play" }, recordButton { TransportButton::Kind::record, "Grabar" },
                    startButton { TransportButton::Kind::start, "Inicio" };
    Label status, songTitleLabel, songInfo, timeLabel;
    Brand::Wordmark wordmark;
    ComboBox countInBox, soundBox;
    bool mixerOpen = true;
    Rectangle<int> mixerBarArea;
    double songLength = 0; // song time, for the clock

    void refreshMixerToggle() { mixerToggle.setButtonText (mixerOpen ? String (u8"Mezcla  ▾") : String (u8"Mezcla  ▸")); }

    // Last opened song or project; else the most recent project; else a new one.
    void openLast()
    {
        const auto lastSong = settings.getValue ("lastSong");
        if (lastSong.isNotEmpty())
            for (auto& song : Library::scan (Library::defaultRoot()))
                if (song.name == lastSong)
                    return openEdit (projectFile (song.name), &song);
        if (File last (settings.getValue ("lastProject")); last.existsAsFile() && Projects::isProject (projectsDir(), last))
            return openEdit (last, nullptr);
        openScratch();
    }

    void openScratch()
    {
        auto projects = Projects::list (projectsDir());
        if (! projects.isEmpty())
            return openEdit (projects[0].file, nullptr);
        createProject (Projects::suggestName (projectsDir()), 120.0, "4/4");
    }

    void createProject (const String& name, double bpm, const String& signature)
    {
        Projects::folder (projectsDir()).createDirectory();
        openEdit (Projects::fileFor (projectsDir(), name.trim()), nullptr);
        Projects::setUp (*edit, bpm, signature);
        clickButton.setToggleState (edit->clickTrackEnabled.get(), dontSendNotification);
        save();
        refreshSongInfo();
    }

    std::unique_ptr<AlertWindow> projectDialog;

    void showNewProject()
    {
        projectDialog = std::make_unique<AlertWindow> ("Nuevo proyecto", String (u8"Un proyecto desde cero: grabas tus pistas sobre el metrónomo, sin canción."),
                                                       MessageBoxIconType::NoIcon);
        projectDialog->addTextEditor ("name", Projects::suggestName (projectsDir()), "Nombre");
        projectDialog->addTextEditor ("bpm", "120", "Tempo (BPM)");
        projectDialog->addComboBox ("sig", Projects::signatures, String (u8"Compás"));
        projectDialog->getComboBoxComponent ("sig")->setSelectedItemIndex (0);
        projectDialog->addButton ("Crear", 1, KeyPress (KeyPress::returnKey));
        projectDialog->addButton ("Cancelar", 0, KeyPress (KeyPress::escapeKey));
        projectDialog->enterModalState (true, ModalCallbackFunction::create ([this] (int result)
        {
            const auto name = projectDialog->getTextEditorContents ("name").trim();
            const auto bpmText = projectDialog->getTextEditorContents ("bpm").trim();
            const auto sig = projectDialog->getComboBoxComponent ("sig")->getText();
            projectDialog = nullptr;
            if (result != 1)
                return;
            if (auto why = Projects::checkName (projectsDir(), name); why.isNotEmpty())
                return AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Nuevo proyecto", why);
            const double bpm = bpmText.getDoubleValue();
            if (! (bpm >= 30 && bpm <= 300))
                return AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Nuevo proyecto", "El tempo va de 30 a 300 BPM");
            createProject (name, bpm, sig);
        }), false);
    }

    void showProjectsMenu()
    {
        PopupMenu m;
        m.addItem (String (u8"Nuevo proyecto…"), [this] { showNewProject(); });
        auto projects = Projects::list (projectsDir());
        if (! projects.isEmpty())
            m.addSectionHeader ("Mis proyectos");
        for (auto& p : projects)
            m.addItem (p.name, true, p.file == currentEditFile, [this, f = p.file] { openEdit (f, nullptr); });
        if (! currentSong.has_value() && Projects::isProject (projectsDir(), currentEditFile))
        {
            m.addSeparator();
            m.addItem (String (u8"Renombrar «") + currentEditFile.getFileNameWithoutExtension() + String (u8"»…"), [this] { renameProject(); });
            m.addItem (String (u8"Quitar «") + currentEditFile.getFileNameWithoutExtension() + String (u8"» (a la papelera)…"), [this] { removeProject(); });
        }
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (projectsButton));
    }

    void renameProject()
    {
        projectDialog = std::make_unique<AlertWindow> ("Renombrar proyecto", String(), MessageBoxIconType::NoIcon);
        projectDialog->addTextEditor ("name", currentEditFile.getFileNameWithoutExtension(), "Nombre");
        projectDialog->addButton ("Renombrar", 1, KeyPress (KeyPress::returnKey));
        projectDialog->addButton ("Cancelar", 0, KeyPress (KeyPress::escapeKey));
        projectDialog->enterModalState (true, ModalCallbackFunction::create ([this] (int result)
        {
            const auto name = projectDialog->getTextEditorContents ("name").trim();
            projectDialog = nullptr;
            if (result != 1 || name == currentEditFile.getFileNameWithoutExtension())
                return;
            if (auto why = Projects::checkName (projectsDir(), name, currentEditFile); why.isNotEmpty())
                return AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Renombrar proyecto", why);
            const auto old = currentEditFile;
            const auto target = Projects::fileFor (projectsDir(), name);
            const auto parking = Projects::folder (projectsDir()).getChildFile (".velody-closing.tracktionedit"); // hidden: never listed
            openEdit (parking, nullptr); // let go of the file (saved) before moving it
            const bool ok = Projects::rename (projectsDir(), old, name);
            openEdit (ok ? target : old, nullptr);
            parking.deleteFile();
            if (! ok)
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Renombrar proyecto", "No se pudo renombrar");
        }), false);
    }

    void removeProject()
    {
        const auto f = currentEditFile;
        AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::QuestionIcon)
                                    .withTitle (String (u8"¿Quitar «") + f.getFileNameWithoutExtension() + String (u8"»?"))
                                    .withMessage (String (u8"El proyecto va a la papelera (puedes recuperarlo de ahí). Tus grabaciones se quedan en la carpeta de proyectos."))
                                    .withButton ("Quitar", 1)
                                    .withButton ("Cancelar", 0),
                                [this, f] (int r)
                                {
                                    if (r != 1)
                                        return;
                                    const auto scratch = Projects::folder (projectsDir()).getChildFile (".velody-closing.tracktionedit"); // hidden: never listed
                                    openEdit (scratch, nullptr); // close it first
                                    f.moveToTrash();
                                    scratch.deleteFile();
                                    openScratch();
                                });
    }

    // Audio, plugins, cloud, Vibes, setup, project folder: one menu instead of a row of buttons.
    void showSettingsMenu()
    {
        PopupMenu m;
        m.addItem (String (u8"Interfaz de audio…"), [this] { EngineHelpers::showAudioDeviceSettings (engine); });
        m.addItem (String (u8"Plugins (VST3 / LV2)…"), [this] { showPluginScanner(); });
        // Latency: the buffer (PipeWire's quantum under JACK), with the round trip it gives.
        auto* dev = engine.getDeviceManager().deviceManager.getCurrentAudioDevice();
        const double rate = dev != nullptr ? dev->getCurrentSampleRate() : 48000.0;
        const int current = dev != nullptr ? dev->getCurrentBufferSizeSamples() : 0;
        PopupMenu latency;
        for (int b : LiveMonitoring::buffers)
            latency.addItem (String (b) + " muestras  (~" + String (LiveMonitoring::roundTripMs (b, rate), 1) + " ms)"
                                 + (b <= 128 ? String (u8"  — para tocar en vivo") : String()),
                             true, b == current, [this, b] { setBuffer (b); });
        m.addSubMenu ("Latencia (buffer)", latency);
        m.addSeparator();
        m.addItem (String (u8"Nube…"), [this] { showCloudSettings(); });
        m.addItem (String (u8"Vibes…"), [this] { showVibesSettings(); });
        m.addItem (String (u8"Instalación (Demucs)…"), [this] { showSetup(); });
        m.addSeparator();
        m.addItem ("Abrir la carpeta de proyectos y grabaciones", [] { projectsDir().revealToUser(); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (settingsButton));
    }

    // "+ Pista": audio, an instrument (your synths + the built-in 4OSC), or an empty MIDI clip to draw in.
    void showNewTrackMenu()
    {
        PopupMenu m;
        m.addItem ("Pista de audio", [this]
        {
            edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 1);
            SongEdit::setMixPan (*edit, mixPanPreference());
            mixer.rebuild();
            resized();
        });
        PopupMenu instruments;
        for (auto& choice : Instruments::available (engine))
            instruments.addItem (choice.name, [this, choice] { addInstrument (choice); });
        if (Instruments::available (engine).size() == 1)
            instruments.addItem (String (u8"¿Falta tu sintetizador? Escanea en Ajustes → Plugins"), false, false, nullptr);
        m.addSubMenu ("Pista de instrumento (MIDI)", instruments);
        // An effect bus: one reverb/delay several tracks feed (Envíos in the mixer).
        PopupMenu busEffects;
        busEffects.addItem (String (u8"Con un efecto… (buscar)"), [this] { showEffectPicker (nullptr); });
        busEffects.addItem (String (u8"Vacío (le pones el efecto después)"), [this] { addBus ("Efectos", {}); });
        m.addSubMenu (String (u8"Bus de efectos (reverb, delay…)"), busEffects);
        m.addSeparator();
        m.addItem (String (u8"Quitar pistas vacías (deja 2 para grabar)"), [this]
        {
            edit->getUndoManager().beginNewTransaction();
            const int n = SongEdit::removeEmptyTracks (*edit, 2);
            refreshAfterEdit();
            status.setText (n == 0 ? String (u8"No hay pistas vacías que quitar")
                                   : String (n) + String (u8" pistas vacías quitadas (Ctrl+Z para deshacer)"), dontSendNotification);
        });
        auto* target = instrumentTrackForClip();
        m.addItem (String (u8"Clip MIDI vacío (4 compases) para dibujar notas"), target != nullptr, false, [this]
        {
            if (auto* t = instrumentTrackForClip())
                if (auto clip = Instruments::addEmptyClip (*t, edit->getTransport().getPosition()))
                    showPianoRoll (*clip);
        });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (newTrackButton));
    }

    // «+ Efecto» on a track, or a new bus's effect (track == nullptr): search, pick, it goes in.
    void showEffectPicker (te::Track::Ptr track)
    {
        auto* panel = new PluginPicker::Panel (PluginPicker::all (engine.getPluginManager().knownPluginList.getTypes()),
                                               PluginPicker::loadRecents (settings),
                                               [this, track] (const PluginPicker::Choice& c)
        {
            if (auto* w = effectWindow.getComponent())
                w->exitModalState (0);
            PluginPicker::saveRecents (settings, PluginPicker::remember (PluginPicker::loadRecents (settings), c.key));
            if (track == nullptr)
                return addBus (c.name, PluginPicker::create (*edit, c));
            if (PluginPicker::addTo (*track, c, &selectionManager) == nullptr)
                status.setText (String (u8"No se pudo cargar «") + c.name + String (u8"»"), dontSendNotification);
        });

        DialogWindow::LaunchOptions o;
        o.dialogTitle = track != nullptr ? String (u8"Agregar efecto a «") + track->getName() + String (u8"»") : String (u8"Efecto del bus");
        o.dialogBackgroundColour = getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.content.setOwned (panel);
        o.componentToCentreAround = this;
        effectWindow = o.launchAsync();
    }

    Component::SafePointer<DialogWindow> effectWindow;

    void addBus (const String& name, te::Plugin::Ptr plugin)
    {
        if (auto* t = Mixing::addBus (*edit, name, plugin))
        {
            SongEdit::setMixPan (*edit, mixPanPreference());
            mixer.rebuild();
            resized();
            status.setText (t->getName() + String (u8" creado: en el mezclador, «Envíos» de cada pista manda señal a él"), dontSendNotification);
        }
    }

    // The selected instrument track, else the first one.
    te::AudioTrack* instrumentTrackForClip()
    {
        if (auto* sel = dynamic_cast<te::AudioTrack*> (selectionManager.getSelectedObject (0)); sel != nullptr && Instruments::isInstrumentTrack (*sel))
            return sel;
        for (auto t : te::getAudioTracks (*edit))
            if (Instruments::isInstrumentTrack (*t))
                return t;
        return nullptr;
    }

    void addInstrument (const Instruments::Choice& choice)
    {
        auto* t = Instruments::addInstrumentTrack (*edit, choice);
        if (t == nullptr)
            return;
        SongEdit::setMixPan (*edit, mixPanPreference());
        midiToTrack (*t);
        selectionManager.selectOnly (t);
        mixer.rebuild();
        resized();
        status.setText ("Pista de instrumento: " + t->getName() + String (u8" — toca tu teclado MIDI, o «+ Pista → Clip MIDI vacío» para dibujar notas"),
                        dontSendNotification);
    }

    void midiToTrack (te::AudioTrack& t)
    {
        Instruments::enableMidiInputs (engine.getDeviceManager());
        edit->getTransport().ensureContextAllocated();
        Instruments::routeMidiInputs (*edit, t);
        edit->restartPlayback();
    }

    void showPianoRoll (te::MidiClip& clip)
    {
        DialogWindow::LaunchOptions o;
        o.dialogTitle = "Notas: " + (clip.getTrack() != nullptr ? clip.getTrack()->getName() : String ("MIDI"));
        o.dialogBackgroundColour = Theme::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.content.setOwned (new PianoRoll::Panel (clip));
        o.componentToCentreAround = this;
        o.launchAsync();
    }

    bool forcedQuantum = false;

    void setBuffer (int buffer)
    {
        auto& dm = engine.getDeviceManager().deviceManager;
        auto error = LiveMonitoring::setBuffer (dm, buffer);
        if (error.isNotEmpty())
            return AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Latencia", error);
        forcedQuantum |= LiveMonitoring::isJack (dm.getCurrentAudioDeviceType());
        // PipeWire applies the new quantum to the running device a moment later.
        Timer::callAfterDelay (600, [sp = SafePointer<MainComponent> (this)] { if (sp != nullptr) sp->refreshButtons(); });
    }

    // Header subtitle: tempo, trim, practice speed/pitch.
    void refreshSongInfo()
    {
        songTitleLabel.setText (currentSong.has_value() ? currentSong->name : currentEditFile.getFileNameWithoutExtension(), dontSendNotification);
        StringArray parts;
        if (! currentSong.has_value() && edit != nullptr)
        {
            parts.add ("Proyecto");
            parts.add (String (roundToInt (Projects::bpm (*edit))) + " BPM");
            parts.add (Projects::signature (*edit));
            parts.add (te::getAudioTracks (*edit).size() == 1 ? String ("1 pista") : String (te::getAudioTracks (*edit).size()) + " pistas");
        }
        else if (currentSong.has_value())
        {
            parts.add (songMeta.bpm > 0 ? String (roundToInt (songMeta.bpm)) + " BPM" : (analysingOpenSong ? String (u8"analizando…") : String ("sin tempo")));
            if (trim.active())
                parts.add (Trim::describe (trim).replace ("Tramo: ", "tramo "));
            if (speed() != 1.0)
                parts.add (String (roundToInt (speed() * 100)) + " %");
            if (semitones() != 0)
                parts.add ((semitones() > 0 ? "+" : "") + String (semitones()) + " st");
        }

        songInfo.setText (parts.joinIntoString (String (u8"  ·  ")), dontSendNotification);
    }
    SongInfo::ChordDisplay chordDisplay;
    double musicStart = 0; // song time where the first sound is

    static inline const Identifier musicStartProp { "velodyMusicStart" }; // cached: decoding stems takes a moment

    // The trim's start when there is one, else where the music starts (leading silence skipped).
    void goToMusicStart() { edit->getTransport().setPosition (te::TimePosition::fromSeconds ((trim.start > 0 ? trim.start : musicStart) / speed())); }

    TextButton trimButton { "Tramo" };
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
        trimButton.setButtonText (trim.active() ? String (u8"Tramo ✓") : String ("Tramo"));
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
        o.folderName = ".config/Velody"; // JUCE puts folderName under ~ on Linux
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
    Slider speedSlider, pitchSlider; // the values (not shown); the BPM/key steppers below move them
    TextButton speedDown { String (u8"−") }, speedUp { "+" }, speedValue, pitchDown { String (u8"−") }, pitchUp { "+" }, pitchValue;
    SongKey::Key songKey;

    // A target tempo in BPM → the practice speed (song tempo × speed = target).
    void setTempo (double targetBpm)
    {
        if (songMeta.bpm > 0)
            speedSlider.setValue (100.0 * Metronome::clampSpeed (targetBpm / songMeta.bpm), sendNotificationSync);
    }

    void stepTempo (int step)
    {
        if (songMeta.bpm > 0)
            setTempo (std::round (songMeta.bpm * speed()) + step);
        else
            speedSlider.setValue (speedSlider.getValue() + step, sendNotificationSync);
    }

    void showTempoMenu()
    {
        PopupMenu m;
        m.addItem (songMeta.bpm > 0 ? "Tempo original (" + String (roundToInt (songMeta.bpm)) + " BPM)" : String ("Velocidad original (100 %)"),
                   [this] { speedSlider.setValue (100, sendNotificationSync); });
        m.addItem (songMeta.bpm > 0 ? String (u8"Escribir el tempo…") : String (u8"Escribir la velocidad (%)…"), [this] { askTempo(); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (speedValue));
    }

    void askTempo()
    {
        const bool bpm = songMeta.bpm > 0;
        projectDialog = std::make_unique<AlertWindow> (bpm ? "Tempo" : "Velocidad",
            bpm ? "BPM para practicar (original " + String (roundToInt (songMeta.bpm)) + ", de " + String (roundToInt (songMeta.bpm * 0.5)) + " a "
                      + String (roundToInt (songMeta.bpm * 1.5)) + "):"
                : String (u8"Porcentaje (50 a 150):"),
            MessageBoxIconType::NoIcon);
        projectDialog->addTextEditor ("v", bpm ? String (roundToInt (songMeta.bpm * speed())) : String (roundToInt (speed() * 100)));
        projectDialog->addButton ("Aplicar", 1, KeyPress (KeyPress::returnKey));
        projectDialog->addButton ("Cancelar", 0, KeyPress (KeyPress::escapeKey));
        projectDialog->enterModalState (true, ModalCallbackFunction::create ([this, bpm] (int r)
        {
            const double v = projectDialog->getTextEditorContents ("v").getDoubleValue();
            projectDialog = nullptr;
            if (r != 1 || v <= 0)
                return;
            if (bpm) setTempo (v);
            else     speedSlider.setValue (jlimit (50.0, 150.0, v), sendNotificationSync);
        }), false);
    }

    void refreshPracticeControls()
    {
        const bool on = currentSong.has_value();
        for (auto* b : { &speedDown, &speedUp, &speedValue, &pitchDown, &pitchUp, &pitchValue })
            b->setEnabled (on);
        const double pct = speed() * 100;
        const String pctText = std::abs (pct - std::round (pct)) < 0.05 ? String (roundToInt (pct)) : String (pct, 1);
        speedValue.setButtonText (songMeta.bpm > 0 ? String (roundToInt (songMeta.bpm * speed())) + " BPM  (" + pctText + " %)"
                                                   : pctText + " %");
        pitchValue.setButtonText (SongKey::describe (songKey, semitones()));
    }
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

    Vibes::Poller vibes { *imports, Vibes::settingsFile(), File::getSpecialLocation (File::tempDirectory), Library::defaultRoot() };

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
        songKey = SongKey::estimate (songMeta.chords);
        refreshPracticeControls();
        refreshClickTrack();
        status.setText (String (u8"Análisis listo: metrónomo, count-in y acordes"), dontSendNotification);
    }
    struct FnTimer : Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } vibesTimer, trimTimer, autosaveTimer;

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

    static inline const Identifier cursorProp { "velodyCursor" }; // the cursor when the song was closed

    void save()
    {
        edit->state.setProperty (cursorProp, jmax (0.0, edit->getTransport().getPosition().inSeconds()), nullptr);
        te::EditFileOperations (*edit).save (true, true, false);
    }

public:
    // The window as it was (size, position, maximised), across starts.
    String savedWindowState() { return settings.getValue ("windowState"); }
    void saveWindowState (const String& s) { settings.setValue ("windowState", s); settings.saveIfNeeded(); }

private:

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
            // A soft cross-fade instead of the screen jumping: the old view fades away on top.
            if (editComponent != nullptr && editComponent->isShowing())
                Desktop::getInstance().getAnimator().fadeOut (editComponent.get(), 220);
            editComponent = nullptr;
            edit = nullptr;
        }

        const bool isNew = ! f.existsAsFile();
        edit = SongEdit::openOrCreate (engine, f);
        if (isNew && song != nullptr)
            SongEdit::addStemTracks (*edit, song->stems);

        currentEditFile = f;
        currentSong = song != nullptr ? std::optional<Library::Song> (*song) : std::nullopt;
        // Reopened at the next start; a project shows its click toggle.
        const bool isProject = song == nullptr && Projects::isProject (projectsDir(), f) && ! f.getFileName().startsWithChar ('.');
        if (song != nullptr)      { settings.setValue ("lastSong", song->name); settings.removeValue ("lastProject"); }
        else if (isProject)       { settings.setValue ("lastProject", f.getFullPathName()); settings.removeValue ("lastSong"); }
        clickButton.setVisible (song == nullptr);
        clickButton.setToggleState (edit->clickTrackEnabled.get(), dontSendNotification);

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
        songKey = SongKey::estimate (songMeta.chords);
        refreshPracticeControls();
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
            // Where you left it (saved with the edit), else where the music starts.
            if (edit->state.hasProperty (cursorProp))
                edit->getTransport().setPosition (te::TimePosition::fromSeconds ((double) edit->state[cursorProp]));
            else
                goToMusicStart();
        }
        startButton.setEnabled (song != nullptr);
        analysingOpenSong = song != nullptr && Setup::ready() && analysis->needs (song->dir);
        if (analysingOpenSong)
            analysis->prioritize (song->name);
        chordDisplay.setChords (songMeta.chords);

        edit->playInStopEnabled = true;
        edit->getTransport().addChangeListener (this);

        // Older versions made one track per channel of a generic audio device (dozens of empty tracks):
        // beyond the 8 input tracks Velody uses, the empty ones go when the edit opens.
        if (SongEdit::removeEmptyTracks (*edit, 8) > 0)
            edit->getUndoManager().clearUndoHistory();
        assignInputsToTracks();
        save();

        editComponent = std::make_unique<EditComponent> (*edit, selectionManager);
        editComponent->getEditViewState().showFooters = true; // footer = plugin chain with "+" button
        editComponent->getEditViewState().showWaveDevices = true;
        editComponent->getEditViewState().showMidiDevices = true;
        addAndMakeVisible (*editComponent);
        editComponent->setAlpha (0.0f);
        Desktop::getInstance().getAnimator().fadeIn (editComponent.get(), 260);
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
        // A stem track deleted by mistake: put the song's tracks back.
        if (currentSong.has_value())
            if (const auto missing = SongEdit::missingStems (*edit, currentSong->stems); ! missing.isEmpty())
            {
                StringArray names;
                for (auto& f : missing)
                    names.add (f.getFileNameWithoutExtension());
                m.addSeparator();
                m.addItem (String (u8"Restaurar las pistas de la canción (") + names.joinIntoString (", ") + ")", [this]
                {
                    if (! currentSong.has_value())
                        return;
                    edit->getUndoManager().beginNewTransaction();
                    const int n = SongEdit::restoreStems (*edit, currentSong->stems);
                    SongEdit::setMixPan (*edit, mixPanPreference());
                    refreshAfterEdit();
                    status.setText (String (n) + (n == 1 ? String (u8" pista restaurada") : String (u8" pistas restauradas")), dontSendNotification);
                });
            }
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (songsButton));
    }

    CloudUi::Listing listing;

    // Ctrl+F or Canciones → Buscar canción…: this laptop's songs at once, the cloud's when they arrive.
    void showSearch()
    {
        auto* panel = new SongSearch::Panel ([this] (const Cloud::Entry& e)
        {
            if (auto* w = searchWindow.getComponent())
                w->exitModalState (0);
            songAction ({ e.state == Cloud::State::cloud ? CloudUi::Action::download : CloudUi::Action::open, e });
        });
        panel->setSongs (cloud.localListing().songs);
        if (cloud.active() && ! cloud.busy())
            cloud.list ([sp = SafePointer<SongSearch::Panel> (panel)] (const CloudUi::Listing& l)
            {
                if (sp != nullptr && ! l.offline)
                    sp->setSongs (l.songs);
            });

        DialogWindow::LaunchOptions o;
        o.dialogTitle = String (u8"Buscar canción");
        o.dialogBackgroundColour = getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.content.setOwned (panel);
        o.componentToCentreAround = this;
        searchWindow = o.launchAsync();
    }

    Component::SafePointer<DialogWindow> searchWindow;

    // "Hay una versión nueva": a button on the top row when GitHub has a newer .deb.
    TextButton updateButton;
    std::optional<Updater::Update> update;
    CloudUi::Runner updateRunner;
    bool updating = false;

    void checkForUpdate()
    {
        if (! Updater::canSelfUpdate (File::getSpecialLocation (File::currentExecutableFile)))
            return; // a build from source
        auto found = std::make_shared<std::optional<Updater::Update>>();
        updateRunner.run ([found] (const Cloud::Progress&) { *found = Updater::check (ProjectInfo::versionString); },
                          [this, found] (const String&)
                          {
                              update = *found;
                              if (! update)
                                  return;
                              updateButton.setButtonText ("Actualizar a " + update->version);
                              updateButton.setTooltip (String (u8"Hay una versión nueva de Velody: ") + update->version);
                              updateButton.setVisible (true);
                              resized();
                          });
    }

    void confirmUpdate()
    {
        if (! update || updating)
            return;
        AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::QuestionIcon)
                                    .withTitle ("Actualizar a Velody " + update->version)
                                    .withMessage (String (u8"Se descarga la versión nueva y se instala (el sistema te pide tu contraseña). "
                                                          u8"Al terminar Velody se cierra guardando tu proyecto y se vuelve a abrir."))
                                    .withButton ("Actualizar", 1)
                                    .withButton (String (u8"Después"), 0),
                                [this] (int r) { if (r == 1) installUpdate(); });
    }

    void installUpdate()
    {
        updating = true;
        updateButton.setEnabled (false);
        auto u = *update;
        updateRunner.run ([u] (const Cloud::Progress& progress)
                          {
                              auto error = Updater::install (u, Setup::systemEnv(), File::getSpecialLocation (File::tempDirectory),
                                                             [&] (Updater::Stage st, int pct) { progress (st == Updater::Stage::download ? pct : -1); },
                                                             [] { return false; });
                              if (error.isNotEmpty())
                                  throw Cloud::Error (error, 0);
                          },
                          [this] (const String& error)
                          {
                              updating = false;
                              updateButton.setEnabled (true);
                              if (error.isNotEmpty())
                                  return AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "No se pudo actualizar", error);
                              // The new files are in /usr now: start them once this one has closed (and saved).
                              std::system ("setsid sh -c 'sleep 2; if command -v pw-jack >/dev/null; then exec pw-jack /usr/bin/velody-native; else exec /usr/bin/velody-native; fi' >/dev/null 2>&1 &");
                              JUCEApplication::getInstance()->systemRequestedQuit();
                          },
                          [this] (int pct)
                          {
                              status.setText (pct >= 0 ? String (u8"Descargando la actualización…  ") + String (pct) + " %"
                                                       : String (u8"Instalando la actualización… (escribe tu contraseña en la ventana del sistema)"),
                                              dontSendNotification);
                          });
    }

    // Shortcuts (REAPER-like where it makes sense).
    bool keyPressed (const KeyPress& k) override
    {
        const auto cmd = ModifierKeys::commandModifier, shift = ModifierKeys::shiftModifier;
        if (k == KeyPress ('f', cmd, 0))                   return showSearch(), true;
        if (edit == nullptr)                               return false;
        if (k == KeyPress (KeyPress::spaceKey))            return stopOrPlay (false), true;
        if (k == KeyPress ('s', 0, 0))                     return splitAtCursor(), true;
        if (k == KeyPress ('d', cmd, 0))                   return duplicateSelected(), true;
        if (k == KeyPress ('l', 0, 0))                     return toggleLoop(), true;
        // Zoom (like REAPER): + / − closer/farther in time, Ctrl+↑ / Ctrl+↓ taller/shorter tracks,
        // Ctrl+0 the whole song. Also with the mouse: Ctrl+wheel, Alt+wheel, Shift+wheel.
        if (editComponent != nullptr)
        {
            const auto c = k.getTextCharacter();
            if ((c == '+' || c == '=') && ! k.getModifiers().isCommandDown()) return editComponent->zoomTimeAtCentre (0.67), true;
            if (c == '-' && ! k.getModifiers().isCommandDown())               return editComponent->zoomTimeAtCentre (1.5), true;
            if (k == KeyPress (KeyPress::upKey, cmd, 0))                      return editComponent->scaleTrackHeight (1.25), true;
            if (k == KeyPress (KeyPress::downKey, cmd, 0))                    return editComponent->scaleTrackHeight (0.8), true;
            if (k == KeyPress ('0', cmd, 0))                                  return fitViewToSong(), true;
        }
        if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey)
            return deleteSelection(), true;
        if (k == KeyPress ('z', cmd, 0))                   return edit->getUndoManager().undo(), refreshAfterEdit(), true;
        if (k == KeyPress ('z', cmd | shift, 0) || k == KeyPress ('y', cmd, 0))
            return edit->getUndoManager().redo(), refreshAfterEdit(), true;
        return false;
    }

    void refreshAfterEdit()
    {
        mixer.rebuild();
        resized();
        refreshButtons();
    }

    Array<te::Clip*> selectedClips()
    {
        Array<te::Clip*> out;
        for (auto* o : selectionManager.getSelectedObjects())
            if (auto* c = dynamic_cast<te::Clip*> (o))
                out.add (c);
        return out;
    }

    void splitAtCursor()
    {
        Array<te::Track*> tracks;
        for (auto* o : selectionManager.getSelectedObjects())
            if (auto* t = dynamic_cast<te::Track*> (o))
                tracks.add (t);
        const auto n = ClipEdit::splitAt (*edit, selectedClips(), tracks, edit->getTransport().getPosition());
        status.setText (n > 0 ? String (u8"Cortado en el cursor") : String (u8"Para cortar: selecciona un clip (o una pista) y pon el cursor sobre él"), dontSendNotification);
    }

    void duplicateSelected()
    {
        edit->getUndoManager().beginNewTransaction();
        for (auto* c : selectedClips())
            ClipEdit::duplicate (*c);
    }

    void toggleLoop()
    {
        auto& t = edit->getTransport();
        if (! t.looping && t.getLoopRange().getLength().inSeconds() < 0.05)
            return status.setText (String (u8"Primero marca la parte: Bucle → inicio y fin, o clic derecho en un clip → Repetir esta parte"), dontSendNotification);
        t.looping = ! t.looping.get();
        refreshButtons();
    }

    void showLoopMenu()
    {
        auto& t = edit->getTransport();
        const auto cursor = t.getPosition();
        const auto range = t.getLoopRange();
        PopupMenu m;
        m.addItem ("Bucle activado  (L)", range.getLength().inSeconds() >= 0.05, t.looping.get(), [this] { toggleLoop(); });
        m.addSeparator();
        m.addItem (String (u8"Inicio del bucle aquí (") + Trim::clock (cursor.inSeconds() * speed()) + ")", [this, cursor]
        {
            auto& tr = edit->getTransport();
            const auto end = tr.getLoopRange().getEnd() > cursor ? tr.getLoopRange().getEnd() : cursor + te::TimeDuration::fromSeconds (8.0);
            ClipEdit::loopBetween (*edit, cursor, end);
            refreshButtons();
        });
        m.addItem (String (u8"Fin del bucle aquí (") + Trim::clock (cursor.inSeconds() * speed()) + ")", cursor > range.getStart(), false, [this, cursor]
        {
            ClipEdit::loopBetween (*edit, edit->getTransport().getLoopRange().getStart(), cursor);
            refreshButtons();
        });
        auto clips = selectedClips();
        m.addItem ("Repetir el clip seleccionado", ! clips.isEmpty(), false, [this, c = clips.isEmpty() ? nullptr : clips[0]]
        {
            if (c != nullptr)
                ClipEdit::loopOver (*edit, *c);
            refreshButtons();
        });
        m.addSeparator();
        m.addItem (String (u8"Grabar con el bucle activo deja varias tomas en un clip: clic derecho en él → Toma"), false, false, nullptr);
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (loopButton));
    }

    void songAction (const CloudUi::Action& a)
    {
        using A = CloudUi::Action;
        switch (a.kind)
        {
            case A::openEmpty:       return showNewProject();
            case A::open:            return openSong (a.entry);
            case A::download:        return downloadAndOpen (a.entry);
            case A::upload:          return uploadSong (a.entry.name);
            case A::uploadAll:       return uploadAllLocal();
            case A::removeCurrent:   return confirmRemoveCurrentSong (a.entry);
            case A::deleteFromCloud: return confirmDeleteCloudOnly (a.entry);
            case A::settings:        return showCloudSettings();
            case A::search:          return showSearch();
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
        openScratch(); // close the song first: its folder is about to move
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
                                                     + (n > 0 ? "\n\nTienes " + String (n) + (n == 1 ? String (u8" grabación") : String (u8" grabaciones")) + String (u8" en esta canción.") : String()));
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
        openScratch(); // close the song first: its files are about to move

        auto r = SongDelete::execute (plan, includeTakes, Library::defaultRoot(), projectsDir());
        if (r.failed())
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, String (u8"No se pudo quitar la canción"), r.getErrorMessage());
    }

    void stopOrPlay (bool record)
    {
        auto& t = edit->getTransport();
        const bool wasRecording = t.isRecording();

        if (record)
            // Grabar again stops and KEEPS the take (the examples' toggleRecord discarded it: stop (true, …)).
            wasRecording ? stopRecordingKeepingTake() : recordWithCountIn();
        else if (t.isPlaying())
            stopPlayback();
        else
            playWithCountIn();

        if (wasRecording)
            save(); // keep the new take on disk right away
    }

    void stopRecordingKeepingTake()
    {
        edit->getTransport().stop (false, false); // discardRecordings = false
        endCountInRecording();
    }

    ComboBox recordCountInBox;
    bool punchForCountIn = false, sawRecording = false;
    te::TimeRange loopBeforeRecord;

    // Clicks on the song's grid with the song silent, then the take starts at the cursor (punch-in).
    void recordWithCountIn()
    {
        endCountInRecording(); // a previous one that never started (no armed input)
        auto& t = edit->getTransport();
        const auto plan = RecordCountIn::plan (songMeta, t.getPosition().inSeconds(), speed(),
                                               RecordCountIn::barsOptions[jmax (0, recordCountInBox.getSelectedItemIndex())]);
        if (! plan.hasCountIn() || countIn == nullptr)
        {
            // A project (no song grid): Tracktion's own count-in, at the project's tempo.
            if (! currentSong.has_value())
                edit->setCountInMode (Projects::countInFor (RecordCountIn::barsOptions[jmax (0, recordCountInBox.getSelectedItemIndex())]));
            return edit->getTransport().record (false);
        }

        countInFrom = plan.punchIn;
        countIn->arm (plan.clicks, countInFrom, clickSound());
        countInClicks = plan.clicks;
        loopBeforeRecord = t.getLoopRange();
        t.looping = false; // Tracktion records in loop mode or punch mode, not both
        t.setLoopRange ({ te::TimePosition::fromSeconds (plan.punchIn), te::TimePosition::fromSeconds (plan.punchIn + 24 * 3600.0) });
        edit->recordingPunchInOut = true;
        punchForCountIn = true;
        sawRecording = false;

        // Same −4.5 s limit as the count-in on Play (see playWithCountIn).
        const auto oldMode = edit->getCountInMode();
        edit->setCountInMode (te::Edit::CountIn::twoBar);
        t.setPosition (te::TimePosition::fromSeconds (plan.clicks.front().time));
        edit->setCountInMode (oldMode);
        t.record (false);
    }

    // After a count-in recording ends: punch mode off and the loop range back.
    void endCountInRecording()
    {
        if (! punchForCountIn)
            return;
        punchForCountIn = false;
        edit->recordingPunchInOut = false;
        edit->getTransport().setLoopRange (loopBeforeRecord);
        if (countIn != nullptr)
            countIn->disarm();
    }

    void stopPlayback()
    {
        auto& t = edit->getTransport();
        const bool inCountIn = countIn != nullptr && countIn->isArmed();
        t.stop (false, false);
        endCountInRecording();

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
            countInClicks = clicks;

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
    std::vector<Metronome::Click> countInClicks; // the count-in being played, for the big countdown
    CountInDisplay::Overlay countInOverlay;

    double speed() const { return Metronome::clampSpeed (speedSlider.getValue() / 100.0); }
    int semitones() const { return Metronome::clampSemitones (pitchSlider.getValue()); }

    void applyPractice()
    {
        if (edit == nullptr)
            return;


        // Keep the same spot in the song when the speed (and so the edit timeline) changes.
        auto& t = edit->getTransport();
        const double songPos = t.getPosition().inSeconds() * appliedSpeed;
        const double ratio = appliedSpeed / speed();
        SongEdit::setPractice (*edit, speed(), semitones());
        t.setPosition (te::TimePosition::fromSeconds (songPos / speed()));
        appliedSpeed = speed();
        // The song got longer/shorter: the same part of it stays on screen, at the same zoom.
        if (editComponent != nullptr)
        {
            auto& v = editComponent->getEditViewState();
            const auto old = v.view();
            v.setView (Zoom::clamp ({ old.start * ratio, old.end * ratio }, Zoom::limit (SongEdit::contentLength (*edit))));
        }
        refreshPracticeControls();
        songLength = currentSong.has_value() ? SongEdit::contentLength (*edit) * speed() : 0.0;
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
        // A generic device (PipeWire's "Default ALSA") can list dozens of input channels: tracks are made
        // for the first few only (a 2-in interface like the Scarlett 2i2 gets its 2).
        constexpr int maxInputTracks = 8;
        int made = 0;

        for (auto instance : edit->getAllInputDevices())
            if (instance->getInputDevice().getDeviceType() == te::InputDevice::waveDevice && made++ < maxInputTracks)
                if (auto t = EngineHelpers::getOrInsertAudioTrackAt (*edit, trackNum++))
                {
                    [[maybe_unused]] auto res = instance->setTarget (t->itemID, true, &edit->getUndoManager(), 0);
                    instance->setRecordingEnabled (t->itemID, true);
                }

        // MIDI keyboards: on, and into the first instrument track (or the one picked later).
        Instruments::enableMidiInputs (engine.getDeviceManager());
        for (auto t : te::getAudioTracks (*edit))
            if (Instruments::isInstrumentTrack (*t))
            {
                Instruments::routeMidiInputs (*edit, *t);
                break;
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
        playButton.setActive (t.isPlaying() && ! t.isRecording());
        recordButton.setActive (t.isRecording());
        loopButton.setToggleState (t.looping.get(), dontSendNotification);
        refreshSongInfo();

        auto* dev = engine.getDeviceManager().deviceManager.getCurrentAudioDevice();
        if (analysingOpenSong && imports->pending() == 0 && exporter == nullptr && ! cloud.busy())
            status.setText (String (u8"Analizando tempo y acordes de esta canción…"), dontSendNotification);
        else if (imports->pending() == 0 && exporter == nullptr && ! cloud.busy()) // while importing/exporting/syncing, the status shows the progress instead
            status.setText (dev == nullptr ? "Sin interfaz de audio"
                                           : dev->getTypeName() + ": " + dev->getName() + "  |  " + String (dev->getCurrentSampleRate(), 0) + " Hz  |  "
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
        else if (source == &selectionManager)
        {
            // Selecting an instrument track (or one of its clips) sends the MIDI keyboard there.
            te::AudioTrack* t = nullptr;
            if (auto* sel = selectionManager.getSelectedObject (0))
            {
                if (auto* at = dynamic_cast<te::AudioTrack*> (sel)) t = at;
                else if (auto* c = dynamic_cast<te::Clip*> (sel)) t = dynamic_cast<te::AudioTrack*> (c->getTrack());
            }
            if (t != nullptr && Instruments::isInstrumentTrack (*t))
                midiToTrack (*t);
            refreshButtons();
        }
        else
        {
            auto& t = edit->getTransport();
            sawRecording |= t.isRecording();
            if (punchForCountIn && sawRecording && ! t.isRecording())
                endCountInRecording();
            refreshButtons();
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class VelodyApp : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Velody"; }
    const String getApplicationVersion() override { return ProjectInfo::versionString; }

    void initialise (const String&) override
    {
        // Settings used to land in ~/Velody (a visible folder in home): into ~/.config/Velody.
        auto oldSettings = File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Velody/Velody.settings");
        auto newSettings = File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Velody/Velody.settings");
        AppSettings::migrate (oldSettings, newSettings);
        if (oldSettings.getParentDirectory().isDirectory() && oldSettings.getParentDirectory().getNumberOfChildFiles (File::findFilesAndDirectories | File::ignoreHiddenFiles) == 0)
            oldSettings.getParentDirectory().deleteFile();

        PluginPaths::setEnvironment (PluginPaths::atStart()); // REAPER's Linux plugins, before the engine starts
        LookAndFeel::setDefaultLookAndFeel (&look);
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override
    {
        window = nullptr;
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

private:
    struct MainWindow : DocumentWindow
    {
        MainWindow() : DocumentWindow ("Velody", Theme::bg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setIcon (Brand::icon (256));
            setContentOwned (new MainComponent(), true);
            setResizable (true, false);
            centreWithSize (getWidth(), getHeight());
            if (auto* main = dynamic_cast<MainComponent*> (getContentComponent()); main != nullptr && main->savedWindowState().isNotEmpty())
                restoreWindowStateFromString (main->savedWindowState());
            setVisible (true);
        }

        ~MainWindow() override
        {
            if (auto* main = dynamic_cast<MainComponent*> (getContentComponent()))
                main->saveWindowState (getWindowStateAsString());
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    Theme::LookAndFeel look; // outlives the window
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (VelodyApp)
