// Reusable native UI kit pieces: design tokens, themes and stroke icons.
// Direct2D/DirectWrite only; no web engine. Include after <d2d1.h>.
#pragma once

#include <d2d1.h>

#include "lucide_render.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace molink::ui {

/// Palette mirroring frontend/src/index.css (:root and [data-theme=light]).
struct palette {
  D2D1_COLOR_F bg, surface, panel, panel_strong, line, line_soft, ink, muted,
      accent, accent_strong, accent_soft, on_accent;
  /// Page gradient endpoints and the two radial glows the web paints behind
  /// everything (index.css --bg-soft / --glow-1 / --glow-2).
  D2D1_COLOR_F bg_soft, glow1, glow2;
};

inline const palette kDark{
    D2D1::ColorF(0x151d2b), D2D1::ColorF(0x1e2938), D2D1::ColorF(0x283649),
    D2D1::ColorF(0x314158), D2D1::ColorF(0x3a4c62), D2D1::ColorF(0x2d3c50),
    D2D1::ColorF(0xeef3fa), D2D1::ColorF(0xa8b5c7), D2D1::ColorF(0x5eead4),
    D2D1::ColorF(0x2dd4bf), D2D1::ColorF(0x5eead4, 0.10f),
    D2D1::ColorF(0x052e28), D2D1::ColorF(0x121a26),
    D2D1::ColorF(0x5eead4, 0.07f), D2D1::ColorF(0x60a5fa, 0.05f)};

inline const palette kLight{
    D2D1::ColorF(0xf5f7fa), D2D1::ColorF(0xffffff), D2D1::ColorF(0xf1f5f9),
    D2D1::ColorF(0xe8eef5), D2D1::ColorF(0xd9e2ec), D2D1::ColorF(0xe8eef5),
    D2D1::ColorF(0x0f172a), D2D1::ColorF(0x5b6b82), D2D1::ColorF(0x0f9488),
    D2D1::ColorF(0x0d7a70), D2D1::ColorF(0x0f9488, 0.10f),
    D2D1::ColorF(0xffffff), D2D1::ColorF(0xeef2f7),
    D2D1::ColorF(0x0f9488, 0.06f), D2D1::ColorF(0x3b82f6, 0.05f)};

enum class theme_mode { automatic, light, dark };

/// Cached solid brush (header-local): the UI redraws every animation frame and
/// each line/text used to create and release its own brush.
inline ID2D1SolidColorBrush* cached_brush(ID2D1RenderTarget* t,
                                          const D2D1_COLOR_F& c) {
  struct entry {
    ID2D1SolidColorBrush* brush;
    uint32_t key;
  };
  static ID2D1RenderTarget* cached_target = nullptr;
  static std::vector<entry> entries;
  if (t != cached_target) {
    for (entry& e : entries) e.brush->Release();
    entries.clear();
    cached_target = t;
  }
  const uint32_t key =
      (static_cast<uint32_t>(c.r * 255.0f + 0.5f) << 24) |
      (static_cast<uint32_t>(c.g * 255.0f + 0.5f) << 16) |
      (static_cast<uint32_t>(c.b * 255.0f + 0.5f) << 8) |
      static_cast<uint32_t>(c.a * 255.0f + 0.5f);
  for (const entry& e : entries) {
    if (e.key == key) return e.brush;
  }
  ID2D1SolidColorBrush* brush = nullptr;
  if (FAILED(t->CreateSolidColorBrush(c, &brush)) || brush == nullptr) {
    return nullptr;
  }
  if (entries.size() > 512) {
    for (entry& e : entries) e.brush->Release();
    entries.clear();
  }
  entries.push_back({brush, key});
  return brush;
}

inline void line(ID2D1RenderTarget* t, float x1, float y1, float x2, float y2,
                 const D2D1_COLOR_F& color, float width = 1.3f) {
  ID2D1SolidColorBrush* b = cached_brush(t, color);
  if (b == nullptr) return;
  t->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width);
}

enum class icon {
  none, settings, clipboard, fullscreen, close, plus, sun, moon, chevron_down,
  monitor, folder, chat, user, shield, pin, pin_off, volume, volume_off,
  activity, keyboard, logout, copy, paste, clipboard_check, clipboard_x,
  gauge, cpu, wrench, rotate, minimize, send, globe, server, info, pencil,
  eye, eye_off, key, star, trash, grid, list, arrow_up_right, upload, check,
  shield_check, shield_off, hand, pointer, house
};

