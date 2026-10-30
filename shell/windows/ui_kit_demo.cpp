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
  IDWriteTextFormat* body_center = nullptr;

  enum class theme_mode { automatic, light, dark };
  theme_mode mode = theme_mode::automatic;
  const palette* theme = &kDark;
  int tab = 0;                 // 0 desktop, 1 files, 2 settings
  bool panel_open_quality = true;
  float panel_anim = 1.0f;     // 0 collapsed .. 1 expanded
  bool collapsed = false;      // floating toolbar collapsed
  float collapse_anim = 0.0f;  // 0 expanded, 1 collapsed
  dropdown_state dropdown;
  int format_index = 2;
  int focus_field = 0;  // 0 none, 1 address, 2 code
  bool more_open = false;
  bool connect_pressed = false;
  std::wstring address_text = L"127.0.0.1:28428";
  std::wstring code_text;
  size_t address_caret = 14, code_caret = 0;
  bool caret_on = true;
  ULONGLONG caret_tick = 0;
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
    static const std::vector<std::wstring> tabs = {L"首页", L"远程桌面",
                                                   L"文件", L"设置"};
    static const icon tab_icons[] = {icon::monitor, icon::monitor,
                                     icon::folder, icon::settings};
    for (int i = 0; i < 4; ++i) {
      const D2D1_RECT_F r = tab_rect(strip, i, tabs);
      pill(g.target, *g.theme, g.small, g.small_left, r, tabs[i], g.tab == i,
           inside(r, g.hover_x, g.hover_y), tab_icons[i]);
    }
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

  if (g.tab == 0) {
    // Home laid out from the measured web spec (web_layout_spec.md).
    // Left column: device card 16..395.
    const D2D1_RECT_F card =
        D2D1::RectF(16, 96, 395, g.more_open ? 470 : 380);
    shadow(g.target, card, 14);
    fill_round(g.target, card, 14, g.theme->surface);
    stroke_round(g.target, card, 14, g.theme->line);
    fill_round(g.target, D2D1::RectF(37, 113, 95, 171), 16,
               g.theme->accent_soft);
    draw_icon(g.target, icon::monitor, 66, 142, g.theme->accent);
    // Platform badge at the avatar's bottom-right, like the web card.
    fill_round(g.target, D2D1::RectF(77, 149, 103, 175), 6,
               g.theme->surface);
    stroke_round(g.target, D2D1::RectF(77, 149, 103, 175), 6, g.theme->line);
    text(g.target, g.small, L"win", D2D1::RectF(77, 149, 103, 175),
         g.theme->muted);
    text(g.target, g.title, L"INTERNET-REMOTE",
         D2D1::RectF(107, 113, 300, 137), g.theme->ink);
    pill(g.target, *g.theme, g.small, g.small_left,
         D2D1::RectF(107, 143, 181, 167), L"windows", false, false);
    fill_round(g.target, D2D1::RectF(239, 172, 247, 180), 4,
               g.theme->accent_strong);
    text(g.target, g.small_left, L"ID 服务器已连接",
         D2D1::RectF(249, 168, 380, 192), g.theme->muted);
    text(g.target, g.body, L"BDV-NCK-9DN",
         D2D1::RectF(37, 197, 300, 221), g.theme->ink);
    text(g.target, g.body, L"验证码  ••••••",
         D2D1::RectF(37, 225, 300, 249), g.theme->ink);
    pill(g.target, *g.theme, g.small, g.small_left,
         D2D1::RectF(129, 238, 177, 262), L"单次", true, false);
    pill(g.target, *g.theme, g.small, g.small_left,
         D2D1::RectF(183, 238, 231, 262), L"多次", false, false);
    text(g.target, g.small_left, L"剩余 1 / 1 次",
         D2D1::RectF(37, 270, 300, 290), g.theme->muted);
    const D2D1_RECT_F more = D2D1::RectF(37, 336, 375, 368);
    fill_round(g.target, more, 8,
               inside(more, g.hover_x, g.hover_y) ? g.theme->panel
                                                  : D2D1::ColorF(0, 0, 0, 0));
    text(g.target, g.small_left, g.more_open ? L"收起设置" : L"更多设置",
         D2D1::RectF(45, 336, 330, 368), g.theme->muted);
    draw_icon(g.target, g.more_open ? icon::chevron_down : icon::plus, 356, 352,
              g.theme->muted);
    if (g.more_open) {
      const wchar_t* rows[] = {L"设备名称", L"更新与版本", L"开机自启", L"服务状态"};
      for (int i = 0; i < 4; ++i) {
        const float ry = 380 + i * 22;
        text(g.target, g.small_left, rows[i],
             D2D1::RectF(45, ry, 330, ry + 20), g.theme->ink);
        toggle(g.target, *g.theme,
               D2D1::RectF(340, ry, 364, ry + 20), i != 2);
      }
    }

    // Right column: connect panel 413..1127.
    const D2D1_RECT_F join = D2D1::RectF(413, 96, w - 16, 380);
    shadow(g.target, join, 14);
    fill_round(g.target, join, 14, g.theme->surface);
    stroke_round(g.target, join, 14, g.theme->line);
    text(g.target, g.title, L"连接设备",
         D2D1::RectF(433, 112, 700, 136), g.theme->ink);
    const D2D1_RECT_F address = D2D1::RectF(433, 160, 900, 196);
    fill_round(g.target, address, 9.6f, g.theme->panel);
    stroke_round(g.target, address, 9.6f,
                 g.focus_field == 1 ? g.theme->accent : g.theme->line_soft);
    const std::wstring address_shown =
        g.address_text.empty() ? L"设备 ID 或 IP[:端口]" : g.address_text;
    text(g.target, g.body, address_shown, D2D1::RectF(445, 160, 890, 196),
         g.address_text.empty() ? g.theme->muted : g.theme->ink);
    if (g.focus_field == 1 && g.caret_on) {
      const float caret_x = 445 + 7.5f * static_cast<float>(g.address_caret);
      line(g.target, caret_x, 166, caret_x, 190, g.theme->accent, 1.5f);
    }
    const D2D1_RECT_F code = D2D1::RectF(910, 160, w - 36, 196);
    fill_round(g.target, code, 9.6f, g.theme->panel);
    stroke_round(g.target, code, 9.6f,
                 g.focus_field == 2 ? g.theme->accent : g.theme->line_soft);
    const std::wstring code_shown =
        g.code_text.empty() ? L"验证码" : g.code_text;
    text(g.target, g.body, code_shown, D2D1::RectF(922, 160, w - 48, 196),
         g.code_text.empty() ? g.theme->muted : g.theme->ink);
    if (g.focus_field == 2 && g.caret_on) {
      const float caret_x = 922 + 7.5f * static_cast<float>(g.code_caret);
      line(g.target, caret_x, 166, caret_x, 190, g.theme->accent, 1.5f);
    }
    // Measured: 连接 button spans the panel width, 36 high, radius 9.6.
    const D2D1_RECT_F connect =
        D2D1::RectF(433, 301, w - 36, 337);
    fill_round(g.target, connect, 9.6f,
               g.connect_pressed ? g.theme->accent : g.theme->accent_strong);
    text(g.target, g.body_center, L"连接", connect, g.theme->on_accent);

    // Recent-session filter chips (measured row at y=402).
    const wchar_t* filters[] = {L"最近", L"已保存", L"局域网", L"联网"};
    const float widths[] = {51, 53, 53, 42};
    float fx = 317;
    for (int i = 0; i < 4; ++i) {
      const D2D1_RECT_F r =
          D2D1::RectF(fx, 402, fx + widths[i], 426);
      pill(g.target, *g.theme, g.small, g.small_left, r, filters[i], i == 0,
           inside(r, g.hover_x, g.hover_y));
      fx += widths[i] + 8;
    }
    text(g.target, g.small, L"暂无最近会话",
         D2D1::RectF(433, 460, w - 16, 484), g.theme->muted);
    g.target->EndDraw();
    return;
  }

  if (g.tab == 3) {
    // Settings page: grouped cards, like the web settings route.
    text(g.target, g.title, L"设置", D2D1::RectF(24, 92, 200, 116),
         g.theme->ink);
    const struct {
      const wchar_t* title;
      const wchar_t* rows[4];
      int count;
    } groups[] = {
        {L"外观", {L"主题", L"界面语言", L"启动时打开首页"}, 3},
        {L"连接", {L"开机自启", L"ID 服务器", L"局域网发现"}, 3},
        {L"传输", {L"剪贴板同步", L"文件传输", L"声音转发"}, 3},
    };
    float card_x = 24;
    for (int gi = 0; gi < 3; ++gi) {
      const D2D1_RECT_F card = D2D1::RectF(card_x, 128, card_x + 360, 420);
      shadow(g.target, card, 14);
      fill_round(g.target, card, 14, g.theme->surface);
      stroke_round(g.target, card, 14, g.theme->line);
      text(g.target, g.title, groups[gi].title,
           D2D1::RectF(card.left + 16, card.top + 12, card.right - 16,
                       card.top + 36),
           g.theme->ink);
      for (int i = 0; i < groups[gi].count; ++i) {
        const float ry = card.top + 52 + i * 52;
        text(g.target, g.small_left, groups[gi].rows[i],
             D2D1::RectF(card.left + 16, ry, card.right - 80, ry + 22),
             g.theme->muted);
        if (gi == 0 && i == 0) {
          // Theme switch lives here on the web.
          const wchar_t* labels[3] = {L"自动", L"亮色", L"暗色"};
          float mx = card.left + 16;
          for (int k = 0; k < 3; ++k) {
            g.hit_theme[k] = D2D1::RectF(mx, ry + 22, mx + 56, ry + 48);
            pill(g.target, *g.theme, g.small, g.small_left, g.hit_theme[k],
                 labels[k], static_cast<int>(g.mode) == k,
                 inside(g.hit_theme[k], g.hover_x, g.hover_y), icon::none);
            mx += 62;
          }
        } else {
          toggle(g.target, *g.theme,
                 D2D1::RectF(card.right - 60, ry, card.right - 36, ry + 22),
                 i != 2);
        }
      }
      card_x += 376;
    }
    g.target->EndDraw();
    return;
  }

  // Floating toolbar: collapses to a compact pill as the animation runs.
  const float collapse = ease_out(g.collapse_anim);
  const float expanded_right = w - 16;
  const float collapsed_right = 236;
  // Measured: controls are 28 high with a 9.6 radius; the bar wraps them
  // with 8 px padding, floating at y=107 in the web UI.
  const D2D1_RECT_F bar =
      D2D1::RectF(16, 107,
                  expanded_right +
                      (collapsed_right - expanded_right) * collapse,
                  163);
  shadow(g.target, bar, 12);
  fill_round(g.target, bar, 12, g.theme->surface);
  stroke_round(g.target, bar, 12, g.theme->line);
  fill_round(g.target, D2D1::RectF(30, 121, 42, 133), 5,
             g.theme->accent_strong);
  text(g.target, g.body, L"moLink", D2D1::RectF(50, 117, 112, 137),
       g.theme->ink);
  if (collapse < 0.99f) {
    const D2D1_RECT_F device = D2D1::RectF(132, 115, 288, 143);
    fill_round(g.target, device, 8, g.theme->panel);
    stroke_round(g.target, device, 8, g.theme->line_soft);
    fill_round(g.target, D2D1::RectF(144, 125, 152, 133), 4,
               g.theme->accent_strong);
    text(g.target, g.small_left, L"INTERNET-REMOTE",
         D2D1::RectF(160, 115, 280, 143), g.theme->muted);
  }
  // Collapse chevron.
  const D2D1_RECT_F collapse_button =
      D2D1::RectF(bar.right - 42, bar.top + 8, bar.right - 14, bar.top + 36);
  g.hit_collapse = collapse_button;
  icon_button(g.target, *g.theme, collapse_button,
              g.collapsed ? icon::chevron_down : icon::close,
              inside(collapse_button, g.hover_x, g.hover_y));
  if (collapse < 0.99f) {
    float x = bar.right - 50;
    auto chip = [&](const std::wstring& label, bool accent,
                    float width) -> D2D1_RECT_F {
      const D2D1_RECT_F r = D2D1::RectF(x - width, 115, x, 143);
      pill(g.target, *g.theme, g.small, g.small_left, r, label, accent,
           inside(r, g.hover_x, g.hover_y), icon::none);
      x -= width + 8;
      return r;
    };
    chip(L"1.2 Mbps", false, 74);
    chip(L"12 ms", false, 52);
    chip(L"VP9", true, 42);
    const D2D1_RECT_F quality_chip = chip(kPresets[g.preset], true, 61);
    if (inside(quality_chip, g.hover_x, g.hover_y)) {
      // handled on click below
    }
  }

  // Content card: one vertical flow, so rows cannot overlap. The panel grows
  // and shrinks with panel_anim, keeping the measured 4px grid.
  const float open = ease_out(g.panel_anim);
  if (open <= 0.02f) {
    // Panel tucked away: the picture area takes the whole row.
    const D2D1_RECT_F stage = D2D1::RectF(16, 120, w - 16, h - 16);
    fill_round(g.target, stage, 14, g.theme->surface);
    stroke_round(g.target, stage, 14, g.theme->line);
    text(g.target, g.small, L"远程画面（共享内存直绘）",
         D2D1::RectF(stage.left, (stage.top + stage.bottom) / 2 - 12,
                     stage.right, (stage.top + stage.bottom) / 2 + 12),
         g.theme->muted);
    g.target->EndDraw();
    return;
  }
  float y = 120;
  const float card_h = 350 * open;
  const D2D1_RECT_F card = D2D1::RectF(16, y, 544, y + card_h);
  shadow(g.target, card, 14);
  fill_round(g.target, card, 14, g.theme->surface);
  stroke_round(g.target, card, 14, g.theme->line);

  y += 14;
  text(g.target, g.title, L"画质", D2D1::RectF(32, y, 160, y + 24),
       g.theme->ink);
  y += 32;
  if (open < 0.6f) {
    g.target->EndDraw();
    return;
  }

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
  make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, &g.body_center);
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
      const ULONGLONG now = GetTickCount64();
      if (now - g.caret_tick >= 500) {
        g.caret_tick = now;
        g.caret_on = !g.caret_on;
        if (g.focus_field != 0) InvalidateRect(window, nullptr, FALSE);
      }
      const float panel_target = g.panel_open_quality ? 1.0f : 0.0f;
      if (std::fabs(g.panel_anim - panel_target) > 0.001f) {
        const float step = 0.12f;
        if (g.panel_anim < panel_target)
          g.panel_anim = (std::min)(panel_target, g.panel_anim + step);
        else
          g.panel_anim = (std::max)(panel_target, g.panel_anim - step);
        InvalidateRect(window, nullptr, FALSE);
      }
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
        static const std::vector<std::wstring> tabs = {L"首页", L"远程桌面",
                                                       L"文件", L"设置"};
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
      {
        RECT client{};
        GetClientRect(window, &client);
        const float cw = client.right * 96.0f / g_dpi;
        const D2D1_RECT_F bar = D2D1::RectF(16, 107, cw - 16, 163);
        const D2D1_RECT_F quality_chip =
            D2D1::RectF(bar.right - 50 - 61, 115, bar.right - 50, 143);
        if (inside(quality_chip, x, y)) {
          g.panel_open_quality = !g.panel_open_quality;
          return 0;
        }
      }
      {
        const D2D1_RECT_F address = D2D1::RectF(433, 160, 900, 196);
        const D2D1_RECT_F code = D2D1::RectF(910, 160, 1100, 196);
        if (inside(address, x, y)) {
          g.focus_field = 1;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(code, x, y)) {
          g.focus_field = 2;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      {
        const D2D1_RECT_F connect = D2D1::RectF(433, 301, 1130, 337);
        if (inside(connect, x, y)) {
          g.connect_pressed = true;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      {
        const D2D1_RECT_F more = D2D1::RectF(37, 336, 375, 368);
        if (inside(more, x, y)) {
          g.more_open = !g.more_open;
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
    case WM_CHAR: {
      const wchar_t ch = static_cast<wchar_t>(wparam);
      if (g.focus_field != 0 && ch >= 32 && ch != 127) {
        std::wstring& target =
            g.focus_field == 1 ? g.address_text : g.code_text;
        size_t& caret = g.focus_field == 1 ? g.address_caret : g.code_caret;
        if (target.size() < 64) {
          target.insert(target.begin() + static_cast<long>(caret), ch);
          ++caret;
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      break;
    }
    case WM_KEYDOWN: {
      if (g.focus_field == 0) break;
      std::wstring& target =
          g.focus_field == 1 ? g.address_text : g.code_text;
      size_t& caret = g.focus_field == 1 ? g.address_caret : g.code_caret;
      if (wparam == VK_BACK) {
        if (caret > 0) {
          target.erase(target.begin() + static_cast<long>(caret - 1));
          --caret;
        }
      } else if (wparam == VK_LEFT) {
        if (caret > 0) --caret;
      } else if (wparam == VK_RIGHT) {
        if (caret < target.size()) ++caret;
      } else if (wparam == VK_HOME) {
        caret = 0;
      } else if (wparam == VK_END) {
        caret = target.size();
      } else if (wparam == VK_ESCAPE) {
        g.focus_field = 0;
      } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                 (wparam == 'V' || wparam == 'v')) {
        if (OpenClipboard(window)) {
          HANDLE data = GetClipboardData(CF_UNICODETEXT);
          if (data != nullptr) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
            if (text != nullptr) {
              for (const wchar_t* c = text; *c != L'\0' && target.size() < 64; ++c) {
                if (*c >= 32 && *c != 127) {
                  target.insert(target.begin() + static_cast<long>(caret), *c);
                  ++caret;
                }
              }
              GlobalUnlock(data);
            }
          }
          CloseClipboard();
        }
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP:
      if (g.connect_pressed) {
        g.connect_pressed = false;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
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
