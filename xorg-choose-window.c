/*

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

*/

#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_keysyms.h>


// TODO (fixes)
// sort out error handling (exit codes, man xcb-requests), memory management, exit cleanup
// mask usages (x3): what should the order be?  doc says just pass in one
// open font once, globally
// put globals in a thing we pass around
//  - also contain ksl/size
//  - also structures for:
//      - wsetups, size, new wsetups, new size
//      - wsetup.overlay_*, wsetup.window
// change wsetup->overlay_rect to be just size
// xcb_image_text_8: handle case with text size > 255

// TODO (improvements)
// help/manpage
// configurable font/text size/colours/placement/padding, better defaults
// only show keys that would need to be pressed, eg. 'aa', 'ao', 'o' when we have 3
// take window IDs as whitelist/blacklist
// options for output format (hex)
// better expose redrawing (only some windows/rects?)
// work out how to clear a window instead of having a gc just for it


// -- types

/**
 * Part of a keysym lookup, used to translate between keysyms and characters.
 *
 * character: the ASCII character
 * keysym: the Xorg keysym to interpret the character as
 */
typedef struct keysyms_lookup_t {
    char character;
    xcb_keysym_t keysym;
} keysyms_lookup_t;

/**
 * A recursive structure holding data about windows, used to track the windows
 * we care about.  Exactly one of `window` (paired with `overlay_*`) and
 * `children` is non-NULL.
 *
 * ?overlay_window: the window we created over the top of the tracked window
 * ?overlay_font_gc: for drawing the text on `overlay_window`
 * ?overlay_fbg_gc: for drawing the background on `overlay_window`
 * ?overlay_rect: the on-screen area covered by `overlay_window`
 * ?window: the pre-existing tracked window
 * character: the character that must be typed to select `window`, or to descend
 *     into `children`
 * ?children: the continuation of the structure
 * children_size: size of `children` (0 if `children` is NULL)
 */
typedef struct window_setup_t {
    xcb_window_t* overlay_window;
    xcb_gcontext_t* overlay_font_gc;
    xcb_gcontext_t* overlay_bg_gc;
    xcb_rectangle_t* overlay_rect;
    xcb_window_t* window;
    char character;
    struct window_setup_t* children;
    int children_size;
} window_setup_t;


// -- globals

/**
 * The connection to the X server.
 */
xcb_connection_t* xcon;
/**
 * The root window.
 */
xcb_window_t xroot;
/**
 * The state for `xcb_ewmh`.
 */
xcb_ewmh_connection_t ewmh;
/**
 * Cached key symbols.
 */
xcb_key_symbols_t* ksymbols;
/**
 * Text colour for overlay windows.
 */
int FG_COLOUR = 0xffffffff;
/**
 * Background colour for overlay windows.
 */
int BG_COLOUR = 0xff333333;
/**
 * Window class set on overlay windows.
 */
char* OVERLAY_WINDOW_CLASS = "overlay\0xorg-choose-window";
/**
 * Number of windows requested from _NET_CLIENT_LIST.
 */
int MAX_WINDOWS = 1024;

/**
 * Keysyms with an obvious 1-character representation.  Only these characters
 * may be used as input.
 */
keysyms_lookup_t all_keysyms_lookup[] = {
    {'0', 0x0030},
    {'1', 0x0031},
    {'2', 0x0032},
    {'3', 0x0033},
    {'4', 0x0034},
    {'5', 0x0035},
    {'6', 0x0036},
    {'7', 0x0037},
    {'8', 0x0038},
    {'9', 0x0039},
    {'a', 0x0061},
    {'b', 0x0062},
    {'c', 0x0063},
    {'d', 0x0064},
    {'e', 0x0065},
    {'f', 0x0066},
    {'g', 0x0067},
    {'h', 0x0068},
    {'i', 0x0069},
    {'j', 0x006a},
    {'k', 0x006b},
    {'l', 0x006c},
    {'m', 0x006d},
    {'n', 0x006e},
    {'o', 0x006f},
    {'p', 0x0070},
    {'q', 0x0071},
    {'r', 0x0072},
    {'s', 0x0073},
    {'t', 0x0074},
    {'u', 0x0075},
    {'v', 0x0076},
    {'w', 0x0077},
    {'x', 0x0078},
    {'y', 0x0079},
    {'z', 0x007a}
};
/**
 * Size of `all_keysyms_lookup`.
 */
