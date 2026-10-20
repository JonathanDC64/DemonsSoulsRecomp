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
