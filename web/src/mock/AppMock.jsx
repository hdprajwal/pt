/* A static mockup of the pt window, standing in for a screenshot. It mirrors
 * the real app's chrome: a searchable project sidebar showing git branches, a
 * path + branch header, per-shell tabs with a running dot, and a hint bar.
 *
 * Deliberately data-driven — changing what the "terminal" shows is an edit to
 * the arrays below, not to the JSX.
 *
 * Note the green: the site's accent is blue, but pt's own accent is green, so
 * the mockup uses green for anything that is app chrome (branches, the active
 * project bar, the running dot) and leaves blue to the surrounding page. */

const PROJECTS = [
  { name: 'pt', branch: 'main', ahead: '+1', count: 1, more: true, active: true },
  { name: 'emberd', branch: 'main', count: 2 },
  { name: 'digest', branch: 'main', count: 1, more: true },
  { name: 'gh-widgets', branch: 'main', count: 1 },
  { name: 'open-frame', branch: 'main', ahead: '+6', count: 1, more: true },
  { name: 'codexpass', branch: 'main', count: 1 },
]

const TABS = [
  { name: 'claude', running: true, active: true },
  { name: 'zsh' },
]

/* Each line is a list of [text, tone] pairs; tone maps to a .t-* class. */
const LINES = [
  [['Ran 3 shell commands', 'f']],
  [],
  [
    ['● ', 'g'],
    ['Rebuilt and reran the suite. ', 'i'],
    ['11/11 passing', 'g'],
    ['.', 'i'],
  ],
  [],
  [
    ['  ', 'f'],
    ['cmake --build build', 'a'],
    [' → linked ', 'f'],
    ['./build/pt', 'i'],
    [' in 12.5s.', 'f'],
  ],
  [
    ['  ', 'f'],
    ['ctest --test-dir build', 'a'],
    [' → all green, including the two', 'f'],
  ],
  [['  new regression cases on the paste path.', 'f']],
  [],
  [
    ['  The scanner sits alongside the parser rather than in', 'f'],
  ],
  [
    ['  front of it, so it consumes nothing and writes nothing', 'f'],
  ],
  [['  back to the pty.', 'f']],
  [],
  [
    ['  ', 'f'],
    ['✳ Brewed for 1h 17m 12s', 'w'],
  ],
  [],
  [
    ['❯ ', 'g'],
    ['delete the pnpm lockfile and commit the site', 'i'],
  ],
]

/* The prompt block pt draws under the input line. */
const PROMPT = [
  [['[hdprajwal@dev pt]', 'f']],
  [
    ['▸▸ ', 'g'],
    ['auto mode on', 'w'],
    [' (shift+tab to cycle) · ← for agents', 'f'],
  ],
]

function Line({ spans }) {
  if (spans.length === 0) return <div className="mock-line">&nbsp;</div>
  return (
    <div className="mock-line">
      {spans.map(([text, tone], i) => (
        <span key={i} className={`t-${tone}`}>
          {text}
        </span>
      ))}
    </div>
  )
}

export function AppMock() {
  return (
    /* Presentational only — the real UI is the app. Hidden from the
       accessibility tree so a screen reader is not walked through fake
       terminal output; the caption and prose carry the meaning. */
    <div
      className="mock"
      role="img"
      aria-label="The pt window: a project sidebar listing repositories with their git branches, shell tabs, terminal output, and a keyboard hint bar"
    >
      <div className="mock-body" aria-hidden="true">
        <aside className="mock-sidebar">
          <div className="mock-search">
            <span className="i">⌕</span> Search or ^K
          </div>

          <ul className="mock-projects">
            {PROJECTS.map((p) => (
              <li key={p.name} className={p.active ? 'active' : undefined}>
                <span className="nm">{p.name}</span>
                {p.branch && <span className="br">{p.branch}</span>}
                {p.ahead && <span className="ah">{p.ahead}</span>}
                <span className="ct">{p.count}</span>
                {p.more && <span className="ch">›</span>}
              </li>
            ))}
          </ul>

          <div className="mock-add">
            <span>+ Add project</span>
            <span className="k">^N</span>
          </div>
        </aside>

        <div className="mock-main">
          <div className="mock-header">
            <span className="nm">pt</span>
            <span className="path">~/dev/personal/pt</span>
            <span className="pill">main +1</span>
            <span className="spacer" />
            <span className="k">^⇧S split</span>
            <span className="k">^K</span>
            <span className="wc">─</span>
            <span className="wc">▫</span>
            <span className="wc">✕</span>
          </div>

          <div className="mock-tabs">
            {TABS.map((t) => (
              <span key={t.name} className={t.active ? 'active' : undefined}>
                <i className={t.running ? 'dot on' : 'dot'} />
                {t.name}
              </span>
            ))}
            <span className="spacer" />
            <span className="k">+</span>
            <span className="k">▥</span>
          </div>

          <div className="mock-pane">
            {LINES.map((spans, i) => (
              <Line key={i} spans={spans} />
            ))}
            <div className="mock-rule" />
            {PROMPT.map((spans, i) => (
              <Line key={`p${i}`} spans={spans} />
            ))}
          </div>

          <div className="mock-status">
            <i className="dot on" />
            <span className="t-g">running</span>
            <span className="spacer" />
            <span className="k">^K palette</span>
            <span className="k">^1…9 projects</span>
            <span className="k">^T new shell</span>
          </div>
        </div>
      </div>
    </div>
  )
}
