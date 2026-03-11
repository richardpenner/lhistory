#include "tui.h"
#include "term.h"
#include "input.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MIN_TUI_ROWS 10
#define MAX_SESSIONS 16
#define MAX_COMMANDS_PER_SESSION 500
#define SEARCH_BUF_SIZE 256
#define RENDER_BUF_SIZE (64 * 1024)

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
#define C_HOME_FG   "\x1b[38;5;114m"
#define C_TAB_BG    "\x1b[48;5;238m"
#define C_TAB_FG    "\x1b[38;5;252m"

typedef enum {
    MODE_NORMAL,
    MODE_SEARCH,
} tui_mode;

typedef struct {
    lh_session *session;
    lh_command_list commands;
    int *filtered;
    int filtered_count;
} tui_column;

typedef struct {
    lh_command *cmd;
    int col_idx;
} merged_row;

typedef struct {
    sqlite3 *db;
    tui_column columns[MAX_SESSIONS];
    int num_columns;
    int active_session;
    int cur_row;
    int scroll_offset;
    lh_term_size size;
    tui_mode mode;
    char search_buf[SEARCH_BUF_SIZE];
    int search_len;
    int needs_redraw;
    char render_buf[RENDER_BUF_SIZE];
    int render_len;
    const char *current_session_id;
    merged_row *merged;
    int merged_count;
    int origin_row;
    int tui_height;
    int col_width;
    int left_margin;
    int cursor_col;
} tui_state;

static tui_state *g_state = NULL;

