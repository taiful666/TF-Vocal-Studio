#include "LibraryView.h"

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
static const char* audioExts = "mp3;wav;m4a;aac;flac;ogg;wma;aif;aiff";

// column layout shared by header and rows: name 3fr, kind 2fr, lyrics 1.4fr, duration 90, action 170
struct Cols { juce::Rectangle<int> name, kind, lyr, dur, act; };
static Cols layoutCols (juce::Rectangle<int> r)
{
    Cols c;
    c.act = r.removeFromRight (170);
    r.removeFromRight (12);
    c.dur = r.removeFromRight (90);
    r.removeFromRight (12);
    const float total = 3.0f + 2.0f + 1.4f;
    const int w = r.getWidth() - 24;
    c.name = r.removeFromLeft ((int) (w * 3.0f / total)); r.removeFromLeft (12);
    c.kind = r.removeFromLeft ((int) (w * 2.0f / total)); r.removeFromLeft (12);
    c.lyr = r;
    return c;
}

//==============================================================================
class LibraryView::Row : public juce::Component
{
public:
    explicit Row (LibraryView& o) : owner (o)
    {
        action.setIcon (icons::play(), true, 14.0f);
        action.setRadius (10.0f);
        folder.setIcon (icons::folder(), false, 17.0f);
        folder.getProperties().set ("iconOnly", true);
        folder.setTooltip (u8 ("ফোল্ডারে দেখুন"));
        addAndMakeVisible (action);
        addAndMakeVisible (folder);
        action.onClick = [this] { activate(); };
        folder.onClick = [this]
        {
            if (auto* it = item()) (it->isTake ? it->take.folder : it->file).revealToUser();
        };
    }

    void update (int r) { row = r; if (auto* it = item()) action.setText (it->isTake ? u8 ("শুনুন") : u8 ("গাইব")); repaint(); }

    const Item* item() const { return juce::isPositiveAndBelow (row, owner.filtered.size()) ? &owner.filtered.getReference (row) : nullptr; }

    void activate()
    {
        if (auto* it = item())
        {
            if (it->isTake) { if (owner.onOpenTake) owner.onOpenTake (it->take); }
            else if (owner.onSing) owner.onSing (it->file);
        }
    }

    void resized() override
    {
        auto c = layoutCols (getLocalBounds().reduced (20, 0));
        auto a = c.act;
        folder.setBounds (a.removeFromRight (40).withSizeKeepingCentre (40, 40));
        a.removeFromRight (8);
        action.setBounds (a.removeFromRight (110).withSizeKeepingCentre (110, 40));
    }

    void paint (juce::Graphics& g) override
    {
        auto* it = item();
        if (it == nullptr) return;
        if (isMouseOver (true)) { g.setColour (juce::Colour (0xff1A1D24)); g.fillRect (getLocalBounds()); }
        g.setColour (juce::Colour (0xff1C1F27));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        auto c = layoutCols (getLocalBounds().reduced (20, 0));
        g.setColour (col::ink);
        g.setFont (font (16.0f, Weight::medium));
        g.drawText (it->name, c.name, juce::Justification::centredLeft, true);
        g.setColour (col::inkSoft);
        g.setFont (font (14.0f));
        g.drawText (it->kind, c.kind, juce::Justification::centredLeft, true);
        g.setColour (it->lyricsColour);
        g.drawText (it->lyricsText, c.lyr, juce::Justification::centredLeft, true);

        double d = it->duration;
        if (d < 0) { const juce::ScopedLock sl (owner.durLock); auto f = owner.durations.find ((it->isTake ? it->take.vocal : it->file).getFullPathName()); if (f != owner.durations.end()) d = f->second; }
        g.setColour (col::inkSoft);
        g.setFont (font (15.0f));
        g.drawText (d >= 0 ? bnTime (d) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")), c.dur, juce::Justification::centredRight);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { activate(); }
    void mouseUp (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) owner.showRowMenu (row); }

private:
    LibraryView& owner;
    int row = -1;
    TFButton action { u8 ("গাইব"), TFButton::Style::tool }, folder { u8 ("ফোল্ডার"), TFButton::Style::ghost };
};

//==============================================================================
LibraryView::LibraryView (AudioEngine& e) : engine (e)
{
    for (auto* b : { &songsTab, &takesTab })
    {
        b->setFontSize (15.0f);
        addAndMakeVisible (b);
    }
    songsTab.onClick = [this] { setTab (false); };
    takesTab.onClick = [this] { setTab (true); };

    addBtn.setIcon (icons::plus(), false, 18.0f);
    addBtn.onClick = [this] { chooseSongs(); };
    addAndMakeVisible (addBtn);

    folderBtn.setIcon (icons::folder(), false, 18.0f);
    folderBtn.onClick = [this] { (showingTakes ? AudioEngine::recordingsFolder() : AudioEngine::songsFolder()).revealToUser(); };
    addAndMakeVisible (folderBtn);

    search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("গানের নাম লিখে খুঁজুন…"), col::dim);
    search.setFont (font (15.0f));
    search.setIndents (40, 12);
    search.setTitle (u8 ("গান খুঁজুন"));
    search.onTextChange = [this] { applyFilter(); };
    addAndMakeVisible (search);

