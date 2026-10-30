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

## Connect panel + recent filters (measured)
| element | x | y | w | h | font | radius |
|---|---|---|---|---|---|---|
| 连接 button (spans panel) | 413 | 301 | 714 | 36 | 12 | 9.6 |
| 更多设置 row | 37 | 344 | 338 | 15 | 10 | 0 |
| 最近 / 已保存 / 局域网 / 联网 | 317 / 370 / 425 / 480 | 402 | 51 / 53 / 53 / 42 | 24 | 11 | pill |
| import/update chip | 48 | 508 | 71 | 23 | 10 | 8 |
| icon buttons (row) | 1037 / 1069 / 1101 | 400 | 28 | 28 | 16 | 6 |

The home is two columns: the device card on the left (x 16-395) and the
connect/chat column on the right (x 413-1127).

## Session toolbar (measured, expanded; row y=122, height 30)
| element | x | w | h | font | radius |
|---|---|---|---|---|---|
| icon buttons (start) | 75 / 105 / 135 | 28 | 28 | 13 | 9.6 |
| 适应 / 1:1 / 拉伸 | 172 / 214 / 249 | 40 / 33 / 40 | 28 | 11 | 9.6 |
| frame-rate select | 291 | 83 | 30 | 11 | 6 |
| quality select (自适应) | 376 | 61 | 28 | 13 | 9.6 |
| codec select (自动/VP9/VP8) | 438 | 72 | 30 | 11 | 6 |
| icon button | 512 | 28 | 28 | 13 | 9.6 |
| resolution select (1920x1080) | 542 | 106 | 30 | 11 | 6 |
| icon buttons (end) | 657 / 687 / 717 / 747 | 28 | 28 | 13 | 9.6 |

The bar floats at y=122 with 28 px controls and 9.6 px radii for pills; selects
use a 6 px radius and 30 px height. When collapsed it becomes a 40x40 ball.
