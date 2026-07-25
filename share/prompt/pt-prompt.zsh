# pt prompt integration (zsh) — source from ~/.zshrc:
#
#     source /path/to/share/prompt/pt-prompt.zsh
#
# Outside pt, PT_PROJECT is unset and this file returns immediately, so it is
# safe to source unconditionally.
#
# Inside pt it installs a precmd hook that, on every prompt:
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

# One-time capability probe, so the prompt path never pays for it.
# --no-optional-locks keeps `git status` from refreshing the index on disk,
# which would fight with any git command running elsewhere in the project.
typeset -g _pt_have_git=$(( $+commands[git] ))
typeset -ga _pt_git
_pt_git=(git)
if (( _pt_have_git )); then
  git --no-optional-locks --version >/dev/null 2>&1 && _pt_git=(git --no-optional-locks)
fi
# A pathological repository must not stall the prompt: cap `git status` when
# timeout(1) is around, and simply show no dirty count if it runs over.
typeset -ga _pt_git_status
_pt_git_status=($_pt_git)
(( $+commands[timeout] )) && _pt_git_status=(timeout 1 $_pt_git)

# "#rrggbb" -> _pt_fg = the matching truecolor SGR. Anything that is not six
# hex digits falls back to pt's first accent rather than producing garbage.
_pt_accent_fg() {
  emulate -L zsh
  setopt localoptions extendedglob
  local h=${1#\#}
  [[ $h == [0-9a-fA-F](#c6) ]] || h=6ee7a0
  _pt_fg=$'\e[38;2;'$((16#${h[1,2]}))';'$((16#${h[3,4]}))';'$((16#${h[5,6]}))'m'
}

# Resolved eagerly: the prompt only recomputes when PT_ACCENT *changes*, so an
# empty-vs-empty comparison must not be what decides whether _pt_fg is ever set.
typeset -g _pt_fg='' _pt_accent_cached=${PT_ACCENT:-}
_pt_accent_fg ${PT_ACCENT:-#6ee7a0}

_pt_precmd() {
  local code=$?          # must be the very first thing this function does
  emulate -L zsh

  local cwd=${(%):-%~}   # home-abbreviated, no fork (prompt expansion)
  printf '\e]0;pt-exit:%d;%s\a' $code $cwd

  if [[ ${PT_ACCENT:-} != $_pt_accent_cached ]]; then
    _pt_accent_fg ${PT_ACCENT:-#6ee7a0}
    _pt_accent_cached=${PT_ACCENT:-}
  fi

  local branch='' dirty=''
  if (( _pt_have_git )); then
    branch=$($_pt_git symbolic-ref --short -q HEAD 2>/dev/null) ||
      branch=$($_pt_git rev-parse --short HEAD 2>/dev/null) || branch=''
  else
    # No git binary: fall back to the branch pt captured when it spawned us.
    branch=${PT_BRANCH:-}
  fi

  if [[ -n $branch ]]; then
    if (( _pt_have_git )); then
      local u=--untracked-files=normal
      [[ ${PT_PROMPT_GIT_UNTRACKED:-} == no ]] && u=--untracked-files=no
      local out
      out=$($_pt_git_status status --porcelain $u 2>/dev/null)
      if [[ -n $out ]]; then
        # Count lines with no external process: strip everything but the
        # newlines, then add one for the last (unterminated) line.
        local nl=${out//[!$'\n']/}
        dirty=" ✚$(( ${#nl} + 1 ))"
      fi
    fi
    branch=" ⑂ $branch"
  fi

  print -r -- "${_pt_fg}${cwd}${branch}${dirty}"$'\e[0m'
}

# add-zsh-hook is itself idempotent, so re-sourcing this file cannot register
# the hook twice; the fallback below keeps that property by hand.
autoload -Uz +X add-zsh-hook 2>/dev/null
if (( $+functions[add-zsh-hook] )); then
  add-zsh-hook precmd _pt_precmd
else
  precmd_functions=( ${precmd_functions:#_pt_precmd} _pt_precmd )
fi
