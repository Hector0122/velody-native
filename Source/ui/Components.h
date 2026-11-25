// Velody's copy of tracktion_engine/examples/common/Components.h (painted with Theme.h).
#include "../Theme.h"
#include "../SongEdit.h"
#include "../ClipEdit.h"
#include "../Zoom.h"
/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com
*/

#pragma once

namespace IDs
{
    #define DECLARE_ID(name)  const juce::Identifier name (#name);
    DECLARE_ID (EDITVIEWSTATE)
    DECLARE_ID (showMasterTrack)
    DECLARE_ID (showGlobalTrack)
    DECLARE_ID (showMarkerTrack)
    DECLARE_ID (showChordTrack)
    DECLARE_ID (showMidiDevices)
    DECLARE_ID (showWaveDevices)
    DECLARE_ID (viewX1)
    DECLARE_ID (viewX2)
    DECLARE_ID (viewY)
    DECLARE_ID (trackHeight)
    DECLARE_ID (drawWaveforms)
    DECLARE_ID (showHeaders)
    DECLARE_ID (showFooters)
    DECLARE_ID (showArranger)
    #undef DECLARE_ID
}

namespace te = tracktion;

//==============================================================================
void drawMidiClip (juce::Graphics&, te::MidiClip&, juce::Rectangle<int>, te::TimeRange);

//==============================================================================
class EditViewState
{
public:
    EditViewState (te::Edit& e, te::SelectionManager& s)
        : edit (e), selectionManager (s)
    {
        state = edit.state.getOrCreateChildWithName (IDs::EDITVIEWSTATE, nullptr);
        
        auto um = &edit.getUndoManager();
        
        showMasterTrack.referTo (state, IDs::showMasterTrack, um, false);
        showGlobalTrack.referTo (state, IDs::showGlobalTrack, um, false);
        showMarkerTrack.referTo (state, IDs::showMarkerTrack, um, false);
        showChordTrack.referTo (state, IDs::showChordTrack, um, false);
        showArrangerTrack.referTo (state, IDs::showArranger, um, false);
        drawWaveforms.referTo (state, IDs::drawWaveforms, um, true);
        showHeaders.referTo (state, IDs::showHeaders, um, true);
        showFooters.referTo (state, IDs::showFooters, um, false);
        showMidiDevices.referTo (state, IDs::showMidiDevices, um, false);
        showWaveDevices.referTo (state, IDs::showWaveDevices, um, true);

        // Velody: zooming and scrolling aren't edits, so they stay out of Ctrl+Z (nullptr undo manager).
        viewX1.referTo (state, IDs::viewX1, nullptr, 0s);
        viewX2.referTo (state, IDs::viewX2, nullptr, 60s);
        viewY.referTo (state, IDs::viewY, nullptr, 0);
        trackHeight.referTo (state, IDs::trackHeight, nullptr, Zoom::defaultHeight);
    }
    
    int timeToX (te::TimePosition time, int width) const
    {
        return roundToInt (((time - viewX1) * width) / (viewX2 - viewX1));
    }
    
    te::TimePosition xToTime (int x, int width) const
    {
        return toPosition ((viewX2 - viewX1) * (double (x) / width)) + toDuration (viewX1.get());
    }
    
    te::TimePosition beatToTime (te::BeatPosition b) const
    {
        auto& ts = edit.tempoSequence;
        return ts.toTime (b);
    }
    
    te::Edit& edit;
    te::SelectionManager& selectionManager;
    
    CachedValue<bool> showMasterTrack, showGlobalTrack, showMarkerTrack, showChordTrack, showArrangerTrack,
                      drawWaveforms, showHeaders, showFooters, showMidiDevices, showWaveDevices;
    
    CachedValue<te::TimePosition> viewX1, viewX2;
    CachedValue<double> viewY;
    CachedValue<int> trackHeight; // Velody: every lane's height (a track can have its own, Zoom::heightOf)

    Zoom::View view() const { return { viewX1.get().inSeconds(), viewX2.get().inSeconds() }; }
    void setView (Zoom::View v)
    {
        viewX1 = te::TimePosition::fromSeconds (v.start);
        viewX2 = te::TimePosition::fromSeconds (v.end);
    }
    
    ValueTree state;
};

//==============================================================================
class ClipComponent : public Component
{
public:
    ClipComponent (EditViewState&, te::Clip::Ptr);
    
