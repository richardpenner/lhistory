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
#define MIN_COL_WIDTH 28
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
    int visible_columns;
    int col_width;
    int col_offset;
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
    lh_db_stats stats;
    int stats_ready;
    int origin_row;
    int tui_height;
    int left_pad;
    int sel_highlight_w;
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

static void shorten_dir(const char *dir, char *out, int maxlen) {
    const char *home = getenv("HOME");
    const char *p = dir;

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

    const char *last_slash = strrchr(p, '/');
    if (last_slash) {
        int tail_len = (int)strlen(last_slash);
        int budget = maxlen - tail_len - 1;
        if (budget > 0 && budget < len) {
            char abbrev[512] = {0};
            int ai = 0;
            for (int i = 0; p[i] && &p[i] < last_slash && ai < budget; i++) {
                if (i == 0 || p[i - 1] == '/') {
                    abbrev[ai++] = p[i];
                    if (p[i] != '/' && p[i + 1] != '/') {
                        while (p[i + 1] && p[i + 1] != '/') i++;
                    }
                } else {
                    abbrev[ai++] = p[i];
                }
            }
            snprintf(out, (size_t)maxlen, "%s%s", abbrev, last_slash);
        } else {
            snprintf(out, (size_t)maxlen, "\xe2\x80\xa6%s", last_slash);
        }
    } else {
        snprintf(out, (size_t)maxlen, "%s", p + len - maxlen + 1);
    }
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
    int rows = s->tui_height - 5;
    return rows > 0 ? rows : 1;
}

static void format_count(int n, char *buf, int bufsize) {
    if (n >= 100000) snprintf(buf, bufsize, "%dk", n / 1000);
    else if (n >= 1000) snprintf(buf, bufsize, "%.1fk", n / 1000.0);
    else snprintf(buf, bufsize, "%d", n);
}

static void format_hour(int h, char *buf, int bufsize) {
    if (h == 0) snprintf(buf, bufsize, "12am");
    else if (h < 12) snprintf(buf, bufsize, "%dam", h);
    else if (h == 12) snprintf(buf, bufsize, "12pm");
    else snprintf(buf, bufsize, "%dpm", h - 12);
}

static void render_command_cell(tui_state *s, lh_command *cmd, int is_selected, int avail_width) {
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

    int time_width = 9;
    int col_cmd = s->col_width - time_width - 1 - s->left_pad;
    if (col_cmd < 1) col_cmd = 1;
    int cmd_max = avail_width - time_width - 1 - s->left_pad;
    if (cmd_max < col_cmd) cmd_max = col_cmd;

    char cmd_trunc[256];
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
        clen = cmd_max;
    }

    buf_puts(s, C_TIME_FG);
    buf_printf(s, "%s ", timebuf);
    if (exit_color[0]) buf_puts(s, exit_color);
    buf_printf(s, "%c", exit_indicator);
    buf_puts(s, C_RESET);

    for (int i = 0; i < s->left_pad; i++) buf_puts(s, " ");
    if (is_selected) {
        int hw = s->sel_highlight_w;
        if (hw > cmd_max) hw = cmd_max;
        buf_puts(s, C_SEL_BG C_SEL_FG);
        buf_printf(s, "%-*s", hw, cmd_trunc);
        buf_puts(s, C_RESET);
        int remain = cmd_max - hw;
        if (remain > 0) {
            int printed = (int)strlen(cmd_trunc);
            if (printed < hw) printed = hw;
            if (printed < cmd_max)
                buf_printf(s, "%*s", cmd_max - printed, "");
        }
    } else {
        buf_printf(s, "%-*s", cmd_max, cmd_trunc);
    }
}

