// Native UI prototype: Direct2D toolbar + quality panel matching the web
// design tokens (frontend/src/index.css). No web engine.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define INITGUID
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

// :root tokens from frontend/src/index.css
const D2D1_COLOR_F kBg = D2D1::ColorF(0x151d2b);
const D2D1_COLOR_F kSurface = D2D1::ColorF(0x1e2938);
const D2D1_COLOR_F kPanel = D2D1::ColorF(0x283649);
const D2D1_COLOR_F kLine = D2D1::ColorF(0x3a4c62);
const D2D1_COLOR_F kLineSoft = D2D1::ColorF(0x2d3c50);
const D2D1_COLOR_F kInk = D2D1::ColorF(0xeef3fa);
const D2D1_COLOR_F kMuted = D2D1::ColorF(0xa8b5c7);
const D2D1_COLOR_F kAccent = D2D1::ColorF(0x5eead4);
const D2D1_COLOR_F kAccentStrong = D2D1::ColorF(0x2dd4bf);
const D2D1_COLOR_F kAccentSoft = D2D1::ColorF(0x5eead4, 0.10f);
const D2D1_COLOR_F kOnAccent = D2D1::ColorF(0x052e28);

float g_dpi = 96.0f;

struct app_state {
  ID2D1Factory* factory = nullptr;
  IDWriteFactory* write = nullptr;
  ID2D1HwndRenderTarget* target = nullptr;
  IDWriteTextFormat* small = nullptr;
  IDWriteTextFormat* small_left = nullptr;
  IDWriteTextFormat* body = nullptr;
  IDWriteTextFormat* title = nullptr;

  bool panel_open = true;
  int preset = 2;
  int rate_mode = 0;
  int format_index = 2;
  float quality = 0.72f;
  float bitrate = 0.35f;
  float fps = 0.5f;
  float hover_x = -1, hover_y = -1;

  D2D1_RECT_F quality_button{};
  D2D1_RECT_F chips[7]{};
  D2D1_RECT_F modes[3]{};
  D2D1_RECT_F sliders[3]{};
  D2D1_RECT_F format_button{};
};

app_state g;

const wchar_t* kPresets[] = {L"极速", L"流畅", L"平衡", L"清晰",
                             L"超清", L"无损", L"自适应"};
const wchar_t* kModes[] = {L"自动", L"恒定质量", L"恒定码率"};
const wchar_t* kFormats[] = {L"跟随档位", L"黑白 8 位", L"16 位 RGB565",
                             L"真彩 BGRA"};

bool inside(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void fill_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                const D2D1_COLOR_F& color) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
  b->Release();
}

void stroke_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                  const D2D1_COLOR_F& color, float width = 1.0f) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
  b->Release();
}

void drop_shadow(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius) {
  const float alphas[] = {0.22f, 0.13f, 0.07f};
  for (int i = 0; i < 3; ++i) {
    const float spread = 1.0f + i * 3.0f;
    const float dy = 3.0f + i * 4.0f;
    fill_round(t,
               D2D1::RectF(r.left - spread, r.top - spread + dy,
                           r.right + spread, r.bottom + spread + dy),
               radius + spread, D2D1::ColorF(0, 0, 0, alphas[i]));
  }
}

void text(ID2D1RenderTarget* t, IDWriteTextFormat* f, const std::wstring& s,
          const D2D1_RECT_F& r, const D2D1_COLOR_F& color) {
  ID2D1SolidColorBrush* b = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &b)) || b == nullptr) return;
  t->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, b,
               D2D1_DRAW_TEXT_OPTIONS_CLIP);
  b->Release();
}

void pill(ID2D1RenderTarget* t, const D2D1_RECT_F& r, const std::wstring& s,
          bool accent, bool hovered) {
  const D2D1_COLOR_F fill = accent ? kAccentSoft
                                   : (hovered ? kPanel : kPanel);
  fill_round(t, r, 8.0f, fill);
  stroke_round(t, r, 8.0f, accent ? D2D1::ColorF(0x5eead4, 0.55f) : kLineSoft);
  text(t, g.small, s, r, accent ? kAccent : kMuted);
}

void slider(ID2D1RenderTarget* t, const D2D1_RECT_F& r, const std::wstring& label,
            const std::wstring& value, float fraction) {
  text(t, g.small_left, label,
       D2D1::RectF(r.left, r.top, r.left + 56, r.bottom), kMuted);
  const float track_left = r.left + 64;
  const float track_right = r.right - 92;
  const float y = (r.top + r.bottom) / 2;
  fill_round(t, D2D1::RectF(track_left, y - 2, track_right, y + 2), 2, kLineSoft);
  const float knob = track_left + (track_right - track_left) * fraction;
  fill_round(t, D2D1::RectF(track_left, y - 2, knob, y + 2), 2, kAccentStrong);
  fill_round(t, D2D1::RectF(knob - 6, y - 6, knob + 6, y + 6), 6, kInk);
  text(t, g.small_left, value,
       D2D1::RectF(track_right + 12, r.top, r.right, r.bottom), kInk);
}

