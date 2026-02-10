#ifndef LHISTORY_TERM_H
#define LHISTORY_TERM_H

typedef struct {
    int rows;
    int cols;
} lh_term_size;

/* Enter raw mode on /dev/tty. Returns fd to tty, or -1 on error. */
int lh_term_raw_enter(void);

/* Restore terminal to original mode. */
void lh_term_raw_exit(void);

/* Get terminal dimensions. */
lh_term_size lh_term_get_size(void);

/* Returns the tty file descriptor (valid after raw_enter). */
int lh_term_fd(void);

/* Write raw bytes to the terminal. */
void lh_term_write(const char *buf, int len);

/* Write a null-terminated string to the terminal. */
void lh_term_puts(const char *s);

/* Query current cursor position. Returns 0 on success, -1 on timeout/error. */
int lh_term_query_cursor(int *row, int *col);

/* Install SIGWINCH handler. The provided callback is called on resize. */
void lh_term_on_resize(void (*callback)(void));

#endif
