// The first Python's download by the host (see setup_http.hpp).
#include "setup_http.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include <fstream>
#include <system_error>
#include <vector>

namespace ui::setup {

#ifdef _WIN32

namespace {

#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
constexpr DWORD kTls13 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#else
constexpr DWORD kTls13 = 0x00002000;  // (older SDKs lack the name)
#endif

std::wstring widen(const char* s) {  // (the pinned URLs are ASCII)
    std::wstring w;
    for (; *s; ++s) w += wchar_t(static_cast<unsigned char>(*s));
    return w;
}

// "WinHttpSendRequest: error 12007", with the call's own last error (read before anything else can change it).
std::string failed(const char* call) {
    return std::string(call) + ": error " + std::to_string(GetLastError());
}

unsigned long long size_of(const std::filesystem::path& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : (unsigned long long)n;
}

}  // namespace

const char* HttpDownload::library() {
    return "WinHTTP";
}

void HttpDownload::close_handles() {  // (mLock held) the request first, then what it was opened from
    if (mRequest) WinHttpCloseHandle(mRequest);
    if (mConnect) WinHttpCloseHandle(mConnect);
    if (mSession) WinHttpCloseHandle(mSession);
    mRequest = mConnect = mSession = nullptr;
}

void HttpDownload::stop() {
    std::lock_guard<std::mutex> lock(mLock);
    close_handles();
}

HttpDownload::Result HttpDownload::get(const char* url, const std::filesystem::path& part,
                                       const std::function<void()>& each, const std::function<bool()>& stopping) {
    mWhy.clear();
    mWriteFailed = false;
    mResumed = false;
    // The end of a try that failed: Stopped when it was stop() that closed the handles under it.
    const auto end = [&](std::string why) {
        {
            std::lock_guard<std::mutex> lock(mLock);
            close_handles();
        }
        if (stopping()) return Result::Stopped;
        mWhy = std::move(why);
        return Result::Failed;
    };
    // A handle, kept where stop() can close it; false when a stop came first.
    const auto keep = [&](HINTERNET h, void*& slot) {
        std::lock_guard<std::mutex> lock(mLock);
        slot = h;
        return !stopping();
    };

    const std::wstring address = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts)) return end(failed("WinHttpCrackUrl"));
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) return end("not an https address");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    // The system's proxy settings (Windows 8.1 and later), else WinHTTP's own (netsh winhttp; none by default).
    HINTERNET session = WinHttpOpen(L"DemonsSoulsRecomp-setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"DemonsSoulsRecomp-setup", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return end(failed("WinHttpOpen"));
    if (!keep(session, mSession)) return end({});
    // TLS 1.2 or later, as curl's --tlsv1.2 (a Windows 10 before 20H1 has no TLS 1.3 here, so 1.2 alone then). If even
    // that is refused, the system's defaults stand: the archive is checked against its pinned SHA-256 after.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | kTls13;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    // A minute for each step and each read: curl's --connect-timeout 60 and its minute with no data.
    WinHttpSetTimeouts(session, 60000, 60000, 60000, 60000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) return end(failed("WinHttpConnect"));
    if (!keep(connect, mConnect)) return end({});
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return end(failed("WinHttpOpenRequest"));
    if (!keep(request, mRequest)) return end({});
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);

    const unsigned long long have = size_of(part);
    const std::wstring range = have ? L"Range: bytes=" + std::to_wstring(have) + L"-" : std::wstring();
    if (!WinHttpSendRequest(request, have ? range.c_str() : WINHTTP_NO_ADDITIONAL_HEADERS, have ? DWORD(-1) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return end(failed("WinHttpSendRequest"));
    if (!WinHttpReceiveResponse(request, nullptr)) return end(failed("WinHttpReceiveResponse"));
    DWORD status = 0, length = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
        return end(failed("WinHttpQueryHeaders"));
    // 206 carries on, if the range is the one asked for; 200 is the whole file, which starts the .part file over.
    bool append = false;
    if (status == 206 && have) {
        wchar_t got[128] = {};
        length = sizeof got - sizeof got[0];
        const std::wstring want = L"bytes " + std::to_wstring(have) + L"-";
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, got, &length,
                                 WINHTTP_NO_HEADER_INDEX) ||
            std::wstring(got).rfind(want, 0) != 0) {
            std::error_code ec;
            std::filesystem::remove(part, ec);
            return end("the server sent a range other than the one asked for; it starts over next time");
        }
        append = true;
        mResumed = true;
    } else if (status != 200) {
        return end("HTTP " + std::to_string(status));
    }

    std::ofstream out(part, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) {
        mWriteFailed = true;
        return end("the .part file could not be opened");
    }
    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &got)) return end(failed("WinHttpReadData"));
        if (got == 0) break;
        if (!out.write(buffer.data(), got) || !out.flush()) {  // (flushed, so the bar's size of the file is right)
            mWriteFailed = true;
            return end("the .part file could not be written");
        }
        if (each) each();
    }
    {
        std::lock_guard<std::mutex> lock(mLock);
        close_handles();
    }
    return Result::Ok;
}

#else

const char* HttpDownload::library() {
    return nullptr;
}

void HttpDownload::close_handles() {}

void HttpDownload::stop() {}

HttpDownload::Result HttpDownload::get(const char*, const std::filesystem::path&, const std::function<void()>&,
                                       const std::function<bool()>&) {
    mWhy = "no download of the host's own here";
    return Result::Failed;
}

#endif

}  // namespace ui::setup
