// Written for Demon's Souls. The first Python's download by the host itself, for a Windows with no curl.exe in its
// System folder (Wine and CrossOver have none): setup_python.cpp. WinHTTP, which Windows and Wine both have, doing what
// the curl line does: https only, TLS 1.2 or later, redirects followed but never from https to http, a try that gets
// nothing for a minute ends, and a stopped try's .part file carried on with a Range request (a server that sends the
// whole file instead starts the file over). Off Windows there is none: curl is on every Linux that setup runs on.
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace ui::setup {

class HttpDownload {
public:
    enum class Result { Ok, Failed, Stopped };

    // The library's name for a line ("WinHTTP"), or null where the host has none of its own.
    static const char* library();

    // `url` into `part`, carrying on from the size it has. `each` runs after each piece is written. `stopping` is the
    // caller's stop flag, read once the connection's handles are open, so a stop() just before them is not missed.
    // Runs to its end on the caller's thread.
    Result get(const char* url, const std::filesystem::path& part, const std::function<void()>& each,
               const std::function<bool()>& stopping);
    // Ends a get() at once, from any thread: WinHTTP fails the call it is in when its handle is closed.
    void stop();

    // About the last get(): why it failed, for a FIX line ("HTTP 404", "WinHttpSendRequest: error 12007"); whether the
    // file could not be written (a full disk, most likely); whether it carried on from the .part file.
    const std::string& why() const { return mWhy; }
    bool write_failed() const { return mWriteFailed; }
    bool resumed() const { return mResumed; }

private:
    void close_handles();

    std::mutex mLock;  // the open handles, which stop() closes
    void* mSession = nullptr;  // (HINTERNETs)
    void* mConnect = nullptr;
    void* mRequest = nullptr;
    std::string mWhy;
    bool mWriteFailed = false;
    bool mResumed = false;
};

}  // namespace ui::setup
