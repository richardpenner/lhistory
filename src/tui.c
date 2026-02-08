#include "tui.h"
#include "term.h"
#include "input.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MIN_COL_WIDTH 28
#define MAX_SESSIONS 16
#define MAX_COMMANDS_PER_SESSION 500
#define SEARCH_BUF_SIZE 256
#define RENDER_BUF_SIZE (64 * 1024)

/* Color scheme */
#define C_RESET    "\x1b[0m"
#define C_BOLD     "\x1b[1m"
#define C_DIM      "\x1b[2m"
#define C_HEADER_BG "\x1b[48;5;236m"
#define C_HEADER_FG "\x1b[38;5;117m"
#define C_SEL_BG   "\x1b[48;5;24m"
#define C_SEL_FG   "\x1b[38;5;255m"
#define C_ERR_FG   "\x1b[38;5;203m"
#define C_OK_FG    "\x1b[38;5;114m"
#define C_TIME_FG  "\x1b[38;5;243m"
#define C_SEARCH_FG "\x1b[38;5;222m"
#define C_BORDER   "\x1b[38;5;240m"
#define C_ACTIVE   "\x1b[38;5;75m"
#define C_STATUS_BG "\x1b[48;5;235m"
#define C_STATUS_FG "\x1b[38;5;250m"

typedef enum {
    MODE_NORMAL,
    MODE_SEARCH,
} tui_mode;

typedef struct {
    lh_session *session;
    lh_command_list commands;
    /* filtered indices (into commands.items) when searching */
    int *filtered;
    int filtered_count;
} tui_column;

typedef struct {
    sqlite3 *db;
    tui_column columns[MAX_SESSIONS];
    int num_columns;
    int visible_columns;
    int col_width;
    int col_offset;     /* first visible column index */
    int cur_col;        /* cursor column (within visible range) */
    int cur_row;        /* cursor row (0 = most recent) */
    int scroll_offset;  /* vertical scroll */
    lh_term_size size;
    tui_mode mode;
    char search_buf[SEARCH_BUF_SIZE];
    int search_len;
    int needs_redraw;
    char render_buf[RENDER_BUF_SIZE];
    int render_len;
    const char *current_session_id; /* pin this session to leftmost column */
} tui_state;

static tui_state *g_state = NULL;

static void resize_handler(void) {
    if (g_state) {
        g_state->size = lh_term_get_size();
        g_state->needs_redraw = 1;
    }
}

static void buf_append(tui_state *s, const char *str, int len) {
    if (s->render_len + len >= RENDER_BUF_SIZE) return;
    memcpy(s->render_buf + s->render_len, str, (size_t)len);
    s->render_len += len;
}

static void buf_puts(tui_state *s, const char *str) {
    buf_append(s, str, (int)strlen(str));
}

static void buf_printf(tui_state *s, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
        buf_append(s, tmp, n);
    }
}

/* Strip control characters (prevent terminal escape injection) */
static void sanitize_command(char *buf, int bufsize) {
    int j = 0;
    for (int i = 0; buf[i] && j < bufsize - 1; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x1b) {
            /* Skip ESC and any following CSI sequence */
            if (buf[i + 1] == '[') {
                i++;
                while (buf[i + 1] && !((unsigned char)buf[i + 1] >= 0x40 && (unsigned char)buf[i + 1] <= 0x7e))
                    i++;
                if (buf[i + 1]) i++; /* skip final byte */
            }
            continue;
        }
        if (c < 32 && c != '\t') continue; /* strip other control chars */
        buf[j++] = (char)c;
    }
    buf[j] = '\0';
}

/* Shorten a directory path: ~/projects/foo → ~/p/foo, or keep last component */
static void shorten_dir(const char *dir, char *out, int maxlen) {
    const char *home = getenv("HOME");
    const char *p = dir;

    /* Replace home prefix with ~ */
    char tmp[512];
    if (home && strncmp(dir, home, strlen(home)) == 0) {
        snprintf(tmp, sizeof(tmp), "~%s", dir + strlen(home));
        p = tmp;
    }

    int len = (int)strlen(p);
    if (len <= maxlen) {
        snprintf(out, (size_t)maxlen, "%s", p);
        return;
    }

    /* Find last component */
    const char *last_slash = strrchr(p, '/');
    if (last_slash) {
        int tail_len = (int)strlen(last_slash);
        int budget = maxlen - tail_len - 1;
        if (budget > 0 && budget < len) {
            /* Abbreviate intermediate dirs */
            char abbrev[512] = {0};
            int ai = 0;
            for (int i = 0; p[i] && &p[i] < last_slash && ai < budget; i++) {
                if (i == 0 || p[i - 1] == '/') {
                    abbrev[ai++] = p[i];
                    if (p[i] != '/' && p[i + 1] != '/') {
                        /* skip to next slash */
                        while (p[i + 1] && p[i + 1] != '/') i++;
                    }
                } else {
                    abbrev[ai++] = p[i];
                }
            }
            snprintf(out, (size_t)maxlen, "%s%s", abbrev, last_slash);
        } else {
            snprintf(out, (size_t)maxlen, "…%s", last_slash);
        }
    } else {
        snprintf(out, (size_t)maxlen, "%s", p + len - maxlen + 1);
    }
}

