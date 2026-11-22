/// The fields of `/api/service/status` that decide the panel's controls.
export interface ServicePanelStatus {
  available: boolean
  installed: boolean
  autostart?: boolean
  stale?: boolean
  elevated?: boolean
}

/// Which controls and warnings the settings service panel shows.
export interface ServicePanelHints {
  /// The Windows session helper (as opposed to the Linux login autostart).
  windows: boolean
  /// Windows always offers install/re-register, even when already installed.
  showReregister: boolean
  reregisterLabel: 'install' | 'reregister'
  /// Opening a UAC prompt is only worth mentioning when the core is not
  /// already elevated.
  showUacHint: boolean
  showStaleWarning: boolean
}

export function servicePanelHints(
  status: ServicePanelStatus | null | undefined,
): ServicePanelHints {
  const windows = status?.available === true && status.autostart !== true
  return {
    windows,
    showReregister: windows,
    reregisterLabel: status?.installed ? 'reregister' : 'install',
    showUacHint: windows && status?.elevated !== true,
    showStaleWarning: windows && status?.stale === true,
  }
}

/// What the user can do after an install failure:
/// - `portable`: the helper is not next to the core (portable/mis-packed).
/// - `manual`: any other failure; suggest the elevated helper directly.
export type ServiceRecovery = 'portable' | 'manual' | null

export function serviceRecoveryHint(
  error: string | null | undefined,
): ServiceRecovery {
  if (!error) return null
  return /molink-service\.exe/i.test(error) ? 'portable' : 'manual'
}

/// Keep the backend's error text verbatim and derive the recovery hint from it.
export function serviceActionFailure(
  caught: unknown,
  fallback: string,
): { message: string; recovery: ServiceRecovery } {
  const message =
    caught instanceof Error && caught.message.trim() !== ''
      ? caught.message
      : fallback
  return { message, recovery: serviceRecoveryHint(message) }
}
