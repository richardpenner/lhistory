#include "input.h"
#include <sys/select.h>
#include <unistd.h>

static int read_byte(int fd) {
    unsigned char c;
    ssize_t n = read(fd, &c, 1);
    if (n <= 0) return -1;
    return c;
}

/* Non-blocking read: returns byte or -1 if nothing available within timeout_us */
static int read_byte_timeout(int fd, int timeout_us) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0, timeout_us};
    if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
        return read_byte(fd);
    }
    return -1;
}

lh_key lh_input_read(int fd) {
    lh_key key = {LH_KEY_NONE, 0};

    int c = read_byte(fd);
    if (c < 0) return key;

    /* Escape sequences */
    if (c == 27) {
        /* Wait briefly — if more bytes follow, it's an escape sequence.
         * If not, it's a bare Escape keypress. */
        int c2 = read_byte_timeout(fd, 50000); /* 50ms */
        if (c2 < 0) {
            key.type = LH_KEY_ESCAPE;
            return key;
        }
        if (c2 == '[') {
            int c3 = read_byte(fd);
            switch (c3) {
                case 'A': key.type = LH_KEY_UP; return key;
                case 'B': key.type = LH_KEY_DOWN; return key;
                case 'C': key.type = LH_KEY_RIGHT; return key;
                case 'D': key.type = LH_KEY_LEFT; return key;
                case '3': {
                    int c4 = read_byte(fd);
                    if (c4 == '~') key.type = LH_KEY_DELETE;
                    return key;
                }
            }
            return key; /* unknown escape sequence */
        }
        /* Alt+key or bare escape — treat as escape */
        key.type = LH_KEY_ESCAPE;
        return key;
    }

    /* Control characters */
    if (c == 3) { key.type = LH_KEY_ESCAPE; return key; } /* Ctrl+C — exit */
    if (c == '\r' || c == '\n') { key.type = LH_KEY_ENTER; return key; }
    if (c == 127 || c == 8) { key.type = LH_KEY_BACKSPACE; return key; }
    if (c == '\t') { key.type = LH_KEY_TAB; return key; }

    /* Regular characters */
    switch (c) {
        case '/': key.type = LH_KEY_SLASH; return key;
        case 'j': key.type = LH_KEY_J; return key;
        case 'k': key.type = LH_KEY_K; return key;
        case 'h': key.type = LH_KEY_H; return key;
        case 'l': key.type = LH_KEY_L; return key;
        case 'q': key.type = LH_KEY_Q; return key;
        case 'g': key.type = LH_KEY_G; return key;
        case 'G': key.type = LH_KEY_G_SHIFT; return key;
        default:
            key.type = LH_KEY_CHAR;
            key.ch = (char)c;
            return key;
    }
}