/// Icons are the web's lucide set (same geometry, 2/24 stroke, round caps and
/// joins) rendered from generated SVG data. The hand-drawn stroke set below
/// stays as a fallback for any glyph without data.
inline void draw_icon(ID2D1RenderTarget* t, icon kind, float cx, float cy,
                      const D2D1_COLOR_F& color) {
  if (draw_lucide_icon(t, static_cast<int>(kind), cx, cy, color)) return;
  const float s = 7.0f;
  switch (kind) {
    case icon::close:
      line(t, cx - s, cy - s, cx + s, cy + s, color);
      line(t, cx - s, cy + s, cx + s, cy - s, color);
      break;
    case icon::plus:
      line(t, cx - s, cy, cx + s, cy, color);
      line(t, cx, cy - s, cx, cy + s, color);
      break;
    case icon::chevron_down:
      line(t, cx - 4, cy - 2, cx, cy + 2, color);
      line(t, cx, cy + 2, cx + 4, cy - 2, color);
      break;
    case icon::clipboard:
      line(t, cx - 5, cy - 6, cx + 5, cy - 6, color);
      line(t, cx - 5, cy - 6, cx - 5, cy + 6, color);
      line(t, cx + 5, cy - 6, cx + 5, cy + 6, color);
      line(t, cx - 5, cy + 6, cx + 5, cy + 6, color);
      line(t, cx - 2, cy - 8, cx + 2, cy - 8, color, 2.0f);
      break;
    case icon::fullscreen:
      // lucide Maximize2: two corner brackets.
      line(t, cx + 2, cy - 6, cx + 6, cy - 6, color);
      line(t, cx + 6, cy - 6, cx + 6, cy - 2, color);
      line(t, cx - 6, cy + 2, cx - 6, cy + 6, color);
      line(t, cx - 6, cy + 6, cx - 2, cy + 6, color);
      break;
    case icon::minimize:
      // lucide Minimize2: the two brackets point inward.
      line(t, cx + 2, cy - 2, cx + 6, cy - 6, color);
      line(t, cx + 2, cy - 2, cx + 6, cy - 2, color);
      line(t, cx - 2, cy + 2, cx - 6, cy + 6, color);
      line(t, cx - 2, cy + 2, cx - 2, cy + 6, color);
      break;
    case icon::settings:
      // lucide Settings2: three slider rows with knobs.
      line(t, cx - 6, cy - 5, cx + 6, cy - 5, color);
      line(t, cx - 6, cy, cx + 6, cy, color);
      line(t, cx - 6, cy + 5, cx + 6, cy + 5, color);
      line(t, cx - 2, cy - 8, cx - 2, cy - 2, color, 2.0f);
      line(t, cx + 3, cy - 3, cx + 3, cy + 3, color, 2.0f);
      line(t, cx - 1, cy + 2, cx - 1, cy + 8, color, 2.0f);
      break;
    case icon::sun:
      for (int i = 0; i < 8; ++i) {
        const float a = i * 3.14159265f / 4.0f;
        line(t, cx + std::cos(a) * 3.0f, cy + std::sin(a) * 3.0f,
             cx + std::cos(a) * 6.5f, cy + std::sin(a) * 6.5f, color, 1.5f);
      }
      break;
    case icon::moon:
      line(t, cx - 2, cy - 6, cx - 4, cy - 2, color);
      line(t, cx - 4, cy - 2, cx - 4, cy + 2, color);
      line(t, cx - 4, cy + 2, cx - 2, cy + 6, color);
      line(t, cx - 2, cy + 6, cx + 4, cy + 4, color);
      line(t, cx - 2, cy - 6, cx + 4, cy - 4, color);
      break;
    case icon::monitor:
      line(t, cx - 6, cy - 5, cx + 6, cy - 5, color);
      line(t, cx - 6, cy - 5, cx - 6, cy + 3, color);
      line(t, cx + 6, cy - 5, cx + 6, cy + 3, color);
      line(t, cx - 6, cy + 3, cx + 6, cy + 3, color);
      line(t, cx - 2, cy + 3, cx - 2, cy + 6, color);
      line(t, cx + 2, cy + 3, cx + 2, cy + 6, color);
      line(t, cx - 4, cy + 6, cx + 4, cy + 6, color);
      break;
    case icon::folder:
      line(t, cx - 6, cy - 3, cx - 2, cy - 3, color);
      line(t, cx - 2, cy - 3, cx - 1, cy - 5, color);
      line(t, cx - 1, cy - 5, cx + 6, cy - 5, color);
      line(t, cx - 6, cy - 3, cx - 6, cy + 5, color);
      line(t, cx + 6, cy - 5, cx + 6, cy + 5, color);
      line(t, cx - 6, cy + 5, cx + 6, cy + 5, color);
      break;
    case icon::chat:
      line(t, cx - 6, cy - 4, cx + 6, cy - 4, color);
      line(t, cx - 6, cy - 4, cx - 6, cy + 3, color);
      line(t, cx + 6, cy - 4, cx + 6, cy + 3, color);
      line(t, cx - 6, cy + 3, cx + 1, cy + 3, color);
      line(t, cx + 1, cy + 3, cx + 3, cy + 6, color);
      line(t, cx + 3, cy + 6, cx + 3, cy + 3, color);
      break;
    case icon::shield:
    case icon::shield_check:
    case icon::shield_off:
      // Security/TLS badge: a small shield outline; the web uses ShieldCheck
      // for E2E/relay and ShieldOff for insecure sessions.
      line(t, cx - 4, cy - 5, cx + 4, cy - 5, color);
      line(t, cx - 4, cy - 5, cx - 4, cy - 1, color);
      line(t, cx + 4, cy - 5, cx + 4, cy - 1, color);
      line(t, cx - 4, cy - 1, cx, cy + 5, color);
      line(t, cx + 4, cy - 1, cx, cy + 5, color);
      if (kind == icon::shield_check) {
        line(t, cx - 2, cy, cx, cy + 2, color);
        line(t, cx, cy + 2, cx + 3, cy - 2, color);
      } else if (kind == icon::shield_off) {
        line(t, cx - 5, cy + 5, cx + 5, cy - 5, color);
      }
      break;
    case icon::user:
      line(t, cx - 3, cy - 4, cx + 3, cy - 4, color);
      line(t, cx - 3, cy - 4, cx - 3, cy + 1, color);
      line(t, cx + 3, cy - 4, cx + 3, cy + 1, color);
      line(t, cx - 5, cy + 6, cx + 5, cy + 6, color);
      break;
    case icon::pin:
    case icon::pin_off:
      // Pushpin: head, collar and needle, like the web's Pin button.
      line(t, cx - 4, cy - 6, cx + 1, cy - 6, color);
      line(t, cx + 1, cy - 6, cx + 1, cy - 1, color);
      line(t, cx - 4, cy - 6, cx - 4, cy - 1, color);
      line(t, cx - 7, cy - 1, cx + 4, cy - 1, color);
      line(t, cx - 2, cy - 1, cx - 2, cy + 2, color);
      line(t, cx - 2, cy + 2, cx + 3, cy + 6, color);
      if (kind == icon::pin_off) {
        line(t, cx - 7, cy - 7, cx + 7, cy + 7, color);
      }
      break;
    case icon::volume:
    case icon::volume_off:
      line(t, cx - 7, cy - 3, cx - 4, cy - 3, color);
      line(t, cx - 4, cy - 3, cx, cy - 7, color);
      line(t, cx, cy - 7, cx, cy + 7, color);
      line(t, cx, cy + 7, cx - 4, cy + 3, color);
      line(t, cx - 4, cy + 3, cx - 7, cy + 3, color);
      line(t, cx - 7, cy + 3, cx - 7, cy - 3, color);
      if (kind == icon::volume) {
        line(t, cx + 3, cy - 3, cx + 5, cy, color);
        line(t, cx + 5, cy, cx + 3, cy + 3, color);
        line(t, cx + 5, cy - 5, cx + 7, cy, color);
        line(t, cx + 7, cy, cx + 5, cy + 5, color);
      } else {
        line(t, cx + 3, cy - 3, cx + 7, cy + 3, color);
        line(t, cx + 7, cy - 3, cx + 3, cy + 3, color);
      }
      break;
    case icon::activity:
      // ECG/log line used by the web for frame-rate and the watermark toggle.
      line(t, cx - 7, cy, cx - 4, cy, color);
      line(t, cx - 4, cy, cx - 2, cy - 5, color);
      line(t, cx - 2, cy - 5, cx, cy + 5, color);
      line(t, cx, cy + 5, cx + 2, cy - 3, color);
      line(t, cx + 2, cy - 3, cx + 4, cy, color);
      line(t, cx + 4, cy, cx + 7, cy, color);
      break;
    case icon::keyboard:
      line(t, cx - 7, cy - 4, cx + 7, cy - 4, color);
      line(t, cx - 7, cy - 4, cx - 7, cy + 4, color);
      line(t, cx + 7, cy - 4, cx + 7, cy + 4, color);
      line(t, cx - 7, cy + 4, cx + 7, cy + 4, color);
      for (int i = -2; i <= 2; ++i)
        line(t, cx + i * 2.5f - 0.5f, cy - 1, cx + i * 2.5f + 0.5f, cy - 1,
             color, 2.0f);
      line(t, cx - 3, cy + 2, cx + 3, cy + 2, color, 2.0f);
      break;
    case icon::logout:
      line(t, cx - 6, cy - 6, cx - 2, cy - 6, color);
      line(t, cx - 6, cy - 6, cx - 6, cy + 6, color);
      line(t, cx - 6, cy + 6, cx - 2, cy + 6, color);
      line(t, cx - 1, cy, cx + 6, cy, color);
      line(t, cx + 3, cy - 3, cx + 6, cy, color);
      line(t, cx + 6, cy, cx + 3, cy + 3, color);
      break;
    case icon::copy:
      line(t, cx - 6, cy - 6, cx + 2, cy - 6, color);
      line(t, cx + 2, cy - 6, cx + 2, cy - 2, color);
      line(t, cx - 2, cy - 2, cx + 6, cy - 2, color);
      line(t, cx + 6, cy - 2, cx + 6, cy + 6, color);
      line(t, cx + 6, cy + 6, cx - 2, cy + 6, color);
      line(t, cx - 2, cy + 6, cx - 2, cy - 2, color);
      break;
    case icon::paste:
      line(t, cx - 5, cy - 5, cx + 5, cy - 5, color);
      line(t, cx - 5, cy - 5, cx - 5, cy + 6, color);
      line(t, cx + 5, cy - 5, cx + 5, cy + 6, color);
      line(t, cx - 5, cy + 6, cx + 5, cy + 6, color);
      line(t, cx - 2, cy - 7, cx + 2, cy - 7, color, 2.0f);
      break;
    case icon::clipboard_check:
    case icon::clipboard_x:
      // Clipboard with a tick or a cross, as in the web's clipboard toggle.
      line(t, cx - 5, cy - 6, cx + 5, cy - 6, color);
      line(t, cx - 5, cy - 6, cx - 5, cy + 6, color);
      line(t, cx + 5, cy - 6, cx + 5, cy + 6, color);
      line(t, cx - 5, cy + 6, cx + 5, cy + 6, color);
      line(t, cx - 2, cy - 8, cx + 2, cy - 8, color, 2.0f);
      if (kind == icon::clipboard_check) {
        line(t, cx - 3, cy, cx - 1, cy + 2, color);
        line(t, cx - 1, cy + 2, cx + 4, cy - 3, color);
      } else {
        line(t, cx - 3, cy - 2, cx + 3, cy + 3, color);
        line(t, cx + 3, cy - 2, cx - 3, cy + 3, color);
      }
      break;
    case icon::gauge:
      // Speedometer arc plus a needle (web's Gauge icon).
      for (int i = 0; i <= 8; ++i) {
        const float a = 3.14159265f + i * 3.14159265f / 8.0f;
        const float nx = cx + std::cos(a) * 6.0f;
        const float ny = cy + 2 + std::sin(a) * 6.0f;
        if (i > 0) {
          const float pa = 3.14159265f + (i - 1) * 3.14159265f / 8.0f;
          line(t, cx + std::cos(pa) * 6.0f, cy + 2 + std::sin(pa) * 6.0f,
               nx, ny, color);
        }
      }
      line(t, cx, cy + 2, cx + 4, cy - 3, color);
      break;
    case icon::cpu:
      line(t, cx - 4, cy - 4, cx + 4, cy - 4, color);
      line(t, cx - 4, cy - 4, cx - 4, cy + 4, color);
      line(t, cx + 4, cy - 4, cx + 4, cy + 4, color);
      line(t, cx - 4, cy + 4, cx + 4, cy + 4, color);
      line(t, cx - 1, cy - 1, cx + 1, cy - 1, color);
      line(t, cx - 1, cy - 1, cx - 1, cy + 1, color);
      line(t, cx + 1, cy - 1, cx + 1, cy + 1, color);
      line(t, cx - 1, cy + 1, cx + 1, cy + 1, color);
      for (int i = -1; i <= 1; ++i) {
        line(t, cx + i * 2.5f, cy - 7, cx + i * 2.5f, cy - 4, color);
        line(t, cx + i * 2.5f, cy + 4, cx + i * 2.5f, cy + 7, color);
      }
      break;
    case icon::wrench:
      line(t, cx - 6, cy + 5, cx + 1, cy - 2, color);
      line(t, cx + 1, cy - 2, cx + 4, cy - 5, color);
      line(t, cx + 4, cy - 5, cx + 6, cy - 3, color);
      line(t, cx + 6, cy - 3, cx + 3, cy, color);
      line(t, cx - 6, cy + 5, cx - 4, cy + 7, color);
      break;
    case icon::globe:
      // Meridian globe: two ellipses and the equator.
      for (int i = 0; i <= 12; ++i) {
        const float a = i * 6.2831853f / 12.0f;
        const float nx = cx + std::cos(a + 0.5235988f) * 6.0f;
        const float ny = cy + std::sin(a + 0.5235988f) * 6.0f;
        const float px = cx + std::cos((i - 1) * 6.2831853f / 12.0f + 0.5235988f) * 6.0f;
        const float py = cy + std::sin((i - 1) * 6.2831853f / 12.0f + 0.5235988f) * 6.0f;
        if (i > 0) line(t, px, py, nx, ny, color);
      }
      line(t, cx - 2, cy - 6, cx - 2, cy + 6, color);
      line(t, cx + 2, cy - 6, cx + 2, cy + 6, color);
      line(t, cx - 6, cy, cx + 6, cy, color);
      break;
    case icon::server:
      line(t, cx - 6, cy - 6, cx + 6, cy - 6, color);
      line(t, cx + 6, cy - 6, cx + 6, cy - 1, color);
      line(t, cx + 6, cy - 1, cx - 6, cy - 1, color);
      line(t, cx - 6, cy - 1, cx - 6, cy - 6, color);
      line(t, cx - 6, cy + 1, cx + 6, cy + 1, color);
      line(t, cx + 6, cy + 1, cx + 6, cy + 6, color);
      line(t, cx + 6, cy + 6, cx - 6, cy + 6, color);
      line(t, cx - 6, cy + 6, cx - 6, cy + 1, color);
      line(t, cx + 3, cy - 4, cx + 4, cy - 4, color, 2.0f);
      line(t, cx + 3, cy + 3, cx + 4, cy + 3, color, 2.0f);
      break;
    case icon::check:
      line(t, cx - 4, cy, cx - 1, cy + 3, color, 2.0f);
      line(t, cx - 1, cy + 3, cx + 5, cy - 4, color, 2.0f);
      break;
    case icon::pencil:
      line(t, cx - 6, cy + 6, cx + 4, cy - 4, color);
      line(t, cx + 4, cy - 4, cx + 6, cy - 2, color);
      line(t, cx + 6, cy - 2, cx - 4, cy + 8, color);
      line(t, cx - 4, cy + 8, cx - 6, cy + 6, color);
      line(t, cx - 6, cy + 6, cx - 6, cy + 4, color);
      break;
    case icon::eye:
    case icon::eye_off:
      line(t, cx - 7, cy, cx - 3, cy - 4, color);
      line(t, cx - 3, cy - 4, cx + 3, cy - 4, color);
      line(t, cx + 3, cy - 4, cx + 7, cy, color);
      line(t, cx + 7, cy, cx + 3, cy + 4, color);
      line(t, cx + 3, cy + 4, cx - 3, cy + 4, color);
      line(t, cx - 3, cy + 4, cx - 7, cy, color);
      line(t, cx - 2, cy, cx - 1, cy + 1, color);
      line(t, cx - 1, cy + 1, cx + 1, cy + 1, color);
      line(t, cx + 1, cy + 1, cx + 2, cy, color);
      line(t, cx + 2, cy, cx + 1, cy - 1, color);
      line(t, cx + 1, cy - 1, cx - 1, cy - 1, color);
      line(t, cx - 1, cy - 1, cx - 2, cy, color);
      if (kind == icon::eye_off) line(t, cx - 7, cy + 6, cx + 7, cy - 6, color);
      break;
    case icon::key:
      line(t, cx - 5, cy - 3, cx - 2, cy - 6, color);
      line(t, cx - 2, cy - 6, cx + 2, cy - 6, color);
      line(t, cx + 2, cy - 6, cx + 5, cy - 3, color);
      line(t, cx + 5, cy - 3, cx + 2, cy, color);
      line(t, cx + 2, cy, cx - 2, cy, color);
      line(t, cx - 2, cy, cx - 5, cy - 3, color);
      line(t, cx + 3, cy + 1, cx + 5, cy + 7, color);
      line(t, cx + 5, cy + 7, cx + 2, cy + 7, color);
      line(t, cx + 4, cy + 4, cx + 2, cy + 4, color);
      break;
    case icon::star: {
      float px = 0, py = 0;
      for (int i = 0; i <= 10; ++i) {
        const float a = -1.5707963f + i * 0.6283185f;
        const float radius = (i % 2 == 0) ? 6.0f : 2.6f;
        const float nx = cx + std::cos(a) * radius;
        const float ny = cy + std::sin(a) * radius;
        if (i > 0) line(t, px, py, nx, ny, color);
        px = nx;
        py = ny;
      }
      break;
    }
    case icon::trash:
      line(t, cx - 7, cy - 5, cx + 7, cy - 5, color);
      line(t, cx - 3, cy - 5, cx - 3, cy - 7, color);
      line(t, cx + 3, cy - 5, cx + 3, cy - 7, color);
      line(t, cx - 5, cy - 5, cx - 4, cy + 7, color);
      line(t, cx + 5, cy - 5, cx + 4, cy + 7, color);
      line(t, cx - 4, cy + 7, cx + 4, cy + 7, color);
      line(t, cx - 1, cy - 2, cx - 1, cy + 4, color);
      line(t, cx + 1, cy - 2, cx + 1, cy + 4, color);
      break;
    case icon::grid:
      line(t, cx - 6, cy - 6, cx - 1, cy - 6, color);
      line(t, cx - 6, cy - 6, cx - 6, cy - 1, color);
      line(t, cx - 6, cy - 1, cx - 1, cy - 1, color);
      line(t, cx - 1, cy - 1, cx - 1, cy - 6, color);
      line(t, cx + 1, cy - 6, cx + 6, cy - 6, color);
      line(t, cx + 6, cy - 6, cx + 6, cy - 1, color);
      line(t, cx + 6, cy - 1, cx + 1, cy - 1, color);
      line(t, cx + 1, cy - 1, cx + 1, cy - 6, color);
      line(t, cx - 6, cy + 1, cx - 1, cy + 1, color);
      line(t, cx - 6, cy + 1, cx - 6, cy + 6, color);
      line(t, cx - 6, cy + 6, cx - 1, cy + 6, color);
      line(t, cx - 1, cy + 6, cx - 1, cy + 1, color);
      line(t, cx + 1, cy + 1, cx + 6, cy + 1, color);
      line(t, cx + 6, cy + 1, cx + 6, cy + 6, color);
      line(t, cx + 6, cy + 6, cx + 1, cy + 6, color);
      line(t, cx + 1, cy + 6, cx + 1, cy + 1, color);
      break;
    case icon::list:
      line(t, cx - 7, cy - 5, cx + 7, cy - 5, color);
      line(t, cx - 7, cy, cx + 7, cy, color);
      line(t, cx - 7, cy + 5, cx + 7, cy + 5, color);
      break;
    case icon::arrow_up_right:
      line(t, cx - 6, cy + 6, cx + 6, cy - 6, color);
      line(t, cx + 6, cy - 6, cx, cy - 6, color);
      line(t, cx + 6, cy - 6, cx + 6, cy, color);
      break;
    case icon::upload:
      line(t, cx, cy + 6, cx, cy - 5, color);
      line(t, cx - 4, cy - 1, cx, cy - 5, color);
      line(t, cx + 4, cy - 1, cx, cy - 5, color);
      line(t, cx - 7, cy + 3, cx - 7, cy + 7, color);
      line(t, cx - 7, cy + 7, cx + 7, cy + 7, color);
      line(t, cx + 7, cy + 3, cx + 7, cy + 7, color);
      break;
    case icon::info:
      for (int i = 0; i <= 12; ++i) {
        const float a = i * 6.2831853f / 12.0f;
        if (i == 0) continue;
        const float pa = (i - 1) * 6.2831853f / 12.0f;
        line(t, cx + std::cos(pa) * 6.0f, cy + std::sin(pa) * 6.0f,
             cx + std::cos(a) * 6.0f, cy + std::sin(a) * 6.0f, color);
      }
      line(t, cx - 0.5f, cy - 3, cx + 0.5f, cy - 3, color, 1.5f);
      line(t, cx, cy - 1, cx, cy + 4, color, 1.5f);
      break;
    case icon::house:
      line(t, cx - 7, cy - 1, cx, cy - 6, color);
      line(t, cx, cy - 6, cx + 7, cy - 1, color);
      line(t, cx - 5, cy + 0, cx - 5, cy + 6, color);
      line(t, cx + 5, cy + 0, cx + 5, cy + 6, color);
      line(t, cx - 5, cy + 6, cx + 5, cy + 6, color);
      line(t, cx - 1, cy + 6, cx - 1, cy + 2, color);
      line(t, cx - 1, cy + 2, cx + 2, cy + 2, color);
      line(t, cx + 2, cy + 2, cx + 2, cy + 6, color);
      break;
    case icon::hand:
      line(t, cx - 5, cy + 5, cx - 5, cy - 1, color);
      line(t, cx - 2, cy + 5, cx - 2, cy - 5, color);
      line(t, cx + 1, cy + 5, cx + 1, cy - 6, color);
      line(t, cx + 4, cy + 5, cx + 4, cy - 3, color);
      line(t, cx - 5, cy + 5, cx + 4, cy + 5, color);
      line(t, cx - 5, cy + 1, cx - 8, cy - 2, color);
      break;
    case icon::pointer:
      line(t, cx - 3, cy - 6, cx - 3, cy + 5, color);
      line(t, cx - 3, cy + 5, cx - 1, cy + 2, color);
      line(t, cx - 1, cy + 2, cx + 1, cy + 6, color);
      line(t, cx + 1, cy + 6, cx + 3, cy + 5, color);
      line(t, cx + 3, cy + 5, cx + 1, cy + 1, color);
      line(t, cx + 1, cy + 1, cx + 5, cy + 1, color);
      line(t, cx + 5, cy + 1, cx - 3, cy - 6, color);
      break;
    case icon::send:
      // Paper plane: a triangle with a fold line.
      line(t, cx - 6, cy - 5, cx + 6, cy, color);
      line(t, cx + 6, cy, cx - 6, cy + 5, color);
      line(t, cx - 6, cy + 5, cx - 2, cy, color);
      line(t, cx - 2, cy, cx - 6, cy - 5, color);
      break;
    case icon::rotate:
      // Rotate-ccw: three-quarter arc with an arrow head.
      for (int i = 0; i <= 8; ++i) {
        const float a = -2.35f + i * (4.71f / 8.0f);
        if (i > 0) {
          const float pa = -2.35f + (i - 1) * (4.71f / 8.0f);
          line(t, cx + std::cos(pa) * 5.0f, cy + std::sin(pa) * 5.0f,
               cx + std::cos(a) * 5.0f, cy + std::sin(a) * 5.0f, color);
        }
      }
      line(t, cx - 6, cy - 5, cx - 2, cy - 6, color);
      line(t, cx - 6, cy - 5, cx - 6, cy - 1, color);
      break;
    default:
      break;
  }
}

