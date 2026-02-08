#ifndef LHISTORY_TUI_H
#define LHISTORY_TUI_H

#include "db.h"

/* Launch the TUI browser. If the user selects a command, it is written to
 * result_buf (up to result_buf_size bytes). Returns the length of the
 * selected command, or 0 if the user cancelled.
 * current_session_id, if non-NULL, pins that session to the leftmost column. */
int lh_tui_browse(sqlite3 *db, char *result_buf, int result_buf_size,
                  const char *current_session_id);

#endif
