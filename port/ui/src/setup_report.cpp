// A problem report from the setup screens (see setup_report.hpp).
#include "setup_report.hpp"

#include "report.hpp"
#include "setup_engine.hpp"
#include "version.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace ui::setup {
namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// A log's first `head` lines and its last `tail` bytes, with the player's name and folders hidden.
std::vector<uint8_t> tail_of(const std::filesystem::path& path, size_t head, size_t tail) {
    return report::bytes(report::redact_log(report::read_head_tail(path, head, tail)));
}

}  // namespace

std::string send_report(const Status& failure) {
    const auto setupDir = data_dir() / "setup";
    std::error_code ec;
    std::vector<report::File> files;
    files.push_back({"system.txt", report::bytes(report::redact(report::system_info()))});
    files.push_back({"setup.log", tail_of(setupDir / "logs" / "run.log", 40, 256 * 1024)});
    const std::filesystem::path step = failure.logPath.empty() ? std::filesystem::path() : from_utf8(failure.logPath);
    if (!step.empty()) files.push_back({"step.log", tail_of(step, 20, 64 * 1024)});
    // After RPCS3 failed: the unlock's log (which RPCS3, its folder, the command and all RPCS3 printed), when it is not
    // the step's log already.
    const auto unlock = setupDir / "logs" / "unlock.log";
    if ((failure.errorCode == "rpcs3" || failure.errorStep == "unlock") && std::filesystem::is_regular_file(unlock, ec) &&
        (step.empty() || !std::filesystem::equivalent(step, unlock, ec)))
        files.push_back({"unlock.log", tail_of(unlock, 40, 128 * 1024)});
    // The host's own lines ([setup] ...): the first Python's download among them, which the engine's log does not have.
    if (const auto host = report::log_path(); !host.empty() && std::filesystem::is_regular_file(host, ec))
        files.push_back({"des.log", tail_of(host, 40, 128 * 1024)});
    files.push_back({"journal.json", tail_of(setupDir / "journal.json", 4000, 256 * 1024)});
    const auto zip = report::write_bundle("setup-" + report::timestamp(), files);
    std::fprintf(stderr, "[setup] report: %s\n", zip.empty() ? "could not be saved" : utf8(zip).c_str());
    if (zip.empty()) return "The report could not be saved.";
    const std::string name = utf8(zip.filename());
    const std::string where = !failure.errorStep.empty() ? failure.errorStep
                              : !failure.errorCode.empty() ? failure.errorCode
                                                           : std::string("setup");
    const std::string body = "**What happened:** " + failure.errorText + "\n**Step:** " + where +
                             "\n**Version:** " + kVersion + "\n\nAttach " + name +
                             " from the folder that opened: it has setup's logs and this PC's facts, and no game files.";
    report::open_issue(report::issue_url("Setup did not finish: " + where, report::redact(body)), zip);
    return "The issue form is open in your browser. Attach " + name + " to it: it is in the folder that opened.";
}

}  // namespace ui::setup
