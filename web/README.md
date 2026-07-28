# pt — website

The landing page for pt. Static output: Vite + Tailwind v4, no framework and no
server runtime.

## Develop

```sh
cd web
npm install
npm run dev
```

## Build

```sh
npm run build
```

Writes plain HTML/CSS/JS to `web/dist/`. Nothing in the output needs a Node
runtime, so any static host serves it directly.

## Deploy

Not wired to any host, and there is no CI workflow for it — deploy when and
where you want. For Cloudflare Pages or Vercel, point the project at this
directory and use:

| setting          | value           |
| ---------------- | --------------- |
| root directory   | `web`           |
| build command    | `npm run build` |
| output directory | `dist`          |

`npx serve dist` (or any static file server) is enough to check the built
output locally first.

## Layout

```
web/
  index.html            markup, one page
  src/
    styles/app.css      design tokens + all component styles
    main.jsx            mounts the mockup, theme toggle, copy button
    mock/AppMock.jsx    the window mockup standing in for a screenshot
  public/favicon.svg    copied to the output root as-is
  vite.config.js
```

## The window mockup

`src/mock/AppMock.jsx` draws a fake pt window — sidebar, tabs, split panes,
statusline — in place of a screenshot. It is data-driven: the projects, tabs and
terminal lines are arrays at the top of the file, so changing what it shows is
an edit to data, not to JSX. Each line is a list of `[text, tone]` pairs, and
tone maps to a `.t-*` class in `app.css`.

It is `role="img"` with a text label and `aria-hidden` innards, so a screen
reader gets the description rather than being walked through fake terminal
output.

**This is the only React on the page**, and it is the reason the JS bundle is
~61 kB gzipped instead of ~0.7 kB. Everything else is server-shipped markup, so
if the script fails the page is still complete and readable. Two ways to get
that weight back if it ever matters:

- replace the mockup with a real screenshot (`<img>`), and drop `react`,
  `react-dom` and `@vitejs/plugin-react`; or
- prerender the mockup to static HTML at build time, since it has no
  interactivity — nothing in it changes after mount.

## Styling

Layout follows the same document shell as `gh-widgets` and `codexpass`: a
896px nav and footer around a 672px `main`, small type (h1 28px, h2 15px, body
14px), bordered `<pre>` blocks and `.table-wrap` tables. Keeping the three
sites structurally identical is deliberate — fixing one teaches you all of
them.

`src/styles/app.css` is plain CSS on element selectors, not utility classes.
Tailwind is imported for preflight only. If you add markup, prefer a semantic
class and a rule in that file over a pile of utilities in the HTML.

The palette and type are CSS custom properties at the top of the file. Dark is
the default on `:root`; light is opt-in via `data-theme="light"` on `<html>`,
which the inline script in `<head>` applies before first paint — from a saved
choice, falling back to the system preference — so the wrong palette is never
rendered and repainted.

Fonts are Geist and Geist Mono, self-hosted through `@fontsource-variable`, so
there is no external font request at runtime. The other two sites pull the same
families from Google Fonts; this one does not, which is the single intentional
divergence.

Changing a colour everywhere means editing one custom property. Do not
hard-code hex values in markup, or light mode silently breaks.

## Content

Copy is kept in sync with the root `README.md` by hand — install command,
requirements and the config example are all repeated there. If those change,
update both.
