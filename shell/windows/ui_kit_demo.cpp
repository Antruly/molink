// Native UI kit demo: themes (dark/light), stroke icons, animated floating
// toolbar, dropdown, tabs and a settings sheet, all Direct2D/DirectWrite with
// no web engine. This file is the seed of the reusable toolkit.
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
#include <vector>

#include "ui_kit.h"

using namespace molink::ui;

namespace {

struct app_state {
  ID2D1Factory* factory = nullptr;
  IDWriteFactory* write = nullptr;
  ID2D1HwndRenderTarget* target = nullptr;
  IDWriteTextFormat* small = nullptr;
  IDWriteTextFormat* small_left = nullptr;
  IDWriteTextFormat* body = nullptr;
  IDWriteTextFormat* title = nullptr;

  enum class theme_mode { automatic, light, dark };
  theme_mode mode = theme_mode::automatic;
  const palette* theme = &kDark;
  int tab = 0;                 // 0 desktop, 1 files, 2 settings
  bool collapsed = false;      // floating toolbar collapsed
  float collapse_anim = 0.0f;  // 0 expanded, 1 collapsed
  dropdown_state dropdown;
  int format_index = 2;
  int preset = 2;
  float hover_x = -1, hover_y = -1;

  D2D1_RECT_F hit_collapse{}, hit_theme[3]{}, hit_tab[3]{}, hit_dropdown{},
      hit_dropdown_item[4]{}, hit_settings{}, hit_modal_close{};
};

app_state g;

const wchar_t* kPresets[] = {L"极速", L"流畅", L"平衡", L"清晰",
                             L"超清", L"无损", L"自适应"};
const wchar_t* kFormats[] = {L"跟随档位", L"黑白 8 位", L"16 位 RGB565",
                             L"真彩 BGRA"};
const wchar_t* kTabs[] = {L"远程桌面", L"文件", L"设置"};

float g_dpi = 96.0f;

bool inside(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

/// Automatic follows the Windows app theme (Settings > Personalisation).
const palette* resolve_theme() {
  if (g.mode == app_state::theme_mode::light) return &kLight;
  if (g.mode == app_state::theme_mode::dark) return &kDark;
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (status == ERROR_SUCCESS) return value != 0 ? &kLight : &kDark;
  return &kDark;
}

void paint(HWND window) {
  g.theme = resolve_theme();
  if (g.target == nullptr) return;
  const D2D1_SIZE_F size = g.target->GetSize();
  const float w = size.width;
  const float h = size.height;
  g.target->BeginDraw();
  g.target->Clear(g.theme->bg);

  // Tab strip.
  {
    const D2D1_RECT_F strip = D2D1::RectF(16, 12, w - 16, 52);
    fill_round(g.target, strip, 10, g.theme->surface);
    stroke_round(g.target, strip, 10, g.theme->line);
    static const std::vector<std::wstring> tabs = {L"远程桌面", L"文件",
                                                   L"设置"};
    tabs_draw(g.target, *g.theme, g.small, strip, tabs, g.tab, g.hover_x,
              g.hover_y);
    // Theme: 自动 / 亮色 / 暗色, then the close button.
    {
      const wchar_t* labels[3] = {L"自动", L"亮色", L"暗色"};
      float mx = w - 56 - 12 - (3 * 48 + 2 * 6);
      for (int i = 0; i < 3; ++i) {
        g.hit_theme[i] = D2D1::RectF(mx, 19, mx + 48, 45);
        pill(g.target, *g.theme, g.small, g.small_left, g.hit_theme[i], labels[i],
             static_cast<int>(g.mode) == i,
             inside(g.hit_theme[i], g.hover_x, g.hover_y), icon::none);
        mx += 48 + 6;
      }
      const D2D1_RECT_F close = D2D1::RectF(w - 56, 19, w - 30, 45);
      icon_button(g.target, *g.theme, close, icon::close,
                  inside(close, g.hover_x, g.hover_y));
      g.hit_modal_close = close;
    }
  }

  if (g.tab == 2) {
    // Settings as a modal: scrim + panel from the kit, rows drawn in content.
    modal_scrim(g.target, size);
    D2D1_RECT_F close{};
    const D2D1_RECT_F content = modal_panel(
        g.target, *g.theme, g.title, size, 600, 340, L"设置", &close);
    g.hit_modal_close = close;
    icon_button(g.target, *g.theme, close, icon::close,
                inside(close, g.hover_x, g.hover_y));
    const wchar_t* rows[] = {L"开机自启", L"剪贴板同步", L"文件传输",
                             L"声音转发", L"硬件加速"};
    for (int i = 0; i < 5; ++i) {
      const float y = content.top + i * 44;
      text(g.target, g.small_left, rows[i],
           D2D1::RectF(content.left, y, content.right - 90, y + 24),
           g.theme->ink);
      toggle(g.target, *g.theme,
             D2D1::RectF(content.right - 68, y, content.right - 24, y + 22),
             i < 3);
    }
    g.target->EndDraw();
    return;
  }

  // Floating toolbar: collapses to a compact pill as the animation runs.
  const float collapse = ease_out(g.collapse_anim);
  const float expanded_right = w - 16;
  const float collapsed_right = 236;
  const D2D1_RECT_F bar =
      D2D1::RectF(16, 64,
                  expanded_right +
                      (collapsed_right - expanded_right) * collapse,
                  108);
  shadow(g.target, bar, 12);
  fill_round(g.target, bar, 12, g.theme->surface);
  stroke_round(g.target, bar, 12, g.theme->line);
  fill_round(g.target, D2D1::RectF(30, 78, 42, 90), 5, g.theme->accent_strong);
  text(g.target, g.body, L"moLink", D2D1::RectF(50, 74, 112, 94),
       g.theme->ink);
  if (collapse < 0.99f) {
    const D2D1_RECT_F device = D2D1::RectF(132, 73, 288, 99);
    fill_round(g.target, device, 8, g.theme->panel);
    stroke_round(g.target, device, 8, g.theme->line_soft);
    fill_round(g.target, D2D1::RectF(144, 83, 152, 91), 4,
               g.theme->accent_strong);
    text(g.target, g.small_left, L"INTERNET-REMOTE",
         D2D1::RectF(160, 73, 280, 99), g.theme->muted);
  }
  // Collapse chevron.
  const D2D1_RECT_F collapse_button =
      D2D1::RectF(bar.right - 34, bar.top + 8, bar.right - 10, bar.bottom - 8);
  g.hit_collapse = collapse_button;
  icon_button(g.target, *g.theme, collapse_button,
              g.collapsed ? icon::chevron_down : icon::close,
              inside(collapse_button, g.hover_x, g.hover_y));
  if (collapse < 0.99f) {
    float x = bar.right - 44;
    auto chip = [&](const std::wstring& label, bool accent, float width) {
      const D2D1_RECT_F r = D2D1::RectF(x - width, 73, x, 99);
      pill(g.target, *g.theme, g.small, g.small_left, r, label, accent, inside(r, g.hover_x, g.hover_y),
           icon::none);
      x -= width + 8;
    };
    chip(L"1.2 Mbps", false, 74);
    chip(L"12 ms", false, 52);
    chip(L"VP9", true, 44);
    chip(std::wstring(L"画质 · ") + kPresets[g.preset], true, 112);
  }

  // Content card: one vertical flow, so rows cannot overlap.
  float y = 120;
  const D2D1_RECT_F card = D2D1::RectF(16, y, 544, 470);
  shadow(g.target, card, 14);
  fill_round(g.target, card, 14, g.theme->surface);
  stroke_round(g.target, card, 14, g.theme->line);

  y += 14;
  text(g.target, g.title, L"画质", D2D1::RectF(32, y, 160, y + 24),
       g.theme->ink);
  y += 32;

  // Preset chips: 4 columns, two rows.
  for (int i = 0; i < 7; ++i) {
    const float cx = 32 + (i % 4) * (116 + 8);
    const float cy = y + (i / 4) * (28 + 8);
    const D2D1_RECT_F r = D2D1::RectF(cx, cy, cx + 116, cy + 28);
    pill(g.target, *g.theme, g.small, g.small_left, r, kPresets[i], g.preset == i,
         inside(r, g.hover_x, g.hover_y), icon::none);
  }
  y += 2 * 28 + 8;
  y += 16;

  // Rate mode.
  text(g.target, g.small_left, L"速率模式",
       D2D1::RectF(32, y, 110, y + 26), g.theme->muted);
  {
    const wchar_t* modes[3] = {L"自动", L"恒定质量", L"恒定码率"};
    const float widths[3] = {56, 84, 84};
    float mx = 104;
    for (int i = 0; i < 3; ++i) {
      const D2D1_RECT_F r = D2D1::RectF(mx, y, mx + widths[i], y + 26);
      pill(g.target, *g.theme, g.small, g.small_left, r, modes[i], g.preset >= 0 && i == 0,
           inside(r, g.hover_x, g.hover_y), icon::none);
      mx += widths[i] + 6;
    }
  }
  y += 26 + 18;

  // Sliders (ui_kit widgets).
  {
    const wchar_t* labels[3] = {L"质量", L"码率", L"帧率"};
    const float fractions[3] = {0.72f, 0.35f, 0.5f};
    wchar_t values[3][32] = {};
    swprintf(values[0], 32, L"45");
    swprintf(values[1], 32, L"4200 kbps");
    swprintf(values[2], 32, L"30 fps");
    for (int i = 0; i < 3; ++i) {
      slider(g.target, *g.theme, g.small_left,
                 D2D1::RectF(32, y + i * 30, 512, y + i * 30 + 20), labels[i],
                 values[i], fractions[i]);
    }
  }
  y += 3 * 30 + 12;

  // Image format dropdown.
  text(g.target, g.small_left, L"图像", D2D1::RectF(32, y, 90, y + 28),
       g.theme->muted);
  const D2D1_RECT_F format = D2D1::RectF(92, y, 512, y + 28);
  static const std::vector<std::wstring> formats = {
      L"跟随档位", L"黑白 8 位", L"16 位 RGB565", L"真彩 BGRA"};
  g.dropdown.index = g.format_index;
  dropdown_draw(g.target, *g.theme, g.small_left, format, formats, g.dropdown,
                g.hover_x, g.hover_y);
  g.format_index = g.dropdown.index;

  // Placeholder picture area on the right.
  const D2D1_RECT_F stage = D2D1::RectF(560, 120, w - 16, h - 16);
  fill_round(g.target, stage, 14, g.theme->surface);
  stroke_round(g.target, stage, 14, g.theme->line);
  text(g.target, g.small, L"远程画面（共享内存直绘）",
       D2D1::RectF(stage.left, (stage.top + stage.bottom) / 2 - 12,
                   stage.right, (stage.top + stage.bottom) / 2 + 12),
       g.theme->muted);

  g.target->EndDraw();
}

// Frameless window: rounded corners on Win11 via DWM, a region fallback
// everywhere else, and a custom caption strip handled in WM_NCHITTEST.
void apply_rounded(HWND window) {
  using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
  HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
  if (dwm != nullptr) {
    auto set = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (set != nullptr) {
      const DWORD preference = 2;  // DWMWCP_ROUND
      set(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &preference,
          sizeof(preference));
    }
    FreeLibrary(dwm);
  }
  RECT client{};
  GetClientRect(window, &client);
  const float scale = g_dpi / 96.0f;
  const int radius = static_cast<int>(14 * scale);
  HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                   radius * 2, radius * 2);
  SetWindowRgn(window, region, TRUE);
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
      if (!create_resources(window)) return -1;
      apply_rounded(window);
      SetTimer(window, 1, 16, nullptr);
      return 0;
    case WM_NCCALCSIZE:
      if (wparam == TRUE) return 0;  // no native frame
      break;
    case WM_NCHITTEST: {
      const LRESULT base = DefWindowProcW(window, message, wparam, lparam);
      const POINT screen = {LOWORD(lparam), HIWORD(lparam)};
      POINT client = screen;
      ScreenToClient(window, &client);
      RECT rc{};
      GetClientRect(window, &rc);
      const int border = static_cast<int>(6 * g_dpi / 96.0f);
      const bool left = client.x < border;
      const bool right = client.x >= rc.right - border;
      const bool top = client.y < border;
      const bool bottom = client.y >= rc.bottom - border;
      if (left && top) return HTTOPLEFT;
      if (right && top) return HTTOPRIGHT;
      if (left && bottom) return HTBOTTOMLEFT;
      if (right && bottom) return HTBOTTOMRIGHT;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      // The tab strip doubles as the title bar.
      if (client.y < static_cast<int>(52 * g_dpi / 96.0f)) return HTCAPTION;
      return base;
    }
    case WM_TIMER: {
      const float target = g.collapsed ? 1.0f : 0.0f;
      if (std::fabs(g.collapse_anim - target) > 0.001f) {
        const float step = 0.08f;
        if (g.collapse_anim < target)
          g.collapse_anim = (std::min)(target, g.collapse_anim + step);
        else
          g.collapse_anim = (std::max)(target, g.collapse_anim - step);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case WM_SIZE: {
      RECT client{};
      GetClientRect(window, &client);
      if (g.target != nullptr) {
        g.target->Resize(D2D1::SizeU(client.right - client.left,
                                     client.bottom - client.top));
      }
      apply_rounded(window);
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
      if (inside(g.hit_modal_close, x, y)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
      }
      for (int i = 0; i < 3; ++i) {
        if (inside(g.hit_theme[i], x, y)) {
          g.mode = static_cast<app_state::theme_mode>(i);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      {
        static const std::vector<std::wstring> tabs = {L"远程桌面", L"文件",
                                                       L"设置"};
        RECT client{};
        GetClientRect(window, &client);
        const float cw = client.right * 96.0f / g_dpi;
        const D2D1_RECT_F strip = D2D1::RectF(16, 12, cw - 16, 52);
        const int tab = tabs_hit(strip, tabs, x, y);
        if (tab >= 0) {
          g.tab = tab;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      if (inside(g.hit_collapse, x, y)) {
        g.collapsed = !g.collapsed;
        return 0;
      }
      {
        static const std::vector<std::wstring> formats = {
            L"跟随档位", L"黑白 8 位", L"16 位 RGB565", L"真彩 BGRA"};
        const D2D1_RECT_F format = D2D1::RectF(92, 366, 512, 394);
        if (dropdown_click(g.dropdown, format, formats, x, y) >= -1) {
          g.format_index = g.dropdown.index;
          InvalidateRect(window, nullptr, FALSE);
        }
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
  for (int i = 1; i < __argc; ++i) {
    if (wcscmp(__wargv[i], L"--theme") == 0 && i + 1 < __argc) {
      const wchar_t* value = __wargv[++i];
      g.mode = wcscmp(value, L"light") == 0
                   ? app_state::theme_mode::light
                   : (wcscmp(value, L"dark") == 0
                          ? app_state::theme_mode::dark
                          : app_state::theme_mode::automatic);
    }
  }
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  cls.lpszClassName = L"moLinkUiKitDemo";
  RegisterClassExW(&cls);
  HWND window = CreateWindowExW(
      0, cls.lpszClassName, L"moLink \x2014 native UI kit",
      WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE,
      80, 60, 1120, 560, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) return 1;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
