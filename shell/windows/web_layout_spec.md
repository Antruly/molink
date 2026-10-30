# Web UI layout spec (measured over CDP, 1280x720, 96 DPI)

Source: the running shell page on molink.ssvr.top, `getBoundingClientRect()` +
`getComputedStyle()`. Native layouts should use these numbers (Tailwind 4px
grid) so the Direct2D UI matches the React UI position for position.

## Title strip
| element | x | y | w | h | font | radius |
|---|---|---|---|---|---|---|
| window buttons (min/max/close) | 1168 / 1204 / 1240 | 6 | 32 | 28 | 16 | 6 |
| 首页 tab | 127 | 49 | 68 | 30 | 12 | 8 |
| device name (in strip) | 211 | 49 | 146 | 30 | 12 | 0 |
| update chip | 1043 | 53 | 56 | 21 | 11 | 6 |
| download chip | 1103 | 53 | 56 | 21 | 11 | 6 |
| icon buttons | 1196 / 1232 | 48 | 32 | 32 | 13 | 9.6 |

## Device card (home)
| element | x | y | w | h | font | radius |
|---|---|---|---|---|---|---|
| avatar | 37 | 113 | 58 | 58 | 16 | 16 |
| device name | 107 | 113 | 174 | 24 | 16 | 0 |
| status dot | 239 | 171 | 24 | 24 | 16 | 4 |
| toggle (single/multi) | 323 | 238 | 24 | 24 | 13 | 9.6 |

## Notes
- The whole UI sits on Tailwind's 4px grid; paddings are multiples of 4.
- Text sizes in use: 11px (chips), 12px (tabs), 13px (icon buttons),
  16px (device name / body headings).
- Radii: 6 (small chips), 8 (tabs/pills), 9.6 (round icon buttons),
  16 (avatar).
- Re-measure after any web style change; extend this table with the connect
  panel inputs and the recent-session rows next.
