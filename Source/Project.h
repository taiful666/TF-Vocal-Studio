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

    // recordings only
    juce::File speaker;     // what the speakers played meanwhile (same timing): lets the music leak be removed
    juce::File camera;      // camera video recorded with it (may be empty)
    double cameraStart = 0; // timeline second of the camera video's first frame
    juce::String label;     // takes: "টেক ৩"
    double recordedAt = 0;  // takes: when (ms since 1970)
};

enum class TrackKind { music, vocal };

/** Every clip fades in and out over this time (no clicks); a re-sung part overlaps its neighbours by
    the same time, so the old and the new take cross-fade smoothly. */
constexpr double clipFadeSeconds = 0.010;

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
    int semitones = 0;          // key change (speed kept)

    // vocal tracks: every recording ever made on this track (to pick the best take of a part)
    juce::Array<ClipInfo> takes;
};

struct ProjectState
{
    juce::String name;
    juce::Array<TrackInfo> tracks;
    int nextId = 1;
    float smartMix = 1.0f;      // music steps back while singing (0 = off)
    bool glue = true;           // master glue compressor

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
    /** Forget the open project without writing it again (its file was deleted). */
    void discard() { stopTimer(); dirty = false; file = juce::File(); }

    // ---- edits (each one is undoable)
    juce::String addMusic (const juce::File& source, const juce::File& audio, double durationSeconds);
    juce::String addVocalTrack (const juce::String& name = {});
    void removeTrack (const juce::String& trackId);
    /** Places a recording; anything already on that track in the same time range is replaced. */
    /** crossfade: the new part overlaps the old take on both sides (a re-sung part; the file has audio around it). */
    void insertRecording (const juce::String& trackId, const juce::File& wav, double start, double in, double out, bool crossfade = false,
                          const juce::File& speakerRef = {}, const juce::File& camera = {}, double cameraStart = 0.0);
    void deleteRange (const juce::String& trackId, juce::Range<double> range);
    void moveClip (const juce::String& trackId, const juce::String& clipId, double newStart);
    /** Splits the clip under this time into two clips. Returns false if there is no clip there. */
    bool splitAt (const juce::String& trackId, double t);
    void deleteClip (const juce::String& trackId, const juce::String& clipId);
    /** Trims a clip's left (fromLeft) or right edge to timeline time t (the audio does not move). */
    void trimClip (const juce::String& trackId, const juce::String& clipId, bool fromLeft, double t);
    /** Puts take `take` (from the track's takes) in place for the time range of clip `clipId`. */
    void useTake (const juce::String& trackId, const juce::String& clipId, const ClipInfo& take);
    /** Replaces the audio file of clips (noise or leak removal): same timing, new file. */
    void replaceClipFiles (const juce::String& trackId, const std::map<juce::String, juce::File>& oldToNew);
    void setMix (float smartMix, bool glue);
    /** Changes the key (semitones) and the audio of a music track together. */
    void setMusicKey (const juce::String& trackId, const juce::File& newAudio, int semitones);
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

/** The project with only one kind of track audible (for "only voice" / "only music" exports).
    Volumes, mutes and effects stay exactly as set. */
ProjectState withOnly (const ProjectState&, TrackKind);

juce::String projectToXml (const ProjectState&);
bool projectFromXml (const juce::String&, ProjectState&);
}
