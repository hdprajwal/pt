# pt prompt integration (fish) — source from ~/.config/fish/config.fish:
#
#     source /path/to/share/prompt/pt-prompt.fish
#
# Outside pt, PT_PROJECT is unset and this file does nothing, so it is safe to
# source unconditionally.
#
# Inside pt it installs a fish_prompt event handler that, on every prompt:
#   * reports the last command's exit code to pt through the terminal title
#     ("pt-exit:<code>;<title>", OSC 0 — pt records the code and strips the
#     marker back out before showing the title), and
#   * prints an identity line "path ⑂ branch ✚N" in the project's accent
#     colour (PT_ACCENT, 24-bit).
#
# The accent is emitted as a raw 24-bit SGR rather than via set_color, because
# set_color falls back to a 256-colour approximation unless COLORTERM says the
# terminal is truecolor — pt's renderer always is.
#
# Knob: PT_PROMPT_GIT_UNTRACKED=no counts only tracked changes, which makes the
# dirty count much cheaper in repositories with large untracked trees (at the
# cost of newly created files no longer registering as dirty).

if test -n "$PT_PROJECT"

    # One-time capability probes, so the prompt path never pays for them.
    # --no-optional-locks keeps `git status` from refreshing the index on disk,
    # which would fight with any git command running elsewhere in the project.
    set -g _pt_git git
    set -g _pt_have_git 0
    if command -q git
        set _pt_have_git 1
        if git --no-optional-locks --version >/dev/null 2>&1
            set _pt_git git --no-optional-locks
        end
    end
    # A pathological repository must not stall the prompt: cap `git status`
    # when timeout(1) is around, and show no dirty count if it runs over.
    # Built as one list here — a variable that expands to nothing in command
    # position is an error in fish.
    set -g _pt_git_status $_pt_git
    command -q timeout; and set _pt_git_status timeout 1 $_pt_git

    # "#rrggbb" -> $_pt_fg = the matching truecolor SGR. Anything that is not
    # six hex digits falls back to pt's first accent instead of producing
    # garbage.
    function _pt_accent_fg
        set -l h (string replace -r '^#' '' -- $argv[1])
        string match -qr '^[0-9a-fA-F]{6}$' -- $h; or set h 6ee7a0
        set -g _pt_fg (printf '\e[38;2;%d;%d;%dm' \
            0x(string sub -s 1 -l 2 -- $h) \
            0x(string sub -s 3 -l 2 -- $h) \
            0x(string sub -s 5 -l 2 -- $h))
    end

    # Resolved eagerly: the prompt only recomputes when PT_ACCENT *changes*, so
    # an empty-vs-empty comparison must not be what decides whether $_pt_fg is
    # ever set.
    set -g _pt_fg ''
    set -g _pt_accent_cached "$PT_ACCENT"
    if test -n "$PT_ACCENT"
        _pt_accent_fg "$PT_ACCENT"
    else
        _pt_accent_fg '#6ee7a0'
    end

    function _pt_prompt --on-event fish_prompt
        set -l code $status   # must be the very first thing this function does

        set -l cwd $PWD
        if test -n "$HOME"
            if test "$PWD" = "$HOME"
                set cwd '~'
            else if string match -q -- "$HOME/*" "$PWD"
                set cwd '~'(string sub -s (math (string length -- "$HOME") + 1) -- "$PWD")
            end
        end
        printf '\e]0;pt-exit:%d;%s\a' $code "$cwd"

        if test "$PT_ACCENT" != "$_pt_accent_cached"
            if test -n "$PT_ACCENT"
                _pt_accent_fg "$PT_ACCENT"
            else
                _pt_accent_fg '#6ee7a0'
            end
            set -g _pt_accent_cached "$PT_ACCENT"
        end

        set -l branch ''
        if test $_pt_have_git -eq 1
            set branch ($_pt_git symbolic-ref --short -q HEAD 2>/dev/null)
            or set branch ($_pt_git rev-parse --short HEAD 2>/dev/null)
            or set branch ''
        else
            # No git binary: fall back to the branch pt captured at spawn.
            set branch "$PT_BRANCH"
        end

        set -l dirty ''
        if test -n "$branch"
            if test $_pt_have_git -eq 1
                set -l u --untracked-files=normal
                test "$PT_PROMPT_GIT_UNTRACKED" = no; and set u --untracked-files=no
                set -l out ($_pt_git_status status --porcelain $u 2>/dev/null)
                test (count $out) -gt 0; and set dirty " ✚"(count $out)
            end
            set branch " ⑂ $branch"
        end

        printf '%s%s%s%s\e[0m\n' "$_pt_fg" "$cwd" "$branch" "$dirty"
    end
end
