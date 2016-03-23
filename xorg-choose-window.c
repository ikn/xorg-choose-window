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
// cleanup: structure, types, names, comments
// sort out error handling (exit codes, man xcb-requests), memory management, exit cleanup
// mask usages (x3): what should the order be?  doc says just pass in one
// open font once, globally
// put globals in a thing we pass around
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


typedef struct keysym_lookup_t {
    char character;
    xcb_keysym_t keysym;
} keysym_lookup_t;

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


xcb_connection_t *xcon;
xcb_window_t xroot;
int FG_COLOUR = 0xffffffff;
int BG_COLOUR = 0xff333333;
char* OVERLAY_WINDOW_CLASS = "overlay\0xorg-choose-window";
int MAX_WINDOWS = 1024;

// keysyms with an obvious 1-character representation
keysym_lookup_t keysym_lookup[] = {
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
int keysym_lookup_size = sizeof(keysym_lookup) / sizeof(*keysym_lookup);


int min (int a, int b) {
    return a < b ? a : b;
}


int max (int a, int b) {
    return a < b ? b : a;
}


void die (char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(1);
}


void exit_match () {
    exit(0);
}


void exit_no_match () {
    exit(0);
}



keysym_lookup_t* find_char_in_lookup (keysym_lookup_t* lookup, int lookup_size,
                                      char c) {
    for (int i = 0; i < lookup_size; i++) {
        if (lookup[i].character == c) {
            return &(lookup[i]);
        }
    }
    return NULL;
}


keysym_lookup_t* find_ks_in_lookup (keysym_lookup_t* lookup, int lookup_size,
                                    xcb_keysym_t ksym) {
    for (int i = 0; i < lookup_size; i++) {
        if (lookup[i].keysym == ksym) {
            return &(lookup[i]);
        }
    }
    return NULL;
}


static void check_cookie (xcb_void_cookie_t cookie, char *msg) {
    xcb_generic_error_t *error = xcb_request_check(xcon, cookie);
    if (error) die("xcb error: %s (%d)\n", msg, error->error_code);
}


void window_move_resize (xcb_window_t win, int x, int y, int w, int h) {
    int mask = (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT);
    uint32_t values[] = { x, y, w, h };
    xcb_configure_window(xcon, win, mask, values);
}

int window_has_property (xcb_window_t window, xcb_atom_t prop) {
    xcb_list_properties_cookie_t c = xcb_list_properties(xcon, window);
    xcb_list_properties_reply_t* r;
    if (!(r = xcb_list_properties_reply(xcon, c, NULL))) {
        die("list_properties");
    }
    xcb_atom_t* props = xcb_list_properties_atoms(r);
    for (int i = 0; i < xcb_list_properties_atoms_length(r); i++) {
        if (props[i] == prop) return 1;
    }
    return 0;
}


xcb_window_t create_overlay_window (int x, int y, int w, int h) {
    xcb_window_t win = xcb_generate_id(xcon);
    uint32_t mask = (XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                     XCB_CW_SAVE_UNDER | XCB_CW_EVENT_MASK);
    uint32_t values[] = {
        BG_COLOUR, 1, 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
    };
    xcb_void_cookie_t cookie = xcb_create_window_checked(
        xcon, XCB_COPY_FROM_PARENT, win, xroot, 0, 0, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);
    check_cookie(cookie, "create_window");

    xcb_icccm_set_wm_class(xcon, win, sizeof(OVERLAY_WINDOW_CLASS),
                           OVERLAY_WINDOW_CLASS);
    window_move_resize(win, x, y, w, h);
    xcb_void_cookie_t map_c = xcb_map_window_checked(xcon, win);
    check_cookie(map_c, "map_window");
    return win;
}


void destroy_wsetup (window_setup_t* wsetup) {
    if (wsetup->children != NULL) {
        for (int i = 0; i < wsetup->children_size; i++) {
            window_setup_t* child = &((wsetup->children)[i]);
            if (wsetup->overlay_bg_gc != NULL) {
                xcb_free_gc(xcon, *(wsetup->overlay_bg_gc));
                free(wsetup->overlay_bg_gc);
            }
            if (wsetup->overlay_font_gc != NULL) {
                xcb_free_gc(xcon, *(wsetup->overlay_font_gc));
                free(wsetup->overlay_font_gc);
            }
            xcb_window_t* w = child->overlay_window;
            if (w != NULL) {
                xcb_destroy_window_checked(xcon, *w);
                free(child->overlay_rect);
            }
            destroy_wsetup(child);
        }
        free(wsetup->children);
    }
    xcb_flush(xcon);
}


static xcb_gc_t get_bg_gc (xcb_window_t win) {
    xcb_gcontext_t gc = xcb_generate_id(xcon);
    uint32_t mask = XCB_GC_FOREGROUND;
    uint32_t value_list[] = { BG_COLOUR };
    xcb_void_cookie_t c = xcb_create_gc_checked(xcon, gc, win, mask,
                                                value_list);
    check_cookie(c, "create_gc");
    return gc;
}


static xcb_gc_t get_font_gc (xcb_window_t win, char *font_name) {
    xcb_font_t font = xcb_generate_id(xcon);
    xcb_void_cookie_t ofc = xcb_open_font_checked(xcon, font, strlen(font_name),
                                                  font_name);
    check_cookie(ofc, "open_font");

    xcb_gcontext_t gc = xcb_generate_id(xcon);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
    uint32_t value_list[] = { FG_COLOUR, BG_COLOUR, font };
    xcb_void_cookie_t c = xcb_create_gc_checked(xcon, gc, win, mask,
                                                value_list);
    check_cookie(c, "create_gc");

    xcb_close_font(xcon, font);
    return gc;
}


// should flush after calling
void set_window_text (window_setup_t* wsetup, char* text) {
    if (wsetup->overlay_window == NULL) return;
    xcb_window_t win = *(wsetup->overlay_window);

    if (wsetup->overlay_bg_gc == NULL) {
        wsetup->overlay_bg_gc = malloc(sizeof(xcb_gcontext_t));
        *(wsetup->overlay_bg_gc) = get_bg_gc(win);
    }
    if (wsetup->overlay_font_gc == NULL) {
        wsetup->overlay_font_gc = malloc(sizeof(xcb_gcontext_t));
        *(wsetup->overlay_font_gc) = get_font_gc(win, "fixed");
    }

    xcb_poly_fill_rectangle(xcon, win, *(wsetup->overlay_bg_gc), 1,
                            wsetup->overlay_rect);
    xcb_image_text_8(xcon, strlen(text), win, *(wsetup->overlay_font_gc),
                     30, 20, text);
}


// should flush after calling
void set_windows_texts_prefixed (window_setup_t* wsetups, int wsetups_size,
                                 char* text) {
    for (int i = 0; i < wsetups_size; i++) {
        window_setup_t* wsetup = &(wsetups[i]);
        int text_size = strlen(text);
        char* new_text = calloc(text_size + 2, sizeof(char));
        strcpy(new_text, text);
        new_text[text_size] = wsetup->character;
        new_text[text_size + 1] = '\0';

        set_window_text(wsetup, new_text);
        if (wsetup->children != NULL) {
            set_windows_texts_prefixed(wsetup->children, wsetup->children_size,
                                       new_text);
        }

        free(new_text);
    }
}


void set_windows_texts (window_setup_t* wsetups, int wsetups_size) {
    char* text = "";
    set_windows_texts_prefixed(wsetups, wsetups_size, text);
    xcb_flush(xcon);
}


int setup_windows (keysym_lookup_t* ksl, int ksl_size, xcb_window_t* windows,
                   int windows_size, char remain_depth,
                   window_setup_t** wsetups) {
    if (remain_depth == 0) {
        *wsetups = calloc(windows_size, sizeof(window_setup_t));
        for (int i = 0; i < windows_size; i++) {
            // an xcb_window_t is an xcb_drawable_t
            xcb_get_geometry_cookie_t ggc = xcb_get_geometry(xcon, windows[i]);
            xcb_get_geometry_reply_t* ggr;
            if (!(ggr = xcb_get_geometry_reply(xcon, ggc, NULL))) {
                die("get_geometry\n");
            }

            xcb_rectangle_t rect = { 0, 0, ggr->width, ggr->height };
            xcb_rectangle_t* rectp = malloc(sizeof(xcb_rectangle_t));
            *rectp = rect;
            // guaranteed that ksl_size <= windows_size
            xcb_window_t* w = malloc(sizeof(xcb_window_t));
            *w = create_overlay_window(
                ggr->border_width + ggr->x, ggr->border_width + ggr->y,
                rect.width, rect.height);

            window_setup_t wsetup = {
                w, NULL, NULL, rectp, &windows[i], ksl[i].character, NULL, 0
            };
            (*wsetups)[i] = wsetup;
        }
        return windows_size;
    } else {
        int p = (int)(pow(ksl_size, remain_depth));
        int n = (int)(ceil((float)windows_size / (float)p));
        *wsetups = calloc(n, sizeof(window_setup_t));

        for (int i = 0; i < n; i++) {
            window_setup_t* children = NULL;
            int children_size = setup_windows(ksl, ksl_size, windows + (i * p),
                                              min(windows_size - (i * p), p),
                                              remain_depth - 1, &children);
            window_setup_t wsetup = {
                NULL, NULL, NULL, NULL, NULL, ksl[i].character,
                children, children_size
            };
            (*wsetups)[i] = wsetup;
        }
        return n;
    }
}


void choose_window (window_setup_t* wsetup) {
    printf("%d\n", *(wsetup->window));
    exit_match();
}


void descend_in_wsetup(window_setup_t* wsetup, window_setup_t** new_wsetups,
                       int* new_wsetups_size);
void descend_in_wsetups (window_setup_t* wsetups, int wsetups_size,
                         window_setup_t** new_wsetups, int* new_wsetups_size) {
    if (wsetups_size == 0) {
        exit_no_match();
    } else if (wsetups_size == 1) {
        descend_in_wsetup(&wsetups[0], new_wsetups, new_wsetups_size);
    } else {
        *new_wsetups = wsetups;
        *new_wsetups_size = wsetups_size;
        set_windows_texts(*new_wsetups, *new_wsetups_size);
    }
}


void descend_in_wsetup (window_setup_t* wsetup, window_setup_t** new_wsetups,
                        int* new_wsetups_size) {
    if (wsetup->window == NULL) {
        descend_in_wsetups(wsetup->children, wsetup->children_size, new_wsetups,
                           new_wsetups_size);
    } else if (wsetup->children_size == 0) {
        choose_window(wsetup);
    } else {
        descend_in_wsetups(wsetup->children, wsetup->children_size, new_wsetups,
                           new_wsetups_size);
    }
}


void descend_in_wsetups_by_index (window_setup_t* wsetups, int wsetups_size,
                                  int index, window_setup_t** new_wsetups,
                                  int* new_wsetups_size) {
    for (int i = 0; i < wsetups_size; i++) {
        if (i == index) {
            descend_in_wsetup(&(wsetups[i]), new_wsetups, new_wsetups_size);
        } else {
            destroy_wsetup(&(wsetups[i]));
        }
    }
}


void descend_in_wsetups_by_char (window_setup_t* wsetups, int wsetups_size,
                                 char c, window_setup_t** new_wsetups,
                                 int* new_wsetups_size) {
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
        descend_in_wsetups_by_index(wsetups, wsetups_size, index, new_wsetups,
                                    new_wsetups_size);
    }
}


