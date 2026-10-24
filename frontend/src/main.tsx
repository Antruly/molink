import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import { initialTheme } from './store/app'
import './i18n'
import './index.css'

// Apply the persisted theme before the first paint: the core uses a new port
// every launch, so localStorage is empty on a fresh origin and the theme comes
// from a cookie instead. Without this the app flashes dark first.
document.documentElement.dataset.theme = initialTheme()

const root = document.getElementById('root')
if (!root) throw new Error('root element is missing')

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
