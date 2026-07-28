import { defineConfig } from 'vite'
import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'

// Static output. `npm run build` writes plain HTML/CSS/JS to dist/, which any
// static host serves as-is — no adapter, no server runtime, no host config.
// React is here only for the window mockup in src/mock/.
export default defineConfig({
  plugins: [tailwindcss(), react()],
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
  },
})
