import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import './i18n'
import './index.css'

const root = document.getElementById('root')
if (!root) throw new Error('root element is missing')

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