    list.setRowHeight (60);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.getViewport()->setScrollBarThickness (8);
    addAndMakeVisible (list);

    setTab (false);
}

LibraryView::~LibraryView() { durPool.removeAllJobs (true, 3000); }

void LibraryView::setTab (bool recordings)
{
    showingTakes = recordings;
    songsTab.setSelected (! recordings);
    takesTab.setSelected (recordings);
    folderBtn.setText (recordings ? u8 ("রেকর্ডিং ফোল্ডার খুলুন") : u8 ("গানের ফোল্ডার খুলুন"));
    refresh();
}

void LibraryView::showRecordings() { setTab (true); }

void LibraryView::refresh()
{
    items.clear();
    juce::Array<juce::File> toMeasure;

    if (! showingTakes)
    {
        for (auto& f : AudioEngine::songsFolder().findChildFiles (juce::File::findFiles, true))
        {
            if (! f.hasFileExtension (audioExts)) continue;
            Item it;
            it.file = f;
            it.name = f.getFileNameWithoutExtension();
            it.kind = f.getFileExtension().substring (1).toUpperCase() + u8 (" ট্র্যাক");
            auto lf = Lyrics::findFor (f);
            if (! lf.existsAsFile()) { it.lyricsText = u8 ("লিরিক্স নেই"); it.lyricsColour = col::danger; }
            else if (lf.hasFileExtension ("lrc")) { it.lyricsText = u8 ("সময় মেলানো"); it.lyricsColour = col::okText; }
            else { it.lyricsText = u8 ("শুধু লেখা"); it.lyricsColour = col::accentHi; }
            it.date = f.getLastModificationTime();
            items.add (it);
            toMeasure.add (f);
        }
        std::sort (items.begin(), items.end(), [] (const Item& a, const Item& b) { return a.name.compareNatural (b.name) < 0; });
    }
    else
    {
        for (auto& d : AudioEngine::recordingsFolder().findChildFiles (juce::File::findDirectories, false))
        {
            auto t = Take::load (d);
            if (! t.isValid()) continue;
            Item it;
            it.isTake = true;
            it.take = t;
            it.file = d;
            it.name = d.getFileName();
            it.kind = t.backing.existsAsFile() ? u8 ("গানের সাথে") : u8 ("শুধু কণ্ঠ");
            auto hasMix = ! d.findChildFiles (juce::File::findFiles, false, "*mix.wav").isEmpty();
            it.lyricsText = hasMix ? u8 ("মিক্স সেভ করা") : u8 ("শুধু রেকর্ডিং");
            it.lyricsColour = hasMix ? col::okText : col::muted;
            it.duration = t.lengthSeconds;
            it.date = d.getCreationTime();
            items.add (it);
        }
        std::sort (items.begin(), items.end(), [] (const Item& a, const Item& b) { return a.date > b.date; });
    }

    // measure song durations in the background
    juce::Component::SafePointer<LibraryView> safe (this);
    for (auto& f : toMeasure)
    {
        {
            const juce::ScopedLock sl (durLock);
            if (durations.count (f.getFullPathName()) > 0) continue;
        }
        durPool.addJob ([this, safe, f]
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            double d = 0.0;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (f) })
                d = r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
            if (safe == nullptr) return;
            { const juce::ScopedLock sl (durLock); durations[f.getFullPathName()] = d; }
            triggerAsyncUpdate();
        });
    }

    applyFilter();
}

