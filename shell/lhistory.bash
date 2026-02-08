# lhistory - bash integration
# Source this file in your .bashrc: eval "$(lhistory init bash)"

# Resolve binary path (works both installed and in development)
_LHISTORY_BIN="$(command -v lhistory 2>/dev/null || echo "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/lhistory")"

# Generate unique session ID for this shell instance
if [[ -z "$LHISTORY_SID" ]]; then
    export LHISTORY_SID="$$-$(date +%s)-$RANDOM"
fi

_LHISTORY_LAST_CMD=""
_LHISTORY_RECORDING=0

# Record command via DEBUG trap
_lhistory_debug_trap() {
    # Skip if we're inside PROMPT_COMMAND
    if [[ "$_LHISTORY_RECORDING" -eq 1 ]]; then
        return
    fi

    # $BASH_COMMAND contains the command about to be executed
    local cmd="$BASH_COMMAND"

    # Skip our own hooks
    [[ "$cmd" == _lhistory_* ]] && return
    [[ "$cmd" == *"lhistory "* ]] && return

    _LHISTORY_LAST_CMD="$cmd"
    ("$_LHISTORY_BIN" record --sid "$LHISTORY_SID" --cmd "$cmd" --dir "$PWD" --shell bash &) 2>/dev/null
}

# Record exit code via PROMPT_COMMAND
_lhistory_prompt_cmd() {
    local exit_code=$?
    _LHISTORY_RECORDING=1
    if [[ -n "$_LHISTORY_LAST_CMD" ]]; then
        ("$_LHISTORY_BIN" finish --sid "$LHISTORY_SID" --exit-code "$exit_code" &) 2>/dev/null
        _LHISTORY_LAST_CMD=""
    fi
    _LHISTORY_RECORDING=0
}

# TUI browser function for readline binding
_lhistory_browse() {
    local selected
    selected="$("$_LHISTORY_BIN" browse </dev/tty)"
    if [[ -n "$selected" ]]; then
        READLINE_LINE="$selected"
        READLINE_POINT=${#selected}
    fi
}

# Install hooks
trap '_lhistory_debug_trap' DEBUG

# Prepend to PROMPT_COMMAND
if [[ -z "$PROMPT_COMMAND" ]]; then
    PROMPT_COMMAND="_lhistory_prompt_cmd"
else
    PROMPT_COMMAND="_lhistory_prompt_cmd;$PROMPT_COMMAND"
fi

# Bind up arrow and Ctrl+R to browse
bind -x '"\e[A": _lhistory_browse'
bind -x '"\C-r": _lhistory_browse'
