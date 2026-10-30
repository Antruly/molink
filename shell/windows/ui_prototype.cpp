// Native UI prototype for the moLink shell: a Direct2D toolbar plus the
// quality panel, drawn without any web engine. Standalone exe for visual
// comparison against the React UI; nothing here touches the session yet.
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
#include <string>
#include <vector>

namespace {

// Design tokens shared with the web UI.
const D2D1_COLOR_F kBackground = D2D1::ColorF(0x0b1020);
const D2D1_COLOR_F kSurface = D2D1::ColorF(0x141a2b);
const D2D1_COLOR_F kSurfaceRaised = D2D1::ColorF(0x1b2336);
const D2D1_COLOR_F kBorder = D2D1::ColorF(0x273149);
const D2D1_COLOR_F kText = D2D1::ColorF(0xe7ecf7);
const D2D1_COLOR_F kMuted = D2D1::ColorF(0x8d99b3);
const D2D1_COLOR_F kAccent = D2D1::ColorF(0x34d399);
const D2D1_COLOR_F kTrack = D2D1::ColorF(0x2a3550);

struct app_state {
  ID2D1Factory* factory = nullptr;
  IDWriteFactory* write = nullptr;
  IDWriteTextFormat* body = nullptr;
  IDWriteTextFormat* small = nullptr;
  IDWriteTextFormat* title = nullptr;
  ID2D1HwndRenderTarget* target = nullptr;

  bool panel_open = true;
  int preset = 2;       // balanced
  int rate_mode = 0;    // auto
  float quality = 0.72f;
  float bitrate = 0.35f;
  float fps = 0.5f;
  int format_index = 0;

  D2D1_RECT_F quality_button{};
  D2D1_RECT_F chips[7]{};
  D2D1_RECT_F modes[3]{};
  D2D1_RECT_F sliders[3]{};
  bool hover_quality = false;
};

app_state g;

const wchar_t* kPresets[] = {L"极速", L"流畅", L"平衡", L"清晰",
                             L"超清", L"无损", L"自适应"};
const wchar_t* kModes[] = {L"自动", L"恒定质量", L"恒定码率"};
const wchar_t* kFormats[] = {L"跟随档位", L"黑白 8 位", L"16 位 RGB565",
                             L"真彩 BGRA"};

float scale_for(HWND window) {
  return static_cast<float>(GetDpiForWindow != nullptr
                                ? GetDpiForWindow(window)
                                : USER_DEFAULT_SCREEN_DPI) /
         96.0f;
}

void fill_round(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                const D2D1_COLOR_F& color) {
  ID2D1SolidColorBrush* brush = nullptr;
  target->CreateSolidColorBrush(color, &brush);
  if (brush == nullptr) return;
  target->FillRoundedRectangle(
      D2D1::RoundedRect(rect, radius, radius), brush);
  brush->Release();
}

void stroke_round(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                  float radius, const D2D1_COLOR_F& color, float width = 1.0f) {
  ID2D1SolidColorBrush* brush = nullptr;
  target->CreateSolidColorBrush(color, &brush);
  if (brush == nullptr) return;
  target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush,
                               width);
  brush->Release();
}

void draw_text(ID2D1RenderTarget* target, IDWriteTextFormat* format,
               const std::wstring& text, const D2D1_RECT_F& rect,
               const D2D1_COLOR_F& color) {
  ID2D1SolidColorBrush* brush = nullptr;
  target->CreateSolidColorBrush(color, &brush);
  if (brush == nullptr) return;
  target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                    rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  brush->Release();
}

void draw_pill(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
               const std::wstring& text, bool accent, float scale) {
  fill_round(target, rect, 7.0f * scale,
             accent ? D2D1::ColorF(0x34d399, 0.16f) : kSurfaceRaised);
  stroke_round(target, rect, 7.0f * scale,
               accent ? D2D1::ColorF(0x34d399, 0.55f) : kBorder);
  draw_text(target, g.small, text,
            D2D1::RectF(rect.left, rect.top + 5.0f * scale, rect.right,
                        rect.bottom),
            accent ? kAccent : kMuted);
}

void draw_slider(ID2D1RenderTarget* target, const D2D1_RECT_F& row,
                 const std::wstring& label, const std::wstring& value,
                 float fraction, float scale) {
  draw_text(target, g.small, label,
            D2D1::RectF(row.left, row.top, row.left + 120 * scale, row.bottom),
            kMuted);
  const float track_left = row.left + 72 * scale;
  const float track_right = row.right - 96 * scale;
  const float y = (row.top + row.bottom) / 2;
  fill_round(target,
             D2D1::RectF(track_left, y - 2 * scale, track_right, y + 2 * scale),
             2 * scale, kTrack);
  const float knob_x = track_left + (track_right - track_left) * fraction;
  fill_round(target,
             D2D1::RectF(track_left, y - 2 * scale, knob_x, y + 2 * scale),
             2 * scale, kAccent);
  fill_round(target,
             D2D1::RectF(knob_x - 6 * scale, y - 6 * scale, knob_x + 6 * scale,
                         y + 6 * scale),
             6 * scale, D2D1::ColorF(0xffffff));
  draw_text(target, g.small, value,
            D2D1::RectF(track_right + 10 * scale, row.top, row.right, row.bottom),
            kText);
}

