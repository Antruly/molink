import { ConnectPanel } from './components/ConnectPanel'
import { Header } from './components/Header'
import { SessionView } from './components/SessionView'
import { useSessionStore } from './store/session'

export default function App() {
  const phase = useSessionStore((state) => state.phase)

  return (
    <div className="flex h-screen flex-col">
      <Header />
      <main className="flex min-h-0 flex-1 flex-col overflow-auto">
        {phase === 'live' ? <SessionView /> : <ConnectPanel />}
      </main>
      <footer className="border-t border-line-soft px-5 py-3 text-center text-[11px] tracking-wide text-fog/70">
        MIT License · moLink Core 0.1.0 · protocol v1
      </footer>
    </div>
  )
}
