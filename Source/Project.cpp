#include "Project.h"
#include "Media.h"
#include "VocalFx.h"
#include "Theme.h"

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
    x.setAttribute ("smartMix", s.smartMix);
    x.setAttribute ("glue", s.glue);
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
        e->setAttribute ("semitones", t.semitones);
        auto writeClip = [] (juce::XmlElement* ce, const ClipInfo& c)
        {
            ce->setAttribute ("id", c.id);
            ce->setAttribute ("file", fileStr (c.file));
            ce->setAttribute ("start", c.start);
            ce->setAttribute ("in", c.in);
            ce->setAttribute ("out", c.out);
            if (c.speaker != juce::File()) ce->setAttribute ("speaker", fileStr (c.speaker));
            if (c.camera != juce::File()) { ce->setAttribute ("camera", fileStr (c.camera)); ce->setAttribute ("cameraStart", c.cameraStart); }
            if (c.label.isNotEmpty()) ce->setAttribute ("label", c.label);
            if (c.recordedAt > 0) ce->setAttribute ("recordedAt", c.recordedAt);
        };
        for (auto& c : t.clips) writeClip (e->createNewChildElement ("Clip"), c);
        for (auto& c : t.takes) writeClip (e->createNewChildElement ("Take"), c);
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
    r.smartMix = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("smartMix", 1.0));
    r.glue = x->getBoolAttribute ("glue", true);
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
        t.semitones = juce::jlimit (-12, 12, e->getIntAttribute ("semitones", 0));
        auto readClip = [] (const juce::XmlElement* ce)
        {
            ClipInfo c;
            c.id = ce->getStringAttribute ("id");
            c.file = strFile (ce->getStringAttribute ("file"));
            c.start = juce::jmax (0.0, ce->getDoubleAttribute ("start"));
            c.in = juce::jmax (0.0, ce->getDoubleAttribute ("in"));
            c.out = ce->getDoubleAttribute ("out");
            c.speaker = strFile (ce->getStringAttribute ("speaker"));
            c.camera = strFile (ce->getStringAttribute ("camera"));
            c.cameraStart = ce->getDoubleAttribute ("cameraStart");
            c.label = ce->getStringAttribute ("label");
            c.recordedAt = ce->getDoubleAttribute ("recordedAt");
            return c;
        };
        for (auto* ce : e->getChildWithTagNameIterator ("Clip"))
            if (auto c = readClip (ce); c.file.existsAsFile() && c.length() > 0.0) t.clips.add (c);
        for (auto* ce : e->getChildWithTagNameIterator ("Take"))
            if (auto c = readClip (ce); c.file.existsAsFile() && c.length() > 0.0) t.takes.add (c);
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
    t.semitones = 0;
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

void Project::insertRecording (const juce::String& trackId, const juce::File& wav, double start, double in, double out, bool crossfade,
                               const juce::File& speakerRef, const juce::File& camera, double cameraStart)
{
    const int i = st.indexOf (trackId);
    if (i < 0 || out - in <= 0.01) return;
    pushUndo();
    auto t = st.tracks.getReference (i);
    const double end = start + (out - in);
    const double h = crossfade && (out - in) > 4 * clipFadeSeconds && in >= clipFadeSeconds ? clipFadeSeconds * 0.5 : 0.0;
    // the old take keeps h seconds inside the new part on each side; the new part reaches h outside it
    removeRangeFromClips (t.clips, { start + h, end - h }, st);
    ClipInfo c;
    c.id = newId ("c"); c.file = wav; c.start = start - h; c.in = in - h; c.out = out + h;
    c.speaker = speakerRef; c.camera = camera; c.cameraStart = cameraStart;
    t.clips.add (c);
    // keep every recording as a take, so a better earlier one can be picked later
    auto take = c;
    take.id = newId ("k");
    take.recordedAt = (double) juce::Time::currentTimeMillis();
    take.label = juce::String::fromUTF8 ("টেক ") + bn (t.takes.size() + 1);
    t.takes.add (take);
    st.tracks.set (i, t);
    changed();
}