void paint(HWND window) {
  if (g.target == nullptr) return;
  const float width = g.target->GetSize().width;
  const float scale = scale_for(window);
  g.target->BeginDraw();
  g.target->Clear(kBackground);

  // The floating toolbar, matching the web quick bar.
  const D2D1_RECT_F bar =
      D2D1::RectF(24 * scale, 20 * scale, width - 24 * scale, 68 * scale);
  fill_round(g.target, bar, 10 * scale, kSurface);
  stroke_round(g.target, bar, 10 * scale, kBorder);

  // Logo dot + name.
  fill_round(g.target,
             D2D1::RectF(38 * scale, 36 * scale, 54 * scale, 52 * scale),
             8 * scale, kAccent);
  draw_text(g.target, g.body, L"moLink",
            D2D1::RectF(62 * scale, 34 * scale, 140 * scale, 54 * scale), kText);

  // Device pill.
  draw_pill(g.target,
            D2D1::RectF(150 * scale, 36 * scale, 320 * scale, 58 * scale),
            L"INTERNET-REMOTE", false, scale);

  // Right cluster.
  float x = width - 24 * scale - 40 * scale;
  draw_pill(g.target, D2D1::RectF(x - 36 * scale, 36 * scale, x, 58 * scale),
            L"×", false, scale);
  x -= 46 * scale;
  draw_pill(g.target, D2D1::RectF(x - 64 * scale, 36 * scale, x, 58 * scale),
            L"全屏", false, scale);
  x -= 74 * scale;
  draw_pill(g.target, D2D1::RectF(x - 76 * scale, 36 * scale, x, 58 * scale),
            L"剪贴板", false, scale);
  x -= 86 * scale;
  draw_pill(g.target, D2D1::RectF(x - 92 * scale, 36 * scale, x, 58 * scale),
            L"1.2 Mbps", false, scale);
  x -= 102 * scale;
  draw_pill(g.target, D2D1::RectF(x - 62 * scale, 36 * scale, x, 58 * scale),
            L"12 ms", false, scale);
  x -= 72 * scale;
  draw_pill(g.target, D2D1::RectF(x - 56 * scale, 36 * scale, x, 58 * scale),
            L"VP9", true, scale);
  x -= 66 * scale;
  g.quality_button =
      D2D1::RectF(x - 128 * scale, 36 * scale, x, 58 * scale);
  draw_pill(g.target, g.quality_button,
            std::wstring(L"画质 · ") + kPresets[g.preset],
            g.panel_open || g.hover_quality, scale);
  if (g.hover_quality) {
    stroke_round(g.target, g.quality_button, 7 * scale, kAccent, 1.5f * scale);
  }

  // Quality panel.
  if (g.panel_open) {
    const D2D1_RECT_F panel =
        D2D1::RectF(24 * scale, 82 * scale, 560 * scale, 400 * scale);
    fill_round(g.target, panel, 12 * scale, kSurface);
    stroke_round(g.target, panel, 12 * scale, kBorder);
    draw_text(g.target, g.title, L"画质",
              D2D1::RectF(44 * scale, 98 * scale, 200 * scale, 124 * scale),
              kText);

    // Preset chips, four per row.
    const float chip_w = 112 * scale;
    const float chip_h = 28 * scale;
    for (int i = 0; i < 7; ++i) {
      const int row = i / 4;
      const int column = i % 4;
      g.chips[i] = D2D1::RectF(44 * scale + column * (chip_w + 10 * scale),
                               134 * scale + row * (chip_h + 10 * scale),
                               44 * scale + column * (chip_w + 10 * scale) +
                                   chip_w,
                               134 * scale + row * (chip_h + 10 * scale) +
                                   chip_h);
      fill_round(g.target, g.chips[i], 8 * scale,
                 g.preset == i ? D2D1::ColorF(0x34d399, 0.18f)
                               : kSurfaceRaised);
      stroke_round(g.target, g.chips[i], 8 * scale,
                   g.preset == i ? kAccent : kBorder);
      draw_text(g.target, g.small, kPresets[i],
                D2D1::RectF(g.chips[i].left, g.chips[i].top + 6 * scale,
                            g.chips[i].right, g.chips[i].bottom),
                g.preset == i ? kAccent : kMuted);
    }

    // Rate mode segmented control.
    draw_text(g.target, g.small, L"速率模式",
              D2D1::RectF(44 * scale, 238 * scale, 160 * scale, 264 * scale),
              kMuted);
    float segment_x = 130 * scale;
    for (int i = 0; i < 3; ++i) {
      const float w = (i == 1 ? 96.0f : 64.0f) * scale;
      g.modes[i] =
          D2D1::RectF(segment_x, 236 * scale, segment_x + w, 262 * scale);
      fill_round(g.target, g.modes[i], 7 * scale,
                 g.rate_mode == i ? D2D1::ColorF(0x34d399, 0.18f)
                                  : kSurfaceRaised);
      stroke_round(g.target, g.modes[i], 7 * scale,
                   g.rate_mode == i ? kAccent : kBorder);
      draw_text(g.target, g.small, kModes[i],
                D2D1::RectF(g.modes[i].left, g.modes[i].top + 5 * scale,
                            g.modes[i].right, g.modes[i].bottom),
                g.rate_mode == i ? kAccent : kMuted);
      segment_x += w + 8 * scale;
    }

    // Sliders.
    const wchar_t* labels[] = {L"质量", L"码率", L"帧率"};
    const float values[] = {g.quality, g.bitrate, g.fps};
    wchar_t buffer[3][32] = {};
    swprintf(buffer[0], 32, L"%d", static_cast<int>(g.quality * 63));
    swprintf(buffer[1], 32, L"%d kbps", static_cast<int>(g.bitrate * 12000));
    swprintf(buffer[2], 32, L"%d fps", 1 + static_cast<int>(g.fps * 59));
    for (int i = 0; i < 3; ++i) {
      g.sliders[i] = D2D1::RectF(44 * scale, 286 * scale + i * 32 * scale,
                                 540 * scale, 310 * scale + i * 32 * scale);
      draw_slider(g.target, g.sliders[i], labels[i], buffer[i], values[i],
                  scale);
    }

    // Format row.
    draw_text(g.target, g.small, L"图像",
              D2D1::RectF(44 * scale, 384 * scale, 160 * scale, 408 * scale),
              kMuted);
    draw_text(g.target, g.small, kFormats[g.format_index],
              D2D1::RectF(130 * scale, 384 * scale, 300 * scale, 408 * scale),
              kText);
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
  const float scale = scale_for(window);
  g.write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            13.0f * scale, L"zh-CN", &g.body);
  g.write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            11.0f * scale, L"zh-CN", &g.small);
  g.write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            14.0f * scale, L"zh-CN", &g.title);
  RECT client{};
  GetClientRect(window, &client);
  g.factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(
          window, D2D1::SizeU(client.right - client.left,
                              client.bottom - client.top)),
      &g.target);
  return g.target != nullptr;
}

