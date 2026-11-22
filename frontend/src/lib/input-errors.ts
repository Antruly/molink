/// Classification and sanitising helpers for host `error` frames that belong to
/// the input channel rather than to the session itself.

/// Error codes that describe one rejected input event. They must never tear
/// down the session or offer a reconnect: the link is healthy, only a key or
/// pointer event did not reach the host.
export function isInputErrorCode(code: string): boolean {
  return (
    code === 'bad_input' ||
    code === 'unknown_input' ||
    code === 'unknown_key' ||
    code === 'bad_key' ||
    code.startsWith('input_')
  )
}

/// Remove credentials from text that is about to enter the diagnostics log.
/// Tokens may appear as `token=...`, `"token":"..."`, `Bearer ...` or inside a
/// query string; the diagnostics buffer is exportable, so none may survive.
///
/// Only credential-shaped keys are masked. A host input error says
/// `unknown key code: MetaLeft`; masking every `code:` would hide the one
/// detail the user needs.
export function redactDiagnosticText(text: string): string {
  return text
    .replace(/(bearer\s+)[A-Za-z0-9._~+/=-]+/gi, '$1***')
    .replace(
      /((?:"?(?:token|secret|password|passwd)"?\s*[=:]\s*"?))[^"\s,;&}]+/gi,
      '$1***',
    )
    .replace(/([?&](?:token|secret|code|session)=)[^&\s]+/gi, '$1***')
}
