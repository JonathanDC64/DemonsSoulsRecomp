// The folder dialog (see folder_picker.hpp). The thread is the same everywhere; below it, one function for each
// platform shows the dialog and waits for it.
//
// Windows: the shell's IFileOpenDialog with FOS_PICKFOLDERS, in a single-threaded COM apartment on the dialog's own
// thread, owned by the game's window so that it stays in front of it.
// Elsewhere: the desktop portal (org.freedesktop.portal.FileChooser, OpenFile with "directory"), which GNOME, KDE and
// the other desktops answer with their own dialog, inside a Flatpak too. It is asked over D-Bus through libdbus-1,
// opened when the dialog is first asked for, as SDL opens it: nothing to build against, and every desktop with a
// portal has it. SDL3's folder dialog works the same way, but the Linux build has SDL2 (docs/76), which has none.
// Where there is no portal: zenity, then kdialog, as SDL3 falls back to them.
#include "folder_picker.hpp"

#ifdef _WIN32
#include <windows.h>
// (after windows.h)
#include <shobjidl.h>
#else
#include "child_process.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#endif

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace ui {
namespace {

// ---- What differs by platform: the dialog ----

#ifdef _WIN32

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n);
    return out;
}

std::string hex(HRESULT hr) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

PickedFolder show_dialog(const std::filesystem::path& start, const std::string& title, HWND owner) {
    PickedFolder out;
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        out.failed = true;
        out.why = "no folder dialog (" + hex(hr) + ")";
        return out;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(wide(title).c_str());
    std::error_code ec;
    if (!start.empty() && std::filesystem::is_directory(start, ec)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    hr = dialog->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) out.folder = path;
            CoTaskMemFree(path);
            item->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        out.failed = true;
        out.why = "the folder dialog failed (" + hex(hr) + ")";
    }
    dialog->Release();
    return out;
}

PickedFolder os_pick(const std::filesystem::path& start, const std::string& title, void* owner) {
    // The shell's dialogs need a single-threaded apartment; this thread is the dialog's own.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    PickedFolder out = show_dialog(start, title, static_cast<HWND>(owner));
    if (SUCCEEDED(com)) CoUninitialize();
    return out;
}

#else

// libdbus-1's own types, as far as this uses them (dbus-errors.h, dbus-message.h). Its error struct starts with its
// name and message; its iterator is opaque, filled in by the library. Both get more room here than they take.
struct DBusConnection;
struct DBusMessage;
struct DBusError {
    const char* name;
    const char* message;
    void* rest[4];
};
struct DBusIter {
    alignas(void*) unsigned char opaque[128];
};
using dbus_bool = uint32_t;
constexpr int kBusSession = 0;  // DBUS_BUS_SESSION
constexpr int kString = 's', kBoolean = 'b', kUint32 = 'u', kArray = 'a', kVariant = 'v', kDictEntry = 'e',
              kByte = 'y', kObjectPath = 'o';

struct Dbus {
    void (*error_init)(DBusError*);
    void (*error_free)(DBusError*);
    DBusConnection* (*bus_get_private)(int, DBusError*);
    const char* (*bus_get_unique_name)(DBusConnection*);
    void (*bus_add_match)(DBusConnection*, const char*, DBusError*);
    void (*connection_set_exit_on_disconnect)(DBusConnection*, dbus_bool);
    void (*connection_close)(DBusConnection*);
    void (*connection_unref)(DBusConnection*);
    dbus_bool (*connection_read_write)(DBusConnection*, int);
    DBusMessage* (*connection_pop_message)(DBusConnection*);
    DBusMessage* (*connection_send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*);
    DBusMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*);
    void (*message_unref)(DBusMessage*);
    dbus_bool (*message_is_signal)(DBusMessage*, const char*, const char*);
    const char* (*message_get_path)(DBusMessage*);
    void (*iter_init_append)(DBusMessage*, DBusIter*);
    dbus_bool (*iter_append_basic)(DBusIter*, int, const void*);
    dbus_bool (*iter_append_fixed_array)(DBusIter*, int, const void*, int);
    dbus_bool (*iter_open_container)(DBusIter*, int, const char*, DBusIter*);
    dbus_bool (*iter_close_container)(DBusIter*, DBusIter*);
    dbus_bool (*iter_init)(DBusMessage*, DBusIter*);
    int (*iter_get_arg_type)(DBusIter*);
    void (*iter_get_basic)(DBusIter*, void*);
    void (*iter_recurse)(DBusIter*, DBusIter*);
    dbus_bool (*iter_next)(DBusIter*);
    dbus_bool (*threads_init_default)();
};