int main (int argc, char** argv) {
    if (argc != 2) die("expected exactly one argument\n");
    char* char_pool = argv[1];
    keysym_lookup_t* local_ks_lookup = calloc(
        strlen(char_pool), sizeof(keysym_lookup_t));
    int local_ks_lookup_size = 0;
    keysym_lookup_t* ksl;
    keysym_lookup_t* ksl2;

    for (size_t i = 0; i < strlen(char_pool); i++) {
        char c = char_pool[i];
        ksl = find_char_in_lookup(keysym_lookup, keysym_lookup_size, c);
        if (ksl == NULL) die("unknown character: %c\n", c);
        ksl2 = find_char_in_lookup(local_ks_lookup, local_ks_lookup_size, c);
        if (ksl2 == NULL) {
            local_ks_lookup[local_ks_lookup_size] = *ksl;
            local_ks_lookup_size += 1;
        }
    }

    if (local_ks_lookup_size < 2) die("expected at least two characters\n");
    local_ks_lookup = realloc(local_ks_lookup,
                              sizeof(keysym_lookup_t) * local_ks_lookup_size);


    int default_screen;
    xcb_screen_t *screen;
    xcon = xcb_connect(NULL, &default_screen);
    if (xcb_connection_has_error(xcon)) die("connect\n");
    // initialise ewmh atoms
    xcb_ewmh_connection_t ewmh;
    xcb_intern_atom_cookie_t *ewmhc = xcb_ewmh_init_atoms(xcon, &ewmh);
    if (!xcb_ewmh_init_atoms_replies(&ewmh, ewmhc, NULL)) die("ewmh init\n");
    screen = xcb_setup_roots_iterator(xcb_get_setup(xcon)).data;
    if (screen == NULL) die("no screens\n");
    xroot = screen->root;

    // wait a little for other programs to release the keyboard
    // since this program is likely to be launched from a hotkey daemon
    struct timespec ts = { 0, 1000000 };
    int status;
    for (int s = 0; s < 1000; s++) {
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
    if (status == XCB_GRAB_STATUS_ALREADY_GRABBED)
        die("grab_keyboard: %d\n", status);

    xcb_key_symbols_t* ksymbols = xcb_key_symbols_alloc(xcon);
    if (ksymbols == NULL) die("key_symbols_alloc\n");


    window_setup_t* wsetups = NULL;
    xcb_query_tree_cookie_t qtc = xcb_query_tree(xcon, xroot);
    xcb_query_tree_reply_t* qtr;
    if (!(qtr = xcb_query_tree_reply(xcon, qtc, NULL))) die("query_tree\n");
    int all_windows_size = xcb_query_tree_children_length(qtr);
    xcb_window_t* all_windows = xcb_query_tree_children(qtr);
    int windows_size = 0;
    xcb_window_t* windows = calloc(all_windows_size, sizeof(xcb_window_t));

    // retrieve window-manager-tracked windows
    int tracked_windows_exists = (
        window_has_property(xroot, ewmh._NET_CLIENT_LIST));
    int tracked_windows_size;
    xcb_window_t* tracked_windows;
    xcb_get_property_cookie_t gpc;
    xcb_get_property_reply_t* gpr;
    if (tracked_windows_exists) {
        gpc = (
            xcb_get_property(xcon, 0, xroot, ewmh._NET_CLIENT_LIST,
                             XCB_ATOM_WINDOW, 0, MAX_WINDOWS));
        if (!(gpr = xcb_get_property_reply(xcon, gpc, NULL))) {
            die("get_property _NET_CLIENT_LIST\n");
        }
        // each window ID is 4 bytes
        tracked_windows_size = xcb_get_property_value_length(gpr) / 4;
        tracked_windows = (xcb_window_t*)xcb_get_property_value(gpr);
    }

    for (int wi = 0; wi < all_windows_size; wi++) {
        // ignore if not tracked by the window manager
        if (tracked_windows_exists) {
            int found = 0;
            for (int wj = 0; wj < tracked_windows_size; wj++) {
                if (tracked_windows[wj] == all_windows[wi]) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        }

        // ignore if hidden or override-redirect
        xcb_get_window_attributes_cookie_t gwac = (
            xcb_get_window_attributes(xcon, all_windows[wi]));
        xcb_get_window_attributes_reply_t* gwar;
        if (!(gwar = xcb_get_window_attributes_reply(xcon, gwac, NULL))) {
            die("get_window_attributes\n");
        }
        if (!(
            gwar->map_state == XCB_MAP_STATE_VIEWABLE &&
            gwar->override_redirect == 0
        )) continue;

        // ignore if window type is not normal
        gpc = xcb_get_property(xcon, 0, all_windows[wi],
                               ewmh._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 1);
        xcb_get_property_reply_t* gpr2;
        if (!(gpr2 = xcb_get_property_reply(xcon, gpc, NULL))) {
            die("get_property _NET_WM_WINDOW_TYPE\n");
        }
        int prop_len = xcb_get_property_value_length(gpr2);
        uint32_t* window_type = (uint32_t*)xcb_get_property_value(gpr2);
        free(gpr2);
        // if reply length is 0, window type isn't defined, so don't ignore
        if (prop_len > 0 && window_type[0] != ewmh._NET_WM_WINDOW_TYPE_NORMAL) {
            continue;
        }

        windows[windows_size] = all_windows[wi];
        windows_size += 1;
    }

    if (tracked_windows_exists) free(gpr);

    windows = realloc(windows, windows_size * sizeof(xcb_window_t));

    if (windows_size == 0) exit_no_match(); // nothing to do
    int wsetups_size = setup_windows(
        local_ks_lookup, local_ks_lookup_size, windows, windows_size,
        (int)(log(max(windows_size - 1, 1)) / log(local_ks_lookup_size)),
        &wsetups);
    window_setup_t* current_wsetups = wsetups;
    int current_wsetups_size = wsetups_size;
    descend_in_wsetups(current_wsetups, current_wsetups_size, &current_wsetups,
                       &current_wsetups_size);


    xcb_generic_event_t *event;
    while (1) {
        if ((event = xcb_poll_for_event(xcon))) {
            switch (event->response_type & ~0x80) {
                case 0: {
                    xcb_generic_error_t* evterr = (xcb_generic_error_t*) event;
                    die("event loop error: %d\n", evterr->error_code);
                    break;
                }
                case XCB_EXPOSE: {
                    set_windows_texts(wsetups, wsetups_size);
                    break;
                }
                case XCB_KEY_PRESS: {
                    xcb_key_press_event_t *kp = (xcb_key_press_event_t *)event;
                    xcb_keysym_t ksym = (
                        xcb_key_press_lookup_keysym(ksymbols, kp, 0));

                    keysym_lookup_t* ksl = find_ks_in_lookup(
                        local_ks_lookup, local_ks_lookup_size, ksym);
                    if (ksl == NULL) {
                        exit_no_match();
                    } else {
                        descend_in_wsetups_by_char(
                            current_wsetups, current_wsetups_size,
                            ksl->character, &current_wsetups,
                            &current_wsetups_size);
                    }
                }
            }

            free(event);
        }
    }

    return 0;
}