int all_keysyms_lookup_size = (
    sizeof(all_keysyms_lookup) / sizeof(*all_keysyms_lookup));


// -- utilities

/**
 * Compute the smaller of two numbers.
 */
int min (int a, int b) {
    return a < b ? a : b;
}


/**
 * Compute the larger of two numbers.
 */
int max (int a, int b) {
    return a < b ? b : a;
}


/**
 * Print a message to stderr and exit the process with a failure status.  Takes
 * arguments like `*printf`.
 */
void die (char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(1);
}


/**
 * Exit the process with a status indicating a window was chosen.
 */
void exit_match () {
    exit(0);
}


/**
 * Exit the process with a status indicating no window was chosen.
 */
void exit_no_match () {
    exit(0);
}


/**
 * Print the chosen window to stdout and exit the process.
 */
void choose_window (xcb_window_t window) {
    printf("%d\n", window);
    exit_match();
}


// -- xorg utilities

/**
 * Perform the check for a checked, reply-less xcb request.
 *
 * cookie: corresponding to the request
 * msg: to print in case of error
 */
void xorg_check_request (xcb_void_cookie_t cookie, char *msg) {
    xcb_generic_error_t *error = xcb_request_check(xcon, cookie);
    if (error) die("xcb error: %s (%d)\n", msg, error->error_code);
}


/**
 * Move and resize a window.
 *
 * x, y: new absolute screen location
 */
void xorg_window_move_resize (xcb_window_t window, int x, int y, int w, int h) {
    int mask = (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT);
    uint32_t values[] = { x, y, w, h };
    xcb_configure_window(xcon, window, mask, values);
}


/**
 * Determine whether a window has a property defined.
 *
 * prop: property ID
 */
int xorg_window_has_property (xcb_window_t window, xcb_atom_t prop) {
    xcb_list_properties_cookie_t lpc = xcb_list_properties(xcon, window);
    xcb_list_properties_reply_t* lpr;
    if (!(lpr = xcb_list_properties_reply(xcon, lpc, NULL))) {
        die("list_properties");
    }

    xcb_atom_t* props = xcb_list_properties_atoms(lpr);
    int result = 0;
    for (int i = 0; i < xcb_list_properties_atoms_length(lpr); i++) {
        if (props[i] == prop) {
            result = 1;
            break;
        }
    }

    free(lpr);
    return result;
}


/**
 * Determine whether a window is 'normal' and visible according to the base Xorg
 * specification.
 */
int xorg_window_normal (xcb_window_t window) {
    xcb_get_window_attributes_cookie_t gwac = (
        xcb_get_window_attributes(xcon, window));
    xcb_get_window_attributes_reply_t* gwar;
    if (!(gwar = xcb_get_window_attributes_reply(xcon, gwac, NULL))) {
        die("get_window_attributes\n");
    }

    return (
        gwar->map_state == XCB_MAP_STATE_VIEWABLE &&
        gwar->override_redirect == 0
    );
}


/**
 * Determine whether a window is 'normal' according EWMH.
 */
int ewmh_window_normal (xcb_window_t window) {
    xcb_get_property_cookie_t gpc = (
        xcb_get_property(xcon, 0, window, ewmh._NET_WM_WINDOW_TYPE,
                         XCB_ATOM_ATOM, 0, 1));
    xcb_get_property_reply_t* gpr;
    if (!(gpr = xcb_get_property_reply(xcon, gpc, NULL))) {
        die("get_property _NET_WM_WINDOW_TYPE\n");
    }
    uint32_t* window_type = (uint32_t*)xcb_get_property_value(gpr);
    int prop_len = xcb_get_property_value_length(gpr);
    free(gpr);

    // if reply length is 0, window type isn't defined, so treat it as normal
    return prop_len == 0 || window_type[0] != ewmh._NET_WM_WINDOW_TYPE_NORMAL;
}


