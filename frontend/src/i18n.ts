import i18n from 'i18next'
import { initReactI18next } from 'react-i18next'
import en from './locales/en.json'
import zhCN from './locales/zh-CN.json'

const LANG_KEY = 'molink.lang'

function preferredLanguage(): string {
  try {
    const stored = window.localStorage.getItem(LANG_KEY)
    if (stored === 'en' || stored === 'zh-CN') return stored
  } catch {
    // ignore
  }
  return navigator.language.toLowerCase().startsWith('zh') ? 'zh-CN' : 'en'
}

const initialLanguage = preferredLanguage()
try {
  // Keep the store's mirror (and the native tray) on the same language.
  window.localStorage.setItem(LANG_KEY, initialLanguage)
} catch {
  // ignore
}

void i18n.use(initReactI18next).init({
  resources: {
    en: { translation: en },
    'zh-CN': { translation: zhCN },
  },
  lng: initialLanguage,
  fallbackLng: 'en',
  interpolation: { escapeValue: false },
})

/// The native Windows shell draws the tray menu and cannot read localStorage.
/// Mirror the language into the core's ui.json; the shell reads it there.
async function persistLanguage(language: string): Promise<void> {
  try {
    const current = await fetch('/api/ui-preferences', { cache: 'no-store' })
    const existing = current.ok ? await current.json() : {}
    await fetch('/api/ui-preferences', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...existing, language }),
    })
  } catch {
    // A remote browser has no loopback core; the tray only exists in the shell.
  }
}

export function setLanguage(language: string): void {
  void i18n.changeLanguage(language)
  try {
    window.localStorage.setItem(LANG_KEY, language)
  } catch {
    // ignore
  }
  void persistLanguage(language)
}

void persistLanguage(initialLanguage)

export default i18n
