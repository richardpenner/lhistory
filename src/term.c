#include "term.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static int tty_fd = -1;
static struct termios orig_termios;
static int raw_mode_active = 0;
static void (*resize_callback)(void) = NULL;

static void sigwinch_handler(int sig) {
    (void)sig;
    if (resize_callback) resize_callback();
}

int lh_term_raw_enter(void) {
    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) return -1;

    if (tcgetattr(tty_fd, &orig_termios) < 0) {
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(tty_fd, TCSAFLUSH, &raw) < 0) {
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }

    raw_mode_active = 1;

    lh_term_puts("\x1b[?25l");
    lh_term_puts("\x1b[?1049h");

    return tty_fd;
}

void lh_term_raw_exit(void) {
    if (!raw_mode_active) return;

    lh_term_puts("\x1b[?1049l");
    lh_term_puts("\x1b[?25h");

    tcsetattr(tty_fd, TCSAFLUSH, &orig_termios);
    close(tty_fd);
    tty_fd = -1;
    raw_mode_active = 0;
}

lh_term_size lh_term_get_size(void) {
    lh_term_size size = {24, 80};
    struct winsize ws;
    int fd = tty_fd >= 0 ? tty_fd : STDOUT_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        size.rows = ws.ws_row;
        size.cols = ws.ws_col;
    }
    return size;
}

int lh_term_fd(void) {
    return tty_fd;
}

void lh_term_write(const char *buf, int len) {
    if (tty_fd >= 0) {
        (void)write(tty_fd, buf, (size_t)len);
    }
}

void lh_term_puts(const char *s) {
    lh_term_write(s, (int)strlen(s));
}

int lh_term_query_cursor(int *row, int *col) {
    if (tty_fd < 0) return -1;
    if (write(tty_fd, "\x1b[6n", 4) != 4) return -1;

    char buf[32];
    int pos = 0;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(tty_fd, &fds);
        struct timeval tv = {0, 100000};
        if (select(tty_fd + 1, &fds, NULL, NULL, &tv) <= 0) return -1;
        if (read(tty_fd, &buf[pos], 1) != 1) return -1;
        if (buf[pos] == 'R') break;
        if (++pos >= (int)sizeof(buf) - 1) return -1;
    }
    buf[pos + 1] = '\0';

    int r = 0, c = 0;
    char *semi = NULL;
    for (int i = 0; i <= pos; i++) {
        if (buf[i] == '[') { r = atoi(&buf[i + 1]); }
        if (buf[i] == ';') { semi = &buf[i]; c = atoi(&buf[i + 1]); }
    }
    if (!semi || r < 1 || c < 1) return -1;
    *row = r;
    *col = c;
    return 0;
}

void lh_term_on_resize(void (*callback)(void)) {
    resize_callback = callback;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
}