template <class F>
bool find(void* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(dlsym(lib, name));
    return out != nullptr;
}

// libdbus-1, once; null where it is not there. Never closed: SDL may hold it too.
const Dbus* dbus() {
    static const Dbus* loaded = []() -> const Dbus* {
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return nullptr;
        static Dbus d;
        const bool ok =
            find(lib, "dbus_error_init", d.error_init) && find(lib, "dbus_error_free", d.error_free) &&
            find(lib, "dbus_bus_get_private", d.bus_get_private) &&
            find(lib, "dbus_bus_get_unique_name", d.bus_get_unique_name) &&
            find(lib, "dbus_bus_add_match", d.bus_add_match) &&
            find(lib, "dbus_connection_set_exit_on_disconnect", d.connection_set_exit_on_disconnect) &&
            find(lib, "dbus_connection_close", d.connection_close) &&
            find(lib, "dbus_connection_unref", d.connection_unref) &&
            find(lib, "dbus_connection_read_write", d.connection_read_write) &&
            find(lib, "dbus_connection_pop_message", d.connection_pop_message) &&
            find(lib, "dbus_connection_send_with_reply_and_block", d.connection_send_with_reply_and_block) &&
            find(lib, "dbus_message_new_method_call", d.message_new_method_call) &&
            find(lib, "dbus_message_unref", d.message_unref) &&
            find(lib, "dbus_message_is_signal", d.message_is_signal) &&
            find(lib, "dbus_message_get_path", d.message_get_path) &&
            find(lib, "dbus_message_iter_init_append", d.iter_init_append) &&
            find(lib, "dbus_message_iter_append_basic", d.iter_append_basic) &&
            find(lib, "dbus_message_iter_append_fixed_array", d.iter_append_fixed_array) &&
            find(lib, "dbus_message_iter_open_container", d.iter_open_container) &&
            find(lib, "dbus_message_iter_close_container", d.iter_close_container) &&
            find(lib, "dbus_message_iter_init", d.iter_init) &&
            find(lib, "dbus_message_iter_get_arg_type", d.iter_get_arg_type) &&
            find(lib, "dbus_message_iter_get_basic", d.iter_get_basic) &&
            find(lib, "dbus_message_iter_recurse", d.iter_recurse) &&
            find(lib, "dbus_message_iter_next", d.iter_next) &&
            find(lib, "dbus_threads_init_default", d.threads_init_default);
        if (!ok || !d.threads_init_default()) return nullptr;
        return &d;
    }();
    return loaded;
}

// One entry of the options dictionary (a{sv}): the key, then the value in a variant of signature `sig`.
void add_option(const Dbus& d, DBusIter& dict, const char* key, int type, const char* sig, const void* value) {
    DBusIter entry, variant;
    d.iter_open_container(&dict, kDictEntry, nullptr, &entry);
    d.iter_append_basic(&entry, kString, &key);
    d.iter_open_container(&entry, kVariant, sig, &variant);
    d.iter_append_basic(&variant, type, value);
    d.iter_close_container(&entry, &variant);
    d.iter_close_container(&dict, &entry);
}

// The folder to open in, as the portal takes it: a byte array with its NUL (ay).
void add_folder_option(const Dbus& d, DBusIter& dict, const std::string& folder) {
    const char* key = "current_folder";
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(folder.c_str());
    DBusIter entry, variant, array;
    d.iter_open_container(&dict, kDictEntry, nullptr, &entry);
    d.iter_append_basic(&entry, kString, &key);
    d.iter_open_container(&entry, kVariant, "ay", &variant);
    d.iter_open_container(&variant, kArray, "y", &array);
    d.iter_append_fixed_array(&array, kByte, &bytes, int(folder.size() + 1));
    d.iter_close_container(&variant, &array);
    d.iter_close_container(&entry, &variant);
    d.iter_close_container(&dict, &entry);
}

