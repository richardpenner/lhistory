#ifndef LHISTORY_IDE_H
#define LHISTORY_IDE_H

/* Detect the current IDE/terminal environment.
 * Returns a static string like "cursor", "vscode", "zed", "wezterm", "iterm",
 * or NULL if no IDE/special terminal detected. */
const char *lh_detect_ide(void);

#endif