static void resize_handler(void) {
    if (g_state) {
        g_state->size = lh_term_get_size();
        if (g_state->tui_height > g_state->size.rows)
            g_state->tui_height = g_state->size.rows;
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

static void sanitize_command(char *buf, int bufsize) {
    int j = 0;
    for (int i = 0; buf[i] && j < bufsize - 1; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x1b) {
            if (buf[i + 1] == '[') {
                i++;
                while (buf[i + 1] && !((unsigned char)buf[i + 1] >= 0x40 && (unsigned char)buf[i + 1] <= 0x7e))
                    i++;
                if (buf[i + 1]) i++;
            }
            continue;
        }
        if (c < 32 && c != '\t') continue;
        buf[j++] = (char)c;
    }
    buf[j] = '\0';
}

static void format_time(int64_t ts_ms, char *out, int maxlen) {
    time_t ts = (time_t)(ts_ms / 1000);
    struct tm *tm = localtime(&ts);
    if (!tm) { snprintf(out, (size_t)maxlen, "--:--:--"); return; }
    snprintf(out, (size_t)maxlen, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static int merged_cmp(const void *a, const void *b) {
    const merged_row *ma = a, *mb = b;
    if (ma->cmd->timestamp != mb->cmd->timestamp)
        return (ma->cmd->timestamp > mb->cmd->timestamp) ? -1 : 1;
    return (ma->cmd->id > mb->cmd->id) ? -1 : 1;
}

static void build_merged_list(tui_state *s) {
    free(s->merged);
    s->merged = NULL;
    s->merged_count = 0;

    int total = 0;
    for (int c = 0; c < s->num_columns; c++)
        total += s->columns[c].filtered_count;

    if (total == 0) return;

    s->merged = malloc((size_t)total * sizeof(merged_row));
    if (!s->merged) return;

    int idx = 0;
    for (int c = 0; c < s->num_columns; c++) {
        tui_column *col = &s->columns[c];
        for (int i = 0; i < col->filtered_count; i++) {
            s->merged[idx].cmd = &col->commands.items[col->filtered[i]];
            s->merged[idx].col_idx = c;
            idx++;
        }
    }
    s->merged_count = idx;
    qsort(s->merged, (size_t)s->merged_count, sizeof(merged_row), merged_cmp);
}

static void apply_search_filter(tui_state *s) {
    for (int c = 0; c < s->num_columns; c++) {
        tui_column *col = &s->columns[c];
        col->filtered_count = 0;

        if (s->search_len == 0) {
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
    build_merged_list(s);
}

static int max_visible_rows(tui_state *s) {
    int rows = s->tui_height - 1;
    return rows > 0 ? rows : 1;
}

static int current_item_count(tui_state *s) {
    if (s->search_len > 0) return s->merged_count;
    if (s->num_columns == 0) return 0;
    return s->columns[s->active_session].filtered_count;
}

static lh_command *current_item(tui_state *s, int idx) {
    if (s->search_len > 0) {
        if (idx < 0 || idx >= s->merged_count) return NULL;
        return s->merged[idx].cmd;
    }
    tui_column *col = &s->columns[s->active_session];
    if (idx < 0 || idx >= col->filtered_count) return NULL;
    return &col->commands.items[col->filtered[idx]];
}

static void render_command_cell(tui_state *s, lh_command *cmd, int is_selected) {
    char timebuf[16];
    format_time(cmd->timestamp, timebuf, sizeof(timebuf));

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

    int cmd_max = s->col_width;

    char cmd_trunc[512];
    snprintf(cmd_trunc, sizeof(cmd_trunc), "%s", cmd->command);
    sanitize_command(cmd_trunc, sizeof(cmd_trunc));
    int clen = (int)strlen(cmd_trunc);
    if (clen > cmd_max) {
        int trunc_at = cmd_max - 1;
        if (trunc_at < 0) trunc_at = 0;
        if (trunc_at + 4 < (int)sizeof(cmd_trunc)) {
            cmd_trunc[trunc_at] = '\0';
            strcat(cmd_trunc, "\xe2\x80\xa6");
        } else {
            cmd_trunc[cmd_max] = '\0';
        }
    }

    buf_printf(s, "%*s", s->left_margin, "");

    buf_puts(s, C_TIME_FG);
    buf_printf(s, "%s ", timebuf);
    if (exit_color[0]) buf_puts(s, exit_color);
    buf_printf(s, "%c", exit_indicator);
    buf_puts(s, C_RESET);

    if (is_selected) {
        buf_puts(s, C_SEL_BG C_SEL_FG);
        buf_printf(s, "%-*s", cmd_max, cmd_trunc);
        buf_puts(s, C_RESET);
    } else {
        buf_printf(s, "%-*s", cmd_max, cmd_trunc);
    }
}

static void render_status_line(tui_state *s) {
    int total_w = 10 + s->col_width;
    buf_printf(s, "%*s", s->left_margin, "");

    buf_puts(s, C_STATUS_BG);
    if (s->mode == MODE_SEARCH) {
        buf_puts(s, C_SEARCH_FG);
        buf_printf(s, " /%s", s->search_buf);
        buf_puts(s, "\xe2\x96\x88");
        char match_info[32];
        snprintf(match_info, sizeof(match_info), " (%d)", s->merged_count);
        buf_puts(s, C_STATUS_FG);
        buf_puts(s, match_info);
        int fill = total_w - s->search_len - 3 - (int)strlen(match_info);
        for (int i = 0; i < fill; i++) buf_puts(s, " ");
    } else {
        int has_left = (s->num_columns > 1 && s->active_session > 0);
        int has_right = (s->num_columns > 1 && s->active_session < s->num_columns - 1);

        tui_column *tc = &s->columns[s->active_session];
        const char *d = tc->session->directory;
        const char *slash = strrchr(d, '/');
        const char *tail = (slash && slash[1]) ? slash + 1 : d;

        char left_arrow[8] = " ";
        char right_arrow[8] = " ";
        if (has_left) snprintf(left_arrow, sizeof(left_arrow), "\xe2\x97\x82");
        if (has_right) snprintf(right_arrow, sizeof(right_arrow), "\xe2\x96\xb8");

        if (has_left || has_right)
            buf_puts(s, C_DIM);
        buf_printf(s, " %s", left_arrow);
        buf_puts(s, C_RESET C_STATUS_BG);
        buf_puts(s, C_STATUS_FG C_BOLD);
        buf_printf(s, " %s ", tail);
        buf_puts(s, C_RESET C_STATUS_BG);
        if (has_left || has_right)
            buf_puts(s, C_DIM);
        buf_printf(s, "%s", right_arrow);
        buf_puts(s, C_RESET C_STATUS_BG);

        int label_len = (int)strlen(tail);
        int arrow_w = 4 + label_len + 2 + 1;
        int fill = total_w - arrow_w;
        for (int i = 0; i < fill; i++) buf_puts(s, " ");
    }
    buf_puts(s, C_RESET);
}

static void render(tui_state *s) {
    s->render_len = 0;
    buf_printf(s, "\x1b[%d;1H", s->origin_row);

    int item_count = current_item_count(s);

    if (item_count > 0) {
        if (s->scroll_offset + s->cur_row >= item_count) {
            s->cur_row = 0;
            s->scroll_offset = 0;
        }
    } else {
        s->cur_row = 0;
        s->scroll_offset = 0;
    }

    int vis_rows = max_visible_rows(s);

    #define MAX_CMD_WIDTH 60
    int longest = 0;
    for (int i = 0; i < item_count; i++) {
        lh_command *cmd = current_item(s, i);
        if (cmd) {
            int len = (int)strlen(cmd->command);
            if (len > longest) longest = len;
        }
    }
    s->col_width = longest < MAX_CMD_WIDTH ? longest : MAX_CMD_WIDTH;
    if (s->col_width < 20) s->col_width = 20;
    int prefix_w = 10;
    s->left_margin = s->cursor_col - prefix_w - 1;
    if (s->left_margin < 0) s->left_margin = 0;
    int total_w = prefix_w + s->col_width;
    int avail = s->size.cols - s->left_margin;
    if (total_w > avail && avail > prefix_w)
        s->col_width = avail - prefix_w;

    render_status_line(s);
    buf_puts(s, "\r\n");

    for (int r = 0; r < vis_rows; r++) {
        int data_row = vis_rows - 1 - r;
        int item_idx = s->scroll_offset + data_row;
        lh_command *cmd = current_item(s, item_idx);
        int is_selected = (data_row == s->cur_row && cmd != NULL);

        if (cmd) {
            render_command_cell(s, cmd, is_selected);
        } else {
            buf_printf(s, "%*s", s->left_margin + total_w, "");
        }
        if (r < vis_rows - 1) buf_puts(s, "\r\n");
    }

    lh_term_write(s->render_buf, s->render_len);
    s->needs_redraw = 0;
}

static void load_data(tui_state *s) {
    lh_session_list sessions = lh_db_recent_sessions(s->db, MAX_SESSIONS);
    s->num_columns = sessions.count;

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
        lh_session *sp = malloc(sizeof(lh_session));
        if (!sp) { s->num_columns = i; break; }
        *sp = sessions.items[i];
        s->columns[i].session = sp;
        s->columns[i].commands = lh_db_session_commands(
            s->db, sp->session_id, MAX_COMMANDS_PER_SESSION);
        int cmd_count = s->columns[i].commands.count;
        s->columns[i].filtered = cmd_count ? malloc((size_t)cmd_count * sizeof(int)) : NULL;
        s->columns[i].filtered_count = 0;
    }

    free(sessions.items);
    apply_search_filter(s);
}

static void cleanup_data(tui_state *s) {
    for (int i = 0; i < s->num_columns; i++) {
        free(s->columns[i].session->session_id);
        free(s->columns[i].session->shell);
        free(s->columns[i].session->ide);
        free(s->columns[i].session->directory);
        free(s->columns[i].session);
        lh_command_list_free(&s->columns[i].commands);
        free(s->columns[i].filtered);
    }
    free(s->merged);
}

int lh_tui_browse(sqlite3 *db, char *result_buf, int result_buf_size,
                  const char *current_session_id) {
    tui_state state = {0};
    state.db = db;
    state.current_session_id = current_session_id;
    g_state = &state;

    int cursor_row = 0, cursor_col = 0;
    {
        int tty_fd = open("/dev/tty", O_RDWR);
        if (tty_fd < 0) return 0;
        struct termios old, tmp;
        tcgetattr(tty_fd, &old);
        tmp = old;
        tmp.c_lflag &= ~(unsigned long)(ECHO | ICANON);
        tmp.c_cc[VMIN] = 1;
        tmp.c_cc[VTIME] = 0;
        tcsetattr(tty_fd, TCSAFLUSH, &tmp);

        (void)write(tty_fd, "\x1b[6n", 4);
        char buf[32];
        int pos = 0;
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(tty_fd, &fds);
            struct timeval tv = {0, 100000};
            if (select(tty_fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
            if (read(tty_fd, &buf[pos], 1) != 1) break;
            if (buf[pos] == 'R') { buf[pos + 1] = '\0'; break; }
            if (++pos >= (int)sizeof(buf) - 1) break;
        }
        for (int i = 0; i <= pos; i++) {
            if (buf[i] == '[') cursor_row = atoi(&buf[i + 1]);
            if (buf[i] == ';') cursor_col = atoi(&buf[i + 1]);
        }

        tcsetattr(tty_fd, TCSAFLUSH, &old);
        close(tty_fd);
    }

    int fd = lh_term_raw_enter();
    if (fd < 0) return 0;

    state.size = lh_term_get_size();
    if (cursor_row < 1) cursor_row = state.size.rows;
    const char *col_env = getenv("LHISTORY_COL");
    if (col_env && atoi(col_env) > 0) cursor_col = atoi(col_env);
    if (cursor_col < 1) cursor_col = 1;
    state.cursor_col = cursor_col;

    lh_term_write("\x1b[?1049h\x1b[?25l", 14);

    state.tui_height = cursor_row;
    if (state.tui_height < 2) state.tui_height = 2;
    state.origin_row = cursor_row - state.tui_height + 1;
    if (state.origin_row < 1) state.origin_row = 1;

    lh_term_on_resize(resize_handler);

    load_data(&state);

    if (state.num_columns == 0) {
        lh_term_write("\x1b[?25h\x1b[?1049l", 14);
        lh_term_raw_exit();
        g_state = NULL;
        return 0;
    }

    state.active_session = 0;
    state.needs_redraw = 1;
    int running = 1;
    int result_len = 0;

    while (running) {
        if (state.needs_redraw) {
            render(&state);
        }

        lh_key key = lh_input_read(fd);
        state.needs_redraw = 1;

        if (state.mode == MODE_SEARCH) {
            switch (key.type) {
                case LH_KEY_ESCAPE:
                    state.mode = MODE_NORMAL;
                    state.search_buf[0] = '\0';
                    state.search_len = 0;
                    apply_search_filter(&state);
                    state.cur_row = 0;
                    state.scroll_offset = 0;
                    break;
                case LH_KEY_ENTER:
                    state.mode = MODE_NORMAL;
                    break;
                case LH_KEY_BACKSPACE:
                    if (state.search_len > 0) {
                        state.search_buf[--state.search_len] = '\0';
                        apply_search_filter(&state);
                        state.cur_row = 0;
                        state.scroll_offset = 0;
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
        int count = current_item_count(&state);

        switch (key.type) {
            case LH_KEY_UP:
            case LH_KEY_K: {
                if (count == 0) break;
                int idx = state.scroll_offset + state.cur_row;
                if (idx + 1 < count) {
                    if (state.cur_row + 1 < vis_rows) {
                        state.cur_row++;
                    } else {
                        state.scroll_offset++;
                    }
                }
                break;
            }

            case LH_KEY_DOWN:
            case LH_KEY_J: {
                if (count == 0) break;
                int idx = state.scroll_offset + state.cur_row;
                if (idx > 0) {
                    if (state.cur_row > 0) {
                        state.cur_row--;
                    } else {
                        state.scroll_offset--;
                    }
                }
                break;
            }

            case LH_KEY_LEFT:
            case LH_KEY_H: {
                if (state.search_len > 0 || state.num_columns <= 1) break;
                state.active_session = (state.active_session - 1 + state.num_columns) % state.num_columns;
                state.cur_row = 0;
                state.scroll_offset = 0;
                break;
            }

            case LH_KEY_RIGHT:
            case LH_KEY_L: {
                if (state.search_len > 0 || state.num_columns <= 1) break;
                state.active_session = (state.active_session + 1) % state.num_columns;
                state.cur_row = 0;
                state.scroll_offset = 0;
                break;
            }

            case LH_KEY_SLASH:
                state.mode = MODE_SEARCH;
                state.search_buf[0] = '\0';
                state.search_len = 0;
                break;

            case LH_KEY_ENTER: {
                int idx = state.scroll_offset + state.cur_row;
                lh_command *cmd = current_item(&state, idx);
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
                if (count > vis_rows) {
                    state.scroll_offset = count - vis_rows;
                    state.cur_row = vis_rows - 1;
                } else if (count > 0) {
                    state.cur_row = count - 1;
                }
                break;

            case LH_KEY_G_SHIFT:
                state.cur_row = 0;
                state.scroll_offset = 0;
                break;

            default:
                break;
        }
    }

    lh_term_write("\x1b[?25h\x1b[?1049l", 14);

    lh_term_raw_exit();
    cleanup_data(&state);
    g_state = NULL;
    return result_len;
}
