// Reusable native UI kit pieces: design tokens, themes and stroke icons.
// Direct2D/DirectWrite only; no web engine. Include after <d2d1.h>.
#pragma once

#include <d2d1.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace molink::ui {

/// Palette mirroring frontend/src/index.css (:root and [data-theme=light]).
struct palette {
  D2D1_COLOR_F bg, surface, panel, panel_strong, line, line_soft, ink, muted,
      accent, accent_strong, accent_soft, on_accent;
};

inline const palette kDark{
    D2D1::ColorF(0x151d2b), D2D1::ColorF(0x1e2938), D2D1::ColorF(0x283649),
    D2D1::ColorF(0x314158), D2D1::ColorF(0x3a4c62), D2D1::ColorF(0x2d3c50),
    D2D1::ColorF(0xeef3fa), D2D1::ColorF(0xa8b5c7), D2D1::ColorF(0x5eead4),
    D2D1::ColorF(0x2dd4bf), D2D1::ColorF(0x5eead4, 0.10f),
    D2D1::ColorF(0x052e28)};

inline const palette kLight{
    D2D1::ColorF(0xf5f7fa), D2D1::ColorF(0xffffff), D2D1::ColorF(0xf1f5f9),
    D2D1::ColorF(0xe8eef5), D2D1::ColorF(0xd9e2ec), D2D1::ColorF(0xe8eef5),
    D2D1::ColorF(0x0f172a), D2D1::ColorF(0x5b6b82), D2D1::ColorF(0x0f9488),
    D2D1::ColorF(0x0d7a70), D2D1::ColorF(0x0f9488, 0.10f),
    D2D1::ColorF(0xffffff)};

enum class theme_mode { automatic, light, dark };

inline void line(ID2D1RenderTarget* t, float x1, float y1, float x2, float y2,
                 const D2D1_COLOR_F& color, float width = 1.5f) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, width);
  b->Release();
}

enum class icon {
  none, settings, clipboard, fullscreen, close, plus, sun, moon, chevron_down,
  monitor, folder, chat, user
};

/// 16x16 stroke icons drawn from line segments: no font dependency.
inline void draw_icon(ID2D1RenderTarget* t, icon kind, float cx, float cy,
                      const D2D1_COLOR_F& color) {
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
      line(t, cx - 5, cy - 2, cx - 5, cy - 5, color);
      line(t, cx - 5, cy - 5, cx - 2, cy - 5, color);
      line(t, cx + 2, cy - 5, cx + 5, cy - 5, color);
      line(t, cx + 5, cy - 5, cx + 5, cy - 2, color);
      line(t, cx - 5, cy + 2, cx - 5, cy + 5, color);
      line(t, cx - 5, cy + 5, cx - 2, cy + 5, color);
      line(t, cx + 2, cy + 5, cx + 5, cy + 5, color);
      line(t, cx + 5, cy + 5, cx + 5, cy + 2, color);
      break;
    case icon::settings:
      line(t, cx - 5, cy - 3, cx + 5, cy - 3, color);
      line(t, cx - 5, cy + 3, cx + 5, cy + 3, color);
      line(t, cx - 2, cy - 3, cx - 2, cy + 3, color, 2.0f);
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
    case icon::user:
      line(t, cx - 3, cy - 4, cx + 3, cy - 4, color);
      line(t, cx - 3, cy - 4, cx - 3, cy + 1, color);
      line(t, cx + 3, cy - 4, cx + 3, cy + 1, color);
      line(t, cx - 5, cy + 6, cx + 5, cy + 6, color);
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
void shadow(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius);
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
