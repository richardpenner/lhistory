# lhistory - fish integration
# Source this file in your config.fish: eval (lhistory init fish)

# Resolve binary path (works both installed and in development)
if command -sq lhistory
    set -g _LHISTORY_BIN (command -s lhistory)
else
    set -g _LHISTORY_BIN (status dirname)/../lhistory
end

# Generate unique session ID for this shell instance
if not set -q LHISTORY_SID
    set -gx LHISTORY_SID (echo $$-(date +%s)-(random))
end

# Record command before execution
function _lhistory_preexec --on-event fish_preexec
    set -g _LHISTORY_LAST_CMD $argv[1]
    command $_LHISTORY_BIN record --sid "$LHISTORY_SID" --cmd "$argv[1]" --dir "$PWD" --shell fish &
    disown 2>/dev/null
end

# Record exit code after command completes
function _lhistory_postexec --on-event fish_postexec
    set -l exit_code $status
    if set -q _LHISTORY_LAST_CMD
        command $_LHISTORY_BIN finish --sid "$LHISTORY_SID" --exit-code "$exit_code" &
        disown 2>/dev/null
        set -e _LHISTORY_LAST_CMD
    end
end

# TUI browser function
function _lhistory_browse
    set -l selected (command $_LHISTORY_BIN browse </dev/tty)
    if test -n "$selected"
        commandline -r -- "$selected"
        commandline -f repaint
    end
end

# Bind up arrow and Ctrl+R to browse
bind \e\[A _lhistory_browse
bind \cr _lhistory_browse
