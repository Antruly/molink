import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    port: 1421,
    strictPort: true,
    proxy: {
      '/api': 'http://127.0.0.1:28431',
      '/healthz': 'http://127.0.0.1:28431',
    },
  },
  build: { outDir: 'dist' },
})