// Widgets (implemented in ui_kit.cpp). They take the active palette and the
// two text formats explicitly so the shell can own the state.
void fill_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                const D2D1_COLOR_F& color);
void stroke_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                  const D2D1_COLOR_F& color, float width = 1.0f);
/// Approximates the web's shadow-md with layered translucent offsets.
void shadow(ID2D1RenderTarget* t, const palette& th, const D2D1_RECT_F& r, float radius);
void text(ID2D1RenderTarget* t, IDWriteTextFormat* format,
          const std::wstring& s, const D2D1_RECT_F& r,
          const D2D1_COLOR_F& color);
/// A rounded pill with optional leading icon; `accent` paints the active
/// style (accent-soft fill, accent border and text).
void pill(ID2D1RenderTarget* t, const palette& theme, IDWriteTextFormat* small,
          IDWriteTextFormat* small_left, const D2D1_RECT_F& r,
          const std::wstring& label, bool accent, bool hovered,
          icon glyph = icon::none);
/// A labelled slider row: label on the left, track, value on the right.
void slider(ID2D1RenderTarget* t, const palette& theme,
            IDWriteTextFormat* small_left, const D2D1_RECT_F& row,
            const std::wstring& label, const std::wstring& value,
            float fraction);
/// Live session numbers shown in the toolbar. The shell fills these from the
/// session (codec preference, wire rate, ping RTT, screen size); the smoothing
/// keeps the chip from flickering while bytes arrive in bursts.
struct session_stats {
  std::wstring codec = L"-";
  std::wstring resolution;
  int kbps = 0;
  int latency_ms = 0;
  /// Measured delivered-frame rate (EMA), for the watermark.
  double fps = 0.0;
  uint64_t last_frame_ms = 0;
  void note_frame(uint64_t now_ms) {
    if (last_frame_ms != 0 && now_ms > last_frame_ms) {
      const double instant = 1000.0 / static_cast<double>(now_ms - last_frame_ms);
      fps = fps == 0.0 ? instant : fps * 0.75 + instant * 0.25;
    }
    last_frame_ms = now_ms;
  }

