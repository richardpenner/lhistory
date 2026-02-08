#ifndef LHISTORY_INPUT_H
#define LHISTORY_INPUT_H

typedef enum {
    LH_KEY_NONE = 0,
    LH_KEY_UP,
    LH_KEY_DOWN,
    LH_KEY_LEFT,
    LH_KEY_RIGHT,
    LH_KEY_ENTER,
    LH_KEY_ESCAPE,
    LH_KEY_BACKSPACE,
    LH_KEY_DELETE,
    LH_KEY_SLASH,
    LH_KEY_TAB,
    LH_KEY_CHAR,     /* regular character — stored in .ch */
    /* vim-style */
    LH_KEY_J,
    LH_KEY_K,
    LH_KEY_H,
    LH_KEY_L,
    LH_KEY_Q,
    LH_KEY_G,        /* go to top (gg) */
    LH_KEY_G_SHIFT,  /* go to bottom (G) */
} lh_key_type;

typedef struct {
    lh_key_type type;
    char ch;          /* the literal character for LH_KEY_CHAR */
} lh_key;

/* Read a single key event from the terminal fd (blocking).
 * fd should be the tty in raw mode. */
lh_key lh_input_read(int fd);

#endif