void paint(HWND window) {
  if (g.target == nullptr) return;
  const D2D1_SIZE_F size = g.target->GetSize();
  const float w = size.width;
  g.target->BeginDraw();
  g.target->Clear(kBg);

  // Background glow, like the web's --glow-1/--glow-2.
  {
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(g.target->CreateSolidColorBrush(
            D2D1::ColorF(0x5eead4, 0.05f), &b)) &&
        b != nullptr) {
      g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(120, 0), 320, 220), b);
      b->Release();
    }
    if (SUCCEEDED(g.target->CreateSolidColorBrush(
            D2D1::ColorF(0x60a5fa, 0.04f), &b)) &&
        b != nullptr) {
      g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w - 80, 40), 360, 240),
                            b);
      b->Release();
    }
  }

  // Floating toolbar: h-10 style bar with 12px radius.
  const D2D1_RECT_F bar = D2D1::RectF(16, 14, w - 16, 58);
  drop_shadow(g.target, bar, 12);
  fill_round(g.target, bar, 12, kSurface);
  stroke_round(g.target, bar, 12, kLine);

  // Logo.
  fill_round(g.target, D2D1::RectF(30, 28, 42, 40), 5, kAccentStrong);
  text(g.target, g.body, L"moLink",
       D2D1::RectF(50, 26, 112, 42), kInk);
  {
    ID2D1SolidColorBrush* divider = nullptr;
    if (SUCCEEDED(g.target->CreateSolidColorBrush(kLineSoft, &divider)) &&
        divider != nullptr) {
      g.target->FillRectangle(D2D1::RectF(120, 26, 121, 46), divider);
      divider->Release();
    }
  }

  // Device pill: status dot + left-aligned name.
  const D2D1_RECT_F device = D2D1::RectF(132, 23, 288, 49);
  fill_round(g.target, device, 8, kPanel);
  stroke_round(g.target, device, 8, kLineSoft);
  fill_round(g.target, D2D1::RectF(144, 33, 152, 41), 4, kAccentStrong);
  text(g.target, g.small_left, L"INTERNET-REMOTE",
       D2D1::RectF(160, 23, 280, 49), kMuted);

  // Right cluster, measured from the right edge.
  float x = w - 16 - 10;
  auto chip = [&](const std::wstring& label, bool accent, float width) {
    D2D1_RECT_F r = D2D1::RectF(x - width, 23, x, 49);
    pill(g.target, r, label, accent, inside(r, g.hover_x, g.hover_y));
    x -= width + 8;
    return r;
  };
  chip(L"×", false, 28);
  chip(L"全屏", false, 52);
  chip(L"剪贴板", false, 64);
  chip(L"1.2 Mbps", false, 74);
  chip(L"12 ms", false, 52);
  chip(L"VP9", true, 44);
  g.quality_button = chip(std::wstring(L"画质 · ") + kPresets[g.preset],
                          g.panel_open, 112);

  if (g.panel_open) {
    const D2D1_RECT_F panel = D2D1::RectF(16, 68, 544, 408);
    drop_shadow(g.target, panel, 14);
    fill_round(g.target, panel, 14, kSurface);
    stroke_round(g.target, panel, 14, kLine);

    text(g.target, g.title, L"画质", D2D1::RectF(32, 82, 160, 104), kInk);

    // Preset chips: 4 columns, 28px tall, 8px gaps.
    const float chip_w = 116;
    const float chip_h = 28;
    for (int i = 0; i < 7; ++i) {
      const float cx = 32 + (i % 4) * (chip_w + 8);
      const float cy = 112 + (i / 4) * (chip_h + 8);
      g.chips[i] = D2D1::RectF(cx, cy, cx + chip_w, cy + chip_h);
      pill(g.target, g.chips[i], kPresets[i], g.preset == i,
           inside(g.chips[i], g.hover_x, g.hover_y));
    }

    // Rate mode.
    text(g.target, g.small_left, L"速率模式",
         D2D1::RectF(32, 202, 100, 224), kMuted);
    float mx = 92;
    const float widths[3] = {56, 84, 84};
    for (int i = 0; i < 3; ++i) {
      g.modes[i] = D2D1::RectF(mx, 200, mx + widths[i], 226);
      pill(g.target, g.modes[i], kModes[i], g.rate_mode == i,
           inside(g.modes[i], g.hover_x, g.hover_y));
      mx += widths[i] + 6;
    }

    // Sliders.
    wchar_t buffer[3][32] = {};
    swprintf(buffer[0], 32, L"%d", static_cast<int>(g.quality * 63));
    swprintf(buffer[1], 32, L"%d kbps", static_cast<int>(g.bitrate * 12000));
    swprintf(buffer[2], 32, L"%d fps", 1 + static_cast<int>(g.fps * 59));
    const wchar_t* labels[3] = {L"质量", L"码率", L"帧率"};
    const float values[3] = {g.quality, g.bitrate, g.fps};
    for (int i = 0; i < 3; ++i) {
      g.sliders[i] = D2D1::RectF(32, 244 + i * 34, 528, 266 + i * 34);
      slider(g.target, g.sliders[i], labels[i], buffer[i], values[i]);
    }

    // Format row: a select-looking button.
    text(g.target, g.small_left, L"图像", D2D1::RectF(32, 360, 90, 382), kMuted);
    g.format_button = D2D1::RectF(92, 356, 528, 384);
    fill_round(g.target, g.format_button, 8, kPanel);
    stroke_round(g.target, g.format_button, 8, kLineSoft);
    text(g.target, g.small_left, kFormats[g.format_index],
         D2D1::RectF(104, 356, 500, 384), kInk);
    {
      ID2D1SolidColorBrush* chevron = nullptr;
      if (SUCCEEDED(g.target->CreateSolidColorBrush(kMuted, &chevron)) &&
          chevron != nullptr) {
        g.target->DrawLine(D2D1::Point2F(500, 366), D2D1::Point2F(505, 372),
                           chevron, 1.4f);
        g.target->DrawLine(D2D1::Point2F(505, 372), D2D1::Point2F(510, 366),
                           chevron, 1.4f);
        chevron->Release();
      }
    }
  }

  g.target->EndDraw();
}

