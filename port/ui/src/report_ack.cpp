// Written for Demon's Souls. See report_ack.hpp.
#include "report_ack.hpp"

#include "modal.hpp"
#include "pane.hpp"
#include "report.hpp"
#include "sound.hpp"
#include "ui.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>

namespace ui {
namespace {

// The answer, kept beside the reports folder. None when there is no data folder (a test run keeps nothing).
std::filesystem::path marker() {
    const auto& dir = report::data_dir();
    return dir.empty() ? dir : dir / L"report_zip_ack.txt";
}

bool answered() {
    std::error_code ec;
    const auto path = marker();
    return !path.empty() && std::filesystem::exists(path, ec);
}

void keep_answer() {
    const auto path = marker();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (FILE* f = report::open_file(path, "wb")) {
        std::fputs("The player has been told to attach the report's zip to the issue (report_ack.cpp).\n", f);
        std::fclose(f);
    }
}

}  // namespace

void with_zip_ack(std::function<void()> then) {
    if (answered()) {
        if (then) then();
        return;
    }
    const auto ticked = std::make_shared<bool>(false);
    const Rml::String where =
        report::no_desktop()
            ? "The report is saved with a link to the issue page beside it. GitHub can't attach the file for you: "
              "when you open the link on a PC, drag the report file (.zip) into the form's \"Report file (.zip)\" box."
            : "The issue page opens in your browser, and the folder with the report file (.zip) opens beside it. "
              "GitHub can't attach the file for you: drag it into the form's \"Report file (.zip)\" box.";
    auto owned = std::make_unique<Modal>(Modal::Props{
        .title = "Before you report",
        .bodyText = where + " The file has the logs needed to find the problem; without it, a report can rarely be "
                            "solved. This is asked only once.",
        .actions =
            {
                ModalAction{
                    .label = "Cancel",
                    .onPressed =
                        [](Modal& m) {
                            play_sound(kSoundWindowClose);
                            m.pop();
                        },
                },
                ModalAction{
                    .label = "Continue",
                    .onPressed =
                        [ticked, then = std::move(then)](Modal& m) {
                            if (!*ticked) return;
                            play_sound(kSoundClick);
                            keep_answer();
                            const auto go = then;  // (the modal and this lambda go with pop)
                            m.pop();
                            if (go) go();
                        },
                    .isDisabled = [ticked] { return !*ticked; },
                },
            },
        .onDismiss = [](Modal& m) { m.pop(); },
        .icon = "question-mark",
    });
    // The tick: a button whose box fills when pressed (Material Symbols' check_box and check_box_outline_blank). Made
    // before the question shows, so the focus starts on it.
    auto& tick = owned->content_pane().add_button(ControlledButton::Props{
        .isSelected = [ticked] { return *ticked; },
    });
    tick.root()->SetClass("report-ack-tick", true);
    Rml::Element* box = append(tick.root(), "icon");
    box->SetClass("check-box-blank", true);
    append_text(tick.root(), "I'll attach the report file to my issue");
    tick.on_pressed([ticked, box] {
        *ticked = !*ticked;
        box->SetClass("check-box", *ticked);
        box->SetClass("check-box-blank", !*ticked);
        play_sound(*ticked ? kSoundItemEnable : kSoundItemDisable);
    });
    // Over what is showing, covering it as a window's own push does (the crash dialog's text would show through).
    if (Document* top = top_document())
        top->push(std::move(owned));
    else
        push_document(std::move(owned));
}

}  // namespace ui
