// Written for Demon's Souls. The setup screens before a run (docs/88 section 2, docs/89), on the setup shell
// (setup_page.hpp), in the setup design's order:
//  1. the start screen: "Set up Demon's Souls" on a first start, else what setup found (Resume, Update, Repair, the
//     game folder that moved, or Play), by setup::launch_state;
//  2. "Where is your game?": the ways to the game's files, one card each: the player's RPCS3 (setup_rpcs3_window.hpp),
//     the decrypted files, or how to get either;
//  3. the game folder: the system's folder dialog, the dumps found in the usual places, and the engine's check
//     (`run --check-only`) as rows, the decrypted files to fix first;
//  4. "Getting your game's files": how to copy the disc and decrypt the files, with free tools;
//  5. "Ready to build": the game, the free space, the time it takes on this PC and "Plug it in first".
// Start building opens the progress screen (setup_window.hpp) over the start screen, which comes back when it closes.
#pragma once

#include "button.hpp"
#include "folder_picker.hpp"
#include "setup_engine.hpp"
#include "setup_page.hpp"
#include "setup_pc.hpp"
#include "setup_rpcs3.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ui {

// A value that another thread puts and the UI's thread takes once a frame (the folder dialog's answer, the places
// found). Shared with that thread, so it outlives a screen that closed first.
template <class T>
class Mailbox {
public:
    void put(T value) {
        std::lock_guard<std::mutex> lock(mLock);
        mValue = std::move(value);
        mFull = true;
    }
    bool take(T& out) {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mFull) return false;
        out = std::move(mValue);
        mFull = false;
        return true;
    }

private:
    std::mutex mLock;
    T mValue{};
    bool mFull = false;
};

// The rail's line under Build before a run: "About 10 to 20 minutes".
std::string build_time_line();

// 1. The start screen: where setup stands, and the one thing to do about it (docs/88 section 3): Start, Resume setup
// (or Start over), Update now, Repair, Choose the game folder, or Play; then Settings and Quit.
class SetupStartWindow : public SetupPage {
public:
    struct Props {
        setup::Request request;        // the folders to begin with (DES_SETUP_GAME and the rest in a test); empty: none
        std::function<void()> onPlay;  // the game is ready (the launch hook starts it, docs/88 section 7); without it,
                                       // Play only closes the setup screens (DES_UI_OPEN=setup: the game runs below)
    };

    explicit SetupStartWindow(Props props);

    void uncover() override;  // (back from a screen it opened: the state may have changed)

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;  // (the update check's answer: updates.hpp)
    void back() override;

private:
    void refresh();
    void open_update();
    void open_source();
    void open_folder();
    void run(setup::Request request);
    void play();
    void ask_quit();

    Props mProps;
    setup::Launch mLaunch;
    Rml::Element* mBody = nullptr;
    Buttons mMain, mMore;
    std::string mUpdateShown;  // the newer release the screen shows (updates.hpp); empty: none
};

// 2. "Where is your game?": a card for each way to the game's files. A card opens its page at once.
class SetupSourceWindow : public SetupPage {
public:
    struct Props {
        setup::Request request;
        std::function<void(setup::Request)> onStart;  // the folders are checked and the player pressed Start building
    };

    explicit SetupSourceWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;

private:
    void open_rpcs3();
    void open_folder();
    void open_help();
    void draw_rpcs3_card();

    Props mProps;
    std::shared_ptr<Mailbox<setup::Rpcs3>> mFound;  // the RPCS3 the host found (setup_rpcs3.hpp), off this thread
    setup::Rpcs3 mRpcs3;
    bool mLooked = false;          // the search has answered
    bool mOpenWhenFound = false;   // the card was pressed before it answered
    Rml::Element* mRpcs3Tag = nullptr;
    Rml::Element* mRpcs3Line = nullptr;
    Buttons mCards, mActions;
};

// 3. The game folder: the folder (Choose folder, or one of the dumps found), a folder for the decrypted files if they
// are not beside the originals, the check's rows, "How do I make these?", Check again, Continue and Back. A folder
// chosen is checked at once.
class SetupFilesWindow : public SetupPage {
public:
    struct Props {
        setup::Request request;                       // the folders to begin with
        std::filesystem::path moved;                  // where the game folder was, when it moved (GameMoved)
        bool rebuild = true;                          // Continue opens Ready to build; false: the run at once
        std::function<void(setup::Request)> onStart;  // the folders are checked: run setup with them
    };

    explicit SetupFilesWindow(Props props);

    bool focus() override;

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;
    bool scroll(int pages) override;

private:
    enum class Picking { None, Game, Decrypted };

    void choose(Picking what);
    void picked(const PickedFolder& folder);
    void check();
    void start();
    void draw_folder();
    void draw_places();
    void show_places_note();
    void draw_check();

    Props mProps;
    setup::Request mRequest;
    setup::Status mStatus;
    bool mChecking = false;  // a check was started for these folders
    Picking mPicking = Picking::None;
    std::shared_ptr<Mailbox<PickedFolder>> mPicked;
    std::shared_ptr<Mailbox<std::vector<setup::Place>>> mFound;
    std::vector<setup::Place> mPlaces;
    bool mScanned = false;
    std::string mPickProblem;
    std::string mShownCheck;  // what the check's part last drew, to draw only changes
    bool mShownOk = false;    // it showed a check that passed

    Rml::Element* mFolderText = nullptr;
    Rml::Element* mDecField = nullptr;  // the decrypted files' folder, chosen before the disc copy
    Rml::Element* mDecText = nullptr;
    Rml::Element* mChooseRow = nullptr;
    Rml::Element* mPlacesNote = nullptr;
    Rml::Element* mPlacesList = nullptr;
    Rml::Element* mCheck = nullptr;
    Rml::Element* mLinks = nullptr;
    Rml::Element* mActionsRow = nullptr;
    Buttons mChooseButton, mPlaceButtons, mDecryptedButton, mLinkButtons, mActions;
    setup::Engine mEngine;  // (last: destroyed first, which ends a check still going)
};

// 4. "Getting your game's files": copying the disc, and decrypting the 27 files once (docs/51 plan (c)).
class SetupHelpWindow : public SetupPage {
public:
    struct Props {
        std::function<void()> onFolder;  // "I have decrypted files": the game folder page; none: no such button
        std::function<void()> onRpcs3;   // "Import from RPCS3": the RPCS3 page; none: no such button
    };

    explicit SetupHelpWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void back() override;
    bool scroll(int pages) override;

private:
    Props mProps;
    Buttons mActions;
};

// 5. "Ready to build": the game, the free space, about how long it takes on this PC, and "Plug it in first" while on
// battery; Start building and Back.
class SetupBeforeWindow : public SetupPage {
public:
    struct Props {
        setup::Request request;
        std::function<void(setup::Request)> onStart;
        std::string fromLine;    // the game row's line; empty: "From <game folder>. The game files check out."
        std::string importLine;  // what comes across from RPCS3 (setup_rpcs3_window.hpp); empty: no such row
    };

    explicit SetupBeforeWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;

private:
    void start();
    void draw();

    Props mProps;
    Rml::Element* mRows = nullptr;
    Rml::Element* mActionsRow = nullptr;
    Buttons mSpaceButton, mActions;
    bool mShownBattery = false;
    unsigned long long mPowerAt = 0;  // when the power was last asked (now_ms)
};

}  // namespace ui