bool create_resources(HWND window) {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.factory))) {
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_IDWriteFactory,
                                 reinterpret_cast<IUnknown**>(&g.write)))) {
    return false;
  }
  g_dpi = static_cast<float>(GetDpiForWindow(window));
  if (g_dpi <= 0) g_dpi = 96.0f;
  auto make = [](float size, DWRITE_FONT_WEIGHT weight, bool centered,
                 IDWriteTextFormat** out) {
    g.write->CreateTextFormat(L"Segoe UI", nullptr, weight,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", out);
    if (*out != nullptr) {
      (*out)->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER
                                        : DWRITE_TEXT_ALIGNMENT_LEADING);
      (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
  };
  make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, true, &g.small);
  make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, false, &g.small_left);
  make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, &g.body);
  make(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, &g.title);

  RECT client{};
  GetClientRect(window, &client);
  g.factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(
          window, D2D1::SizeU(client.right - client.left,
                              client.bottom - client.top)),
      &g.target);
  if (g.target == nullptr) return false;
  g.target->SetDpi(g_dpi, g_dpi);
  return true;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      return create_resources(window) ? 0 : -1;
    case WM_SIZE: {
      RECT client{};
      GetClientRect(window, &client);
      if (g.target != nullptr) {
        g.target->Resize(D2D1::SizeU(client.right - client.left,
                                     client.bottom - client.top));
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(window, &ps);
      paint(window);
      EndPaint(window, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      g.hover_x = static_cast<float>(LOWORD(lparam)) * 96.0f / g_dpi;
      g.hover_y = static_cast<float>(HIWORD(lparam)) * 96.0f / g_dpi;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSELEAVE:
      g.hover_x = g.hover_y = -1;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      const float x = static_cast<float>(LOWORD(lparam)) * 96.0f / g_dpi;
      const float y = static_cast<float>(HIWORD(lparam)) * 96.0f / g_dpi;
      if (inside(g.quality_button, x, y)) {
        g.panel_open = !g.panel_open;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      for (int i = 0; i < 7; ++i) {
        if (inside(g.chips[i], x, y)) {
          g.preset = i;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      for (int i = 0; i < 3; ++i) {
        if (inside(g.modes[i], x, y)) {
          g.rate_mode = i;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F& row = g.sliders[i];
        const float left = row.left + 64;
        const float right = row.right - 92;
        if (x >= left - 8 && x <= right + 8 && y >= row.top &&
            y <= row.bottom) {
          const float value = (std::max)(0.0f,
                                         (std::min)(1.0f,
                                                    (x - left) / (right - left)));
          if (i == 0) g.quality = value;
          if (i == 1) g.bitrate = value;
          if (i == 2) g.fps = value;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      if (inside(g.format_button, x, y)) {
        g.format_index = (g.format_index + 1) % 4;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  cls.lpszClassName = L"moLinkNativeUiProto";
  RegisterClassExW(&cls);
  HWND window = CreateWindowExW(
      0, cls.lpszClassName, L"moLink \x2014 native UI prototype",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 60, 1120, 470, nullptr, nullptr,
      instance, nullptr);
  if (window == nullptr) return 1;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
