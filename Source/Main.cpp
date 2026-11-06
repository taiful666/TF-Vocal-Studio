#include "MainComponent.h"
#include "BinaryData.h"

namespace tf
{
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (juce::PropertiesFile& props)
        : DocumentWindow ("TF Vocal Studio", col::bg, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setIcon (juce::ImageCache::getFromMemory (TFData::icon256_png, TFData::icon256_pngSize));
        content = new MainComponent (props);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (1100, 720, 10000, 10000);

        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.toNearestInt();
        centreWithSize (juce::jmin (1440, area.getWidth() - 40), juce::jmin (900, area.getHeight() - 40));
        if (area.getWidth() < 1500 || area.getHeight() < 950)
            setFullScreen (true);   // maximised on smaller screens
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (content != nullptr && ! content->canQuit())
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle (juce::String::fromUTF8 ("রেকর্ডিং বা সেভ চলছে"))
                                              .withMessage (juce::String::fromUTF8 ("এখন বন্ধ করলে চলমান রেকর্ডিং সেভ হয়ে বন্ধ হবে। বন্ধ করবেন?"))
                                              .withButton (juce::String::fromUTF8 ("সেভ করে বন্ধ করুন"), 1)
                                              .withButton (juce::String::fromUTF8 ("বাতিল"), 0)
                                              .withAssociatedComponent (this),
                                          [] (int r) { if (r == 1) juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
            return;
        }
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    MainComponent* content = nullptr;
};
}

//==============================================================================
class TFVocalStudioApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "TF Vocal Studio"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "TF Vocal Studio";
        o.filenameSuffix = ".settings";
        o.folderName = "TF Vocal Studio";
        o.osxLibrarySubFolder = "Application Support";
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        appProps.setStorageParameters (o);

        lnf = std::make_unique<tf::TFLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lnf.get());

        window = std::make_unique<tf::MainWindow> (*appProps.getUserSettings());
        openFromCommandLine (commandLine);
    }

    void shutdown() override
    {
        window = nullptr;
        appProps.saveIfNeeded();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lnf = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr && window->content != nullptr)
            window->content->stopEverything();   // stops and saves any recording
        quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window != nullptr) { window->toFront (true); openFromCommandLine (commandLine); }
    }

private:
    void openFromCommandLine (const juce::String& cl)
    {
        auto path = cl.trim().unquoted();
        if (path.isNotEmpty() && juce::File::isAbsolutePath (path) && window != nullptr && window->content != nullptr)
            window->content->openFile (juce::File (path));
    }

    juce::ApplicationProperties appProps;
    std::unique_ptr<tf::TFLookAndFeel> lnf;
    std::unique_ptr<tf::MainWindow> window;
};

START_JUCE_APPLICATION (TFVocalStudioApp)