    void paint (Graphics& g) override;
    void mouseDown (const MouseEvent& e) override;
    // Velody: drag the middle to move (also to another track), the edges to trim; right-click: menu.
    void mouseDrag (const MouseEvent& e) override;
    void mouseUp (const MouseEvent& e) override;
    void mouseMove (const MouseEvent& e) override;
    
    te::Clip& getClip() { return *clip; }
    
protected:
    EditViewState& editViewState;
    te::Clip::Ptr clip;

    void paintFades (Graphics&); // fade-in/out shading (audio clips)

private:
    enum class Drag { none, move, trimStart, trimEnd } drag = Drag::none;
    te::TimePosition dragStart, dragEnd;
    double secondsPerPixel() const;
    Drag zoneAt (int x) const;
    void showMenu();
};

//==============================================================================
class AudioClipComponent : public ClipComponent
{
public:
    AudioClipComponent (EditViewState&, te::Clip::Ptr);
    
    te::WaveAudioClip* getWaveAudioClip() { return dynamic_cast<te::WaveAudioClip*> (clip.get()); }
    
    void paint (Graphics& g) override;
    
private:
    void updateThumbnail();
    void drawWaveform (Graphics& g, te::AudioClipBase& c, te::SmartThumbnail& thumb, Colour colour,
                       int left, int right, int y, int h, int xOffset);
    void drawChannels (Graphics& g, te::SmartThumbnail& thumb, Rectangle<int> area,
                       te::TimeRange time, bool useLeft, bool useRight,
                       float leftGain, float rightGain);

    std::unique_ptr<te::SmartThumbnail> thumbnail;
};

//==============================================================================
class MidiClipComponent : public ClipComponent
{
public:
    MidiClipComponent (EditViewState&, te::Clip::Ptr);
    
    te::MidiClip* getMidiClip() { return dynamic_cast<te::MidiClip*> (clip.get()); }
    
    void paint (Graphics& g) override;
    void mouseDoubleClick (const MouseEvent&) override; // Velody: opens the piano roll
};

//==============================================================================
class RecordingClipComponent : public Component,
                               private Timer
{
public:
    RecordingClipComponent (te::Track::Ptr t, EditViewState&);
    
    void paint (Graphics& g) override;
    
private:
    void timerCallback() override;
    void updatePosition();
    void initialiseThumbnailAndPunchTime();
    void drawThumbnail (Graphics& g, Colour waveformColour) const;
    bool getBoundsAndTime (juce::Rectangle<int>& bounds, tracktion::TimeRange& times) const;
    
    te::Track::Ptr track;
    EditViewState& editViewState;
    
    te::RecordingThumbnailManager::Thumbnail::Ptr thumbnail;
    te::TimePosition punchInTime = -1.0s;
};

//==============================================================================
class TrackHeaderComponent : public Component,
                             private te::ValueTreeAllEventListener
{
public:
    TrackHeaderComponent (EditViewState&, te::Track::Ptr);
    ~TrackHeaderComponent() override;
    
    void paint (Graphics& g) override;
    void mouseDown (const MouseEvent& e) override;
    // Velody: drag the bottom edge to make this track taller/shorter (double-click it: back to the rest's).
    void mouseMove (const MouseEvent& e) override;
    void mouseDrag (const MouseEvent& e) override;
    void mouseDoubleClick (const MouseEvent& e) override;
    void resized() override;
    static constexpr int edgeGrab = 5;
    bool onBottomEdge (const MouseEvent& e) const { return e.y >= getHeight() - edgeGrab; }
    
private:
    void valueTreeChanged() override {}
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    int dragStartHeight = 0;
    
    EditViewState& editViewState;
    te::Track::Ptr track;
    
    ValueTree inputsState;
    Label trackName;
    TextButton armButton {String (u8"●")}, muteButton {"M"}, soloButton {"S"}, inputButton {"In"};
    TextButton monitorButton {"Mon"}; // Velody: hear the input through this track's plugins
};

//==============================================================================
class PluginComponent : public TextButton
{
public:
    PluginComponent (EditViewState&, te::Plugin::Ptr);
    ~PluginComponent() override;
    
    using TextButton::clicked;
    void clicked (const ModifierKeys& modifiers) override;
    
private:
    EditViewState& editViewState;
    te::Plugin::Ptr plugin;
};

//==============================================================================
class TrackFooterComponent : public Component,
                             private FlaggedAsyncUpdater,
                             private te::ValueTreeAllEventListener
{
public:
    TrackFooterComponent (EditViewState&, te::Track::Ptr);
    ~TrackFooterComponent() override;
    
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void resized() override;
    
private:
    void valueTreeChanged() override {}
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    void handleAsyncUpdate() override;
    
    void buildPlugins();
    
    EditViewState& editViewState;
    te::Track::Ptr track;
    
    TextButton addButton {"+ Efecto"};
    OwnedArray<PluginComponent> plugins;
    
    bool updatePlugins = false;
};

