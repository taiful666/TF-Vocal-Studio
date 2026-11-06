#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace tf
{
/** In-app updates from the official GitHub releases of TF Vocal Studio.
    Safe by design: HTTPS only, only this project's own release files, and the downloaded Setup.exe
    must match the SHA-256 published with the release before it is run. */
class Updater : private juce::Thread
{
public:
    struct Release
    {
        juce::String version, notes, setupUrl, sha256;   // sha256: lower-case hex
        juce::int64 size = 0;
        bool isValid() const { return version.isNotEmpty() && setupUrl.isNotEmpty() && sha256.length() == 64; }
    };

    Updater();
    ~Updater() override;

    /** Looks for a newer release in the background. done(found, release, error) on the message thread. */
    void check (std::function<void (bool, Release, juce::String)> done);
    /** Downloads and verifies the installer. progress 0..1; done(ok, setupFile or error) on the message thread. */
    void download (const Release&, std::function<void (float)> progress, std::function<void (bool, juce::String)> done);
    void cancel() { signalThreadShouldExit(); }
    bool isBusy() const { return isThreadRunning(); }
    /** Starts the installer silently (it reopens the app when done). The caller then quits. */
    static bool runInstaller (const juce::File& setup);

    // ---- pieces (public for the tests)
    static constexpr const char* repoApi = "https://api.github.com/repos/taiful666/TF-Vocal-Studio/releases/latest";
    static bool isNewer (const juce::String& candidate, const juce::String& current);
    /** Reads the GitHub "latest release" JSON; sha256File: contents of the .sha256 asset, if any. */
    static Release parseRelease (const juce::String& json);
    static bool isTrustedUrl (const juce::String& url);
    static juce::String sha256Of (const juce::File&);

private:
    void run() override;
    enum class Job { none, check, download } job = Job::none;
    Release target;
    std::function<void (bool, Release, juce::String)> checkDone;
    std::function<void (float)> progressCb;
    std::function<void (bool, juce::String)> downloadDone;
};
}
