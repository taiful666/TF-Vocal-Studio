#include "Project.h"
#include "Media.h"
#include "VocalFx.h"

namespace tf
{
//==============================================================================
double ProjectState::length() const
{
    double l = 0.0;
    for (auto& t : tracks)
        for (auto& c : t.clips) l = juce::jmax (l, c.end());
    return l;
}

int ProjectState::indexOf (const juce::String& id) const
{
    for (int i = 0; i < tracks.size(); ++i)
        if (tracks.getReference (i).id == id) return i;
    return -1;
}

const TrackInfo* ProjectState::musicTrack() const
{
    for (auto& t : tracks)
        if (t.kind == TrackKind::music) return &t;
    return nullptr;
}

double ProjectState::musicTimeAt (double t) const
{
    auto* m = musicTrack();
    if (m == nullptr) return -1.0;
    for (auto& c : m->clips)
        if (t >= c.start && t < c.end())
            return (c.in + (t - c.start)) * m->speed;   // clip times are in the (stretched) file's seconds
    // between/after clips: extrapolate from the nearest clip so lyrics keep moving smoothly
    const ClipInfo* best = nullptr;
    for (auto& c : m->clips) if (c.start <= t && (best == nullptr || c.start > best->start)) best = &c;
    if (best != nullptr) return (best->in + (t - best->start)) * m->speed;
    if (! m->clips.isEmpty()) return (m->clips.getReference (0).in + (t - m->clips.getReference (0).start)) * m->speed;
    return -1.0;
}

//==============================================================================
static juce::String fileStr (const juce::File& f) { return f == juce::File() ? juce::String() : f.getFullPathName(); }
static juce::File strFile (const juce::String& s) { return s.isNotEmpty() && juce::File::isAbsolutePath (s) ? juce::File (s) : juce::File(); }

juce::String projectToXml (const ProjectState& s)
{
    juce::XmlElement x ("TFVocalProject");
    x.setAttribute ("version", 2);
    x.setAttribute ("name", s.name);
    x.setAttribute ("nextId", s.nextId);
    for (auto& t : s.tracks)
    {
        auto* e = x.createNewChildElement ("Track");
        e->setAttribute ("id", t.id);
        e->setAttribute ("name", t.name);
        e->setAttribute ("kind", t.kind == TrackKind::music ? "music" : "vocal");
        e->setAttribute ("mute", t.mute);
        e->setAttribute ("solo", t.solo);
        e->setAttribute ("gain", t.gain);
        e->setAttribute ("pan", t.pan);
        e->setAttribute ("fx", t.fx);
        e->setAttribute ("preset", t.presetId);
        e->setAttribute ("source", fileStr (t.source));
        e->setAttribute ("baseAudio", fileStr (t.baseAudio));
        e->setAttribute ("speed", t.speed);
        for (auto& c : t.clips)
        {
            auto* ce = e->createNewChildElement ("Clip");
            ce->setAttribute ("id", c.id);
            ce->setAttribute ("file", fileStr (c.file));
            ce->setAttribute ("start", c.start);
            ce->setAttribute ("in", c.in);
            ce->setAttribute ("out", c.out);
        }
    }
    return x.toString();
}

bool projectFromXml (const juce::String& text, ProjectState& s)
{
    auto x = juce::XmlDocument::parse (text);
    if (x == nullptr || ! x->hasTagName ("TFVocalProject")) return false;
    ProjectState r;
    r.name = x->getStringAttribute ("name");
    r.nextId = x->getIntAttribute ("nextId", 1);
    for (auto* e : x->getChildWithTagNameIterator ("Track"))
    {
        TrackInfo t;
        t.id = e->getStringAttribute ("id");
        t.name = e->getStringAttribute ("name");
        t.kind = e->getStringAttribute ("kind") == "music" ? TrackKind::music : TrackKind::vocal;
        t.mute = e->getBoolAttribute ("mute");
        t.solo = e->getBoolAttribute ("solo");
        t.gain = (float) juce::jlimit (0.0, 1.5, e->getDoubleAttribute ("gain", 0.8));
        t.pan = (float) juce::jlimit (-1.0, 1.0, e->getDoubleAttribute ("pan"));
        t.fx = e->getStringAttribute ("fx");
        t.presetId = e->getStringAttribute ("preset");
        t.source = strFile (e->getStringAttribute ("source"));
        t.baseAudio = strFile (e->getStringAttribute ("baseAudio"));
        t.speed = juce::jlimit (0.5, 1.5, e->getDoubleAttribute ("speed", 1.0));
        for (auto* ce : e->getChildWithTagNameIterator ("Clip"))
        {
            ClipInfo c;
            c.id = ce->getStringAttribute ("id");
            c.file = strFile (ce->getStringAttribute ("file"));
            c.start = ce->getDoubleAttribute ("start");
            c.in = ce->getDoubleAttribute ("in");
            c.out = ce->getDoubleAttribute ("out");
            if (c.file.existsAsFile() && c.length() > 0.0) t.clips.add (c);
        }
        r.tracks.add (t);
    }
    s = r;
    return true;
}

//==============================================================================
Project::Project() {}
Project::~Project() { if (dirty) saveNow(); }

juce::File Project::projectsFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TF Vocal Studio").getChildFile ("Projects");
    f.createDirectory();
    return f;
}

