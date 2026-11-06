#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace tf
{
//==============================================================================
/** A piece of an audio file placed on the timeline. Times in seconds. */
struct ClipInfo
{
    juce::String id;
    juce::File file;        // decodable audio (WAV cache, recording, or the original audio file)
    double start = 0.0;     // timeline position
    double in = 0.0;        // start inside the file
    double out = 0.0;       // end inside the file
    double length() const noexcept { return out - in; }
    double end() const noexcept { return start + length(); }
};

enum class TrackKind { music, vocal };

struct TrackInfo
{
    juce::String id, name;
    TrackKind kind = TrackKind::vocal;
    bool mute = false, solo = false;
    float gain = 0.8f;          // 0..1.5 (linear)
    float pan = 0.0f;           // -1..1
    juce::Array<ClipInfo> clips;

    // vocal tracks
    juce::String fx;            // FxParams::toString()
    juce::String presetId;

    // music tracks
    juce::File source;          // what the user added (audio or video) — lyrics/video belong to it
    juce::File baseAudio;       // decodable audio of the source at normal speed
    double speed = 1.0;         // tempo (pitch kept)
};

struct ProjectState
{
    juce::String name;
    juce::Array<TrackInfo> tracks;
    int nextId = 1;

    double length() const;                       // end of the last clip
    int indexOf (const juce::String& trackId) const;
    const TrackInfo* musicTrack() const;         // the first music track, if any
    /** Song-time (original music, normal speed) for a timeline time, or -1 outside the music. */
    double musicTimeAt (double timelineSeconds) const;
};

//==============================================================================
/** The open project: edits with undo/redo, saved automatically. Message thread only. */
class Project : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    Project();
    ~Project() override;

    const ProjectState& state() const noexcept { return st; }
    juce::File getFile() const noexcept { return file; }

    static juce::File projectsFolder();
    void newProject (const juce::String& name);
    bool open (const juce::File& f);
    void saveNow();

    // ---- edits (each one is undoable)
    juce::String addMusic (const juce::File& source, const juce::File& audio, double durationSeconds);
    juce::String addVocalTrack (const juce::String& name = {});
    void removeTrack (const juce::String& trackId);
    /** Places a recording; anything already on that track in the same time range is replaced. */
    void insertRecording (const juce::String& trackId, const juce::File& wav, double start, double in, double out);
    void deleteRange (const juce::String& trackId, juce::Range<double> range);
    void moveClip (const juce::String& trackId, const juce::String& clipId, double newStart);
    void setTrack (const TrackInfo& t, bool undoable = true);
    /** Swaps the music audio (tempo-changed or vocal-removed copy). newBase: the new normal-speed audio, if it changed. */
    void replaceMusicAudio (const juce::String& trackId, const juce::File& newAudio, double newSpeed, const juce::File& newBase = {});

    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    void undo();
    void redo();

    /** Begin an edit gesture that changes the state many times (e.g. dragging a clip): one undo step. */
    void beginGesture();
    void endGesture();

private:
    void timerCallback() override;
    void pushUndo();
    void changed();
    juce::String newId (const char* prefix);
    static void removeRangeFromClips (juce::Array<ClipInfo>& clips, juce::Range<double> r, ProjectState& s);

    ProjectState st;
    juce::File file;
    std::vector<ProjectState> undoStack, redoStack;
    bool inGesture = false, gestureSaved = false;
    bool dirty = false;
};

juce::String projectToXml (const ProjectState&);
bool projectFromXml (const juce::String&, ProjectState&);
}
