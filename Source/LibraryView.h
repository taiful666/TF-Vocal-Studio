#pragma once

#include "Theme.h"
#include "AudioEngine.h"
#include "Lyrics.h"
#include "Media.h"
#include "Project.h"

namespace tf
{
class LibraryView : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private juce::ListBoxModel,
                    private juce::AsyncUpdater
{
public:
    explicit LibraryView (AudioEngine&);
    ~LibraryView() override;

    std::function<void (const juce::File&)> onSing;
    std::function<void (const juce::File&)> onOpenProject;
    std::function<void()> onNewProject;
    std::function<void (const juce::String&)> toast;

    void refresh();
    void showRecordings();
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override { if (isVisible()) refresh(); }

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    struct Item
    {
        juce::File file;          // song file, or project file (.tfvs)
        bool isTake = false;      // true = a project
        juce::String name, kind, lyricsText;
        juce::Colour lyricsColour;
        double duration = -1.0;
        juce::Time date;
    };

private:
    int getNumRows() override { return filtered.size(); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;
    void handleAsyncUpdate() override { list.repaint(); for (int i = 0; i < getNumRows(); ++i) if (auto* c = list.getComponentForRowNumber (i)) c->repaint(); }

    void applyFilter();
    void addSongs (const juce::Array<juce::File>&);
    void chooseSongs();
    void showRowMenu (int row);
    void separate (const juce::File& song);
    void chooseForSeparation();
    void setTab (bool recordings);
    void renameItem (const Item& it);
    juce::String songName (const juce::File& f) const;
    juce::StringPairArray loadNames() const;
    void saveName (const juce::File& f, const juce::String& name);

    class Row;
    friend class Row;

    AudioEngine& engine;
    bool showingTakes = false;
    juce::Array<Item> items, filtered;

    TFButton songsTab { juce::String::fromUTF8 ("গান"), TFButton::Style::segment },
             takesTab { juce::String::fromUTF8 ("আমার প্রজেক্ট"), TFButton::Style::segment },
             addBtn   { juce::String::fromUTF8 ("গান যোগ করুন"), TFButton::Style::accent },
             folderBtn { juce::String::fromUTF8 ("গানের ফোল্ডার খুলুন") },
             sepBtn { juce::String::fromUTF8 ("গান বাছুন"), TFButton::Style::accent },
             sepCancel { juce::String::fromUTF8 ("বাতিল"), TFButton::Style::tool };
    juce::TextEditor search;
    juce::ListBox list { "songs", this };
    juce::Rectangle<int> tabWell, listCard, asideCard, headerRow, searchArea;

    // durations are read on a background thread (MP3 length needs a scan)
    juce::CriticalSection durLock;
    std::map<juce::String, double> durations;
    juce::ThreadPool durPool { juce::ThreadPoolOptions().withThreadName ("TF durations").withNumberOfThreads (1) };
    std::unique_ptr<juce::FileChooser> chooser;

    // AI separation / copying jobs
    juce::ThreadPool jobPool { juce::ThreadPoolOptions().withThreadName ("TF library jobs").withNumberOfThreads (1) };
    std::atomic<bool> sepBusy { false }, sepCancelFlag { false };
    float sepProgress = 0.0f;
    juce::String sepStatus, sepName;
    juce::Rectangle<int> sepBar;
};
}
