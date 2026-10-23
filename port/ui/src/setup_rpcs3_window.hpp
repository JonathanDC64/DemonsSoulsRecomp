// Written for Demon's Souls. The setup screens' RPCS3 route (docs/90 section 1), on the setup shell (setup_page.hpp):
//  1. "Your game in RPCS3": the engine's check with the player's RPCS3 (`run --check-only --rpcs3`) as rows: RPCS3,
//     its user, the game in its library, then what to bring across (save, trophies, settings);
//  2. "RPCS3 was not found": the folder dialog for an RPCS3 the host did not find, and how to get it ready;
//  3. "Unlocking the game files": the unlock alone (`run --unlock-only`), file by file, before Ready to build.
// "Where is your game?" (setup_screens.hpp) opens the first or the second; Continue goes on to the third when files
// still need unlocking, else to Ready to build. Nothing here writes in RPCS3's folders: the engine reads them, and
// runs RPCS3 on copies in setup's own folder.
#pragma once

#include "button.hpp"
#include "folder_picker.hpp"
#include "setup_engine.hpp"
#include "setup_page.hpp"
#include "setup_rpcs3.hpp"
#include "setup_screens.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// 1. "Your game in RPCS3".
class SetupRpcs3Window : public SetupPage {
public:
    struct Props {
        setup::Rpcs3 rpcs3;
        setup::Request request;                       // the folders to begin with
        std::function<void(setup::Request)> onStart;  // the player pressed Start building
    };

    explicit SetupRpcs3Window(Props props);

    bool focus() override;

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;
    bool scroll(int pages) override;

private:
    enum class Picking { None, Rpcs3, Game };
    enum Item { Save, Trophies, Settings, kItems };

    void check();
    void choose(Picking what);
    void picked(const PickedFolder& folder);
    void draw();
    void draw_imports(Rml::Element* parent);
    void go_on();
    bool supported() const;

    Props mProps;
    setup::Request mRequest;
    setup::Status mStatus;
    bool mChecking = false;
    bool mTicked[kItems] = {};
    bool mTickedSet = false;  // the boxes were set from this check's @found lines
    Picking mPicking = Picking::None;
    std::shared_ptr<Mailbox<PickedFolder>> mPicked;
    std::string mPickProblem;
    std::string mShown;  // what the page last drew, to draw only changes
    bool mShownOk = false;

    Rml::Element* mBody = nullptr;
    Buttons mRowButtons, mLinkButtons, mGameButtons, mChecks, mActions;
    setup::Engine mEngine;  // (last: destroyed first, which ends a check still going)
};

// 2. "RPCS3 was not found".
class SetupRpcs3MissingWindow : public SetupPage {
public:
    struct Props {
        std::function<void(const setup::Rpcs3&)> onFound;  // the player picked a folder with RPCS3 in it
    };

    explicit SetupRpcs3MissingWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;

private:
    void choose();

    Props mProps;
    bool mPicking = false;
    std::shared_ptr<Mailbox<PickedFolder>> mPicked;
    Rml::Element* mFolderText = nullptr;
    Rml::Element* mProblem = nullptr;
    Buttons mActions;
};

// 3. "Unlocking the game files". It starts the unlock when it opens; a portable RPCS3 first asks for it to be closed.
class SetupUnlockWindow : public SetupPage {
public:
    struct Props {
        setup::Request request;  // with the RPCS3 (and the game folder, if it is not RPCS3's library entry)
        bool shared = false;     // a portable RPCS3: setup starts it as it is (docs/90 2.5)
        bool eboot = true;       // the game's code is among the files to unlock
        int scripts = 26;        // how many of the 26 script files are
        bool firmware = false;   // and the PS3 system software's libsre.sprx
        std::string fromLine;    // Ready to build's line for the game
        std::string importLine;  // and for what comes across from RPCS3; empty: nothing
        std::function<void(setup::Request)> onStart;
    };

    explicit SetupUnlockWindow(Props props);

protected:
    std::vector<std::vector<Button*>> nav_lines() const override;
    void tick() override;
    void back() override;

private:
    void start();
    void draw();
    void go_on();

    Props mProps;
    setup::Status mStatus;
    bool mRunning = false;  // an unlock was started
    bool mDetailsShown = false;
    std::string mShown;
    Rml::Element* mBody = nullptr;
    Buttons mActions;
    setup::Engine mEngine;
};

}  // namespace ui