  void note_bitrate(double kbps_now, double alpha = 0.3) {
    if (kbps == 0) {
      kbps = static_cast<int>(kbps_now);
      return;
    }
    kbps = static_cast<int>(kbps * (1.0 - alpha) + kbps_now * alpha);
  }
  void note_latency(int ms) { latency_ms = ms < 0 ? 0 : ms; }
};

/// Stateful dropdown: `open` and the selected index travel with the caller.
struct dropdown_state {
  bool open = false;
  int index = 0;
};

/// Width of a tab, derived from its label so draw and hit-testing agree.
float tab_width(const std::wstring& label);
D2D1_RECT_F tab_rect(const D2D1_RECT_F& strip, int index,
                     const std::vector<std::wstring>& labels);
/// Draws a tab strip; `active` is the selected tab.
void tabs_draw(ID2D1RenderTarget* t, const palette& theme,
               IDWriteTextFormat* small, const D2D1_RECT_F& strip,
               const std::vector<std::wstring>& labels, int active,
               float hover_x, float hover_y);
/// Returns the tab under the point, or -1.
int tabs_hit(const D2D1_RECT_F& strip,
             const std::vector<std::wstring>& labels, float x, float y);

/// Draws a select field and, when open, its list below it.
void dropdown_draw(ID2D1RenderTarget* t, const palette& theme,
                   IDWriteTextFormat* small_left, const D2D1_RECT_F& field,
                   const std::vector<std::wstring>& items, dropdown_state& s,
                   float hover_x, float hover_y);
