/// A host-only installation may be controlled but must not control anything
/// else. The connect page is the one place that would offer an outbound
/// target, so its layout decision lives here (and is unit tested); the page
/// renders the result. The backend enforces the same rule, so this only
/// decides what the user sees.
export const HOST_ONLY_LOCAL_HOST = '127.0.0.1'
export const HOST_ONLY_FALLBACK_PORT = 28418

export interface LocalScreenTarget {
  host: string
  port: number
}

export interface HomeLayout {
  hostOnly: boolean
  /// The device-ID/address form is never rendered in host-only mode.
  showConnectForm: boolean
  /// Recents, saved, LAN and online devices are all outbound targets.
  showDeviceList: boolean
  /// The one action host-only mode offers, or null in normal mode.
  localScreen: LocalScreenTarget | null
}

/// `hostOnly` must be exactly true; an older core that does not send the
/// field keeps the full connect page. `port` is the core's bound port from
/// /api/state; a missing or invalid value falls back to the default.
export function homeLayout(
  hostOnly: boolean | undefined,
  port: number | undefined,
): HomeLayout {
  if (hostOnly !== true) {
    return {
      hostOnly: false,
      showConnectForm: true,
      showDeviceList: true,
      localScreen: null,
    }
  }
  const usable =
    port !== undefined && Number.isFinite(port) && port > 0 && port <= 65535
  return {
    hostOnly: true,
    showConnectForm: false,
    showDeviceList: false,
    localScreen: {
      host: HOST_ONLY_LOCAL_HOST,
      port: usable ? port : HOST_ONLY_FALLBACK_PORT,
    },
  }
}
