# lhistory - zsh integration
# Source this file in your .zshrc: eval "$(lhistory init zsh)"

# Resolve binary path
_LHISTORY_BIN="${commands[lhistory]:-lhistory}"

# Generate unique session ID for this shell instance
if [[ -z "$LHISTORY_SID" ]]; then
    export LHISTORY_SID="$$-$(date +%s)-${RANDOM}"
fi

# Record command before execution (async, non-blocking)
_lhistory_preexec() {
    _LHISTORY_LAST_CMD="$1"
    ("$_LHISTORY_BIN" record --sid "$LHISTORY_SID" --cmd "$1" --dir "$PWD" --shell zsh &) 2>/dev/null
}

# Record exit code after command completes (async, non-blocking)
_lhistory_precmd() {
    local exit_code=$?
    if [[ -n "$_LHISTORY_LAST_CMD" ]]; then
        ("$_LHISTORY_BIN" finish --sid "$LHISTORY_SID" --exit-code "$exit_code" &) 2>/dev/null
        _LHISTORY_LAST_CMD=""
    fi
}

# TUI browser widget for zle
_lhistory_browse_widget() {
    local selected
    selected="$("$_LHISTORY_BIN" browse </dev/tty)"
    if [[ -n "$selected" ]]; then
        LBUFFER="$selected"
        RBUFFER=""
    fi
    zle reset-prompt
}

# Register hooks
autoload -Uz add-zsh-hook
add-zsh-hook preexec _lhistory_preexec
add-zsh-hook precmd _lhistory_precmd

# Register zle widget and bind to up arrow
zle -N _lhistory_browse_widget
bindkey '\e[A' _lhistory_browse_widget
# Also bind Ctrl+R for muscle memory
bindkey '^R' _lhistory_browse_widget
