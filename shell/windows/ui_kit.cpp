#include "ui_kit.h"

#include <dwrite.h>

#include <algorithm>

namespace molink::ui {

void fill_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                const D2D1_COLOR_F& color) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
  b->Release();
}

void stroke_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                  const D2D1_COLOR_F& color, float width) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
  b->Release();
}

void shadow(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius) {
  const float alphas[] = {0.20f, 0.12f, 0.06f};
  for (int i = 0; i < 3; ++i) {
    const float spread = 1.0f + i * 3.0f;
    const float dy = 3.0f + i * 4.0f;
    fill_round(t,
               D2D1::RectF(r.left - spread, r.top - spread + dy,
                           r.right + spread, r.bottom + spread + dy),
               radius + spread, D2D1::ColorF(0, 0, 0, alphas[i]));
  }
}

void text(ID2D1RenderTarget* t, IDWriteTextFormat* format,
          const std::wstring& s, const D2D1_RECT_F& r,
          const D2D1_COLOR_F& color) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), format, r, b,
               D2D1_DRAW_TEXT_OPTIONS_CLIP);
  b->Release();
}

void pill(ID2D1RenderTarget* t, const palette& theme, IDWriteTextFormat* small,
          IDWriteTextFormat* small_left, const D2D1_RECT_F& r,
          const std::wstring& label, bool accent, bool hovered, icon glyph) {
  fill_round(t, r, 8, accent ? theme.accent_soft : theme.panel);
  stroke_round(t, r, 8, accent ? theme.accent : theme.line_soft);
  if (hovered && !accent) stroke_round(t, r, 8, theme.line);
  if (glyph != icon::none) {
    draw_icon(t, glyph, r.left + 14, (r.top + r.bottom) / 2,
              accent ? theme.accent : theme.muted);
    text(t, small_left, label,
         D2D1::RectF(r.left + 26, r.top, r.right - 8, r.bottom),
         accent ? theme.accent : theme.muted);
  } else {
    text(t, small, label, r, accent ? theme.accent : theme.muted);
  }
}

void slider(ID2D1RenderTarget* t, const palette& theme,
            IDWriteTextFormat* small_left, const D2D1_RECT_F& row,
            const std::wstring& label, const std::wstring& value,
            float fraction) {
  text(t, small_left, label, D2D1::RectF(row.left, row.top, row.left + 64,
                                         row.bottom),
       theme.muted);
  const float left = row.left + 64;
  const float right = row.right - 92;
  const float mid = (row.top + row.bottom) / 2;
  fill_round(t, D2D1::RectF(left, mid - 2, right, mid + 2), 2, theme.line_soft);
  const float f = (std::max)(0.0f, (std::min)(1.0f, fraction));
  const float knob = left + (right - left) * f;
  fill_round(t, D2D1::RectF(left, mid - 2, knob, mid + 2), 2,
             theme.accent_strong);
  fill_round(t, D2D1::RectF(knob - 6, mid - 6, knob + 6, mid + 6), 6, theme.ink);
  text(t, small_left, value, D2D1::RectF(right + 10, row.top, row.right,
                                         row.bottom),
       theme.ink);
}

float tab_width(const std::wstring& label) {
  // Leading icon (14) + gap (8) + label, plus 10 px padding on each side.
  return 42.0f + 12.0f * static_cast<float>(label.size());
}

D2D1_RECT_F tab_rect(const D2D1_RECT_F& strip, int index,
                     const std::vector<std::wstring>& labels) {
  float x = strip.left + 10;
  for (int i = 0; i < index && i < static_cast<int>(labels.size()); ++i) {
    x += tab_width(labels[i]) + 6;
  }
  const float w = index < static_cast<int>(labels.size())
                      ? tab_width(labels[index])
                      : 0.0f;
  return D2D1::RectF(x, strip.top + 7, x + w, strip.bottom - 7);
}

void tabs_draw(ID2D1RenderTarget* t, const palette& theme,
               IDWriteTextFormat* small, const D2D1_RECT_F& strip,
               const std::vector<std::wstring>& labels, int active,
               float hover_x, float hover_y) {
  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    const D2D1_RECT_F r = tab_rect(strip, i, labels);
    const bool hovered = hover_x >= r.left && hover_x <= r.right &&
                         hover_y >= r.top && hover_y <= r.bottom;
    pill(t, theme, small, small, r, labels[i], i == active, hovered);
  }
}

int tabs_hit(const D2D1_RECT_F& strip,
             const std::vector<std::wstring>& labels, float x, float y) {
  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    const D2D1_RECT_F r = tab_rect(strip, i, labels);
    if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
  }
  return -1;
}