void update_hover(HWND window, float x, float y, bool inside) {
  const float scale = scale_for(window);
  const D2D1_POINT_2F point = D2D1::Point2F(x, y);
  const bool over_quality =
      x >= g.quality_button.left && x <= g.quality_button.right &&
      y >= g.quality_button.top && y <= g.quality_button.bottom;
  if (over_quality != g.hover_quality) {
    g.hover_quality = over_quality;
    InvalidateRect(window, nullptr, FALSE);
  }
  (void)point;
  (void)inside;
  (void)scale;
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
    case WM_MOUSEMOVE:
      update_hover(window, static_cast<float>(LOWORD(lparam)),
                   static_cast<float>(HIWORD(lparam)), true);
      return 0;
    case WM_LBUTTONDOWN: {
      const float x = static_cast<float>(LOWORD(lparam));
      const float y = static_cast<float>(HIWORD(lparam));
      const D2D1_POINT_2F point = D2D1::Point2F(x, y);
      if (x >= g.quality_button.left && x <= g.quality_button.right &&
          y >= g.quality_button.top && y <= g.quality_button.bottom) {
        g.panel_open = !g.panel_open;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      for (int i = 0; i < 7; ++i) {
        const D2D1_RECT_F& chip = g.chips[i];
        if (x >= chip.left && x <= chip.right && y >= chip.top &&
            y <= chip.bottom) {
          g.preset = i;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F& mode = g.modes[i];
        if (x >= mode.left && x <= mode.right && y >= mode.top &&
            y <= mode.bottom) {
          g.rate_mode = i;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F& row = g.sliders[i];
        const float track_left = row.left + 72 * scale_for(window);
        const float track_right = row.right - 96 * scale_for(window);
        if (x >= track_left - 8 && x <= track_right + 8 && y >= row.top &&
            y <= row.bottom) {
          const float value =
              (std::max)(0.0f, (std::min)(1.0f, (x - track_left) /
                                                    (track_right - track_left)));
          if (i == 0) g.quality = value;
          if (i == 1) g.bitrate = value;
          if (i == 2) g.fps = value;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      (void)point;
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
  HWND window = CreateWindowExW(0, cls.lpszClassName,
                                L"moLink \x2014 native UI prototype",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 60, 1200,
                                470, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) return 1;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