/**
 * Get all windows from the X server.
 *
 * windows (output): window IDs
 * windows_size (output): size of `windows`
 */
void xorg_get_windows (xcb_window_t** windows, int* windows_size) {
    xcb_query_tree_cookie_t qtc = xcb_query_tree(xcon, xroot);
    xcb_query_tree_reply_t* qtr;
    if (!(qtr = xcb_query_tree_reply(xcon, qtc, NULL))) die("query_tree\n");
    xcb_window_t* referenced_windows = xcb_query_tree_children(qtr);
    int size = xcb_query_tree_children_length(qtr);

    // copy for easier usage
    *windows = calloc(size, sizeof(xcb_window_t));
    for (int i = 0; i < size; i++) (*windows)[i] = referenced_windows[i];
    free(qtr);

    *windows_size = size;
}


/**
 * Get windows managed by the window manager.
 *
 * is_defined (output): whether the window manager defines the windows it
 *     tracks; if 0, `windows` and `windows_size` will not be set
 * windows (output): window IDs
 * windows_size (output): size of `windows`
 */
void xorg_get_managed_windows (int* is_defined, xcb_window_t** windows,
                               int* windows_size) {
    *is_defined = xorg_window_has_property(xroot, ewmh._NET_CLIENT_LIST);

    if (*is_defined) {
        xcb_get_property_cookie_t gpc = (
            xcb_get_property(xcon, 0, xroot, ewmh._NET_CLIENT_LIST,
                             XCB_ATOM_WINDOW, 0, MAX_WINDOWS));
        xcb_get_property_reply_t* gpr;
        if (!(gpr = xcb_get_property_reply(xcon, gpc, NULL))) {
            die("get_property _NET_CLIENT_LIST\n");
        }
        xcb_window_t* referenced_windows = (
            (xcb_window_t*)xcb_get_property_value(gpr));
        // each window ID is 4 bytes
        int size = xcb_get_property_value_length(gpr) / 4;

        // copy for easier usage
        *windows = calloc(size, sizeof(xcb_window_t));
        for (int i = 0; i < size; i++) (*windows)[i] = referenced_windows[i];
        free(gpr);

        *windows_size = size;
    }
}


/**
 * Determine whether a window is managed by the window manager.
 *
 * managed_windows: windows that are managed by the window manager
 */
int xorg_window_managed (xcb_window_t window, xcb_window_t* managed_windows,
                         int managed_windows_size) {
    int found = 0;
    for (int i = 0; i < managed_windows_size; i++) {
        if (managed_windows[i] == window) {
            found = 1;
            break;
        }
    }
    return found;
}


/**
 * Initialise the connection to the X server.  Sets `xcon`, `xroot`, `ewmh` and
 * `ksymbols`.
 */
void initialise_xorg () {
    int default_screen; // unused
    xcb_screen_t *screen;
    xcon = xcb_connect(NULL, &default_screen);
    if (xcb_connection_has_error(xcon)) die("connect\n");

    screen = xcb_setup_roots_iterator(xcb_get_setup(xcon)).data;
    if (screen == NULL) die("no screens\n");
    xroot = screen->root;

    xcb_intern_atom_cookie_t *ewmhc = xcb_ewmh_init_atoms(xcon, &ewmh);
    if (!xcb_ewmh_init_atoms_replies(&ewmh, ewmhc, NULL)) die("ewmh init\n");

    ksymbols = xcb_key_symbols_alloc(xcon);
    if (ksymbols == NULL) die("key_symbols_alloc\n");
}


// -- input handling

