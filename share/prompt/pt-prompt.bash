# pt prompt integration (bash) — source from ~/.bashrc:
#
#     source /path/to/share/prompt/pt-prompt.bash
#
# Outside pt, PT_PROJECT is unset and this file returns immediately, so it is
# safe to source unconditionally.
#
# Inside pt it prepends a PROMPT_COMMAND entry that, on every prompt:
#   * reports the last command's exit code to pt through the terminal title
#     ("pt-exit:<code>;<title>", OSC 0 — pt records the code and strips the
#     marker back out before showing the title), and
#   * prints an identity line "path ⑂ branch ✚N" in the project's accent
#     colour (PT_ACCENT, 24-bit).
#
# Knob: PT_PROMPT_GIT_UNTRACKED=no counts only tracked changes, which makes the
# dirty count much cheaper in repositories with large untracked trees (at the
# cost of newly created files no longer registering as dirty).

[[ -n ${PT_PROJECT:-} ]] || return 0

# One-time capability probes, so the prompt path never pays for them.
# --no-optional-locks keeps `git status` from refreshing the index on disk,
# which would fight with any git command running elsewhere in the project.
_pt_git=(git)
if command -v git >/dev/null 2>&1; then
  git --no-optional-locks --version >/dev/null 2>&1 && _pt_git=(git --no-optional-locks)
  _pt_have_git=1
else
  _pt_have_git=0
fi
# A pathological repository must not stall the prompt: cap `git status` when
# timeout(1) is around, and simply show no dirty count if it runs over.
_pt_git_status=("${_pt_git[@]}")
command -v timeout >/dev/null 2>&1 && _pt_git_status=(timeout 1 "${_pt_git[@]}")

# "#rrggbb" -> _pt_fg = the matching truecolor SGR. Anything that is not six
# hex digits falls back to pt's first accent rather than producing garbage.
_pt_accent_fg() {
  local h=${1#\#}
  [[ $h =~ ^[0-9a-fA-F]{6}$ ]] || h=6ee7a0
  printf -v _pt_fg '\e[38;2;%d;%d;%dm' \
    "$((16#${h:0:2}))" "$((16#${h:2:2}))" "$((16#${h:4:2}))"
}

# Resolved eagerly: the prompt only recomputes when PT_ACCENT *changes*, so an
# empty-vs-empty comparison must not be what decides whether _pt_fg is ever set.
_pt_fg=''
_pt_accent_cached=${PT_ACCENT:-}
_pt_accent_fg "${PT_ACCENT:-#6ee7a0}"

_pt_precmd() {
  local code=$?          # must be the very first thing this function does

  local cwd=$PWD
  if [[ -n ${HOME:-} ]]; then
    if [[ $PWD == "$HOME" ]]; then
      cwd='~'
    elif [[ $PWD == "$HOME"/* ]]; then
      cwd="~${PWD#"$HOME"}"
    fi
  fi
  printf '\e]0;pt-exit:%d;%s\a' "$code" "$cwd"

  if [[ ${PT_ACCENT:-} != "$_pt_accent_cached" ]]; then
    _pt_accent_fg "${PT_ACCENT:-#6ee7a0}"
    _pt_accent_cached=${PT_ACCENT:-}
  fi

  local branch='' dirty=''
  if ((_pt_have_git)); then
    branch=$("${_pt_git[@]}" symbolic-ref --short -q HEAD 2>/dev/null) ||
      branch=$("${_pt_git[@]}" rev-parse --short HEAD 2>/dev/null) || branch=''
  else
    # No git binary: fall back to the branch pt captured when it spawned us.
    branch=${PT_BRANCH:-}
  fi

  if [[ -n $branch ]]; then
    if ((_pt_have_git)); then
      local u=--untracked-files=normal
      [[ ${PT_PROMPT_GIT_UNTRACKED:-} == no ]] && u=--untracked-files=no
      local out
      out=$("${_pt_git_status[@]}" status --porcelain "$u" 2>/dev/null)
      if [[ -n $out ]]; then
        # Count lines with no external process: strip everything but the
        # newlines, then add one for the last (unterminated) line.
        local nl=${out//[!$'\n']/}
        dirty=" ✚$((${#nl} + 1))"
      fi
    fi
    branch=" ⑂ $branch"
  fi

  printf '%s%s%s%s\e[0m\n' "$_pt_fg" "$cwd" "$branch" "$dirty"
}

# Prepend to PROMPT_COMMAND, skipping if already installed so that re-sourcing
# this file does not print the identity line twice. bash 5.1+ lets
# PROMPT_COMMAND be an array; handle both shapes.
_pt_install() {
  local flags
  flags=$(declare -p PROMPT_COMMAND 2>/dev/null)
  flags=${flags#declare }
  flags=${flags%% *}
  if [[ $flags == -*a* ]]; then
    local e
    local -a keep=()
    for e in "${PROMPT_COMMAND[@]}"; do
      [[ $e == _pt_precmd ]] || keep+=("$e")
    done
    PROMPT_COMMAND=(_pt_precmd ${keep[@]+"${keep[@]}"})
  else
    case ";${PROMPT_COMMAND:-};" in
      *";_pt_precmd;"*) ;;
      *) PROMPT_COMMAND="_pt_precmd${PROMPT_COMMAND:+;$PROMPT_COMMAND}" ;;
    esac
  fi
}
_pt_install
unset -f _pt_install
