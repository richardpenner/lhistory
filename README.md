# lhistory

Cross-shell command history browser. See commands from all your terminal tabs, IDE terminals, and shell instances side by side in a multi-column TUI.

```
                          lhistory
 ──────────────────────────────────────────────────────────
  [Cursor] ~/projects/foo        ~/dotfiles
 ──────────────────────────────────────────────────────────
  14:23:01  make test             14:23:05  git pull
  14:22:58  git diff              14:22:30  vim ~/.zshrc
  14:22:45 !cargo build           14:21:12  brew update
  14:22:30  cd src                14:20:01  ls
 ──────────────────────────────────────────────────────────
  cargo build
  ↑↓/jk:navigate  ←→/hl:columns  /:search  Enter:select
```

## Features

- **Multi-column view** — each shell session gets its own column, time-aligned
- **Cross-shell** — works with zsh, bash, and fish
- **IDE-aware** — detects Cursor, VS Code, Zed, and labels columns accordingly
- **Non-blocking** — commands recorded asynchronously via background subshell
- **Fast** — single 1.1MB binary with bundled SQLite, sub-millisecond recording
- **Replaces up-arrow** — press ↑ to browse history across all sessions

## Install

### Homebrew

```
brew install richardpenner/tap/lhistory
```

### From source

```
git clone https://github.com/richardpenner/lhistory.git
cd lhistory
make
sudo make install
```

Then add to your shell:

```
lhistory install
```

Or manually add to your rc file:

```sh
# ~/.zshrc, ~/.bashrc, or ~/.config/fish/config.fish
eval "$(lhistory init zsh)"    # or bash, or fish
```

## Usage

After installation, lhistory hooks into your shell automatically:

- **Up arrow** or **Ctrl+R** — open the history browser
- **↑↓ / j k** — navigate commands
- **←→ / h l** — switch between session columns
- **/** — search across all sessions
- **Enter** — select command and insert it at your prompt
- **q / Esc** — quit without selecting
- **g / G** — jump to top / bottom

Commands that exited non-zero are marked with `!`.

The current shell session always appears as the leftmost column.

## How it works

lhistory installs shell hooks that record every command to a local SQLite database (`~/.local/share/lhistory/history.db`). Recording happens in a background subshell so your prompt is never delayed.

Each terminal tab, split pane, or IDE terminal panel gets a unique session ID (generated from PID, timestamp, and a random value). This means opening three tabs in iTerm, a split in tmux, and a terminal in Cursor gives you five separate sessions — each tracked independently with its own column in the TUI.

lhistory also detects *where* each session is running. It checks environment variables (`TERM_PROGRAM`, `CURSOR_SESSION_ID`, `VSCODE_PID`, `ZED_TERM`) and walks the process tree to identify the parent application. Column headers show the app name — so you can tell at a glance which commands ran in Cursor vs your regular terminal.

When you press up arrow, lhistory launches a TUI that queries the database and displays commands from all recent sessions in a multi-column layout aligned by time.

## Requirements

- macOS or Linux
- C compiler (cc/gcc/clang)
- One of: zsh, bash, or fish

No external dependencies — SQLite is bundled.

## License

MIT