/**
 * Find an item in a keysym lookup given its character.
 *
 * ksl: keysym lookup to search
 * c: character to search for
 *
 * returns: found item, NULL if no matching items were found
 */
keysyms_lookup_t* keysyms_lookup_find_char (
    keysyms_lookup_t* ksl, int ksl_size, char c
) {
    for (int i = 0; i < ksl_size; i++) {
        if (ksl[i].character == c) {
            return &(ksl[i]);
        }
    }
    return NULL;
}


/**
 * Find an item in a keysym lookup given its keysym.
 *
 * ksl: keysym lookup to search
 * ksym: keysym to search for
 *
 * returns: found item, NULL if no matching items were found
 */
keysyms_lookup_t* keysyms_lookup_find_keysym (
    keysyms_lookup_t* ksl, int ksl_size, xcb_keysym_t ksym
) {
    for (int i = 0; i < ksl_size; i++) {
        if (ksl[i].keysym == ksym) {
            return &(ksl[i]);
        }
    }
    return NULL;
}


/**
 * Parse command-line arguments.
 *
 * argc, argv: as passed to `main`
 * ksl (output): keysym lookup containing allowed characters
 * ksl_size (output): size of `ksl`
 */
void parse_args (int argc, char** argv, keysyms_lookup_t** ksl, int* ksl_size) {
    if (argc != 2) die("expected exactly one argument\n");

    // character pool argument: check validity of characters, compile lookup
    char* char_pool = argv[1];
    // we won't put more than `char_pool` items in `ksl`
    *ksl = calloc(strlen(char_pool), sizeof(keysyms_lookup_t));
    int size = 0;

    for (int i = 0; i < strlen(char_pool); i++) {
        char c = char_pool[i];
        keysyms_lookup_t* ksl_item = keysyms_lookup_find_char(
            all_keysyms_lookup, all_keysyms_lookup_size, c);
        if (ksl_item == NULL) die("unknown character: %c\n", c);
        // don't allow duplicates in lookup
        if (keysyms_lookup_find_char(*ksl, size, c) == NULL) {
            (*ksl)[size] = *ksl_item;
            size += 1;
        }
    }

    if (size < 2) die("expected at least two characters\n");
    *ksl = realloc(*ksl, sizeof(keysyms_lookup_t) * size);
    *ksl_size = size;
}


/**
 * Acquire a Xorg keyboard grab on the root window.
 */