bool Project::splitAt (const juce::String& trackId, double tl)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return false;
    auto t = st.tracks.getReference (i);
    for (int k = 0; k < t.clips.size(); ++k)
    {
        auto c = t.clips.getReference (k);
        if (tl <= c.start + 0.02 || tl >= c.end() - 0.02) continue;
        pushUndo();
        auto a = c, b = c;
        a.out = c.in + (tl - c.start);
        b.id = newId ("c");
        b.in = a.out;
        b.start = tl;
        t.clips.set (k, a);
        t.clips.insert (k + 1, b);
        st.tracks.set (i, t);
        changed();
        return true;
    }
    return false;
}

void Project::deleteClip (const juce::String& trackId, const juce::String& clipId)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return;
    auto t = st.tracks.getReference (i);
    for (int k = 0; k < t.clips.size(); ++k)
        if (t.clips.getReference (k).id == clipId)
        {
            pushUndo();
            t.clips.remove (k);
            st.tracks.set (i, t);
            changed();
            return;
        }
}

void Project::trimClip (const juce::String& trackId, const juce::String& clipId, bool fromLeft, double tl)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return;
    auto t = st.tracks.getReference (i);
    for (int k = 0; k < t.clips.size(); ++k)
    {
        auto c = t.clips.getReference (k);
        if (c.id != clipId) continue;
        if (fromLeft)
        {
            // the left edge can go back only as far as the file has audio (in >= 0)
            const double nt = juce::jlimit (c.start - c.in, c.end() - 0.05, tl);
            c.in += nt - c.start;
            c.start = nt;
        }
        else
        {
            double fileLen = c.out;
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (c.file) }) fileLen = (double) r->lengthInSamples / r->sampleRate;
            const double ne = juce::jlimit (c.start + 0.05, c.start + (fileLen - c.in), tl);
            c.out = c.in + (ne - c.start);
        }
        pushUndo();
        t.clips.remove (k);
        removeRangeFromClips (t.clips, { c.start, c.end() }, st);
        t.clips.add (c);
        st.tracks.set (i, t);
        changed();
        return;
    }
}

void Project::useTake (const juce::String& trackId, const juce::String& clipId, const ClipInfo& take)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return;
    const auto& tr = st.tracks.getReference (i);
    const ClipInfo* cur = nullptr;
    for (auto& c : tr.clips) if (c.id == clipId) cur = &c;
    if (cur == nullptr) return;
    // the part of the take that covers this clip's time
    const double a = juce::jmax (cur->start, take.start), b = juce::jmin (cur->end(), take.end());
    if (b - a < 0.05) return;
    const double in = take.in + (a - take.start);
    auto keepTakes = tr.takes;
    // cross-fade into the neighbours when the take file has audio around this part
    insertRecording (trackId, take.file, a, in, in + (b - a), in >= clipFadeSeconds, take.speaker, take.camera, take.cameraStart);
    // choosing a take must not add a new take
    const int j = st.indexOf (trackId);
    if (j >= 0) { auto t = st.tracks.getReference (j); t.takes = keepTakes; st.tracks.set (j, t); changed(); }
}

void Project::replaceClipFiles (const juce::String& trackId, const std::map<juce::String, juce::File>& m)
{
    const int i = st.indexOf (trackId);
    if (i < 0 || m.empty()) return;
    pushUndo();
    auto t = st.tracks.getReference (i);
    for (auto& c : t.clips)
        if (auto it = m.find (c.file.getFullPathName()); it != m.end()) c.file = it->second;
    st.tracks.set (i, t);
    changed();
}

void Project::setMix (float smartMix, bool glue)
{
    pushUndo();
    st.smartMix = juce::jlimit (0.0f, 1.0f, smartMix);
    st.glue = glue;
    changed();
}

void Project::setMusicKey (const juce::String& trackId, const juce::File& newAudio, int semis)
{
    const int i = st.indexOf (trackId);
    if (i < 0) return;
    pushUndo();
    auto t = st.tracks.getReference (i);
    for (auto& c : t.clips) c.file = newAudio;
    t.semitones = semis;
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

ProjectState withOnly (const ProjectState& s, TrackKind k)
{
    auto r = s;
    bool soloInKind = false;
    for (auto& t : r.tracks) if (t.kind == k && t.solo) soloInKind = true;
    for (auto& t : r.tracks)
    {
        if (t.kind != k) { t.mute = true; t.solo = false; }
        else if (! soloInKind) t.solo = false;
    }
    return r;
}
}
