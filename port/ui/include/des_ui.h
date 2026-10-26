/* The wrapper UI's interface to the rest of the port (port/ui, docs/32). */
#ifndef DES_UI_H
#define DES_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called first thing in ps3_title_options_init (des_options.cpp), before the
 * game's options are read: loads settings.ini into the environment variables
 * that are not set yet, and installs the overlay (start screen, menu,
 * settings) on the D3D12 backend. DES_UI=0 skips all of it. */
void des_ui_early_init(void);

/* ps3recomp's launch hook (ps3_title_launch, des_options.cpp), first thing in
 * main, before it reads its arguments (docs/88 section 3): -1 boots the game,
 * -2 shows setup's screens alone, with no game (ps3recomp's PS3_LAUNCH_WINDOW).
 * Setup alone with DES_UI_OPEN=setup or setup-run. The game with an ELF on the
 * command line (a developer's run), or once setup has built it (setup's
 * ready.txt: the ELF, PS3_GAME_MODULE and PS3_VFS_ROOT are filled in); else
 * setup. DES_UI=0: the game, as before. */
int des_ui_launch(int* argc, char*** argv);

/* ps3_title_prework (des_prework.cpp), before the game's code runs: when the
 * game goes online with a server the privacy note is about and the player has
 * not accepted this version of it (port/ui/src/privacy.hpp), shows the note
 * and returns once the player answers. Decline leaves this run offline. With
 * no way to show it (no presenting, or the UI could not start) the answer is
 * Decline at once. Otherwise returns at once. */
void des_ui_privacy_wait(void);

/* Setup's screens alone (ps3_title_window_only_init), in place of
 * des_ui_early_init: settings.ini, the overlay and setup's start screen, whose
 * Play starts the game in a new process of this program and closes this one. */
void des_ui_setup_only_init(void);

/* Mouse look (docs/41; port/ui/src/mouse.cpp): the mouse's counts (x right,
 * y down) gathered on the window's thread since the last call, while the game
 * has the mouse (DES_MOUSE_LOOK on, the window in front, no document of the
 * UI open); 0 otherwise. des_mouse_look (des_options.cpp) takes them once a
 * game frame while the follow camera runs, with mouse look on or off, and the
 * game has the mouse (and its buttons and wheel) only while these calls keep
 * coming. Any thread. */
void des_mouse_take(int* dx, int* dy);

/* Mouse look's settings changed: the window's thread takes or lets go of the
 * mouse to match, soon after. Any thread. */
void des_mouse_refresh(void);

/* At each of the game's flip requests (des_options.cpp, the game's thread):
 * the report test knob DES_HANG_AT_FRAME (port/ui/src/report.hpp). */
void des_report_flip_hook(void);

/* The UI's sounds, by id (ui::Sound in port/ui/src/sound.hpp, in this order). */
enum {
    DES_UI_SOUND_CLICK = 0,       /* a button pressed */
    DES_UI_SOUND_PLAY,            /* the start screen's Continue */
    DES_UI_SOUND_BINDING_CHANGED,
    DES_UI_SOUND_MENU_OPEN,       /* the menu bar opened */
    DES_UI_SOUND_MENU_CLOSE,
    DES_UI_SOUND_WINDOW_OPEN,     /* a window (settings, a question) opened */
    DES_UI_SOUND_WINDOW_CLOSE,
    DES_UI_SOUND_TAB_CHANGED,
    DES_UI_SOUND_ITEM_FOCUS,      /* the cursor moved */
    DES_UI_SOUND_ITEM_CHANGE,     /* a value changed */
    DES_UI_SOUND_ITEM_ENABLE,     /* a value turned on */
    DES_UI_SOUND_ITEM_DISABLE,    /* a value turned off */
    DES_UI_SOUND_WARNING          /* a warning question */
};

/* Plays a UI sound with the game's own menu sounds. Null (the default) is
 * silent. Called on the render thread, from the overlay's frame hook. */
extern void (*g_des_ui_sound)(int id);

#ifdef __cplusplus
}
#endif
#endif