void dropdown_draw(ID2D1RenderTarget* t, const palette& theme,
                   IDWriteTextFormat* small_left, const D2D1_RECT_F& field,
                   const std::vector<std::wstring>& items, dropdown_state& s,
                   float hover_x, float hover_y) {
  fill_round(t, field, 8, theme.panel);
  stroke_round(t, field, 8, theme.line_soft);
  text(t, small_left, items[s.index],
       D2D1::RectF(field.left + 12, field.top, field.right - 28, field.bottom),
       theme.ink);
  draw_icon(t, icon::chevron_down, field.right - 16, (field.top + field.bottom) / 2,
            theme.muted);
  if (!s.open) return;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    const D2D1_RECT_F item = D2D1::RectF(
        field.left, field.bottom + 6 + i * 30, field.right,
        field.bottom + 32 + i * 30);
    const bool hovered = hover_x >= item.left && hover_x <= item.right &&
                         hover_y >= item.top && hover_y <= item.bottom;
    fill_round(t, item, 8,
               i == s.index ? theme.accent_soft
                            : (hovered ? theme.panel_strong : theme.panel));
    stroke_round(t, item, 8,
                 i == s.index ? theme.accent : theme.line_soft);
    text(t, small_left, items[i],
         D2D1::RectF(item.left + 12, item.top, item.right, item.bottom),
         i == s.index ? theme.accent : theme.ink);
  }
}

int dropdown_click(dropdown_state& s, const D2D1_RECT_F& field,
                   const std::vector<std::wstring>& items, float x, float y) {
  const bool in_field = x >= field.left && x <= field.right && y >= field.top &&
                        y <= field.bottom;
  if (in_field) {
    s.open = !s.open;
    return -1;
  }
  if (!s.open) return -2;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    const D2D1_RECT_F item = D2D1::RectF(
        field.left, field.bottom + 6 + i * 30, field.right,
        field.bottom + 32 + i * 30);
    if (x >= item.left && x <= item.right && y >= item.top && y <= item.bottom) {
      s.index = i;
      s.open = false;
      return i;
    }
  }
  s.open = false;
  return -2;
}

void tooltip(ID2D1RenderTarget* t, const palette& theme,
             IDWriteTextFormat* small, float x, float y,
             const std::wstring& label, const D2D1_SIZE_F& size) {
  const float w = 16 + 7.0f * static_cast<float>(label.size());
  const float h = 24;
  float left = x + 12;
  if (left + w > size.width - 8) left = size.width - 8 - w;
  float top = y + 14;
  if (top + h > size.height - 8) top = y - h - 8;
  const D2D1_RECT_F r = D2D1::RectF(left, top, left + w, top + h);
  shadow(t, r, 6);
  fill_round(t, r, 6, theme.panel_strong);
  stroke_round(t, r, 6, theme.line);
  text(t, small, label, r, theme.ink);
}

void toast(ID2D1RenderTarget* t, const palette& theme,
           IDWriteTextFormat* small, const D2D1_SIZE_F& size,
           const toast_state& state, uint64_t now_ms) {
  if (state.text.empty() || now_ms >= state.until_ms) return;
  const float w = 28 + 7.0f * static_cast<float>(state.text.size());
  const float h = 32;
  const float left = (size.width - w) / 2;
  const float top = size.height - h - 24;
  const D2D1_RECT_F r = D2D1::RectF(left, top, left + w, top + h);
  shadow(t, r, 8);
  fill_round(t, r, 8, theme.panel_strong);
  stroke_round(t, r, 8, theme.line);
  text(t, small, state.text, r, theme.ink);
}

void modal_scrim(ID2D1RenderTarget* t, const D2D1_SIZE_F& size) {
  fill_round(t, D2D1::RectF(0, 0, size.width, size.height), 0,
             D2D1::ColorF(0, 0, 0, 0.45f));
}

D2D1_RECT_F modal_panel(ID2D1RenderTarget* t, const palette& theme,
                        IDWriteTextFormat* title, const D2D1_SIZE_F& size,
                        float width, float height,
                        const std::wstring& heading,
                        D2D1_RECT_F* close_rect) {
  const D2D1_RECT_F panel =
      D2D1::RectF((size.width - width) / 2, (size.height - height) / 2,
                  (size.width + width) / 2, (size.height + height) / 2);
  shadow(t, panel, 14);
  fill_round(t, panel, 14, theme.surface);
  stroke_round(t, panel, 14, theme.line);
  text(t, title, heading,
       D2D1::RectF(panel.left + 20, panel.top + 14, panel.right - 60,
                   panel.top + 40),
       theme.ink);
  if (close_rect != nullptr) {
    *close_rect = D2D1::RectF(panel.right - 44, panel.top + 14,
                              panel.right - 18, panel.top + 40);
  }
  return D2D1::RectF(panel.left + 20, panel.top + 48, panel.right - 20,
                     panel.bottom - 16);
}

void toggle(ID2D1RenderTarget* t, const palette& theme, const D2D1_RECT_F& r,
            bool on) {
  fill_round(t, r, (r.bottom - r.top) / 2,
             on ? theme.accent : theme.panel_strong);
  const float half = (r.bottom - r.top) / 2;
  const float cx = on ? r.right - half : r.left + half;
  const float cy = (r.top + r.bottom) / 2;
  fill_round(t, D2D1::RectF(cx - half + 3, cy - half + 3, cx + half - 3,
                            cy + half - 3),
             half - 3, on ? theme.on_accent : theme.muted);
}

void icon_button(ID2D1RenderTarget* t, const palette& theme,
                 const D2D1_RECT_F& r, icon glyph, bool hovered) {
  fill_round(t, r, 8, hovered ? theme.panel : D2D1::ColorF(0, 0, 0, 0));
  draw_icon(t, glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2,
            theme.muted);
}

}  // namespace molink::ui