static void render(tui_state *s) {
    s->render_len = 0;
    buf_printf(s, "\x1b[%d;1H", s->origin_row);

    int divisor = s->size.cols / MIN_COL_WIDTH;
    if (divisor < 1) divisor = 1;
    s->col_width = s->size.cols / divisor;
    if (s->col_width < MIN_COL_WIDTH) s->col_width = MIN_COL_WIDTH;
    s->visible_columns = s->size.cols / s->col_width;
    if (s->visible_columns > s->num_columns) s->visible_columns = s->num_columns;
    if (s->visible_columns < 1) s->visible_columns = 1;

    int active_abs_col = -1;
    if (s->merged_count > 0) {
        int idx = s->scroll_offset + s->cur_row;
        if (idx >= 0 && idx < s->merged_count)
            active_abs_col = s->merged[idx].col_idx;
    }

    if (active_abs_col >= 0) {
        if (active_abs_col < s->col_offset)
            s->col_offset = active_abs_col;
        else if (active_abs_col >= s->col_offset + s->visible_columns)
            s->col_offset = active_abs_col - s->visible_columns + 1;
    }
    if (s->col_offset + s->visible_columns > s->num_columns)
        s->col_offset = s->num_columns - s->visible_columns;
    if (s->col_offset < 0) s->col_offset = 0;

    if (s->merged_count > 0) {
        if (s->scroll_offset + s->cur_row >= s->merged_count) {
            s->cur_row = 0;
            s->scroll_offset = 0;
        }
    } else {
        s->cur_row = 0;
        s->scroll_offset = 0;
    }

    int vis_rows = max_visible_rows(s);
    int used = s->visible_columns * s->col_width;

    /* === Title bar === */
    {
        char title[64];
        snprintf(title, sizeof(title), " lhistory %s ", LHISTORY_VERSION);
        int tlen = (int)strlen(title);

        if (s->stats_ready) {
            char segments[8][80];
            int seg_lens[8];
            int nseg = 0;

            char buf[16];
            format_count(s->stats.commands_today, buf, sizeof(buf));
            snprintf(segments[nseg], 80, "today: %s", buf); seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;
            format_count(s->stats.commands_7d, buf, sizeof(buf));
            snprintf(segments[nseg], 80, "7d: %s", buf); seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;
            format_count(s->stats.commands_30d, buf, sizeof(buf));
            snprintf(segments[nseg], 80, "30d: %s", buf); seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;
            format_count(s->stats.commands_all, buf, sizeof(buf));
            snprintf(segments[nseg], 80, "all: %s", buf); seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;

            if (s->stats.top_count > 0) {
                char top[80] = "top: ";
                for (int i = 0; i < s->stats.top_count; i++) {
                    if (i > 0) strcat(top, ", ");
                    strcat(top, s->stats.top_commands[i]);
                }
                snprintf(segments[nseg], 80, "%s", top); seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;
            }

            if (s->stats.busiest_hour >= 0) {
                char hbuf[8];
                format_hour(s->stats.busiest_hour, hbuf, sizeof(hbuf));
                snprintf(segments[nseg], 80, "peak: %s", hbuf); seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;
            }

            if (s->stats.top_typo[0]) {
                snprintf(segments[nseg], 80, "typo: \"%s\" (%dx)", s->stats.top_typo, s->stats.typo_count);
                seg_lens[nseg] = (int)strlen(segments[nseg]); nseg++;
            }

            int fit = 0;
            int stats_width = 0;
            for (int i = 0; i < nseg; i++) {
                int sep = (i == 0) ? 0 : 3;
                int need = seg_lens[i] + sep;
                if (tlen + 2 + stats_width + need > s->size.cols) break;
                stats_width += need;
                fit++;
            }

            if (fit > 0) {
                int gap = s->size.cols - tlen - stats_width - 1;
                buf_puts(s, "\x1b[48;5;24m\x1b[38;5;255m" C_BOLD);
                buf_puts(s, title);
                for (int i = 0; i < gap; i++) buf_puts(s, " ");
                buf_puts(s, "\x1b[22m\x1b[38;5;250m");
                for (int i = 0; i < fit; i++) {
                    if (i > 0) buf_printf(s, " %s\xe2\x94\x82%s ", C_DIM, "\x1b[22m\x1b[38;5;250m");
                    buf_puts(s, segments[i]);
                }
                buf_puts(s, " ");
            } else {
                int pad_l = (s->size.cols - tlen) / 2;
                int pad_r = s->size.cols - tlen - pad_l;
                buf_puts(s, "\x1b[48;5;24m\x1b[38;5;255m" C_BOLD);
                for (int i = 0; i < pad_l; i++) buf_puts(s, " ");
                buf_puts(s, title);
                for (int i = 0; i < pad_r; i++) buf_puts(s, " ");
            }
        } else {
            int pad_l = (s->size.cols - tlen) / 2;
            int pad_r = s->size.cols - tlen - pad_l;
            buf_puts(s, "\x1b[48;5;24m\x1b[38;5;255m" C_BOLD);
            for (int i = 0; i < pad_l; i++) buf_puts(s, " ");
            buf_puts(s, title);
            for (int i = 0; i < pad_r; i++) buf_puts(s, " ");
        }
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
            if (ide_upper[0] >= 'a' && ide_upper[0] <= 'z') ide_upper[0] -= 32;
            snprintf(header, sizeof(header), "[%s] %s", ide_upper, dir_short);
        } else {
            snprintf(header, sizeof(header), "%s", dir_short);
        }

        int hlen = (int)strlen(header);
        if (hlen > s->col_width - 1) {
            header[s->col_width - 2] = '\0';
        }

        if (abs_c == active_abs_col) {
            buf_puts(s, C_ACTIVE C_BOLD);
        } else {
            buf_puts(s, C_HEADER_FG);
        }

        buf_printf(s, " %-*s", s->col_width - 1, header);
        buf_puts(s, C_RESET C_HEADER_BG);
    }
    for (int i = used; i < s->size.cols; i++) buf_puts(s, " ");
    buf_puts(s, C_RESET "\r\n");

    /* === Border line === */
    buf_puts(s, C_BORDER);
    for (int c = 0; c < s->visible_columns; c++) {
        for (int i = 0; i < s->col_width; i++) buf_puts(s, "\xe2\x94\x80");
    }
    for (int i = used; i < s->size.cols; i++) buf_puts(s, "\xe2\x94\x80");
    buf_puts(s, C_RESET "\r\n");

    /* === Compute highlight width from longest command in active column === */
    s->sel_highlight_w = 0;
    if (active_abs_col >= 0) {
        for (int i = 0; i < s->merged_count; i++) {
            if (s->merged[i].col_idx == active_abs_col) {
                int len = (int)strlen(s->merged[i].cmd->command);
                if (len > s->sel_highlight_w) s->sel_highlight_w = len;
            }
        }
    }

    /* === Command rows (merged timeline: bottom = newest) === */
    for (int r = 0; r < vis_rows; r++) {
        int data_row = vis_rows - 1 - r;
        int merged_idx = s->scroll_offset + data_row;
        merged_row *entry = (merged_idx < s->merged_count) ? &s->merged[merged_idx] : NULL;
        int is_cursor_row = (data_row == s->cur_row);

        int filled = 0;
        for (int c = 0; c < s->visible_columns; c++) {
            int abs_c = s->col_offset + c;
            int has_cmd = (entry && entry->col_idx == abs_c);
            int is_selected = (is_cursor_row && has_cmd);

            if (has_cmd) {
                int avail = s->size.cols - c * s->col_width;
                render_command_cell(s, entry->cmd, is_selected, avail);
                filled = s->size.cols;
                break;
            } else {
                buf_printf(s, "%-*s", s->col_width, "");
                filled += s->col_width;
            }
        }
        for (int i = filled; i < s->size.cols; i++) buf_puts(s, " ");
        buf_puts(s, "\r\n");
    }

    /* === Status bar === */
    buf_puts(s, C_STATUS_BG C_STATUS_FG);
    {
        lh_command *sel = NULL;
        int sel_idx = s->scroll_offset + s->cur_row;
        if (sel_idx >= 0 && sel_idx < s->merged_count)
            sel = s->merged[sel_idx].cmd;
        if (sel) {
            char preview[512];
            snprintf(preview, sizeof(preview), " %s", sel->command);
            sanitize_command(preview, sizeof(preview));
            int plen = (int)strlen(preview);
            if (plen > s->size.cols) {
                preview[s->size.cols - 1] = '\0';
            }
            buf_printf(s, "%-*s", s->size.cols, preview);
        } else {
            buf_printf(s, "%-*s", s->size.cols, "");
        }
    }
    buf_puts(s, C_RESET "\r\n");

    buf_puts(s, C_STATUS_BG);
    if (s->mode == MODE_SEARCH) {
        buf_puts(s, C_SEARCH_FG);
        buf_printf(s, " /%s", s->search_buf);
        buf_puts(s, "\xe2\x96\x88");
        int fill = s->size.cols - s->search_len - 3;
        for (int i = 0; i < fill; i++) buf_puts(s, " ");
    } else {
        buf_puts(s, C_STATUS_FG C_DIM);
        char help[] = " \xe2\x86\x91\xe2\x86\x93/jk:navigate  \xe2\x86\x90\xe2\x86\x92/hl:sessions  /:search  Enter:select  q/Esc:quit";
        int hlen = (int)strlen(help);
        if (hlen > s->size.cols) help[s->size.cols] = '\0';
        buf_printf(s, "%-*s", s->size.cols, help);
    }
    buf_puts(s, C_RESET);

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

    int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) return 0;

    int cursor_row = 0, cursor_col = 0;
    {
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

    lh_term_on_resize(resize_handler);
    state.size = lh_term_get_size();

    if (cursor_row < 1) cursor_row = state.size.rows;
    int cmd_text_offset = 10;
    state.left_pad = (cursor_col > cmd_text_offset + 1) ? cursor_col - cmd_text_offset - 1 : 0;
    state.origin_row = 1;
    state.tui_height = cursor_row + 2;
    if (state.tui_height < MIN_TUI_ROWS)
        state.tui_height = MIN_TUI_ROWS;
    if (state.tui_height > state.size.rows)
        state.tui_height = state.size.rows;

    load_data(&state);

    if (state.num_columns == 0) {
        lh_term_raw_exit();
        g_state = NULL;
        return 0;
    }

    lh_db_compute_stats(state.db, &state.stats);
    state.stats_ready = 1;

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

        switch (key.type) {
            case LH_KEY_UP:
            case LH_KEY_K: {
                if (state.merged_count == 0) break;
                int idx = state.scroll_offset + state.cur_row;
                if (idx >= state.merged_count) break;
                int cur_c = state.merged[idx].col_idx;
                int next = -1;
                for (int i = idx + 1; i < state.merged_count; i++) {
                    if (state.merged[i].col_idx == cur_c) { next = i; break; }
                }
                if (next < 0) break;
                if (next >= state.scroll_offset + vis_rows) {
                    state.scroll_offset = next - vis_rows + 1;
                    state.cur_row = vis_rows - 1;
                } else {
                    state.cur_row = next - state.scroll_offset;
                }
                break;
            }

            case LH_KEY_DOWN:
            case LH_KEY_J: {
                if (state.merged_count == 0) break;
                int idx = state.scroll_offset + state.cur_row;
                if (idx >= state.merged_count) break;
                int cur_c = state.merged[idx].col_idx;
                int next = -1;
                for (int i = idx - 1; i >= 0; i--) {
                    if (state.merged[i].col_idx == cur_c) { next = i; break; }
                }
                if (next < 0) break;
                if (next < state.scroll_offset) {
                    state.scroll_offset = next;
                    state.cur_row = 0;
                } else {
                    state.cur_row = next - state.scroll_offset;
                }
                break;
            }

            case LH_KEY_LEFT:
            case LH_KEY_H: {
                if (state.merged_count == 0) break;
                int idx = state.scroll_offset + state.cur_row;
                if (idx >= state.merged_count) break;
                int cur_c = state.merged[idx].col_idx;
                if (cur_c <= 0) break;
                int target_col = cur_c - 1;

                int best = -1;
                for (int d = 1; d <= state.merged_count; d++) {
                    if (idx - d >= 0 && state.merged[idx - d].col_idx == target_col) { best = idx - d; break; }
                    if (idx + d < state.merged_count && state.merged[idx + d].col_idx == target_col) { best = idx + d; break; }
                }
                if (best < 0) break;

                if (best < state.scroll_offset) {
                    state.scroll_offset = best;
                    state.cur_row = 0;
                } else if (best >= state.scroll_offset + vis_rows) {
                    state.cur_row = vis_rows - 1;
                    state.scroll_offset = best - vis_rows + 1;
                } else {
                    state.cur_row = best - state.scroll_offset;
                }
                break;
            }

            case LH_KEY_RIGHT:
            case LH_KEY_L: {
                if (state.merged_count == 0) break;
                int idx = state.scroll_offset + state.cur_row;
                if (idx >= state.merged_count) break;
                int cur_c = state.merged[idx].col_idx;
                if (cur_c >= state.num_columns - 1) break;
                int target_col = cur_c + 1;

                int best = -1;
                for (int d = 1; d <= state.merged_count; d++) {
                    if (idx - d >= 0 && state.merged[idx - d].col_idx == target_col) { best = idx - d; break; }
                    if (idx + d < state.merged_count && state.merged[idx + d].col_idx == target_col) { best = idx + d; break; }
                }
                if (best < 0) break;

                if (best < state.scroll_offset) {
                    state.scroll_offset = best;
                    state.cur_row = 0;
                } else if (best >= state.scroll_offset + vis_rows) {
                    state.cur_row = vis_rows - 1;
                    state.scroll_offset = best - vis_rows + 1;
                } else {
                    state.cur_row = best - state.scroll_offset;
                }
                break;
            }

            case LH_KEY_SLASH:
                state.mode = MODE_SEARCH;
                state.search_buf[0] = '\0';
                state.search_len = 0;
                break;

            case LH_KEY_ENTER: {
                int idx = state.scroll_offset + state.cur_row;
                if (idx >= 0 && idx < state.merged_count) {
                    lh_command *cmd = state.merged[idx].cmd;
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
                if (state.merged_count > vis_rows) {
                    state.scroll_offset = state.merged_count - vis_rows;
                    state.cur_row = vis_rows - 1;
                } else if (state.merged_count > 0) {
                    state.cur_row = state.merged_count - 1;
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

    lh_term_raw_exit();
    cleanup_data(&state);
    g_state = NULL;
    return result_len;
}