// file:///home/a%20b -> /home/a b; empty for anything else.
std::string path_of_uri(const char* uri) {
    const std::string s(uri);
    if (s.rfind("file://", 0) != 0) return {};
    std::string out;
    for (size_t i = 7; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The Response signal (u response, a{sv} results): 0 is a choice, in results["uris"]; 1 and 2 are none.
void read_response(const Dbus& d, DBusMessage* m, PickedFolder& out) {
    DBusIter it;
    if (!d.iter_init(m, &it) || d.iter_get_arg_type(&it) != kUint32) return;
    uint32_t code = 2;
    d.iter_get_basic(&it, &code);
    if (code != 0 || !d.iter_next(&it) || d.iter_get_arg_type(&it) != kArray) return;
    DBusIter dict;
    d.iter_recurse(&it, &dict);
    for (; d.iter_get_arg_type(&dict) == kDictEntry; d.iter_next(&dict)) {
        DBusIter entry, value, uris;
        d.iter_recurse(&dict, &entry);
        const char* key = nullptr;
        if (d.iter_get_arg_type(&entry) != kString) continue;
        d.iter_get_basic(&entry, &key);
        if (!key || std::strcmp(key, "uris") != 0 || !d.iter_next(&entry) || d.iter_get_arg_type(&entry) != kVariant)
            continue;
        d.iter_recurse(&entry, &value);
        if (d.iter_get_arg_type(&value) != kArray) continue;
        d.iter_recurse(&value, &uris);
        if (d.iter_get_arg_type(&uris) != kString) continue;
        const char* uri = nullptr;
        d.iter_get_basic(&uris, &uri);
        if (uri) out.folder = path_of_uri(uri);
    }
}

// The portal's dialog: OpenFile, then its Response. False if there is no portal to ask (no libdbus, no session bus,
// or no file chooser behind the portal).
bool portal_dialog(const std::filesystem::path& start, const std::string& title, PickedFolder& out) {
    const Dbus* dp = dbus();
    if (!dp) return false;
    const Dbus& d = *dp;
    DBusError err;
    d.error_init(&err);
    DBusConnection* bus = d.bus_get_private(kBusSession, &err);
    d.error_free(&err);
    if (!bus) return false;
    d.connection_set_exit_on_disconnect(bus, 0);  // (by default a lost bus ends the process)

    // The request's object path follows from this connection's name and a token of ours (the portal's documented
    // scheme), so its Response is matched before the call and cannot be missed.
    static std::atomic<unsigned> sCount{0};
    const std::string token = "des_setup_" + std::to_string(getpid()) + "_" + std::to_string(++sCount);
    std::string sender = d.bus_get_unique_name(bus) ? d.bus_get_unique_name(bus) : "";
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    for (char& c : sender)
        if (c == '.') c = '_';
    std::string path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token, handle;
    const auto match = [&](const std::string& p) {
        const std::string rule =
            "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + p + "'";
        d.bus_add_match(bus, rule.c_str(), nullptr);  // (no error: sent with the call below, without a wait)
    };
    match(path);

    DBusMessage* call = d.message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                  "org.freedesktop.portal.FileChooser", "OpenFile");
    bool asked = false;
    if (call) {
        DBusIter args, dict;
        const char* parent = "";  // (a parent window needs its X11 or Wayland handle; the dialog opens on its own)
        const char* caption = title.c_str();
        const char* tokenText = token.c_str();
        const dbus_bool yes = 1;
        d.iter_init_append(call, &args);
        d.iter_append_basic(&args, kString, &parent);
        d.iter_append_basic(&args, kString, &caption);
        d.iter_open_container(&args, kArray, "{sv}", &dict);
        add_option(d, dict, "handle_token", kString, "s", &tokenText);
        add_option(d, dict, "directory", kBoolean, "b", &yes);
        add_option(d, dict, "modal", kBoolean, "b", &yes);
        std::error_code ec;
        if (!start.empty() && std::filesystem::is_directory(start, ec)) add_folder_option(d, dict, start.string());
        d.iter_close_container(&args, &dict);
        d.error_init(&err);
        DBusMessage* reply = d.connection_send_with_reply_and_block(bus, call, -1, &err);
        d.message_unref(call);
        if (reply) {
            asked = true;
            DBusIter it;
            const char* got = nullptr;
            if (d.iter_init(reply, &it) && d.iter_get_arg_type(&it) == kObjectPath) d.iter_get_basic(&it, &got);
            if (got && path != got) {  // (portals older than the scheme choose their own path)
                handle = got;
                match(handle);
            }
            d.message_unref(reply);
        }
        d.error_free(&err);
    }

    // Until the dialog closes: as long as the player keeps it open.
    for (bool answered = false; asked && !answered;) {
        if (!d.connection_read_write(bus, 500)) {
            out.failed = true;
            out.why = "the session bus closed while the folder dialog was open";
            break;
        }
        while (DBusMessage* m = d.connection_pop_message(bus)) {
            const char* p = d.message_get_path(m);
            if (!answered && d.message_is_signal(m, "org.freedesktop.portal.Request", "Response") && p &&
                (path == p || handle == p)) {
                read_response(d, m, out);
                answered = true;
            }
            d.message_unref(m);
        }
    }
    d.connection_close(bus);
    d.connection_unref(bus);
    return asked;
}

// zenity (GNOME's), then kdialog (KDE's): each prints the folder chosen and exits 0, or exits 1 when closed. False if
// neither could start.
bool tool_dialog(const std::filesystem::path& start, const std::string& title, PickedFolder& out) {
    const std::string from = start.empty() ? std::string(".") : start.string();
    const std::pair<const char*, std::vector<std::string>> tools[] = {
        {"zenity", {"--file-selection", "--directory", "--title=" + title, "--filename=" + from + "/"}},
        {"kdialog", {"--getexistingdirectory", from, "--title", title}},
    };
    for (const auto& [name, args] : tools) {
        std::mutex lock;
        std::condition_variable ended;
        bool done = false;
        int status = -1;
        std::vector<std::string> lines;
        ChildProcess child;  // (after what its callbacks use, so that it goes first)
        const bool started = child.start(
            name, args,
            [&](std::string line) {
                std::lock_guard<std::mutex> hold(lock);
                lines.push_back(std::move(line));
            },
            [&](int s, bool) {
                std::lock_guard<std::mutex> hold(lock);
                status = s;
                done = true;
                ended.notify_all();
            });
        if (!started) continue;
        std::unique_lock<std::mutex> hold(lock);
        ended.wait(hold, [&] { return done; });
        // The folder is the last line that names one (GTK's warnings come on the same pipe).
        std::error_code ec;
        for (auto it = lines.rbegin(); status == 0 && it != lines.rend(); ++it) {
            if (!it->empty() && (*it)[0] == '/' && std::filesystem::is_directory(*it, ec)) {
                out.folder = *it;
                break;
            }
        }
        return true;
    }
    return false;
}

PickedFolder os_pick(const std::filesystem::path& start, const std::string& title, void*) {
    PickedFolder out;
    if (!portal_dialog(start, title, out) && !tool_dialog(start, title, out)) {
        out.failed = true;
        out.why = "no desktop portal, zenity or kdialog";
    }
    return out;
}

#endif

// ---- The same everywhere ----

// DES_UI_PICK=<folder>[;<folder>...] (a test of the setup screens, with DES_UI_KEYS and DES_UI_SHOT): each dialog
// answers with the next folder of the list at once, and none is shown; past the list's end, as if it were closed
// without a choice. False without the variable.
bool scripted_pick(PickedFolder& out) {
    static std::mutex lock;
    static std::vector<std::filesystem::path> folders;
    static size_t next = 0;
    static int scripted = -1;
    std::lock_guard<std::mutex> hold(lock);
    if (scripted < 0) {
        const char* v = std::getenv("DES_UI_PICK");
        scripted = v && *v ? 1 : 0;
        for (std::string all = scripted ? v : ""; !all.empty();) {
            const size_t semi = all.find(';');
            if (semi) folders.emplace_back(all.substr(0, semi));
            all = semi == std::string::npos ? std::string() : all.substr(semi + 1);
        }
    }
    if (!scripted) return false;
    out.folder = next < folders.size() ? folders[next++] : std::filesystem::path{};
    return true;
}

}  // namespace

void pick_folder(const std::filesystem::path& start, const std::string& title, void* owner,
                 std::function<void(PickedFolder)> done) {
    std::thread([start, title, owner, done = std::move(done)] {
        PickedFolder out;
        if (scripted_pick(out)) {
            const std::u8string name = out.folder.u8string();
            std::fprintf(stderr, "[setup] the folder dialog (DES_UI_PICK): %s\n",
                         out.folder.empty() ? "closed" : std::string(name.begin(), name.end()).c_str());
            if (done) done(std::move(out));
            return;
        }
        out = os_pick(start, title, owner);
        if (out.failed) std::fprintf(stderr, "[setup] the folder dialog: %s\n", out.why.c_str());
        if (done) done(std::move(out));
    }).detach();
}

}  // namespace ui
