#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Project.h"

namespace tf
{
//==============================================================================
/** Waveform overviews of audio files (computed once in the background, 100 points per second). */
class PeakCache : public juce::ChangeBroadcaster
{
public:
    PeakCache();
    ~PeakCache() override;
    /** Peak (0..1) of file between two file times, or -1 while it is still being read. */
    float peak (const juce::File& f, double t0, double t1);
private:
    struct Entry { std::vector<float> data; bool ready = false; };
    juce::CriticalSection lock;
    std::map<juce::String, std::shared_ptr<Entry>> entries;
    juce::ThreadPool pool { juce::ThreadPoolOptions().withThreadName ("TF peaks").withNumberOfThreads (1) };
};

//==============================================================================
class TimelineView : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    TimelineView (Project&, AudioEngine&);
    ~TimelineView() override;

    juce::String getSelectedTrack() const { return selectedTrack; }
    void selectTrack (const juce::String& id);
    juce::Range<double> getSelection() const { return selection; }
    juce::String getSelectionTrack() const { return selectionTrack; }
    void clearSelection();
    /** Delete key: the selected part, or else the selected clip. */
    void deleteSelection();
    void zoomToFit();
    /** Scissors: splits the selected track's clip at the playhead (or every track with a clip there). */
    void splitAtPlayhead();
    juce::String getSelectedClip() const { return selectedClip; }
    void selectClip (const juce::String& trackId, const juce::String& clipId);

    std::function<void (const juce::String&)> onTrackSelected;
    std::function<void (const juce::String& trackId, juce::Range<double>)> onRedoPart;
    std::function<void (juce::Range<double>)> onExportPart;
    std::function<void (const juce::String&)> toast;
    /** Track actions offered on right-click that live elsewhere (clean-up, presets, speed...). */
    enum class Action { cleanNoise, cleanBleed, balance, preset, speed, key, changeMusic, exportTrack };
    std::function<void (Action, const juce::String& trackId, const juce::String& value)> onTrackAction;
    std::function<juce::StringPairArray()> presetList;   // id -> name, for the right-click menu

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    int getIdealHeight() const;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuildHeaders();
    void updateToolbar();
    double xToTime (float x) const;
    float timeToX (double t) const;
    int laneIndexAt (int y) const;
    juce::Rectangle<int> laneArea (int index) const;
    const ClipInfo* clipAt (int lane, double t, juce::String& trackIdOut) const;
    juce::Rectangle<float> clipRect (int lane, const ClipInfo& c) const;
    juce::Rectangle<float> closeRect (const juce::Rectangle<float>& clip) const;
    juce::Rectangle<float> scissorsRect() const;
    /** Hit test: which clip, and whether on its left/right edge (trim) or its x (delete). */
    struct Hit { const ClipInfo* clip = nullptr; juce::String trackId; int lane = -1; int edge = 0; bool close = false; };
    Hit hitTest (juce::Point<int> p) const;
    void showClipMenu (const Hit&, double t);
    void showTrackMenu (const juce::String& trackId);
    void updateVScroll();
    double visibleSeconds() const;
    void ensurePlayheadVisible();

    class TrackHeader;
    Project& project;
    AudioEngine& engine;
    PeakCache peaks;

    juce::OwnedArray<TrackHeader> headers;
    juce::Rectangle<int> toolbarArea, rulerArea, lanesArea, headerColumn;
    juce::ScrollBar hScroll { false };

    TFButton selectTool { juce::String::fromUTF8 ("বাছাই"), TFButton::Style::segment },
             moveTool { juce::String::fromUTF8 ("সরানো"), TFButton::Style::segment },
             cutBtn { juce::String::fromUTF8 ("কেটে ফেলুন") },
             redoPartBtn { juce::String::fromUTF8 ("এই অংশ আবার গাই") },
             loopBtn { juce::String::fromUTF8 ("লুপ") },
             exportPartBtn { juce::String::fromUTF8 ("অংশ সেভ") },
             addTrackBtn { juce::String::fromUTF8 ("কণ্ঠ ট্র্যাক") },
             undoBtn { juce::String::fromUTF8 ("আগের অবস্থা") }, redoBtn { juce::String::fromUTF8 ("আবার করুন") },
             zoomOutBtn { juce::String::fromUTF8 ("ছোট") }, zoomInBtn { juce::String::fromUTF8 ("বড়") };
    juce::Slider zoomSlider;
    juce::ScrollBar vScroll { true };
    int vOffset = 0;
    juce::Rectangle<int> toolWell;

    bool moveMode = false;
    double pxPerSec = 40.0, scrollSec = 0.0;
    juce::String selectedTrack, selectionTrack;
    juce::Range<double> selection;

    enum class Drag { none, select, move, seek, trim };
    Drag drag = Drag::none;
    juce::String selectedClip, selectedClipTrack;
    int trimEdge = 0;          // -1 left, +1 right
    double trimTo = 0.0;
    double dragAnchor = 0.0, dragOrigStart = 0.0, dragDelta = 0.0;
    juce::String dragTrack, dragClip;
    bool dragMoved = false;
    double lastPlayhead = -1.0;
    int lastLaneCount = -1;
    friend struct UITest;
};
}