void initialise_input () {
    // wait a little for other programs to release the keyboard
    // since this program is likely to be launched from a hotkey daemon
    struct timespec ts = { 0, 1000000 }; // 1ms
    int status;
    for (int s = 0; s < 1000; s++) { // up to 1s total
        xcb_grab_keyboard_cookie_t gkc = xcb_grab_keyboard(
            xcon, 0, xroot, XCB_CURRENT_TIME,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_keyboard_reply_t* gkr;

        if ((gkr = xcb_grab_keyboard_reply(xcon, gkc, NULL))) {
            status = gkr->status;
            free(gkr);
            if (status == XCB_GRAB_STATUS_ALREADY_GRABBED) {
                nanosleep(&ts, NULL);
                continue;
            } else if (status == XCB_GRAB_STATUS_SUCCESS) {
                break;
            } else {
                die("grab_keyboard: %d\n", status);
            }
        } else {
            die("grab_keyboard\n");
        }
    }

    if (status == XCB_GRAB_STATUS_ALREADY_GRABBED) {
        die("grab_keyboard: %d\n", status);
    }
}


// -- overlay windows

/**
 * Create an overlay window.
 *
 * x, y: absolute screen location
 * w, h: window size
 */
xcb_window_t* overlay_create (int x, int y, int w, int h) {
    xcb_window_t* win = malloc(sizeof(xcb_window_t));
    *win = xcb_generate_id(xcon);
    uint32_t mask = (XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                     XCB_CW_SAVE_UNDER | XCB_CW_EVENT_MASK);
    uint32_t values[] = {
        BG_COLOUR, 1, 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
    };
    xcb_void_cookie_t cwc = xcb_create_window_checked(
        xcon, XCB_COPY_FROM_PARENT, *win, xroot, 0, 0, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);
    xorg_check_request(cwc, "create_window");

    xcb_icccm_set_wm_class(
        xcon, *win, sizeof(OVERLAY_WINDOW_CLASS), OVERLAY_WINDOW_CLASS);
    xorg_window_move_resize(*win, x, y, w, h);
    xcb_void_cookie_t mwc = xcb_map_window_checked(xcon, *win);
    xorg_check_request(mwc, "map_window");
    return win;
}


/**
 * Create a graphics context for drawing the background of an overlay window.
 *
 * win: the overlay window
 */
xcb_gc_t* overlay_get_bg_gc (xcb_window_t win) {
    xcb_gcontext_t* gc = malloc(sizeof(xcb_gcontext_t));
    *gc = xcb_generate_id(xcon);
    uint32_t mask = XCB_GC_FOREGROUND;
    uint32_t value_list[] = { BG_COLOUR };
    xcb_void_cookie_t cgc = (
        xcb_create_gc_checked(xcon, *gc, win, mask, value_list));
    xorg_check_request(cgc, "create_gc");
    return gc;
}


/**
 * Create a graphics context for drawing the text of an overlay window.
 *
 * win: the overlay window
 * font_name: name of the font used to render the text
 */
xcb_gc_t* overlay_get_font_gc (xcb_window_t win, char *font_name) {
    xcb_font_t font = xcb_generate_id(xcon);
    xcb_void_cookie_t ofc = (
        xcb_open_font_checked(xcon, font, strlen(font_name), font_name));
    xorg_check_request(ofc, "open_font");

    xcb_gcontext_t* gc = malloc(sizeof(xcb_gcontext_t));
    *gc = xcb_generate_id(xcon);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
    uint32_t value_list[] = { FG_COLOUR, BG_COLOUR, font };
    xcb_void_cookie_t cgc = (
        xcb_create_gc_checked(xcon, *gc, win, mask, value_list));
    xorg_check_request(cgc, "create_gc");

    xcb_close_font(xcon, font);
    return gc;
}


/**
 * Set the text on an overlay window.  `xcb_flush` should be called after
 * calling this function.
 *
 * wsetup: containing the overlay window (if there is no overlay window, this
 *     function does nothing)
 * text: text to render (null-terminated)
 */
void overlay_set_text (window_setup_t* wsetup, char* text) {
    if (wsetup->overlay_window == NULL) return;
    xcb_window_t win = *(wsetup->overlay_window);

    if (wsetup->overlay_bg_gc == NULL) {
        wsetup->overlay_bg_gc = overlay_get_bg_gc(win);
    }
    if (wsetup->overlay_font_gc == NULL) {
        wsetup->overlay_font_gc = overlay_get_font_gc(win, "fixed");
    }

    xcb_poly_fill_rectangle(xcon, win, *(wsetup->overlay_bg_gc), 1,
                            wsetup->overlay_rect);
    xcb_image_text_8(xcon, strlen(text), win, *(wsetup->overlay_font_gc),
                     30, 20, text);
}


/**
 * See `overlays_set_text`.  `xcb_flush` should be called after calling * this
 * function.
 *
 * text: prefix to text rendered to every overlay window (null-terminated)
 */
void _overlays_set_text (window_setup_t* wsetups, int wsetups_size,
                         char* text) {
    for (int i = 0; i < wsetups_size; i++) {
        window_setup_t* wsetup = &(wsetups[i]);
        int text_size = strlen(text);
        // next level down is 1 character longer, plus 1 for null
        char* new_text = calloc(text_size + 2, sizeof(char));
        strcpy(new_text, text);
        new_text[text_size] = wsetup->character;
        new_text[text_size + 1] = '\0';

        overlay_set_text(wsetup, new_text);
        if (wsetup->children != NULL) {
            _overlays_set_text(
                wsetup->children, wsetup->children_size, new_text);
        }

        free(new_text);
    }
}


/**
 * Update text on all overlay windows in a setup structure.
 *
 * wsetups: array containing structures containing overlay windows, possibly
 *    nested
 */
void overlays_set_text (window_setup_t* wsetups, int wsetups_size) {
    char* text = "";
    _overlays_set_text(wsetups, wsetups_size, text);
    xcb_flush(xcon);
}


// -- wsetup utilities


/**
 * See `initialise_window_tracking`.
 *
 * remain_depth: number of nested levels remaining (we choose a character in the
 * string to type at every level; if 0, we choose the last character)
 */
void _initialise_window_tracking (
    keysyms_lookup_t* ksl, int ksl_size,
    xcb_window_t* windows, int windows_size, int remain_depth,
    window_setup_t** wsetups, int* wsetups_size
) {
    if (remain_depth == 0) {
        *wsetups = calloc(windows_size, sizeof(window_setup_t));
        *wsetups_size = windows_size;
        xcb_get_geometry_cookie_t ggc;
        xcb_get_geometry_reply_t* ggr;

        for (int i = 0; i < windows_size; i++) {
            // an xcb_window_t is an xcb_drawable_t
            ggc = xcb_get_geometry(xcon, windows[i]);
            if (!(ggr = xcb_get_geometry_reply(xcon, ggc, NULL))) {
                die("get_geometry\n");
            }

            xcb_rectangle_t rect = { 0, 0, ggr->width, ggr->height };
            xcb_rectangle_t* rectp = malloc(sizeof(xcb_rectangle_t));
            *rectp = rect;
            // guaranteed that ksl_size <= windows_size
            xcb_window_t* w = overlay_create(
                ggr->border_width + ggr->x, ggr->border_width + ggr->y,
                rect.width, rect.height);

            window_setup_t wsetup = {
                w, NULL, NULL, rectp, &windows[i], ksl[i].character, NULL, 0
            };
            (*wsetups)[i] = wsetup;
        }


    } else {
        // number of windows 'used up' per iteration
        int p = (int)(pow(ksl_size, remain_depth));
        // required number of iterations to use all windows
        int n = (int)(ceil((float)windows_size / (float)p));
        *wsetups = calloc(n, sizeof(window_setup_t));
        *wsetups_size = n;

        for (int i = 0; i < n; i++) {
            window_setup_t* children = NULL;
            int children_size;
            _initialise_window_tracking(
                ksl, ksl_size,
                windows + (i * p), min(windows_size - (i * p), p),
                remain_depth - 1, &children, &children_size);
            window_setup_t wsetup = {
                NULL, NULL, NULL, NULL, NULL, ksl[i].character,
                children, children_size
            };
            (*wsetups)[i] = wsetup;
        }
    }
}


/**
 * Construct data for tracked windows in a nested structure matching the
 * characters that need to be typed to choose them.
 *
 * ksl: lookup for allowed characters
 * windows: tracked windows
 * wsetups (output): generated structure
 * wsetups_size (output): size of `wsetups`
 */
void initialise_window_tracking (
    keysyms_lookup_t* ksl, int ksl_size,
    xcb_window_t* windows, int windows_size,
    window_setup_t** wsetups, int* wsetups_size
) {
    _initialise_window_tracking(
        ksl, ksl_size, windows, windows_size,
        // the length of each tracking string
        (int)(log(max(windows_size - 1, 1)) / log(ksl_size)),
        wsetups, wsetups_size);
}


/**
 * Destroy all overlay windows in a setup structure and free the structure's
 * used memory.
 */
void wsetup_free (window_setup_t* wsetup) {
    if (wsetup->children != NULL) {
        for (int i = 0; i < wsetup->children_size; i++) {
            window_setup_t* child = &((wsetup->children)[i]);
            xcb_window_t* w = child->overlay_window;
            if (w != NULL) {
                xcb_destroy_window_checked(xcon, *w);
                free(child->overlay_rect);
            }
            wsetup_free(child);

            if (wsetup->overlay_bg_gc != NULL) {
                xcb_free_gc(xcon, *(wsetup->overlay_bg_gc));
                free(wsetup->overlay_bg_gc);
            }
            if (wsetup->overlay_font_gc != NULL) {
                xcb_free_gc(xcon, *(wsetup->overlay_font_gc));
                free(wsetup->overlay_font_gc);
            }
        }

        free(wsetup->children);
    }

    xcb_flush(xcon);
}


// mutually recursive with `wsetups_descend`
void wsetup_descend(window_setup_t* wsetup, window_setup_t** new_wsetups,
                    int* new_wsetups_size);
/**
 * Reduce a setup structure recursively, if possible, by removing redundant
 * 'outer layers'.  Updates text rendered on overlay windows.  Exits the process
 * if we end up with 0 or 1 windows.
 *
 * wsetups: array of setup structures
 * new_wsetups (output): reduced structure
 * new_wsetups_size (output): `new_wsetups` size
 */
void wsetups_descend (window_setup_t* wsetups, int wsetups_size,
                      window_setup_t** new_wsetups, int* new_wsetups_size) {
    if (wsetups_size == 0) {
        exit_no_match();
    } else if (wsetups_size == 1) {
        wsetup_descend(&wsetups[0], new_wsetups, new_wsetups_size);
    } else {
        *new_wsetups = wsetups;
        *new_wsetups_size = wsetups_size;
        overlays_set_text(*new_wsetups, *new_wsetups_size);
    }
}


/**
 * Reduce a setup structure recursively, if possible, by removing redundant
 * 'outer layers'.  Updates text rendered on overlay windows.  Exits the process
 * if we end up with 0 or 1 windows.
 *
 * wsetups: setup structure
 * new_wsetups (output): reduced structure
 * new_wsetups_size (output): `new_wsetups` size
 */
void wsetup_descend (window_setup_t* wsetup,
                     window_setup_t** new_wsetups, int* new_wsetups_size) {
    if (wsetup->window == NULL) {
        wsetups_descend(wsetup->children, wsetup->children_size,
                        new_wsetups, new_wsetups_size);
    } else if (wsetup->children_size == 0) {
        choose_window(*(wsetup->window));
    } else {
        wsetups_descend(wsetup->children, wsetup->children_size,
                        new_wsetups, new_wsetups_size);
    }
}


/**
 * Reduce a setup structure by choosing an item, then reduce recursively like
 * `wsetup_descend`.  Frees removed parts of the structure.
 *
 * wsetups: array of setup structures
 * index: array index in `wsetups` to choose
 * new_wsetups (output): reduced structure
 * new_wsetups_size (output): `new_wsetups` size
 */
void wsetups_descend_by_index (
    window_setup_t* wsetups, int wsetups_size, int index,
    window_setup_t** new_wsetups, int* new_wsetups_size
) {
    for (int i = 0; i < wsetups_size; i++) {
        if (i == index) {
            wsetup_descend(&(wsetups[i]), new_wsetups, new_wsetups_size);
        } else {
            wsetup_free(&(wsetups[i]));
        }
    }
}


/**
 * Reduce a setup structure by choosing a character, then reduce recursively
 * like `wsetup_descend`.  Exits the process if the character doesn't correspond
 * to any options.  Frees removed parts of the structure.
 *
 * wsetups: array of setup structures
 * c: character to choose
 * new_wsetups (output): reduced structure
 * new_wsetups_size (output): `new_wsetups` size
 */
void wsetups_descend_by_char (
    window_setup_t* wsetups, int wsetups_size, char c,
    window_setup_t** new_wsetups, int* new_wsetups_size
) {
    int index = -1;
    for (int i = 0; i < wsetups_size; i++) {
        if (wsetups[i].character == c) {
            index = i;
            break;
        }
    }

    if (index == -1) {
        exit_no_match();
    } else {
        wsetups_descend_by_index(wsetups, wsetups_size, index,
                                 new_wsetups, new_wsetups_size);
    }
}


// -- program

/**
 * Get the windows to track.
 *
 * windows (output): window IDs
 * windows_size (output): size of `windows`
 */
void initialise_tracked_windows (xcb_window_t** windows, int* windows_size) {
    xcb_window_t* all_windows;
    int all_windows_size;
    xorg_get_windows(&all_windows, &all_windows_size);
    int managed_windows_defined;
    xcb_window_t* managed_windows;
    int managed_windows_size;
    xorg_get_managed_windows(&managed_windows_defined, &managed_windows,
                             &managed_windows_size);

    *windows = calloc(all_windows_size, sizeof(xcb_window_t));
    int size = 0;
    for (int i = 0; i < all_windows_size; i++) {
        if (
            // ignore if not managed by the window manager
            !(managed_windows_defined && !xorg_window_managed(
                all_windows[i], managed_windows, managed_windows_size
            )) &&

            xorg_window_normal(all_windows[i]) &&

            ewmh_window_normal(all_windows[i])
        ) {
            (*windows)[size] = all_windows[i];
            size += 1;
        }
    }
    *windows = realloc(*windows, size * sizeof(xcb_window_t));
    *windows_size = size;

    if (managed_windows_defined) free(managed_windows);
    free(all_windows);
}


/**
 * Make adjustments to tracking windows based on a keypress event.  Exits the
 * process if this chooses a window.
 *
 * ksl: lookup for allowed characters
 * wsetups: current window tracking information
 * new_wsetups (output): new window tracking information after adjustments
 * new_wsetups_size (output): size of `new_wsetups`
 */
void handle_keypress (
    xcb_key_press_event_t* kp, keysyms_lookup_t* ksl, int ksl_size,
    window_setup_t* wsetups, int wsetups_size,
    window_setup_t** new_wsetups, int* new_wsetups_size
) {
    xcb_keysym_t ksym = xcb_key_press_lookup_keysym(ksymbols, kp, 0);
    keysyms_lookup_t* ksl_item = (
        keysyms_lookup_find_keysym(ksl, ksl_size, ksym));

    if (ksl_item == NULL) {
        exit_no_match();
    } else {
        wsetups_descend_by_char(wsetups, wsetups_size, ksl_item->character,
                                new_wsetups, new_wsetups_size);
    }
}


int main (int argc, char** argv) {
    keysyms_lookup_t* ksl;
    int ksl_size;
    parse_args(argc, argv, &ksl, &ksl_size);
    initialise_xorg();
    initialise_input();

    xcb_window_t* windows;
    int windows_size;
    window_setup_t* wsetups = NULL;
    int wsetups_size;
    initialise_tracked_windows(&windows, &windows_size);
    if (windows_size == 0) exit_no_match(); // nothing to do
    initialise_window_tracking(ksl, ksl_size, windows, windows_size,
                               &wsetups, &wsetups_size);
    wsetups_descend(wsetups, wsetups_size, &wsetups, &wsetups_size);

    xcb_generic_event_t *event;
    while (1) if ((event = xcb_poll_for_event(xcon))) {
        switch (event->response_type & ~0x80) {
            case 0: {
                xcb_generic_error_t* evterr = (xcb_generic_error_t*) event;
                die("event loop error: %d\n", evterr->error_code);
                break;
            }
            case XCB_EXPOSE: {
                overlays_set_text(wsetups, wsetups_size);
                break;
            }
            case XCB_KEY_PRESS: {
                handle_keypress((xcb_key_press_event_t*)event, ksl, ksl_size,
                                wsetups, wsetups_size, &wsetups, &wsetups_size);
                break;
            }
        }

        free(event);
    }

    return 0;
}
