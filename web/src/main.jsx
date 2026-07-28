import { createRoot } from 'react-dom/client'
import { AppMock } from './mock/AppMock.jsx'

/* The stylesheet is linked from <head> rather than imported here, so it is in
 * the document before first paint instead of arriving with the module. */

/* ---- app mockup --------------------------------------------------------
 * The only React on the page. Everything else is server-shipped markup, so
 * if this fails to mount the page is still complete and readable. */
const mockRoot = document.getElementById('app-mock')
if (mockRoot) createRoot(mockRoot).render(<AppMock />)

/* ---- theme -------------------------------------------------------------
 * Dark is the default on :root; light is opt-in via data-theme, which the
 * inline script in <head> has already applied if it was the saved choice. */
document.querySelector('.theme-toggle')?.addEventListener('click', () => {
  const light = document.documentElement.dataset.theme === 'light'
  if (light) delete document.documentElement.dataset.theme
  else document.documentElement.dataset.theme = 'light'
  try {
    localStorage.setItem('theme', light ? 'dark' : 'light')
  } catch {
    /* private mode — the change still applies for this session */
  }
})

/* ---- copy the install command ------------------------------------------
 * Reads textContent rather than a duplicated string, so the button cannot
 * drift out of sync with what is rendered. */
const copyBtn = document.getElementById('copy-btn')
const cmd = document.getElementById('install-cmd')

copyBtn?.addEventListener('click', async () => {
  const text = cmd?.textContent?.replace(/\s+/g, ' ').trim() ?? ''
  try {
    await navigator.clipboard.writeText(text)
    copyBtn.textContent = 'copied'
  } catch {
    copyBtn.textContent = 'failed'
  }
  setTimeout(() => {
    copyBtn.textContent = 'copy'
  }, 1600)
})