static void format_time(int64_t ts_ms, char *out, int maxlen) {
    time_t ts = (time_t)(ts_ms / 1000);
    struct tm *tm = localtime(&ts);
    snprintf(out, (size_t)maxlen, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void apply_search_filter(tui_state *s) {
    for (int c = 0; c < s->num_columns; c++) {
        tui_column *col = &s->columns[c];
        col->filtered_count = 0;

        if (s->search_len == 0) {
            /* No filter — all commands visible */
            for (int i = 0; i < col->commands.count; i++) {
                col->filtered[i] = i;
            }
            col->filtered_count = col->commands.count;
        } else {
            for (int i = 0; i < col->commands.count; i++) {
                if (strcasestr(col->commands.items[i].command, s->search_buf)) {
                    col->filtered[col->filtered_count++] = i;
                }
            }
        }
    }
}

static int max_visible_rows(tui_state *s) {
    /* title=1, header=1, border=1, status_bar=2 at bottom */
    return s->size.rows - 5;
}

/* Get the command at a given (col, row) in the filtered view.
 * Returns NULL if out of bounds. */
static lh_command *get_command(tui_state *s, int col_idx, int row) {
    int abs_col = s->col_offset + col_idx;
    if (abs_col < 0 || abs_col >= s->num_columns) return NULL;
    tui_column *col = &s->columns[abs_col];
    int idx = s->scroll_offset + row;
    if (idx < 0 || idx >= col->filtered_count) return NULL;
    return &col->commands.items[col->filtered[idx]];
}

static void render(tui_state *s) {
    s->render_len = 0;

    /* Clear screen, home cursor */
    buf_puts(s, "\x1b[H\x1b[2J");

    /* Recalculate visible columns */
    s->col_width = s->size.cols / (s->size.cols / MIN_COL_WIDTH);
    if (s->col_width < MIN_COL_WIDTH) s->col_width = MIN_COL_WIDTH;
    s->visible_columns = s->size.cols / s->col_width;
    if (s->visible_columns > s->num_columns) s->visible_columns = s->num_columns;
    if (s->visible_columns < 1) s->visible_columns = 1;

    /* Clamp cursor */
    if (s->cur_col >= s->visible_columns) s->cur_col = s->visible_columns - 1;
    if (s->col_offset + s->visible_columns > s->num_columns)
        s->col_offset = s->num_columns - s->visible_columns;
    if (s->col_offset < 0) s->col_offset = 0;

    int vis_rows = max_visible_rows(s);

    /* === Title bar === */
    {
        const char *title = " lhistory ";
        int tlen = (int)strlen(title);
        int pad_left = (s->size.cols - tlen) / 2;
        int pad_right = s->size.cols - tlen - pad_left;
        buf_puts(s, "\x1b[48;5;24m\x1b[38;5;255m" C_BOLD);
        for (int i = 0; i < pad_left; i++) buf_puts(s, " ");
        buf_puts(s, title);
        for (int i = 0; i < pad_right; i++) buf_puts(s, " ");
        buf_puts(s, C_RESET "\r\n");
    }

    /* === Header row === */
    buf_puts(s, C_HEADER_BG);
    for (int c = 0; c < s->visible_columns; c++) {
        int abs_c = s->col_offset + c;
        tui_column *col = &s->columns[abs_c];

        char dir_short[64];
        shorten_dir(col->session->directory, dir_short, s->col_width - 2);

        char header[128];
        if (col->session->ide) {
            char ide_upper[16];
            snprintf(ide_upper, sizeof(ide_upper), "%s", col->session->ide);
            /* capitalize first char */
            if (ide_upper[0] >= 'a' && ide_upper[0] <= 'z') ide_upper[0] -= 32;
            snprintf(header, sizeof(header), "[%s] %s", ide_upper, dir_short);
        } else {
            snprintf(header, sizeof(header), "%s", dir_short);
        }

        /* Truncate header to col width */
        int hlen = (int)strlen(header);
        if (hlen > s->col_width - 1) {
            header[s->col_width - 2] = '\0';
            hlen = s->col_width - 2;
        }

        if (c == s->cur_col) {
            buf_puts(s, C_ACTIVE C_BOLD);
        } else {
            buf_puts(s, C_HEADER_FG);
        }

        buf_printf(s, " %-*s", s->col_width - 1, header);
        buf_puts(s, C_RESET C_HEADER_BG);
    }
    /* Fill rest of line */
    int used = s->visible_columns * s->col_width;
    for (int i = used; i < s->size.cols; i++) buf_puts(s, " ");
    buf_puts(s, C_RESET "\r\n");

    /* === Border line === */
    buf_puts(s, C_BORDER);
    for (int c = 0; c < s->visible_columns; c++) {
        for (int i = 0; i < s->col_width; i++) buf_puts(s, "─");
    }
    for (int i = used; i < s->size.cols; i++) buf_puts(s, "─");
    buf_puts(s, C_RESET "\r\n");

    /* === Command rows === */
    for (int r = 0; r < vis_rows; r++) {
        for (int c = 0; c < s->visible_columns; c++) {
            lh_command *cmd = get_command(s, c, r);
            int is_selected = (c == s->cur_col && r == s->cur_row);

            if (is_selected) {
                buf_puts(s, C_SEL_BG C_SEL_FG);
            }

            if (cmd) {
                /* Time prefix */
                char timebuf[16];
                format_time(cmd->timestamp, timebuf, sizeof(timebuf));

                /* Exit code coloring */
                const char *exit_color = "";
                char exit_indicator = ' ';
                if (cmd->has_exit_code) {
                    if (cmd->exit_code != 0) {
                        exit_color = C_ERR_FG;
                        exit_indicator = '!';
                    } else {
                        exit_color = C_OK_FG;
                        exit_indicator = ' ';
                    }
                }

                /* Truncate command to fit */
                int time_width = 9; /* "HH:MM:SS " */
                int cmd_max = s->col_width - time_width - 2;
                if (cmd_max < 1) cmd_max = 1;

                char cmd_trunc[256];
                snprintf(cmd_trunc, sizeof(cmd_trunc), "%s", cmd->command);
                sanitize_command(cmd_trunc, sizeof(cmd_trunc));
                int clen = (int)strlen(cmd_trunc);
                if (clen > cmd_max) {
                    /* Leave room for "…" (3 bytes UTF-8) + null */
                    int trunc_at = cmd_max - 1;
                    if (trunc_at < 0) trunc_at = 0;
                    if (trunc_at + 4 < (int)sizeof(cmd_trunc)) {
                        cmd_trunc[trunc_at] = '\0';
                        strcat(cmd_trunc, "\xe2\x80\xa6"); /* … */
                    } else {
                        cmd_trunc[cmd_max] = '\0';
                    }
                    clen = cmd_max;
                }

                if (!is_selected) {
                    buf_puts(s, C_TIME_FG);
                }
                buf_printf(s, "%s ", timebuf);
                if (!is_selected && exit_color[0]) {
                    buf_puts(s, exit_color);
                }
                buf_printf(s, "%c", exit_indicator);
                if (!is_selected) {
                    buf_puts(s, C_RESET);
                }
                buf_printf(s, "%-*s", cmd_max, cmd_trunc);
            } else {
                buf_printf(s, "%-*s", s->col_width, "");
            }

            if (is_selected) {
                buf_puts(s, C_RESET);
            }
        }
        /* Fill rest of line */
        for (int i = used; i < s->size.cols; i++) buf_puts(s, " ");
        buf_puts(s, "\r\n");
    }

    /* === Status bar (bottom 2 lines) === */
    /* Full command preview */
    buf_puts(s, C_STATUS_BG C_STATUS_FG);
    lh_command *sel = get_command(s, s->cur_col, s->cur_row);
    if (sel) {
        char preview[512];
        snprintf(preview, sizeof(preview), " %s", sel->command);
        sanitize_command(preview, sizeof(preview));
        int plen = (int)strlen(preview);
        if (plen > s->size.cols) {
            preview[s->size.cols - 1] = '\0';
            plen = s->size.cols - 1;
        }
        buf_printf(s, "%-*s", s->size.cols, preview);
    } else {
        buf_printf(s, "%-*s", s->size.cols, "");
    }
    buf_puts(s, C_RESET "\r\n");

    /* Help / search line */
    buf_puts(s, C_STATUS_BG);
    if (s->mode == MODE_SEARCH) {
        buf_puts(s, C_SEARCH_FG);
        buf_printf(s, " /%s", s->search_buf);
        /* cursor indicator */
        buf_puts(s, "█");
        int fill = s->size.cols - s->search_len - 3;
        for (int i = 0; i < fill; i++) buf_puts(s, " ");
    } else {
        buf_puts(s, C_STATUS_FG C_DIM);
        char help[] = " ↑↓/jk:navigate  ←→/hl:columns  /:search  Enter:select  q/Esc:quit";
        int hlen = (int)strlen(help);
        if (hlen > s->size.cols) help[s->size.cols] = '\0';
        buf_printf(s, "%-*s", s->size.cols, help);
    }
    buf_puts(s, C_RESET);

    /* Flush render buffer */
    lh_term_write(s->render_buf, s->render_len);
    s->needs_redraw = 0;
}

static void load_data(tui_state *s) {
    lh_session_list sessions = lh_db_recent_sessions(s->db, MAX_SESSIONS);
    s->num_columns = sessions.count;

    /* If current session is specified, swap it to index 0 */
    if (s->current_session_id && sessions.count > 1) {
        for (int i = 1; i < sessions.count; i++) {
            if (strcmp(sessions.items[i].session_id, s->current_session_id) == 0) {
                lh_session tmp = sessions.items[0];
                sessions.items[0] = sessions.items[i];
                sessions.items[i] = tmp;
                break;
            }
        }
    }

    for (int i = 0; i < sessions.count; i++) {
        /* Copy session into its own allocation so cleanup can free each independently */
        lh_session *sp = malloc(sizeof(lh_session));
        *sp = sessions.items[i];
        s->columns[i].session = sp;
        s->columns[i].commands = lh_db_session_commands(
            s->db, sp->session_id, MAX_COMMANDS_PER_SESSION);
        int cmd_count = s->columns[i].commands.count;
        s->columns[i].filtered = cmd_count ? malloc((size_t)cmd_count * sizeof(int)) : NULL;
        s->columns[i].filtered_count = 0;
    }

    /* Free the contiguous sessions array — strings are now owned by the copied structs */
    free(sessions.items);

    apply_search_filter(s);
}

static void cleanup_data(tui_state *s) {
    for (int i = 0; i < s->num_columns; i++) {
        /* Free the session (was allocated by lh_db_recent_sessions) */
        free(s->columns[i].session->session_id);
        free(s->columns[i].session->shell);
        free(s->columns[i].session->ide);
        free(s->columns[i].session->directory);
        free(s->columns[i].session);
        lh_command_list_free(&s->columns[i].commands);
        free(s->columns[i].filtered);
    }
}

int lh_tui_browse(sqlite3 *db, char *result_buf, int result_buf_size,
                  const char *current_session_id) {
    tui_state state = {0};
    state.db = db;
    state.current_session_id = current_session_id;
    g_state = &state;

    /* Enter raw mode */
    int fd = lh_term_raw_enter();
    if (fd < 0) return 0;

    lh_term_on_resize(resize_handler);
    state.size = lh_term_get_size();

    /* Load data */
    load_data(&state);

    if (state.num_columns == 0) {
        lh_term_raw_exit();
        g_state = NULL;
        return 0;
    }

    state.needs_redraw = 1;
    int running = 1;
    int result_len = 0;

    while (running) {
        if (state.needs_redraw) {
            render(&state);
        }

        lh_key key = lh_input_read(fd);

        if (state.mode == MODE_SEARCH) {
            switch (key.type) {
                case LH_KEY_ESCAPE:
                    state.mode = MODE_NORMAL;
                    state.search_buf[0] = '\0';
                    state.search_len = 0;
                    apply_search_filter(&state);
                    state.cur_row = 0;
                    state.scroll_offset = 0;
                    state.needs_redraw = 1;
                    break;
                case LH_KEY_ENTER:
                    state.mode = MODE_NORMAL;
                    state.needs_redraw = 1;
                    break;
                case LH_KEY_BACKSPACE:
                    if (state.search_len > 0) {
                        state.search_buf[--state.search_len] = '\0';
                        apply_search_filter(&state);
                        state.cur_row = 0;
                        state.scroll_offset = 0;
                        state.needs_redraw = 1;
                    }
                    break;
                case LH_KEY_CHAR:
                case LH_KEY_SLASH:
                case LH_KEY_J:
                case LH_KEY_K:
                case LH_KEY_H:
                case LH_KEY_L:
                case LH_KEY_Q:
                case LH_KEY_G:
                case LH_KEY_G_SHIFT: {
                    char c = key.ch;
                    /* Map named keys back to chars for search */
                    if (key.type == LH_KEY_SLASH) c = '/';
                    else if (key.type == LH_KEY_J) c = 'j';
                    else if (key.type == LH_KEY_K) c = 'k';
                    else if (key.type == LH_KEY_H) c = 'h';
                    else if (key.type == LH_KEY_L) c = 'l';
                    else if (key.type == LH_KEY_Q) c = 'q';
                    else if (key.type == LH_KEY_G) c = 'g';
                    else if (key.type == LH_KEY_G_SHIFT) c = 'G';

                    if (state.search_len < SEARCH_BUF_SIZE - 1 && c >= 32) {
                        state.search_buf[state.search_len++] = c;
                        state.search_buf[state.search_len] = '\0';
                        apply_search_filter(&state);
                        state.cur_row = 0;
                        state.scroll_offset = 0;
                        state.needs_redraw = 1;
                    }
                    break;
                }
                default:
                    break;
            }
            continue;
        }

        /* Normal mode */
        int vis_rows = max_visible_rows(&state);
        int abs_col = state.col_offset + state.cur_col;
        int max_row = 0;
        if (abs_col >= 0 && abs_col < state.num_columns) {
            max_row = state.columns[abs_col].filtered_count - 1;
        }

        switch (key.type) {
            case LH_KEY_UP:
            case LH_KEY_K:
                if (state.cur_row > 0) {
                    state.cur_row--;
                } else if (state.scroll_offset > 0) {
                    state.scroll_offset--;
                }
                state.needs_redraw = 1;
                break;

            case LH_KEY_DOWN:
            case LH_KEY_J:
                if (state.scroll_offset + state.cur_row < max_row) {
                    if (state.cur_row < vis_rows - 1) {
                        state.cur_row++;
                    } else {
                        state.scroll_offset++;
                    }
                }
                state.needs_redraw = 1;
                break;

            case LH_KEY_LEFT:
            case LH_KEY_H:
                if (state.cur_col > 0) {
                    state.cur_col--;
                } else if (state.col_offset > 0) {
                    state.col_offset--;
                }
                state.needs_redraw = 1;
                break;

            case LH_KEY_RIGHT:
            case LH_KEY_L:
                if (state.cur_col < state.visible_columns - 1) {
                    state.cur_col++;
                } else if (state.col_offset + state.visible_columns < state.num_columns) {
                    state.col_offset++;
                }
                state.needs_redraw = 1;
                break;

            case LH_KEY_SLASH:
                state.mode = MODE_SEARCH;
                state.search_buf[0] = '\0';
                state.search_len = 0;
                state.needs_redraw = 1;
                break;

            case LH_KEY_ENTER: {
                lh_command *cmd = get_command(&state, state.cur_col, state.cur_row);
                if (cmd) {
                    int clen = (int)strlen(cmd->command);
                    if (clen >= result_buf_size) clen = result_buf_size - 1;
                    memcpy(result_buf, cmd->command, (size_t)clen);
                    result_buf[clen] = '\0';
                    result_len = clen;
                }
                running = 0;
                break;
            }

            case LH_KEY_ESCAPE:
            case LH_KEY_Q:
                running = 0;
                break;

            case LH_KEY_G:
                state.cur_row = 0;
                state.scroll_offset = 0;
                state.needs_redraw = 1;
                break;

            case LH_KEY_G_SHIFT:
                if (max_row >= vis_rows) {
                    state.scroll_offset = max_row - vis_rows + 1;
                    state.cur_row = vis_rows - 1;
                } else {
                    state.cur_row = max_row;
                }
                state.needs_redraw = 1;
                break;

            default:
                break;
        }
    }

    lh_term_raw_exit();
    cleanup_data(&state);
    g_state = NULL;
    return result_len;
}
