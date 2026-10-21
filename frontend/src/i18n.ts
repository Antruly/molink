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

void i18n.use(initReactI18next).init({
  resources: {
    en: { translation: en },
    'zh-CN': { translation: zhCN },
  },
  lng: preferredLanguage(),
  fallbackLng: 'en',
  interpolation: { escapeValue: false },
})

export function setLanguage(language: string): void {
  void i18n.changeLanguage(language)
  try {
    window.localStorage.setItem(LANG_KEY, language)
  } catch {
    // ignore
  }
}

export default i18n
