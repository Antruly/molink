#include "ui_kit.h"

#include <dwrite.h>

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
         D2D1::RectF(r.left + 26, r.top, r.right - 6, r.bottom),
         accent ? theme.accent : theme.muted);
  } else {
    text(t, small, label, r, accent ? theme.accent : theme.muted);
  }
}

void icon_button(ID2D1RenderTarget* t, const palette& theme,
                 const D2D1_RECT_F& r, icon glyph, bool hovered) {
  fill_round(t, r, 8, hovered ? theme.panel : D2D1::ColorF(0, 0, 0, 0));
  draw_icon(t, glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2,
            theme.muted);
}

}  // namespace molink::ui