void LibraryView::applyFilter()
{
    filtered.clear();
    auto q = search.getText().trim();
    for (auto& it : items)
        if (q.isEmpty() || it.name.containsIgnoreCase (q))
            filtered.add (it);
    list.updateContent();
    list.repaint();
    repaint();
}

juce::Component* LibraryView::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* r = dynamic_cast<Row*> (existing);
    if (r == nullptr) { delete existing; r = new Row (*this); }
    r->update (row);
    return r;
}

void LibraryView::chooseSongs()
{
    chooser = std::make_unique<juce::FileChooser> (u8 ("গান ও লিরিক্স ফাইল বাছুন (একসাথে কয়েকটা বাছতে পারেন)"),
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.wma;*.aif;*.aiff;*.lrc;*.txt");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc) { addSongs (fc.getResults()); });
}

void LibraryView::addSongs (const juce::Array<juce::File>& files)
{
    int added = 0;
    auto dest = AudioEngine::songsFolder();
    for (auto& f : files)
    {
        if (! f.existsAsFile()) continue;
        if (f.hasFileExtension (audioExts))
        {
            auto target = dest.getChildFile (f.getFileName());
            if (target.existsAsFile() && target.getSize() == f.getSize()) continue;
            target = target.getNonexistentSibling();
            if (f.copyFileTo (target)) ++added;
            // bring matching lyrics along
            auto lf = Lyrics::findFor (f);
            if (lf.existsAsFile()) lf.copyFileTo (target.withFileExtension (lf.getFileExtension()));
        }
        else if (f.hasFileExtension ("lrc;txt"))
        {
            f.copyFileTo (dest.getChildFile (f.getFileName()));
        }
    }
    if (added > 0 && toast) toast (bn (added) + u8 ("টি গান যোগ হয়েছে"));
    if (showingTakes) setTab (false); else refresh();
}

bool LibraryView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension (juce::String (audioExts) + ";lrc;txt")) return true;
    return false;
}

void LibraryView::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> fs;
    for (auto& f : files) fs.add (juce::File (f));
    addSongs (fs);
}

void LibraryView::showRowMenu (int row)
{
    if (! juce::isPositiveAndBelow (row, filtered.size())) return;
    auto it = filtered[row];
    juce::PopupMenu m;
    m.addItem (1, it.isTake ? u8 ("শুনুন") : u8 ("গাইব"));
    m.addItem (2, u8 ("ফোল্ডারে দেখুন"));
    m.addSeparator();
    m.addItem (3, u8 ("মুছে ফেলুন…"));
    m.showMenuAsync ({}, [this, it] (int r)
    {
        if (r == 1) { if (it.isTake) { if (onOpenTake) onOpenTake (it.take); } else if (onSing) onSing (it.file); }
        if (r == 2) (it.isTake ? it.take.folder : it.file).revealToUser();
        if (r == 3)
        {
            juce::Component::SafePointer<LibraryView> safe (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle (u8 ("মুছে ফেলবেন?"))
                                              .withMessage (u8 ("\"") + it.name + u8 ("\" রিসাইকেল বিনে চলে যাবে। পরে চাইলে সেখান থেকে ফেরত আনতে পারবেন।"))
                                              .withButton (u8 ("মুছে ফেলুন"), 1)
                                              .withButton (u8 ("বাতিল"), 0)
                                              .withAssociatedComponent (this),
                                          [safe, it] (int result)
            {
                if (result != 1 || safe == nullptr) return;
                if (it.isTake) it.take.folder.moveToTrash();
                else
                {
                    it.file.moveToTrash();
                    auto lf = Lyrics::findFor (it.file);
                    if (lf.existsAsFile()) lf.moveToTrash();
                }
                safe->refresh();
            });
        }
    });
}

void LibraryView::resized()
{
    auto r = getLocalBounds().reduced (28, 24);
    auto aside = r.removeFromRight (juce::jlimit (320, 420, (int) (r.getWidth() * 0.28f)));
    r.removeFromRight (20);
    asideCard = aside.withHeight (300);

    auto top = r.removeFromTop (48);
    tabWell = top.removeFromLeft (290).withSizeKeepingCentre (290, 46);
    auto tw = tabWell.reduced (4);
    songsTab.setBounds (tw.removeFromLeft (100));
    tw.removeFromLeft (2);
    takesTab.setBounds (tw);
    top.removeFromLeft (12);
    addBtn.setBounds (top.removeFromRight (176));
    top.removeFromRight (12);
    searchArea = top;
    search.setBounds (top);

    r.removeFromTop (16);
    listCard = r;
    headerRow = listCard.withHeight (46);
    list.setBounds (listCard.withTrimmedTop (46).reduced (1, 0).withTrimmedBottom (8));

    folderBtn.setBounds (asideCard.reduced (20).removeFromBottom (46));
}

