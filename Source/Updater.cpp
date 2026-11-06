#include "Updater.h"
#include <juce_cryptography/juce_cryptography.h>

namespace tf
{
static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
static const juce::String downloadPrefix ("https://github.com/taiful666/TF-Vocal-Studio/releases/download/");

Updater::Updater() : juce::Thread ("TF updater") {}
Updater::~Updater() { signalThreadShouldExit(); stopThread (8000); }

bool Updater::isNewer (const juce::String& candidate, const juce::String& current)
{
    auto parts = [] (juce::String v)
    {
        v = v.trim().trimCharactersAtStart ("vV");
        auto t = juce::StringArray::fromTokens (v, ".", "");
        std::array<int, 3> n { 0, 0, 0 };
        for (int i = 0; i < 3 && i < t.size(); ++i) n[(size_t) i] = t[i].retainCharacters ("0123456789").getIntValue();
        return n;
    };
    const auto a = parts (candidate), b = parts (current);
    return a > b;
}

bool Updater::isTrustedUrl (const juce::String& url)
{
    // only this project's own release files, over HTTPS
    return url.startsWith (downloadPrefix) && ! url.contains ("..") && ! url.containsAnyOf (" \\\"'<>");
}

Updater::Release Updater::parseRelease (const juce::String& json)
{
    Release r;
    auto v = juce::JSON::parse (json);
    if (! v.isObject()) return r;
    if ((bool) v.getProperty ("draft", false) || (bool) v.getProperty ("prerelease", false)) return r;
    r.version = v.getProperty ("tag_name", {}).toString().trimCharactersAtStart ("vV");
    r.notes = v.getProperty ("body", {}).toString().substring (0, 2000);
    if (auto* assets = v.getProperty ("assets", {}).getArray())
    {
        for (auto& a : *assets)
        {
            const auto name = a.getProperty ("name", {}).toString();
            const auto url = a.getProperty ("browser_download_url", {}).toString();
            if (name.startsWith ("TF-Vocal-Studio-Setup-") && name.endsWithIgnoreCase (".exe") && isTrustedUrl (url))
            {
                r.setupUrl = url;
                r.size = (juce::int64) a.getProperty ("size", 0);
                // GitHub publishes each file's checksum as "digest": "sha256:<hex>"
                auto d = a.getProperty ("digest", {}).toString();
                if (d.startsWithIgnoreCase ("sha256:")) r.sha256 = d.substring (7).trim().toLowerCase();
            }
        }
        if (r.sha256.isEmpty())
            for (auto& a : *assets)
            {
                const auto name = a.getProperty ("name", {}).toString();
                if (name.endsWithIgnoreCase (".exe.sha256") && isTrustedUrl (a.getProperty ("browser_download_url", {}).toString()))
                    r.sha256 = "@" + a.getProperty ("browser_download_url", {}).toString();   // fetched later
            }
    }
    return r;
}

juce::String Updater::sha256Of (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return {};
    return juce::SHA256 (in).toHexString().toLowerCase();
}

void Updater::check (std::function<void (bool, Release, juce::String)> done)
{
    if (isThreadRunning()) return;
    job = Job::check;
    checkDone = std::move (done);
    startThread();
}

void Updater::download (const Release& r, std::function<void (float)> progress, std::function<void (bool, juce::String)> done)
{
    if (isThreadRunning()) return;
    job = Job::download;
    target = r;
    progressCb = std::move (progress);
    downloadDone = std::move (done);
    startThread();
}

static juce::String fetchText (const juce::String& url, juce::String& error)
{
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (15000)
                    .withStatusCode (&status)
                    .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: TF-Vocal-Studio-Updater");
    auto in = juce::URL (url).createInputStream (opts);
    if (in == nullptr || status >= 400) { error = u8 ("ইন্টারনেটে যোগাযোগ করা যায়নি।"); return {}; }
    return in->readEntireStreamAsString();
}

void Updater::run()
{
    if (job == Job::check)
    {
        juce::String err;
        auto json = fetchText (repoApi, err);
        auto rel = json.isNotEmpty() ? parseRelease (json) : Release();
        if (rel.sha256.startsWith ("@"))
        {
            juce::String e2;
            auto txt = fetchText (rel.sha256.substring (1), e2).trim();
            rel.sha256 = txt.upToFirstOccurrenceOf (" ", false, false).toLowerCase().retainCharacters ("0123456789abcdef");
        }
        const bool newer = rel.isValid() && isNewer (rel.version, JUCE_APPLICATION_VERSION_STRING);
        if (json.isNotEmpty() && ! rel.isValid() && err.isEmpty()) err = u8 ("নতুন সংস্করণের তথ্য পড়া যায়নি।");
        auto cb = checkDone;
        juce::MessageManager::callAsync ([cb, newer, rel, err] { if (cb) cb (newer, rel, err); });
        return;
    }
    if (job == Job::download)
    {
        auto fail = [this] (const juce::String& msg)
        {
            auto cb = downloadDone;
            juce::MessageManager::callAsync ([cb, msg] { if (cb) cb (false, msg); });
        };
        if (! target.isValid() || ! isTrustedUrl (target.setupUrl)) { fail (u8 ("আপডেটের ঠিকানা বিশ্বাসযোগ্য নয় — ডাউনলোড করা হয়নি।")); return; }
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TF Vocal Studio update");
        dir.deleteRecursively();
        dir.createDirectory();
        auto out = dir.getChildFile ("TF-Vocal-Studio-Setup-" + juce::File::createLegalFileName (target.version) + ".exe");
        int status = 0;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (20000).withStatusCode (&status)
                        .withExtraHeaders ("User-Agent: TF-Vocal-Studio-Updater");
        auto in = juce::URL (target.setupUrl).createInputStream (opts);
        if (in == nullptr || status >= 400) { fail (u8 ("ডাউনলোড শুরু করা যায়নি — ইন্টারনেট দেখুন।")); return; }
        juce::FileOutputStream fos (out);
        if (! fos.openedOk()) { fail (u8 ("ফাইল রাখার জায়গা পাওয়া যায়নি।")); return; }
        const auto total = target.size > 0 ? target.size : in->getTotalLength();
        juce::HeapBlock<char> buf (1 << 16);
        juce::int64 got = 0;
        float lastP = -1.0f;
        while (! in->isExhausted())
        {
            if (threadShouldExit()) { fos.flush(); out.deleteFile(); fail (u8 ("বাতিল করা হয়েছে।")); return; }
            const int n = in->read (buf.get(), 1 << 16);
            if (n <= 0) break;
            fos.write (buf.get(), (size_t) n);
            got += n;
            if (total > 0 && progressCb)
            {
                const float p = (float) got / (float) total;
                if (p - lastP > 0.01f) { lastP = p; auto cb = progressCb; juce::MessageManager::callAsync ([cb, p] { cb (p); }); }
            }
            if (got > (juce::int64) 600 * 1024 * 1024) { fos.flush(); out.deleteFile(); fail (u8 ("ফাইলটা অস্বাভাবিক বড় — বাতিল করা হলো।")); return; }
        }
        fos.flush();
        if (total > 0 && got != total) { out.deleteFile(); fail (u8 ("ডাউনলোড পুরো হয়নি — আবার চেষ্টা করুন।")); return; }
        // the file must be exactly the one published with the release
        if (sha256Of (out) != target.sha256) { out.deleteFile(); fail (u8 ("ডাউনলোড করা ফাইলটা মিলছে না (নিরাপত্তা পরীক্ষা) — চালানো হয়নি।")); return; }
        auto cb = downloadDone;
        const auto path = out.getFullPathName();
        juce::MessageManager::callAsync ([cb, path] { if (cb) cb (true, path); });
    }
}

bool Updater::runInstaller (const juce::File& setup)
{
    if (! setup.existsAsFile()) return false;
   #if JUCE_WINDOWS
    return setup.startAsProcess ("/SILENT /SP- /NOCANCEL /CLOSEAPPLICATIONS");
   #else
    return false;
   #endif
}
}