void Project::newProject (const juce::String& name)
{
    if (dirty) saveNow();
    st = {};
    st.name = name;
    undoStack.clear(); redoStack.clear();
    file = projectsFolder().getChildFile (juce::File::createLegalFileName (name).substring (0, 60) + " "
                                          + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".tfvs").getNonexistentSibling();
    addVocalTrack();
    undoStack.clear();
    stopTimer();
    dirty = false;          // an untouched new project is not written to disk
    sendChangeMessage();
}

bool Project::open (const juce::File& f)
{
    ProjectState s;
    if (! projectFromXml (f.loadFileAsString(), s)) return false;
    if (dirty) saveNow();
    st = s;
    file = f;
    undoStack.clear(); redoStack.clear();
    dirty = false;
    sendChangeMessage();
    return true;
}

void Project::saveNow()
{
    if (file == juce::File()) return;
    auto tmp = file.getSiblingFile (file.getFileName() + ".tmp");
    if (tmp.replaceWithText (projectToXml (st)) && tmp.moveFileTo (file))   // atomic replace: never a half-written project
        dirty = false;
    stopTimer();
}

void Project::timerCallback() { saveNow(); }

void Project::pushUndo()
{
    if (inGesture) { if (gestureSaved) return; gestureSaved = true; }
    undoStack.push_back (st);
    if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void Project::changed()
{
    dirty = true;
    startTimer (800);   // auto-save shortly after the last change
    sendChangeMessage();
}

juce::String Project::newId (const char* prefix) { return juce::String (prefix) + juce::String (st.nextId++); }

void Project::beginGesture() { inGesture = true; gestureSaved = false; }
void Project::endGesture()   { inGesture = false; gestureSaved = false; }

void Project::undo()
{
    if (undoStack.empty()) return;
    redoStack.push_back (st);
    st = undoStack.back();
    undoStack.pop_back();
    changed();
}

void Project::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back (st);
    st = redoStack.back();
    redoStack.pop_back();
    changed();
}

juce::String Project::addMusic (const juce::File& source, const juce::File& audio, double dur)
{
    pushUndo();
    // one music track: replace its content
    int idx = -1;
    for (int i = 0; i < st.tracks.size(); ++i) if (st.tracks.getReference (i).kind == TrackKind::music) { idx = i; break; }
    TrackInfo t;
    if (idx >= 0) t = st.tracks.getReference (idx);
    else { t.id = newId ("t"); t.kind = TrackKind::music; t.gain = 0.65f; }
    t.name = juce::String::fromUTF8 ("মিউজিক");
    t.source = source;
    t.baseAudio = audio;
    t.speed = 1.0;
    t.clips.clear();
    ClipInfo c;
    c.id = newId ("c"); c.file = audio; c.start = 0.0; c.in = 0.0; c.out = dur;
    t.clips.add (c);
    if (idx >= 0) st.tracks.set (idx, t); else st.tracks.insert (0, t);
    if (st.name.isEmpty() || st.name.startsWith (juce::String::fromUTF8 ("নতুন"))) st.name = media::displayName (source);
    changed();
    return t.id;
}