/// Handles a click. Returns the chosen index when one was picked, -1 while
/// only opening/closing, and -2 when the click was outside.
int dropdown_click(dropdown_state& s, const D2D1_RECT_F& field,
                   const std::vector<std::wstring>& items, float x, float y);

/// Floating tooltip next to (x, y); keep it inside the client area.
void tooltip(ID2D1RenderTarget* t, const palette& theme,
             IDWriteTextFormat* small, float x, float y,
             const std::wstring& label, const D2D1_SIZE_F& size);
/// Bottom-centre toast; fades out in the last 400 ms before `until_ms`.
struct toast_state {
  std::wstring text;
  uint64_t until_ms = 0;
};
void toast(ID2D1RenderTarget* t, const palette& theme,
           IDWriteTextFormat* small, const D2D1_SIZE_F& size,
           const toast_state& state, uint64_t now_ms);
/// Dims the content behind a modal.
void modal_scrim(ID2D1RenderTarget* t, const D2D1_SIZE_F& size);
/// Draws a modal frame centred in `size`; returns the inner content rect and
/// the close-button rect.
D2D1_RECT_F modal_panel(ID2D1RenderTarget* t, const palette& theme,
                        IDWriteTextFormat* title, const D2D1_SIZE_F& size,
                        float width, float height,
                        const std::wstring& heading,
                        D2D1_RECT_F* close_rect);

/// A small on/off switch.
void toggle(ID2D1RenderTarget* t, const palette& theme, const D2D1_RECT_F& r,
            bool on);

/// A borderless icon-only button that only shows a surface when hovered.
void icon_button(ID2D1RenderTarget* t, const palette& theme,
                 const D2D1_RECT_F& r, icon glyph, bool hovered);

/// Ease-out cubic for collapse/expand and panel transitions.
inline float ease_out(float t) {
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

}  // namespace molink::ui