//==============================================================================
class TrackComponent : public Component,
                       private te::ValueTreeAllEventListener,
                       private FlaggedAsyncUpdater,
                       private ChangeListener
{
public:
    TrackComponent (EditViewState&, te::Track::Ptr);
    ~TrackComponent() override;
    te::Track* getTrack() const { return track.get(); } // Velody: where a dragged clip lands
    
    void paint (Graphics& g) override;
    void mouseDown (const MouseEvent& e) override;
    void resized() override;

private:
    void changeListenerCallback (ChangeBroadcaster*) override;

    void valueTreeChanged() override {}
    
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    
    void handleAsyncUpdate() override;
    
    void buildClips();
    void buildRecordClips();
    
    EditViewState& editViewState;
    te::Track::Ptr track;
    
    OwnedArray<ClipComponent> clips;
    std::unique_ptr<RecordingClipComponent> recordingClip;
    
    bool updateClips = false, updatePositions = false, updateRecordClips = false;
};

//==============================================================================
class PlayheadComponent : public Component,
                          private Timer
{
public:
    PlayheadComponent (te::Edit&, EditViewState&);
    
    void paint (Graphics& g) override;
    bool hitTest (int x, int y) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;

private:
    void timerCallback() override;
    
    te::Edit& edit;
    EditViewState& editViewState;
    
    int xPosition = 0;
    bool firstTimer = true;
};

//==============================================================================
// Velody: called when a track's monitoring is turned on (Main warns about speakers once).
namespace VelodyUi
{
    inline std::function<void()> onMonitorOn;
    inline std::function<void (te::MidiClip&)> onEditMidiClip; // double-click on a MIDI clip
    inline std::function<void (te::Track::Ptr)> onAddEffect;    // «+ Efecto»: the search window (PluginPicker.h)
}

// Velody: seconds ruler over the lanes; a click moves the playhead there.
class TimeRuler : public Component, private Timer
{
public:
    TimeRuler (te::Edit& e, EditViewState& s) : edit (e), state (s) { startTimerHz (5); }
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent& e) override { mouseDown (e); }
private:
    void timerCallback() override { repaint(); }
    te::Edit& edit;
    EditViewState& state;
};

class EditComponent : public Component,
                      private te::ValueTreeAllEventListener,
                      private FlaggedAsyncUpdater,
                      private ChangeListener,
                      private ScrollBar::Listener
{
public:
    EditComponent (te::Edit&, te::SelectionManager&);
    ~EditComponent() override;
    
    EditViewState& getEditViewState()   { return editViewState; }
    // Velody: the track list scrolls (wheel / scrollbar) when there are more tracks than fit.
    // Velody zoom (like REAPER): Ctrl+wheel = closer/farther in time (around the mouse), Alt+wheel = taller
    // or shorter tracks, Shift+wheel = move along the song, wheel = scroll the tracks.
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    void scrollTracksBy (double pixels);
    void zoomTime (double factor, double anchorSeconds);
    void zoomTimeAtCentre (double factor) { auto v = editViewState.view(); zoomTime (factor, (v.start + v.end) / 2); }
    void scrollTime (double seconds);
    void scaleTrackHeight (double factor);
    void followPlayhead();               // while playing: turn the page when the playhead leaves the view
    double timeLimit() const;
    double laneTimeAt (int x) const;     // the time under x (in this component)
    int laneHeight (int index) const;
    ScrollBar trackScroll { true }, timeScroll { false };
    
private:
    void valueTreeChanged() override {}
    
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    
    void handleAsyncUpdate() override;
    void resized() override;
    
    void changeListenerCallback (ChangeBroadcaster*) override { repaint(); }
    void scrollBarMoved (ScrollBar*, double newStart) override;
    int contentHeight() const;

    
    void buildTracks();
    
    te::Edit& edit;
    
    EditViewState editViewState;
    
    PlayheadComponent playhead {edit, editViewState};
    TimeRuler ruler {edit, editViewState};
    OwnedArray<TrackComponent> tracks;
    OwnedArray<TrackHeaderComponent> headers;
    OwnedArray<TrackFooterComponent> footers;
    
    bool updateTracks = false, updateZoom = false;
    int headerW = 170, footerW = 0;
};

#include "Components.cpp"