juce::String Project::addVocalTrack (const juce::String& name)
{
    pushUndo();
    TrackInfo t;
    t.id = newId ("t");
    t.kind = TrackKind::vocal;
    int n = 0;
    for (auto& x : st.tracks) if (x.kind == TrackKind::vocal) ++n;
    juce::String num (n + 1);
    static const char* digits[] = { "০", "১", "২", "৩", "৪", "৫", "৬", "৭", "৮", "৯" };
    juce::String bnNum;
    for (auto ch : num) bnNum << juce::String::fromUTF8 (digits[ch - '0']);
    t.name = name.isNotEmpty() ? name : juce::String::fromUTF8 ("কণ্ঠ ") + bnNum;
    t.gain = 1.0f;
    // new voice tracks start from the last voice track's sound
    for (int i = st.tracks.size(); --i >= 0;)
        if (st.tracks.getReference (i).kind == TrackKind::vocal) { t.fx = st.tracks.getReference (i).fx; t.presetId = st.tracks.getReference (i).presetId; break; }
    if (t.fx.isEmpty())
    {
        // the very first voice: the warm ghazal preset (a good start for most songs and recitation)
        FxParams f;
        for (auto& b : builtInPresets()) if (b.id == "ghazal") { b.applyTo (f); t.presetId = b.id; }
        t.fx = f.toString();
    }
    st.tracks.add (t);
    changed();
    return t.id;
}

void Project::removeTrack (const juce::String& id)
{
    const int i = st.indexOf (id);
    if (i < 0) return;
    pushUndo();
    st.tracks.remove (i);
    changed();
}

void Project::removeRangeFromClips (juce::Array<ClipInfo>& clips, juce::Range<double> r, ProjectState& s)
{
    juce::Array<ClipInfo> out;
    for (auto& c : clips)
    {
        if (c.end() <= r.getStart() || c.start >= r.getEnd()) { out.add (c); continue; }
        if (c.start < r.getStart())
        {
            auto a = c;
            a.out = c.in + (r.getStart() - c.start);
            if (a.length() > 0.005) out.add (a);
        }
        if (c.end() > r.getEnd())
        {
            auto b = c;
            b.id = "c" + juce::String (s.nextId++);
            b.in = c.in + (r.getEnd() - c.start);
            b.start = r.getEnd();
            if (b.length() > 0.005) out.add (b);
        }
    }
    clips = out;
}

void Project::insertRecording (const juce::String& trackId, const juce::File& wav, double start, double in, double out)
{
    const int i = st.indexOf (trackId);
    if (i < 0 || out - in <= 0.01) return;
    pushUndo();
    auto t = st.tracks.getReference (i);
    removeRangeFromClips (t.clips, { start, start + (out - in) }, st);
    ClipInfo c;
    c.id = newId ("c"); c.file = wav; c.start = start; c.in = in; c.out = out;
    t.clips.add (c);
    st.tracks.set (i, t);
    changed();
}

void Project::deleteRange (const juce::String& trackId, juce::Range<double> r)
{
    const int i = st.indexOf (trackId);
    if (i < 0 || r.getLength() <= 0.0) return;
    pushUndo();
    auto t = st.tracks.getReference (i);
    removeRangeFromClips (t.clips, r, st);
    st.tracks.set (i, t);
    changed();
}

void Project::moveClip (const juce::String& trackId, const juce::String& clipId, double newStart)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return;
    auto t = st.tracks.getReference (i);
    for (int k = 0; k < t.clips.size(); ++k)
        if (t.clips.getReference (k).id == clipId)
        {
            pushUndo();
            auto c = t.clips.getReference (k);
            t.clips.remove (k);
            c.start = juce::jmax (0.0, newStart);
            removeRangeFromClips (t.clips, { c.start, c.end() }, st);   // the moved clip wins where they overlap
            t.clips.add (c);
            st.tracks.set (i, t);
            changed();
            return;
        }
}

void Project::setTrack (const TrackInfo& t, bool undoable)
{
    const int i = st.indexOf (t.id);
    if (i < 0) return;
    if (undoable) pushUndo();
    st.tracks.set (i, t);
    changed();
}

void Project::replaceMusicAudio (const juce::String& trackId, const juce::File& newAudio, double newSpeed, const juce::File& newBase)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return;
    pushUndo();
    auto t = st.tracks.getReference (i);
    const double f = t.speed / newSpeed;   // file seconds scale with 1/speed
    const double origin = t.clips.isEmpty() ? 0.0 : t.clips.getReference (0).start;
    for (auto& c : t.clips)
    {
        c.file = newAudio;
        c.in *= f;
        c.out *= f;
        c.start = origin + (c.start - origin) * f;
    }
    t.speed = newSpeed;
    if (newBase != juce::File()) t.baseAudio = newBase;
    st.tracks.set (i, t);
    changed();
}
}
