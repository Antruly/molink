/// Notes are shared by device name: the same machine can appear as a recent
/// entry, a saved device, a LAN peer and in the online list, and every card
/// must show the same note. Names come from the directory and are stable, so
/// a normalized name is a better key than host:port (which changes with DHCP)
/// or the device id (which older/address-only entries do not have).
export function deviceNoteKey(name: string): string {
  return name.trim().toLocaleLowerCase().replace(/\s+/g, ' ')
}

/// Notes are one short line; keep line breaks and runs of whitespace out so a
/// pasted address cannot break the card layout.
export function cleanDeviceNote(value: string): string {
  return value
    .replace(/[\r\n\t]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 80)
}