void LibraryView::paintOverChildren (juce::Graphics& g)
{
    drawIcon (g, icons::search(), juce::Rectangle<float> (18, 18).withCentre ({ (float) searchArea.getX() + 22.0f, (float) searchArea.getCentreY() }), col::dim, false, 2.0f);
}

void LibraryView::paint (juce::Graphics& g)
{
    // tab well
    g.setColour (col::well);
    g.fillRoundedRectangle (tabWell.toFloat(), 12.0f);
    g.setColour (col::border);
    g.drawRoundedRectangle (tabWell.toFloat().reduced (0.5f), 12.0f, 1.0f);


    // list card
    auto lc = listCard.toFloat().reduced (0.5f);
    g.setColour (col::panel);  g.fillRoundedRectangle (lc, 18.0f);
    g.setColour (col::border); g.drawRoundedRectangle (lc, 18.0f, 1.0f);
    g.fillRect (listCard.getX(), headerRow.getBottom() - 1, listCard.getWidth(), 1);

    auto c = layoutCols (headerRow.reduced (20, 0));
    g.setColour (col::muted);
    g.setFont (font (14.0f));
    g.drawText (showingTakes ? u8 ("রেকর্ডিং") : u8 ("গান"), c.name, juce::Justification::centredLeft);
    g.drawText (u8 ("ধরন"), c.kind, juce::Justification::centredLeft);
    g.drawText (showingTakes ? u8 ("অবস্থা") : u8 ("লিরিক্স"), c.lyr, juce::Justification::centredLeft);
    g.drawText (u8 ("সময়"), c.dur, juce::Justification::centredRight);

    if (filtered.isEmpty())
    {
        auto e = listCard.withTrimmedTop (46).toFloat();
        g.setColour (col::ink);
        g.setFont (font (20.0f, Weight::semibold));
        juce::String title, sub;
        if (search.getText().isNotEmpty()) { title = u8 ("কিছু পাওয়া যায়নি"); sub = u8 ("অন্য নাম লিখে খুঁজে দেখুন"); }
        else if (showingTakes) { title = u8 ("এখনো কোনো রেকর্ডিং নেই"); sub = u8 ("স্টুডিওতে গিয়ে রেকর্ড বাটনে চাপুন"); }
        else { title = u8 ("লাইব্রেরিতে এখনো গান নেই"); sub = u8 ("\"গান যোগ করুন\" চাপুন, বা গানের ফাইল এখানে টেনে আনুন"); }
        g.drawText (title, e.withY (e.getCentreY() - 36).withHeight (34), juce::Justification::centred);
        g.setColour (col::muted);
        g.setFont (font (15.0f));
        g.drawText (sub, e.withY (e.getCentreY() + 2).withHeight (26), juce::Justification::centred);
    }

    // aside
    auto ac = asideCard.toFloat().reduced (0.5f);
    g.setColour (col::panel);  g.fillRoundedRectangle (ac, 18.0f);
    g.setColour (col::border); g.drawRoundedRectangle (ac, 18.0f, 1.0f);
    auto a = asideCard.reduced (20);
    g.setColour (col::ink);
    g.setFont (font (18.0f, Weight::semibold));
    g.drawText (u8 ("লিরিক্স যেভাবে আসবে"), a.removeFromTop (30), juce::Justification::centredLeft);
    a.removeFromTop (6);
    g.setColour (col::muted);
    g.setFont (font (15.0f));
    g.drawFittedText (u8 ("গানের ফাইলের পাশে একই নামের .lrc ফাইল রাখুন — যেমন \"আমার গান.mp3\" আর \"আমার গান.lrc\"। "
                          "তাহলে গান চলার সাথে লিরিক্স নিজে থেকে স্ক্রল হবে আর গাওয়া অংশ সোনালি হবে।\n\n"
                          "গান যোগ করার সময় একই নামের লিরিক্স ফাইল থাকলে সেটাও সাথে চলে আসবে।"),
                      a.withTrimmedBottom (60), juce::Justification::topLeft, 12, 1.0f);
}
}
