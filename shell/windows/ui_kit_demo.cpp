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
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <wincodec.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "avatar_image.h"
#include "session_client.h"
#include "ui_kit.h"

using namespace molink::ui;
using namespace molink::net;

namespace {

struct app_state {
  HWND window = nullptr;
  ID2D1Factory* factory = nullptr;
  IDWriteFactory* write = nullptr;
  ID2D1HwndRenderTarget* target = nullptr;
  IDWriteTextFormat* small = nullptr;
  IDWriteTextFormat* small_left = nullptr;
  IDWriteTextFormat* body = nullptr;
  IDWriteTextFormat* title = nullptr;
  IDWriteTextFormat* body_center = nullptr;
  IDWriteTextFormat* small_bold = nullptr;   // 11 px semibold, centred
  IDWriteTextFormat* small_right = nullptr;  // 11 px normal, right-aligned
  IDWriteTextFormat* tiny = nullptr;         // 10 px normal, centred
  IDWriteTextFormat* menu_text = nullptr;    // 12 px normal, leading
  IDWriteTextFormat* heading = nullptr;      // 12 px medium, leading
  IDWriteTextFormat* name16 = nullptr;       // 16 px semibold, leading
  IDWriteTextFormat* label12 = nullptr;      // 12 px normal, leading
  IDWriteTextFormat* body_medium = nullptr;  // 13 px medium, leading
  IDWriteTextFormat* subtitle = nullptr;     // 15 px semibold, leading
  IDWriteTextFormat* mono = nullptr;         // 20 px mono, leading
  IDWriteTextFormat* mono_small = nullptr;   // 11 px mono, leading
  IDWriteTextFormat* mono_input = nullptr;   // 14 px mono, leading
  IDWriteTextFormat* tiny_wrap = nullptr;    // 10 px, leading, top, wrapped

  enum class theme_mode { automatic, light, dark };
  theme_mode mode = theme_mode::automatic;
  const palette* theme = &kDark;
  int tab = 0;                 // 0 home, 1 session
  bool session_open = false;   // the session tab is present in the header
  int focus_field = 0;  // 0 none, 1 address, 2 code
  int sel_field = -1;   // text field owning the current selection
  size_t sel_start = 0, sel_end = 0, sel_anchor = 0;
  bool text_selecting = false;
  bool more_open = false;
  bool connect_pressed = false;

  // Session toolbar, modelled on the web: a 40x40 floating ball that opens
  // into the top bar. `pinned` keeps the bar open when the pointer clicks
  // outside it, exactly like the web's toolbar pin.
  bool bar_open = false;
  bool pinned = false;
  bool immersive = false;
  bool window_fullscreen = false;
  WINDOWPLACEMENT saved_placement{};
  bool ball_placed = false;
  float ball_x = 0, ball_y = 0;       // client coordinates (DIPs)
  bool dragging_ball = false;
  bool ball_moved = false;
  float drag_dx = 0, drag_dy = 0;     // pointer offset inside the dragged item
  float drag_start_x = 0, drag_start_y = 0;
  bool dragging_bar = false;
  bool bar_moved = false;
  float bar_dx = 0, bar_dy = 0;       // offset from the top-centre anchor
  float bar_anchor_x = 0, bar_anchor_y = 0;
  bool dragging_volume = false;
  bool watermark = false;
  bool audio_on = true;
  bool clipboard_on = true;
  // The web defaults to control mode (its home mode switch starts on
  // `control` unless the user picked `view`); the toolbar button still turns
  // it off for a view-only session.
  bool control_on = true;
  bool chat_open = false;
  bool files_open = false;
  float audio_volume = 0.8f;
  int display_mode = 0;               // 0 fit, 1 actual, 2 fill
  int fps_index = 2;                  // 自动 / 15 / 30 / 60
  int codec_index = 0;                // 自动 / H.264 / VP9 / VP8 / AV1
  int res_index = 0;
  int open_menu = -1;  // -1 none, 0 fps, 1 codec, 2 resolution, 3 format
  D2D1_RECT_F menu_field{};
  dropdown_state dropdown;

  // Quality popover, copied from the web QualityPanel: preset grid, rate
  // mode, quality/bitrate/fps sliders, image format and reset.
  bool quality_open = false;
  int rate_mode = 0;        // 0 auto, 1 cq, 2 cbr
  int qp = 0;               // 0 = follow the preset
  int bitrate_kbps = 0;     // 0 = follow the preset
  int fps_value = 30;
  int format_index = 0;     // preset / gray8 / rgb565 / bgra
  int dragging_slider = -1; // 0 qp, 1 bitrate, 2 fps
  D2D1_RECT_F qp_panel{}, qp_close{}, qp_preset[7]{}, qp_rate[3]{},
      qp_slider[3]{}, qp_format{}, qp_reset{};

  D2D1_RECT_F stage{};
  D2D1_RECT_F hit_ball{}, hit_bar{};
  D2D1_RECT_F hit_pin{}, hit_collapse{}, hit_fullscreen{}, hit_display[3]{},
      hit_fps{}, hit_quality{}, hit_codec{}, hit_watermark{}, hit_resolution{},
      hit_clip_toggle{}, hit_clip_get{}, hit_clip_set{}, hit_audio{},
      hit_volume{}, hit_chat{}, hit_files{}, hit_control{}, hit_sas{},
      hit_tool_theme{}, hit_tool_settings{}, hit_transport{}, hit_kbps{},
      hit_latency{}, hit_disconnect{}, hit_path{}, hit_protocol{},
      hit_clip_menu{}, hit_tools_menu{}, hit_appearance_menu{};
  D2D1_RECT_F hit_theme_sq{};
  // Live values the shell will feed from the session.
  session_stats stats = {L"VP9", L"1920x1080", 0, 0};
  std::unique_ptr<molink::net::session_client> session;
  std::string session_host;
  int session_port = 0;
  std::string session_code;
  ID2D1Bitmap* avatar_bitmap = nullptr;
  // Shared-memory picture from the local core (--surface <path>).
  std::wstring surface_path;
  HANDLE surface_file = INVALID_HANDLE_VALUE;
  HANDLE surface_mapping = nullptr;
  uint8_t* surface_base = nullptr;
  ID2D1Bitmap* surface_bitmap = nullptr;
  uint32_t surface_w = 0, surface_h = 0;
  uint64_t surface_seq = 0;
  /// A fresh connect drops the surface bitmap but the file still holds the
  /// previous session's last stream frame; only a sequence newer than this one
  /// proves the core has published for the new session.
  bool surface_hold = false;
  uint64_t surface_hold_seq = 0;
  /// When each picture source was last refreshed, so the painter picks the
  /// live one when a session moves between streams and tiles.
  ULONGLONG surface_at = 0, stream_at = 0;
  /// Throttles the 8 MB surface copy to the painter's budget (~30 fps).
  ULONGLONG surface_copy_at = 0;
  // Decoded tile frames from a direct host session.
  ID2D1Bitmap* title_icon = nullptr;
  ID2D1Bitmap* stream_bitmap = nullptr;
  uint32_t stream_w = 0, stream_h = 0;
  uint64_t stream_seq = 0;
  std::vector<uint8_t> stream_pixels;
  D2D1_RECT_F picture_rect{};
  bool remote_mouse_down = false;
  int remote_buttons = 0;
  std::wstring avatar_path;
  std::wstring address_text = L"127.0.0.1:28428";
  std::wstring code_text;
  size_t address_caret = 14, code_caret = 0;
  bool caret_on = true;
  ULONGLONG caret_tick = 0;
  int preset = 2;
  float hover_x = -1, hover_y = -1;

  D2D1_RECT_F hit_theme[3]{}, hit_tab[3]{}, hit_settings{}, hit_modal_close{},
      hit_win[3]{}, hit_session_close{}, hit_avatar{}, hit_more{},
      hit_connect{}, hit_filter[4]{}, hit_auth[2]{}, hit_setting_toggle[9]{};
  int filter_index = 0;
  bool more_on[4] = {true, true, false, true};
  bool setting_on[18] = {true, true, true, true, true, true, true, true,
                         true, true, true, true, true, true, true, true,
                         true, true};
  bool auth_multi = false;
  std::wstring toast_text;
  ULONGLONG toast_until = 0;
  ULONGLONG click_flash_until = 0;

  // Settings modal (web SettingsPanel: centred 768 px dialog).
  bool settings_open = false;
  float settings_anim = 0.0f;
  int settings_section = 0;
  float settings_scroll = 0.0f;
  float settings_content_h = 0.0f;
  float settings_max_scroll = 0.0f;
  D2D1_RECT_F settings_view{}, settings_scrollbar{}, settings_thumb{};
  D2D1_RECT_F hit_seg_theme[2]{}, hit_seg_lang[2]{};
  D2D1_RECT_F hit_lock_field{}, hit_fps_field{};
  D2D1_RECT_F hit_sec_toggle[6]{}, hit_disp_toggle[3]{};
  D2D1_RECT_F hit_dev_name{}, hit_dev_apply{}, hit_long_term{},
      hit_long_apply{}, hit_stun{}, hit_tcp{}, hit_udp{}, hit_ports_save{},
      hit_server_toggle{}, hit_server_url{}, hit_server_invite{},
      hit_server_save{}, hit_server_default{};
  D2D1_RECT_F hit_quality_card[8]{};
  D2D1_RECT_F hit_service_btn[5]{};
  D2D1_RECT_F hit_update_btn{}, hit_repo_btn{}, hit_site_btn{}, hit_star_btn{};
  int settings_lock = 0;
  int settings_lang = 1;
  bool sec_allow_remote = true;
  bool sec_allow_web = true;
  bool sec_discovery = true;
  bool sec_access_requests = true;
  bool sec_encrypt_remote = true;
  bool sec_encrypt_lan = false;
  bool disp_auto_audio = true;
  /// Draw the host's own pointer over the picture (virtual cursor). Off by
  /// default, like the web: the local OS cursor already shows where you are,
  /// and two pointers read as a broken session.
  bool disp_remote_cursor = false;
  bool server_enabled = true;
  /// Host-global video path, mirroring the web's preferences panel:
  /// 0 stream, 1 mixed, 2 tiles (static).
  int video_mode = 1;
  D2D1_RECT_F hit_seg_video[3]{};
  int settings_quality = 0;
  std::wstring set_device_name = L"INTERNET-REMOTE";
  std::wstring set_tcp = L"28418";
  std::wstring set_udp = L"28422";
  std::wstring set_long_term;
  std::wstring stun_server;
  std::wstring server_url = L"https://molink.ssvr.top";
  std::wstring server_invite;
  size_t set_caret_dev = 15, set_caret_tcp = 5, set_caret_udp = 5,
         set_caret_long = 0, set_caret_stun = 0, set_caret_url = 22,
         set_caret_invite = 0;
  bool service_installed = false;
  bool service_running = false;
  /// 0 idle, 1 checking, 2 up to date, 3 update available.
  int update_state = 0;
  std::wstring update_latest;

  // ---- Home (web SimpleHomeView) ----
  float home_scroll = 0.0f;
  float home_content_h = 0.0f;
  float home_max_scroll = 0.0f;
  D2D1_RECT_F home_view{};
  bool code_visible = false;
  bool connect_code_visible = false;
  /// Real verification code from the local core; empty until it answers. The
  /// card used to hard-code 482913 on every machine.
  std::wstring device_code;
  int code_remaining = 0;
  int code_uses = 0;
  bool code_budget_known = false;
  bool connecting = false;
  /// Auto-reconnect state: attempt counter and the next try (0 = idle).
  bool reconnecting = false;
  /// The current tab has completed a `welcome` at least once, so a later
  /// drop is worth retrying (a rejected request is not).
  bool session_accepted = false;
  int retry_attempt = 0;
  ULONGLONG retry_at = 0;
  /// Bumped by every connect and by closing the tab, so a late worker result
  /// cannot reopen a session the user already dropped.
  std::atomic<unsigned> connect_generation{0};
  std::wstring session_label = L"INTERNET-REMOTE";
  bool session_was_live = false;
  bool proxy_mode = false;
  /// The local core delivers decoded stream frames on a shared surface.
  bool surface_mode = false;
  /// The local core advertised at least one stream decoder (VP9/VP8/AV1) in
  /// its ready line. False only for builds without libvpx/dav1d; the native
  /// shell then stays on tiles instead of asking the host for a stream that
  /// the core cannot decode (a blank picture with working input).
  bool core_stream_decoders = true;
  /// This connect went through the core (surface/streams). If it fails before
  /// the host accepts, retry once with a direct tile dial.
  bool core_connect = false;
  bool force_direct = false;
  bool local_core_started = false;
  int local_core_port = 0;
  std::string local_core_token;
  /// Bearer for the loopback settings API (from the core's ready line).
  std::string local_api_token;
  HANDLE local_core_process = nullptr;
  std::wstring core_path;
  /// Identity reported by the local core's loopback API. It used to be a
  /// compile-time placeholder (BDV-NCK-9DN / 192.168.1.8:28418), which made
  /// every install look like the same, already-registered device; keep it
  /// empty until /api/device answers so a missing core is visible.
  std::wstring device_id;
  std::wstring device_ip;
  /// Directory state from /api/state: what the "ID 服务器" chip reports.
  bool id_registered = false;
  bool id_online = false;
  bool id_server_enabled = true;
  std::wstring id_status = L"ID 服务器未连接";
  std::wstring id_error;
  ULONGLONG id_refresh_at = 0;
  /// Backoff for restarting a host core that exited while this shell stayed.
  ULONGLONG core_restart_at = 0;
  /// Single-instance guard (Local\moLinkShellInstance), shared with the
  /// WebView2 shell so only one of them owns the host core at a time.
  HANDLE instance_mutex = nullptr;
  int intent = 0;            // 0 desktop, 1 files, 2 chat
  bool force_relay = false;  // force relay checkbox
  int device_tab = 0;        // recent / saved / lan / online
  int history_view = 0;      // 0 cards, 1 list
  std::wstring connect_error;
  struct recent_entry {
    std::wstring label;
    std::wstring target;
    std::wstring platform = L"windows";
    /// Access code saved on the connect that succeeded (web `rememberCode`).
    std::wstring token;
    bool online = false;
    /// Starred by the user: the row also appears in 已保存 (web savedDevices).
    bool saved = false;
    /// User-given name/note; shown instead of the host's device name.
    std::wstring note;
    /// Host avatar from welcome: "preset:<colour>", "data:" for an uploaded
    /// picture we cannot decode, or empty.
    std::wstring avatar;
    uint64_t at_ms = 0;
  };
  std::vector<recent_entry> recents;
  /// A second click on the same recent row within the system double-click
  /// time connects; the first click only fills the address/code fields.
  ULONGLONG last_recent_click_ms = 0;
  int last_recent_click = -1;
  /// In-app rename sheet for a recent/saved row (focus_field 20 is its input).
  bool rename_open = false;
  /// Dev hook: --rename-test opens the sheet at startup for screenshots.
  bool rename_test = false;
  int rename_index = -1;
  std::wstring rename_text;
  size_t rename_caret = 0;
  D2D1_RECT_F hit_rename_field{}, hit_rename_ok{}, hit_rename_cancel{};
  std::wstring connect_target;
  D2D1_RECT_F hit_recent_card[4]{};
  bool close_prompt = false;
  bool close_remember = false;
  int close_behavior = 0;  // 0 ask, 1 minimize, 2 close session tab
  D2D1_RECT_F hit_close_panel{}, hit_close_cancel{}, hit_close_tab{},
      hit_close_tray{}, hit_close_remember{};
  D2D1_RECT_F hit_recent_action[4][3]{};
  D2D1_RECT_F hit_recent_forget[4]{};
  D2D1_RECT_F hit_recent_star[4]{};
  D2D1_RECT_F hit_recent_edit[4]{};
  /// Scratch target of the last start_connect(), saved with the recents row
  /// once the host accepts the session.
  std::wstring connect_code;
  // Tray icon: the web shell's "minimize to tray" hides the window and keeps
  // the app in the notification area instead of the taskbar.
  bool tray_added = false;
  bool quitting = false;
  HICON tray_icon = nullptr;
  // Right-click menu for the text fields (cut/copy/paste/select all).
  int ctx_field = 0;
  D2D1_RECT_F ctx_panel{}, hit_ctx[4]{};
  bool ctx_cut = false, ctx_copy = false, ctx_paste = false;
  // Inline code retry in the session error banner.
  std::wstring retry_code;
  size_t retry_caret = 0;
  D2D1_RECT_F hit_retry_input{}, hit_retry_btn{};
  bool tracking_leave = false;
  bool access_notice_shown = false;
  bool intent_applied = false;
  std::wstring seen_session_error;
  D2D1_RECT_F hit_self_avatar{}, hit_self_name{}, hit_copy_id{},
      hit_code_eye{}, hit_code_copy{}, hit_code_refresh{}, hit_code_single{},
      hit_code_multi{}, hit_more_settings{}, hit_connect_input_home{},
      hit_connect_send{}, hit_connect_code_home{}, hit_connect_code_eye{},
      hit_connect_btn{}, hit_relay{}, hit_mode[3]{}, hit_notice_update{},
      hit_dev_tab[4]{}, hit_view_cards{}, hit_view_list{};
  D2D1_RECT_F hit_set_panel{}, hit_set_close{}, hit_set_nav[6]{},
      hit_set_theme[3]{}, hit_set_toggle[18]{}, hit_about_check{},
      hit_about_link{};

  // Chat drawer and file-transfer drawer (web SessionView side panels).
  struct chat_msg {
    std::wstring text;
    bool mine = true;
  };
  std::vector<chat_msg> chat_messages;
  std::wstring chat_input;
  size_t chat_caret = 0;
  float chat_anim = 0.0f;
  int chat_unread = 0;
  float files_anim = 0.0f;
  /// Web FileTransferPanel tabs: 0 send, 1 receive (host browser).
  int files_tab = 0;
  std::wstring files_path;   ///< host path being listed
  std::wstring files_root;   ///< host home root from the listing
  std::wstring files_error;
  bool files_loading = false;
  int files_scroll = 0;
  std::vector<molink::net::file_listing_entry> files_entries;
  std::wstring download_dir;
  /// Test hook: `--send-file <path>` offers the file once the session is up.
  std::wstring send_after_connect;
  bool send_after_done = false;
  D2D1_RECT_F hit_chat_panel{}, hit_chat_close{}, hit_chat_input{},
      hit_chat_send{}, hit_files_panel{}, hit_files_close{}, hit_files_pick{},
      hit_files_drop{}, hit_files_tab[2]{}, hit_files_up{}, hit_files_home{},
      hit_files_refresh{}, hit_files_entry[12]{}, hit_files_cancel[6]{};
  // Delayed tooltips, matching the web's `title` attributes.
  int tip_id = -1;
  int tip_candidate = -1;
  ULONGLONG tip_since = 0;
  bool tip_shown = false;
  D2D1_RECT_F tip_rect{};
  std::wstring tip_text;
};

app_state g;

/// A local core with a published surface decodes the stream codecs itself.
bool surface_stream_mode() {
  // Either we started the core ourselves (surface_mode) or a launcher handed
  // us a running core through MOLINK_CORE_TOKEN with --surface.
  if (g.surface_path.empty()) return false;
  return g.surface_mode || std::getenv("MOLINK_CORE_TOKEN") != nullptr;
}

constexpr UINT WM_APP_CONNECT_DONE = WM_APP + 7;
constexpr UINT WM_APP_SURFACE_READY = WM_APP + 8;
constexpr UINT WM_APP_TRAY = WM_APP + 9;
constexpr UINT WM_APP_UPDATE_RESULT = WM_APP + 10;
constexpr UINT WM_APP_START_CONNECT = WM_APP + 11;
constexpr UINT kTrayId = 1;

struct connect_result {
  molink::net::session_client* client = nullptr;
  std::wstring error;
  unsigned generation = 0;
};

void save_theme_mode();
void show_toast(const std::wstring& text);
size_t caret_for_field(int field, float x);
void clear_text_selection();
void request_files_browse();

const wchar_t* kPresets[] = {L"极速", L"流畅", L"平衡", L"清晰",
                             L"超清", L"无损", L"自适应"};
/// Quality preset names as the core expects them (QualityPanel PRESETS).
const char* kPresetModes[] = {"turbo", "smooth", "balanced", "quality",
                              "ultra", "lossless", "auto"};
const wchar_t* kFpsChoices[] = {L"自动", L"15 fps", L"30 fps", L"60 fps"};
// The native client decodes tiles only; the stream entries stay visible but
// disabled so the menu matches the web's list without offering codecs that
// would leave a blank picture.
const wchar_t* kCodecChoices[] = {L"自动", L"平铺",   L"H.264",
                                  L"VP9",  L"VP8", L"AV1"};
const wchar_t* kResolutionChoices[] = {L"1920x1080", L"1280x720",
                                       L"2560x1440", L"3840x2160"};
const wchar_t* kDisplayModes[] = {L"适应", L"1:1", L"拉伸"};
const wchar_t* kFormats[] = {L"跟随档位", L"黑白 8 位", L"16 位 RGB565",
                             L"真彩 BGRA"};

float g_dpi = 96.0f;
float g_dpi_base = 96.0f;

/// GetDpiForWindow and SetProcessDpiAwarenessContext are Windows 10 APIs; the
/// Win7 x86 build resolves them at runtime and falls back to the system DPI /
/// legacy DPI awareness.
UINT system_dpi_for(HWND window) {
  using GetDpiFn = UINT(WINAPI*)(HWND);
  static GetDpiFn get_dpi = reinterpret_cast<GetDpiFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get_dpi != nullptr && window != nullptr) {
    const UINT dpi = get_dpi(window);
    if (dpi > 0) return dpi;
  }
  HDC dc = GetDC(window);
  if (dc == nullptr) return 96;
  const UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
  ReleaseDC(window, dc);
  return dpi > 0 ? dpi : 96;
}

void enable_dpi_awareness() {
  using SetCtxFn = BOOL(WINAPI*)(HANDLE);
  static SetCtxFn set_ctx = reinterpret_cast<SetCtxFn>(GetProcAddress(
      GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (set_ctx != nullptr) {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (no Win10-only types here).
    set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
    return;
  }
  SetProcessDPIAware();
}

/// UI DPI: on an OS-scaled display the system DPI already does the job. On a
/// large window at 96 DPI (Wine, or a 4K monitor at 100 %) scale the DIPs up
/// so 28 px controls do not become tiny; the logical layout stays >= 1280x720.
float ui_dpi_for(HWND window) {
  const float base = g_dpi_base > 0 ? g_dpi_base : 96.0f;
  if (base > 96.5f) return base;
  RECT client{};
  GetClientRect(window, &client);
  const float w = static_cast<float>(client.right - client.left);
  const float h = static_cast<float>(client.bottom - client.top);
  if (w <= 0 || h <= 0) return base;
  const float scale = (std::min)(w / 1280.0f, h / 720.0f);
  const float ui_scale = (std::min)((std::max)(1.0f, scale), 2.0f);
  return base * ui_scale;
}

bool inside(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

/// Every interactive surface gets a slot so the 140 ms web hover transition
/// (`.btn { transition: ... 140ms ease }`) can be replayed in D2D. Paint sets
/// the targets, the timer eases the values, and the helpers tint from them.
enum hover_slot {
  hv_ball,
  hv_pin, hv_collapse, hv_fullscreen, hv_session_chip,
  hv_display0, hv_display1, hv_display2,
  hv_fps, hv_quality, hv_codec, hv_watermark, hv_resolution,
  hv_clip_toggle, hv_clip_get, hv_clip_set, hv_audio, hv_volume,
  hv_chat, hv_files, hv_control, hv_sas, hv_theme, hv_settings,
  hv_path, hv_protocol, hv_transport, hv_kbps, hv_latency,
  hv_clip_menu, hv_tools_menu, hv_appearance_menu, hv_disconnect,
  hv_win0, hv_win1, hv_win2,
  hv_home_tab, hv_plus, hv_session_tab, hv_session_close, hv_header_theme,
  hv_header_settings, hv_auth_single, hv_auth_multi,
  hv_avatar, hv_more, hv_connect, hv_field_address, hv_field_code,
  hv_filter0, hv_filter1, hv_filter2, hv_filter3,
  hv_toggle0, hv_toggle1, hv_toggle2, hv_toggle3,
  hv_settings_toggle0, hv_settings_toggle1, hv_settings_toggle2,
  hv_settings_toggle3, hv_settings_toggle4, hv_settings_toggle5,
  hv_settings_toggle6, hv_settings_toggle7, hv_settings_toggle8,
  hv_settings_theme0, hv_settings_theme1, hv_settings_theme2,
  hv_qp_close, hv_qp_preset0, hv_qp_preset1, hv_qp_preset2, hv_qp_preset3,
  hv_qp_preset4, hv_qp_preset5, hv_qp_preset6,
  hv_qp_rate0, hv_qp_rate1, hv_qp_rate2, hv_qp_format, hv_qp_reset,
  hv_menu_item0, hv_menu_item1, hv_menu_item2, hv_menu_item3, hv_menu_item4,
  hv_menu_item5, hv_menu_item6, hv_menu_item7,
  hv_set_close, hv_set_nav0, hv_set_nav1, hv_set_nav2, hv_set_nav3,
  hv_set_nav4, hv_set_nav5,
  hv_set_theme0, hv_set_theme1, hv_set_theme2,
  hv_set_toggle0, hv_set_toggle1, hv_set_toggle2, hv_set_toggle3,
  hv_set_toggle4, hv_set_toggle5, hv_set_toggle6, hv_set_toggle7,
  hv_set_toggle8, hv_set_toggle9, hv_set_toggle10, hv_set_toggle11,
  hv_set_toggle12, hv_set_toggle13, hv_set_toggle14, hv_set_toggle15,
  hv_set_toggle16, hv_set_toggle17,
  hv_about_check, hv_about_link,
  hv_set_seg_theme0, hv_set_seg_theme1, hv_set_seg_lang0, hv_set_seg_lang1,
  hv_set_video0, hv_set_video1, hv_set_video2,
  hv_set_field_lock, hv_set_field_fps, hv_set_field_dev, hv_set_field_long,
  hv_set_field_tcp, hv_set_field_udp, hv_set_btn_dev, hv_set_btn_long,
  hv_set_btn_ports, hv_set_btn_service0, hv_set_btn_service1,
  hv_set_btn_service2, hv_set_btn_service3, hv_set_btn_service4,
  hv_set_btn_update, hv_set_btn_repo, hv_set_btn_site, hv_set_btn_star,
  hv_home_address, hv_home_code_eye,
  hv_recent_card0, hv_recent_card1, hv_recent_card2, hv_recent_card3,
  hv_set_field_stun, hv_set_field_url, hv_set_field_invite,
  hv_set_btn_server_save, hv_set_btn_server_default,
  hv_set_quality0, hv_set_quality1, hv_set_quality2, hv_set_quality3,
  hv_set_quality4, hv_set_quality5, hv_set_quality6, hv_set_quality7,
  hv_chat_close, hv_chat_send, hv_files_close, hv_files_pick,
  hv_count
};

float g_hover_value[hv_count] = {};
float g_hover_target[hv_count] = {};

/// Sets this frame's hover target and returns the eased 0..1 amount.
inline float hover_lerp(int id, bool hovered) {
  g_hover_target[id] = hovered ? 1.0f : 0.0f;
  return g_hover_value[id];
}

void note_tip(int id, const wchar_t* label, const D2D1_RECT_F& r,
              bool hovered) {
  if (!hovered) return;
  g.tip_candidate = id;
  g.tip_rect = r;
  g.tip_text = label;
}

D2D1_COLOR_F mix_color(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                      a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

/// Steps every hover transition one 16 ms frame. Returns true while any value
/// is still moving so the caller can repaint.
bool flash_active(bool hovered) {
  return hovered && g.click_flash_until > GetTickCount64();
}

bool advance_hover() {
  bool moving = false;
  for (int i = 0; i < hv_count; ++i) {
    const float target = g_hover_target[i];
    float value = g_hover_value[i];
    if (value == target) continue;
    value += (target - value) * 0.22f;
    if (std::fabs(target - value) < 0.01f) value = target;
    g_hover_value[i] = value;
    moving = true;
  }
  return moving;
}

/// Multiplies a colour's alpha (hover-reveal animations).
inline D2D1_COLOR_F fade_alpha(const D2D1_COLOR_F& c, float a) {
  return D2D1::ColorF(c.r, c.g, c.b, c.a * a);
}

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/// Cuts a string to fit `max_width` and appends an ellipsis (tab labels).
std::wstring elide_text(IDWriteTextFormat* format, const std::wstring& text,
                        float max_width);

/// Exact advance width from DirectWrite (falls back to the rough estimate
/// before the factories exist).
float measure_text(IDWriteTextFormat* format, const std::wstring& s) {
  if (format == nullptr || g.write == nullptr || s.empty()) return 0.0f;
  // Every label in the settings panel measures its string while the modal
  // animates; CreateTextLayout per call was the main cost of that animation.
  struct measured {
    IDWriteTextFormat* format;
    std::wstring text;
    float width;
  };
  static std::vector<measured> cache;
  for (const measured& item : cache) {
    if (item.format == format && item.text == s) return item.width;
  }
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(g.write->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()),
                                       format, 4096.0f, 200.0f, &layout)) ||
      layout == nullptr) {
    return 0.0f;
  }
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  layout->Release();
  const float width = metrics.widthIncludingTrailingWhitespace;
  if (cache.size() > 1024) cache.clear();
  cache.push_back({format, s, width});
  return width;
}

std::wstring elide_text(IDWriteTextFormat* format, const std::wstring& text,
                        float max_width) {
  if (measure_text(format, text) <= max_width) return text;
  const std::wstring ellipsis = L"…";
  std::wstring out = text;
  while (!out.empty() &&
         measure_text(format, out + ellipsis) > max_width) {
    out.pop_back();
  }
  return out + ellipsis;
}

/// Rough advance width of 11 px UI text: half-width for Latin, full for CJK.
float text_width(const std::wstring& s, float size = 11.0f) {
  float w = 0;
  for (wchar_t c : s) w += c < 128 ? size * 0.55f : size;
  return w;
}

/// The toolbar is built from slots; `width` is the control width in DIPs.
enum bar_kind {
  bk_pin, bk_collapse, bk_fullscreen, bk_separator, bk_display_fit,
  bk_display_actual, bk_display_fill, bk_fps, bk_quality, bk_codec,
  bk_watermark, bk_resolution, bk_clip_toggle, bk_clip_get, bk_clip_set,
  bk_audio, bk_volume, bk_chat, bk_files, bk_control, bk_sas, bk_theme,
  bk_settings, bk_transport, bk_path, bk_protocol, bk_clip_menu, bk_tools_menu,
  bk_appearance_menu, bk_kbps, bk_latency, bk_session_chip, bk_disconnect
};

struct bar_slot {
  bar_kind kind;
  float width;
};

/// 0 = minimal, 1 = compact (default), 2 = full. The widest tier that fits
/// the stage wins, so the bar never overflows or scrolls like the web's.
std::vector<bar_slot> bar_layout(int tier, bool with_chip) {
  std::vector<bar_slot> slots;
  auto add = [&](bar_kind kind, float width) {
    slots.push_back({kind, width});
  };
  add(bk_pin, 28);
  add(bk_collapse, 28);
  if (with_chip) add(bk_session_chip, 118);
  add(bk_fullscreen, 28);
  add(bk_separator, 9);
  add(bk_display_fit, 44);
  add(bk_display_actual, 36);
  add(bk_display_fill, 44);
  if (tier >= 1) add(bk_fps, 76);
  add(bk_quality, 70);
  if (tier >= 1) {
    add(bk_codec, 76);
    add(bk_watermark, 28);
  }
  add(bk_resolution, 104);
  add(bk_separator, 9);
  if (tier >= 2) {
    // Wide window: every web control stands on its own.
    add(bk_clip_toggle, 28);
    add(bk_clip_get, 28);
    add(bk_clip_set, 28);
    add(bk_audio, 28);
    if (g.audio_on) add(bk_volume, 56);
    add(bk_separator, 9);
    add(bk_chat, 28);
    add(bk_files, 28);
    add(bk_control, 28);
    add(bk_sas, 28);
    add(bk_separator, 9);
    add(bk_theme, 28);
    add(bk_settings, 28);
    add(bk_separator, 9);
    add(bk_path, 48);
    add(bk_protocol, 48);
  } else if (tier == 1) {
    // Narrow window: the web folds each group into a ToolbarMenu.
    add(bk_clip_menu, 46);
    add(bk_audio, 28);
    if (g.audio_on) add(bk_volume, 56);
    add(bk_separator, 9);
    add(bk_tools_menu, 46);
    add(bk_separator, 9);
    add(bk_appearance_menu, 46);
    add(bk_separator, 9);
    add(bk_transport, 88);
  } else {
    add(bk_transport, 88);
  }
  add(bk_separator, 9);
  add(bk_disconnect, 28);
  return slots;
}

float bar_width(const std::vector<bar_slot>& slots) {
  float w = 12;
  for (size_t i = 0; i < slots.size(); ++i) {
    w += slots[i].width;
    if (i + 1 < slots.size()) w += 2;
  }
  return w;
}

/// Menus: 0 fps, 1 codec, 2 resolution, 3 image format, 4 clipboard,
/// 5 tools, 6 appearance (the compact toolbar's groups, like the web).
int menu_count(int which) {
  if (which == 0) return 4;
  if (which == 1) return 6;
  if (which == 2) return 4;
  if (which == 3) return 4;
  if (which == 4) return 3;
  if (which == 5) return 4;
  if (which == 6) return 2;
  if (which == 10) return 6;
  if (which == 11) return 4;
  return 2;
}

const wchar_t* menu_label(int which, int index) {
  if (which == 0) return kFpsChoices[index];
  if (which == 1) return kCodecChoices[index];
  if (which == 2) return kResolutionChoices[index];
  if (which == 3) return kFormats[index];
  if (which == 10) {
    static const wchar_t* locks[] = {L"30 秒", L"1 分钟", L"2 分钟",
                                     L"5 分钟", L"15 分钟", L"从不"};
    return locks[index];
  }
  if (which == 11) {
    static const wchar_t* fps[] = {L"自适应（按画质档位和负载）", L"15 帧",
                                   L"30 帧", L"60 帧"};
    return fps[index];
  }
  if (which == 4) {
    if (index == 0) return g.clipboard_on ? L"关闭剪贴板" : L"开启剪贴板";
    static const wchar_t* items[] = {nullptr, L"获取远端剪贴板",
                                     L"发送到远端剪贴板"};
    return items[index];
  }
  if (which == 5) {
    if (index == 0) return L"聊天";
    if (index == 1) return L"文件";
    return index == 2 ? (g.control_on ? L"停止控制" : L"开始控制")
                      : L"Ctrl+Alt+Del";
  }
  return index == 0 ? L"切换主题" : L"设置";
}

int menu_current(int which) {
  if (which == 0) return g.fps_index;
  if (which == 1) return g.codec_index;
  if (which == 2) return g.res_index;
  if (which == 3) return g.format_index;
  if (which == 10) return g.settings_lock;
  if (which == 11) return g.fps_index >= 0 ? g.fps_index : 0;
  return -1;
}

bool menu_active(int which, int index) {
  if (which == 4) return index == 0 && g.clipboard_on;
  if (which == 5) {
    if (index == 0) return g.chat_open;
    if (index == 1) return g.files_open;
    if (index == 2) return g.control_on;
  }
  return false;
}

bool menu_disabled(int which, int index) {
  if (which == 4 && index > 0 && !g.clipboard_on) return true;
  // A direct client paints 自动/平铺 only; with a decoding core the stream
  // codecs are real again. H.264 needs a GPU encoder and stays off.
  if (which == 1 && index >= 2) {
    if (index == 2) return true;  // H.264
    return !surface_stream_mode();  // VP9/VP8/AV1
  }
  return false;
}

/// Knob values are "follow the preset" until the user moves them.
bool quality_custom() {
  return g.qp != 0 || g.bitrate_kbps != 0 || g.rate_mode != 0 ||
         g.format_index != 0;
}

const char* rate_mode_name(int mode) {
  return mode == 0 ? "auto" : (mode == 1 ? "cq" : "cbr");
}

const char* format_name(int index) {
  return index == 0 ? "preset"
                    : (index == 1 ? "gray8" : (index == 2 ? "rgb565" : "bgra"));
}

std::wstring wide_of(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(),
                      static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::string utf8_of(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()), out.data(), size, nullptr,
                      nullptr);
  return out;
}

std::string json_escape(const std::wstring& text) {
  const std::string utf8 = utf8_of(text);
  std::string out;
  out.reserve(utf8.size() + 8);
  for (char c : utf8) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

void session_send(const std::string& json) {
  if (g.session != nullptr) g.session->send_json(json);
}

bool looks_like_device_id(const std::wstring& target) {
  if (target.empty() || target.size() > 16) return false;
  for (wchar_t c : target) {
    const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                    (c >= L'0' && c <= L'9') || c == L'-';
    if (!ok) return false;
  }
  return true;
}

/// Session/diagnostic log (defined below; used by the core launcher too).
void native_log(const std::wstring& text);

std::wstring find_core_exe() {
  if (!g.core_path.empty()) return g.core_path;
  wchar_t buffer[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"MOLINK_CORE", buffer, MAX_PATH) > 0) {
    return buffer;
  }
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
    std::wstring path = exe;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      path = path.substr(0, slash + 1) + L"molink-core.exe";
      if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return path;
      }
    }
  }
  if (GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH) > 0) {
    std::wstring path = std::wstring(buffer) + L"\\moLink\\molink-core.exe";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  return std::wstring();
}

/// %LOCALAPPDATA%\moLink\native-core.log — the core's stderr. The shell owns
/// the process, so without this a failed directory registration (TLS, DNS,
/// ip_denied) is invisible after the fact.
std::wstring native_core_log_path() {
  wchar_t base[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
    return std::wstring();
  }
  const std::wstring dir = std::wstring(base) + L"\\moLink";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\native-core.log";
}

/// Local-core proxy mode: start molink-core with MOLINK_SURFACE=1, read its
/// ready line and use its /ws/direct proxy. The core decodes streams and
/// publishes the native picture surface, so H.264/VP9/AV1 work even though
/// the demo itself only decodes tiles.
bool start_local_core(int* port, std::string* client_token,
                      std::wstring* surface, std::string* error,
                      int preferred_port = 28418) {
  const std::wstring core = find_core_exe();
  if (core.empty()) {
    if (error != nullptr) *error = "molink-core.exe not found";
    native_log(L"local core: not found");
    return false;
  }
  native_log(L"local core: starting " + core);
  SetEnvironmentVariableW(L"MOLINK_SURFACE", L"1");
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) {
    if (error != nullptr) *error = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  HANDLE core_log = INVALID_HANDLE_VALUE;
  const std::wstring core_log_path = native_core_log_path();
  if (!core_log_path.empty()) {
    core_log = CreateFileW(core_log_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  startup.hStdError = core_log != INVALID_HANDLE_VALUE
                          ? core_log
                          : GetStdHandle(STD_ERROR_HANDLE);
  startup.hStdInput = nullptr;
  std::wstring command = L"\"" + core +
                         L"\" --print-ready --parent-pid " +
                         std::to_wstring(GetCurrentProcessId()) + L" --port " +
                         std::to_wstring(preferred_port);
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(write_pipe);
  if (core_log != INVALID_HANDLE_VALUE) CloseHandle(core_log);
  if (!started) {
    wchar_t message[128] = {};
    std::swprintf(message, 128, L"local core: CreateProcess failed err=%lu (%.60s)",
                  static_cast<unsigned long>(GetLastError()), core.c_str());
    native_log(message);
    CloseHandle(read_pipe);
    if (error != nullptr) *error = "could not start molink-core";
    return false;
  }
  CloseHandle(process.hThread);
  std::string line;
  const ULONGLONG deadline = GetTickCount64() + 10000;
  // libuvcpp's console logger writes its startup lines to stdout, before the
  // core prints the ready JSON. Waiting for the first newline therefore parsed
  // a log line and reported "no ready line" -- the shell then ran without a
  // local core (no surface, no decoded streams). Wait for the marker itself.
  while (GetTickCount64() < deadline) {
    const size_t marker = line.find("\"event\":\"ready\"");
    if (marker != std::string::npos && line.find('\n', marker) != std::string::npos) {
      break;
    }
    // A core that could not bind the preferred port exits at once; retry with
    // an ephemeral port instead of waiting the full ten seconds.
    if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
    DWORD available = 0;
    if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) {
      break;
    }
    if (available > 0) {
      char buffer[4096];
      DWORD got = 0;
      if (!ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr)) break;
      line.append(buffer, got);
      if (line.size() > 256 * 1024) break;
    } else {
      Sleep(50);
    }
  }
  CloseHandle(read_pipe);
  // Keep only the ready JSON. nlohmann sorts object keys, so the line starts
  // with "capture", not "event": search for the marker and take the object
  // that contains it, starting after the previous newline.
  {
    const size_t marker = line.find("\"event\":\"ready\"");
    if (marker != std::string::npos) {
      const size_t start = line.rfind('\n', marker) == std::string::npos
                               ? 0
                               : line.rfind('\n', marker) + 1;
      const size_t brace = line.find('{', start);
      const size_t end = line.find('\n', marker);
      if (brace != std::string::npos) {
        line = line.substr(brace, end == std::string::npos
                                      ? std::string::npos
                                      : end - brace);
      }
    }
  }
  auto find_number = [&line](const char* key, int* out) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = line.find(needle);
    if (at == std::string::npos) return false;
    *out = std::atoi(line.c_str() + at + needle.size());
    return true;
  };
  auto find_string = [&line](const char* key, std::string* out) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = line.find(needle);
    if (at == std::string::npos) return false;
    const size_t begin = at + needle.size();
    const size_t end = line.find('"', begin);
    if (end == std::string::npos) return false;
    *out = line.substr(begin, end - begin);
    return true;
  };
  int ready_port = 0;
  std::string token;
  std::string surface_utf8;
  if (line.empty() || line.front() != '{' || !find_number("port", &ready_port) ||
      ready_port <= 0) {
    native_log(L"local core: no ready line: " +
               std::wstring(line.begin(), line.end()));
    TerminateProcess(process.hProcess, 0);
    CloseHandle(process.hProcess);
    // The fixed port may be taken (an older install's core, another service).
    // An ephemeral port still registers correctly with the directory; the
    // firewall rule is program-based, so reachability does not depend on the
    // number.
    if (preferred_port != 0) {
      native_log(L"local core: port " + std::to_wstring(preferred_port) +
                 L" unavailable; retrying with an ephemeral port");
      return start_local_core(port, client_token, surface, error, 0);
    }
    if (error != nullptr) *error = "core did not report a ready port";
    return false;
  }
  native_log(L"local core: ready port " + std::to_wstring(ready_port));
  // The ready line lists the decoders this core carries (the native surface
  // is driven by the core, not by the shell). An empty array means tiles only.
  g.core_stream_decoders =
      line.find("\"video_decoders\":[\"") != std::string::npos;
  find_string("client_token", &token);
  std::string api_token;
  find_string("token", &api_token);
  find_string("surface", &surface_utf8);
  std::string unescaped;
  for (size_t i = 0; i < surface_utf8.size(); ++i) {
    if (surface_utf8[i] == '\\' && i + 1 < surface_utf8.size() &&
        surface_utf8[i + 1] == '\\') {
      unescaped += '\\';
      ++i;
    } else {
      unescaped += surface_utf8[i];
    }
  }
  *port = ready_port;
  *client_token = token;
  *surface = wide_of(unescaped);
  g.local_core_process = process.hProcess;
  g.local_core_port = ready_port;
  g.local_core_token = token;
  g.surface_mode = true;
  if (!api_token.empty()) g.local_api_token = api_token;
  g.local_core_started = true;
  return true;
}

/// Start a real session to the target's core (/ws/session) on a worker
/// thread so a slow or unreachable host never blocks the UI. The result comes
/// back as WM_APP_CONNECT_DONE with a heap session_client (or nullptr).
int local_core_port() {
  // The host core this shell started (`start_local_core`) binds an ephemeral
  // port; every loopback call (settings, identity) must follow the ready line,
  // not the historical default. MOLINK_CORE_PORT is for attaching to a core
  // someone else started.
  if (g.local_core_port > 0) return g.local_core_port;
  const char* value = std::getenv("MOLINK_CORE_PORT");
  if (value != nullptr) {
    const int port = std::atoi(value);
    if (port > 0 && port <= 65535) return port;
  }
  return 28418;
}

// ---------------------------------------------------------------------------
// Native session log + loopback settings API.
// ---------------------------------------------------------------------------

/// Log file for the one thing a screenshot cannot show: why a session ended.
/// Written to %LOCALAPPDATA%\moLink\native-session.log as UTF-8 lines.
void native_log(const std::wstring& text) {
  wchar_t base[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) return;
  const std::wstring dir = std::wstring(base) + L"\\moLink";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path = dir + L"\\native-session.log";
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t stamp[48] = {};
  std::swprintf(stamp, 48, L"%04d-%02d-%02d %02d:%02d:%02d  ", now.wYear,
                now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  const std::wstring line = std::wstring(stamp) + text + L"\r\n";
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(),
                                        static_cast<int>(line.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return;
  std::string utf8(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, line.data(),
                      static_cast<int>(line.size()), utf8.data(), bytes,
                      nullptr, nullptr);
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
            nullptr);
  CloseHandle(file);
}

// ---------------------------------------------------------------------------
// About panel: product logo and browser links.
// ---------------------------------------------------------------------------

/// Version of this native shell (shipped together with the core). Bump it in
/// the same commit that tags a release.
constexpr wchar_t kNativeVersion[] = L"0.10.0";
constexpr wchar_t kReleasesApi[] =
    L"https://api.github.com/repos/Antruly/molink/releases/latest";

constexpr wchar_t kRepoUrl[] = L"https://github.com/Antruly/molink";
constexpr wchar_t kSiteUrl[] = L"https://ssvr.top/molink";
/// Native builds ship as their own assets on the same release page. The
/// desktop/web package (moLink.exe from the WebView2 shell, or the .deb) must
/// never be offered to this process as its own update.
constexpr wchar_t kNativeReleasesUrl[] =
    L"https://github.com/Antruly/molink/releases/latest";

void open_url(const wchar_t* url) {
  ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

/// Filled five-point star with rounded corners (the kit only has the stroke
/// outline, and the web draws lucide's star with round joins). Used for a
/// saved row's star: solid amber reads as "on".
void draw_solid_star(ID2D1RenderTarget* target, float cx, float cy, float r,
                     const D2D1_COLOR_F& color) {
  if (target == nullptr || g.factory == nullptr) return;
  const double kPi = 3.14159265358979323846;
  const int kPoints = 10;
  float px[kPoints];
  float py[kPoints];
  for (int i = 0; i < kPoints; ++i) {
    const double angle = -kPi / 2.0 + i * 2.0 * kPi / kPoints;
    const double radius = (i % 2 == 0) ? r : r * 0.45;
    px[i] = cx + static_cast<float>(radius * std::cos(angle));
    py[i] = cy + static_cast<float>(radius * std::sin(angle));
  }
  float ax[kPoints], ay[kPoints], bx[kPoints], by[kPoints];
  for (int i = 0; i < kPoints; ++i) {
    const int p = (i + kPoints - 1) % kPoints;
    const int n = (i + 1) % kPoints;
    const float dxp = px[p] - px[i];
    const float dyp = py[p] - py[i];
    const float dxn = px[n] - px[i];
    const float dyn = py[n] - py[i];
    const float lp = std::sqrt(dxp * dxp + dyp * dyp);
    const float ln = std::sqrt(dxn * dxn + dyn * dyn);
    if (lp <= 0.0f || ln <= 0.0f) continue;
    // Fillet length: a third of the shorter edge gives a clearly rounded tip
    // without turning the star into a blob.
    const float t = 0.32f * (std::min)(lp, ln);
    ax[i] = px[i] + dxp / lp * t;
    ay[i] = py[i] + dyp / lp * t;
    bx[i] = px[i] + dxn / ln * t;
    by[i] = py[i] + dyn / ln * t;
  }
  ID2D1PathGeometry* path = nullptr;
  if (FAILED(g.factory->CreatePathGeometry(&path)) || path == nullptr) return;
  ID2D1GeometrySink* sink = nullptr;
  if (SUCCEEDED(path->Open(&sink)) && sink != nullptr) {
    sink->BeginFigure(D2D1::Point2F(bx[0], by[0]),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i <= kPoints; ++i) {
      const int v = i % kPoints;
      sink->AddLine(D2D1::Point2F(ax[v], ay[v]));
      D2D1_QUADRATIC_BEZIER_SEGMENT curve;
      curve.point1 = D2D1::Point2F(px[v], py[v]);
      curve.point2 = D2D1::Point2F(bx[v], by[v]);
      sink->AddQuadraticBezier(curve);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(target->CreateSolidColorBrush(color, &brush)) &&
        brush != nullptr) {
      target->FillGeometry(path, brush);
      brush->Release();
    }
  }
  path->Release();
}

/// HTTPS GET through WinHTTP (system proxy + TLS). Returns false on any
/// network or HTTP failure; callers treat that as "could not check".
bool http_get(const wchar_t* url, std::string* body) {
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {};
  wchar_t path[1024] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 1024;
  if (!WinHttpCrackUrl(url, 0, 0, &parts)) return false;
  HINTERNET session =
      WinHttpOpen(L"moLink-native/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) return false;
  WinHttpSetTimeouts(session, 3000, 3000, 5000, 8000);
  HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request =
      connection == nullptr
          ? nullptr
          : WinHttpOpenRequest(
                connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE
                                                       : 0);
  const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
  bool ok = request != nullptr &&
            WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr);
  if (ok && body != nullptr) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 0 && (status < 200 || status >= 300)) ok = false;
    body->clear();
    char buffer[2048];
    DWORD read = 0;
    while (WinHttpReadData(request, buffer, sizeof(buffer), &read) && read > 0) {
      body->append(buffer, read);
    }
  }
  if (request != nullptr) WinHttpCloseHandle(request);
  if (connection != nullptr) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}

/// Compares dotted versions ("v0.9.82" vs "0.9.82"): true when `candidate`
/// is newer than `current`.
bool version_newer(const std::wstring& candidate, const std::wstring& current) {
  auto parse = [](const std::wstring& text, int out[4]) {
    int count = 0;
    int value = 0;
    bool any = false;
    for (size_t i = 0; i <= text.size(); ++i) {
      const wchar_t c = i < text.size() ? text[i] : L'.';
      if (c >= L'0' && c <= L'9') {
        value = value * 10 + (c - L'0');
        any = true;
      } else if (any) {
        if (count < 4) out[count++] = value;
        value = 0;
        any = false;
      }
    }
  };
  int a[4] = {0, 0, 0, 0};
  int b[4] = {0, 0, 0, 0};
  parse(candidate, a);
  parse(current, b);
  for (int i = 0; i < 4; ++i) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

struct update_result {
  int state = 0;  ///< 2 up to date, 3 update available, 0 check failed
  std::wstring latest;
};

/// Asks GitHub for the newest release tag on a worker thread; the UI stays
/// responsive and gets WM_APP_UPDATE_RESULT. The button only opens the
/// download page once a newer tag is known.
void start_update_check() {
  if (g.update_state == 1) return;
  g.update_state = 1;
  InvalidateRect(g.window, nullptr, FALSE);
  std::thread([] {
    auto* result = new update_result();
    std::string body;
    if (http_get(kReleasesApi, &body)) {
      const std::string needle = "\"tag_name\":\"";
      const size_t at = body.find(needle);
      if (at != std::string::npos) {
        const size_t begin = at + needle.size();
        const size_t end = body.find('"', begin);
        if (end != std::string::npos) {
          const std::string tag = body.substr(begin, end - begin);
          result->latest.assign(tag.begin(), tag.end());
          result->state =
              version_newer(result->latest, kNativeVersion) ? 3 : 2;
        }
      }
    }
    if (!PostMessageW(g.window, WM_APP_UPDATE_RESULT, 0,
                      reinterpret_cast<LPARAM>(result))) {
      delete result;
    }
  }).detach();
}

/// The exe's icon resource rendered as a D2D bitmap for the About header.
/// The stroke-icon kit has no product mark, and a scaled-up glyph reads as a
/// placeholder next to the web build.
ID2D1Bitmap* g_about_logo = nullptr;

ID2D1Bitmap* about_logo(ID2D1RenderTarget* target) {
  if (g_about_logo != nullptr) return g_about_logo;
  HICON icon = static_cast<HICON>(LoadImageW(
      GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 128, 128,
      LR_DEFAULTCOLOR));
  if (icon == nullptr) return nullptr;
  ICONINFO info{};
  ID2D1Bitmap* result = nullptr;
  if (GetIconInfo(icon, &info)) {
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wic)))) {
      IWICBitmap* bitmap = nullptr;
      if (info.hbmColor != nullptr &&
          SUCCEEDED(wic->CreateBitmapFromHBITMAP(
              info.hbmColor, nullptr, WICBitmapUseAlpha, &bitmap))) {
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(
                bitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
            target != nullptr) {
          target->CreateBitmapFromWicBitmap(converter, nullptr, &result);
        }
        if (converter != nullptr) converter->Release();
        if (bitmap != nullptr) bitmap->Release();
      }
      wic->Release();
    }
    if (info.hbmColor != nullptr) DeleteObject(info.hbmColor);
    if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
  }
  DestroyIcon(icon);
  g_about_logo = result;
  return result;
}

/// One loopback request to the local core. The native shell is the desktop
/// app of this machine, so its security panel edits the host's own settings
/// through the same API the bundled web page uses; loopback needs no bearer
/// token, and `allow_web_client` never gates /api/settings.
bool local_api_request(const wchar_t* method, const std::wstring& path,
                       std::string* body, int* status_out) {
  if (status_out != nullptr) *status_out = 0;
  HINTERNET session = WinHttpOpen(
      L"moLink-native/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) return false;
  WinHttpSetTimeouts(session, 1500, 1500, 1500, 1500);
  HINTERNET connection = WinHttpConnect(
      session, L"127.0.0.1",
      static_cast<INTERNET_PORT>(local_core_port()), 0);
  HINTERNET request =
      connection != nullptr
          ? WinHttpOpenRequest(connection, method, path.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               0)
          : nullptr;
  // The core's /api/* guard wants the per-launch session token and the
  // shell's client token, both printed on its ready line. Without them
  // (attached to a core we did not start) the call simply comes back 401/403
  // and the panel keeps its defaults.
  std::wstring headers;
  if (!g.local_api_token.empty()) {
    headers = L"Authorization: Bearer " +
              std::wstring(g.local_api_token.begin(), g.local_api_token.end());
  }
  if (!g.local_core_token.empty()) {
    if (!headers.empty()) headers += L"\r\n";
    headers += L"X-MoLink-Client: " +
               std::wstring(g.local_core_token.begin(),
                            g.local_core_token.end());
  }
  bool ok = false;
  if (request != nullptr &&
      WinHttpSendRequest(request,
                         headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                         : headers.c_str(),
                         headers.empty() ? 0 : static_cast<DWORD>(-1),
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
      if (status_out != nullptr) *status_out = static_cast<int>(status);
      ok = status >= 200 && status < 300;
    }
    if (body != nullptr) {
      body->clear();
      char buffer[2048];
      DWORD read = 0;
      while (WinHttpReadData(request, buffer, sizeof(buffer), &read) &&
             read > 0) {
        body->append(buffer, read);
      }
    }
  }
  if (request != nullptr) WinHttpCloseHandle(request);
  if (connection != nullptr) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}

bool json_flag(const std::string& json, const char* key, bool fallback) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return fallback;
  return json.compare(at + needle.size(), 4, "true") == 0;
}

std::string json_text(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return std::string();
  const size_t start = at + needle.size();
  const size_t end = json.find('"', start);
  if (end == std::string::npos) return std::string();
  return json.substr(start, end - start);
}

int json_int(const std::string& json, const char* key, int fallback) {
  const std::string needle = std::string("\"") + key + "\":";
  size_t at = json.find(needle);
  if (at == std::string::npos) return fallback;
  at += needle.size();
  while (at < json.size() && json[at] == ' ') ++at;
  char* end = nullptr;
  const long value = std::strtol(json.c_str() + at, &end, 10);
  if (end == json.c_str() + at) return fallback;
  return static_cast<int>(value);
}

/// First string element of `"key":["a","b"]`. The loopback payloads are
/// small and fixed-shape, so a flat scan is enough here (the same approach as
/// json_text/json_flag above).
std::string json_first_array_text(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":[";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return std::string();
  const size_t quote = json.find('"', at + needle.size());
  if (quote == std::string::npos) return std::string();
  const size_t end = json.find('"', quote + 1);
  if (end == std::string::npos) return std::string();
  return json.substr(quote + 1, end - quote - 1);
}

/// Pull the host's security panel from the local core. Failures keep the
/// built-in defaults (the panel is also the app's first-run state).
void load_host_settings() {
  std::string body;
  int status = 0;
  if (local_api_request(L"GET", L"/api/settings", &body, &status) &&
      !body.empty()) {
    g.sec_allow_remote =
        json_flag(body, "allow_remote_access", g.sec_allow_remote);
    g.sec_allow_web = json_flag(body, "allow_web_client", g.sec_allow_web);
    g.sec_discovery = json_flag(body, "discovery_enabled", g.sec_discovery);
    g.sec_access_requests =
        json_flag(body, "allow_access_requests", g.sec_access_requests);
    g.sec_encrypt_remote =
        json_flag(body, "encrypt_remote", g.sec_encrypt_remote);
    g.sec_encrypt_lan = json_flag(body, "encrypt_lan", g.sec_encrypt_lan);
    const std::string name = json_text(body, "device_name");
    if (!name.empty()) g.set_device_name = wide_of(name);
  }
  std::string encoding;
  if (local_api_request(L"GET", L"/api/encoding", &encoding, &status) &&
      !encoding.empty()) {
    const std::string mode = json_text(encoding, "video_mode");
    if (mode == "stream") g.video_mode = 0;
    else if (mode == "mixed") g.video_mode = 1;
    else if (mode == "tiles") g.video_mode = 2;
  }
}

/// Refresh the self card from the local core.
///
/// The card used to be mock data: every build showed the same device id
/// (BDV-NCK-9DN), the same code (482913) and an unconditional "ID 服务器已连接"
/// chip. That is why several installed native clients looked like one device
/// and why the directory had never seen "BDV-NCK-9DN". The id shown is the
/// directory's display id once registered (what a remote user types), and the
/// local identity before that; the code is the host's real temporary code.
void load_device_identity() {
  std::string device_body;
  int status = 0;
  const bool have_device =
      local_api_request(L"GET", L"/api/device", &device_body, &status) &&
      !device_body.empty();

  std::string state_body;
  const bool have_state =
      local_api_request(L"GET", L"/api/state", &state_body, &status) &&
      !state_body.empty();

  std::wstring id = g.device_id;
  std::wstring code = g.device_code;
  std::wstring address = g.device_ip;
  bool registered = false;
  bool online = false;
  bool server_enabled = true;
  std::wstring error;
  int remaining = 0;
  int uses = 0;
  bool budget_known = false;

  if (have_device) {
    const std::string local_id = json_text(device_body, "device_id");
    const std::string local_code = json_text(device_body, "value");
    const std::string local_address =
        json_first_array_text(device_body, "addresses");
    const int port = json_int(device_body, "port", 0);
    id = wide_of(local_id);
    code = wide_of(local_code);
    address = local_address.empty()
                  ? std::wstring()
                  : wide_of(local_address) +
                        (port > 0 ? L":" + std::to_wstring(port) : L"");
    remaining = json_int(device_body, "remaining", 0);
    uses = json_int(device_body, "uses", 0);
    budget_known = remaining > 0 || uses > 0;
    // Seed the single/multi switch from the host once; afterwards the user's
    // choice drives the next "change code" request and must not be reset by
    // the 5 s refresh.
    if (!g.code_budget_known) {
      g.auth_multi = json_text(device_body, "mode") != "single";
    }
  }

  std::wstring id_status;
  if (have_state) {
    registered = json_flag(state_body, "registered", false);
    online = json_flag(state_body, "online", false);
    server_enabled = json_flag(state_body, "enabled", true);
    const std::string display_id = json_text(state_body, "display_id");
    if (registered && !display_id.empty()) id = wide_of(display_id);
    error = wide_of(json_text(state_body, "last_error"));
    if (!server_enabled) {
      id_status = L"ID 服务器已关闭";
    } else if (registered && online) {
      id_status = L"ID 服务器已连接";
    } else if (registered) {
      id_status = L"ID 服务器重连中";
    } else {
      id_status = L"ID 服务器未连接";
    }
  } else if (!have_device) {
    id_status = L"本机核心未运行";
    id.clear();
    code.clear();
    address.clear();
  } else {
    id_status = L"ID 服务器状态不可用";
  }

  const bool changed =
      id != g.device_id || code != g.device_code || address != g.device_ip ||
      registered != g.id_registered || online != g.id_online ||
      server_enabled != g.id_server_enabled || id_status != g.id_status ||
      error != g.id_error || remaining != g.code_remaining ||
      uses != g.code_uses || budget_known != g.code_budget_known;
  g.device_id = std::move(id);
  g.device_code = std::move(code);
  g.device_ip = std::move(address);
  g.id_registered = registered;
  g.id_online = online;
  g.id_server_enabled = server_enabled;
  g.id_status = std::move(id_status);
  g.id_error = std::move(error);
  g.code_remaining = remaining;
  g.code_uses = uses;
  g.code_budget_known = budget_known;
  if (changed) {
    native_log(L"identity: " + g.id_status +
               (g.id_error.empty() ? std::wstring()
                                   : L" | " + g.id_error));
  }
  if (changed && g.window != nullptr) {
    InvalidateRect(g.window, nullptr, FALSE);
  }
}

/// Ask the core for a fresh temporary code; the card then reloads it from
/// /api/device. Without a core there is nothing to rotate, so say so instead
/// of inventing one locally (the old refresh button did exactly that).
bool refresh_device_code(std::wstring* error) {
  std::wstring path = L"/api/auth/code?mode=";
  path += g.auth_multi ? L"multi" : L"single";
  int status = 0;
  const bool ok = local_api_request(L"POST", path, nullptr, &status);
  if (!ok) {
    if (error != nullptr) {
      *error = status == 0 ? L"本机核心未运行，验证码未更换"
                           : L"主机拒绝了这次修改";
    }
    return false;
  }
  load_device_identity();
  return true;
}

/// Push the whole security panel; the API takes every flag in one call and
/// applies the remote-access switch immediately.
bool save_host_settings(std::wstring* error) {
  std::wstring path = L"/api/settings?allow_remote=";
  path += g.sec_allow_remote ? L"1" : L"0";
  path += g.sec_allow_web ? L"&allow_web=1" : L"&allow_web=0";
  path += g.sec_discovery ? L"&discovery=1" : L"&discovery=0";
  path += g.sec_access_requests ? L"&access_requests=1" : L"&access_requests=0";
  path += g.sec_encrypt_remote ? L"&encrypt_remote=1" : L"&encrypt_remote=0";
  path += g.sec_encrypt_lan ? L"&encrypt_lan=1" : L"&encrypt_lan=0";
  int status = 0;
  if (!local_api_request(L"POST", path, nullptr, &status)) {
    if (error != nullptr) {
      *error = status == 0 ? L"本机核心未运行，设置未保存"
                           : L"主机拒绝了这次修改";
    }
    return false;
  }
  load_host_settings();
  return true;
}

/// Newest sequence the core has published to the shared surface (0 when the
/// surface is not open). Defined next to surface_poll.
uint64_t surface_sequence_now();

void start_connect(const std::wstring& target, const std::wstring& code) {
  if (g.connecting) return;
  if (g.session != nullptr) {
    g.session->stop();
    g.session.reset();
  }
  g.session_was_live = false;
  g.access_notice_shown = false;
  g.intent_applied = false;
  g.seen_session_error.clear();
  // A reconnect keeps the last decoded frame on screen (the web leaves the
  // final picture under its disconnected overlay); a fresh connect clears it.
  if (!g.reconnecting) {
    g.stream_seq = 0;
    if (g.stream_bitmap != nullptr) {
      g.stream_bitmap->Release();
      g.stream_bitmap = nullptr;
    }
    g.stream_w = g.stream_h = 0;
    // The shared surface still holds the last frame of the previous session
    // (the core only overwrites it when the new host sends a stream). Drop the
    // bitmap and ignore the buffer until a newer sequence arrives, otherwise a
    // tile-only host stays hidden behind the old desktop while input already
    // reaches the new machine.
    if (g.surface_bitmap != nullptr) {
      g.surface_bitmap->Release();
      g.surface_bitmap = nullptr;
    }
    g.surface_w = g.surface_h = 0;
    g.surface_at = 0;
    g.stream_at = 0;
    g.surface_hold = true;
    g.surface_hold_seq = surface_sequence_now();
  }
  g.connecting = true;
  g.retry_at = 0;
  const unsigned generation = ++g.connect_generation;
  g.session_label = target;
  g.connect_target = target;
  g.connect_code = code;
  g.retry_code.clear();
  g.retry_caret = 0;

  const bool target_is_id = looks_like_device_id(target);
  const std::string target_utf8 = utf8_of(target);
  std::string host = target_utf8;
  int port = 28418;
  if (!target_is_id) {
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
      const std::string port_text = host.substr(colon + 1);
      bool numeric = !port_text.empty();
      for (char c : port_text) {
        if (c < '0' || c > '9') numeric = false;
      }
      if (numeric) {
        port = std::atoi(port_text.c_str());
        host = host.substr(0, colon);
      }
    }
  }
  const std::string token = utf8_of(code);
  const bool proxy = g.proxy_mode;
  // A running core with a published surface decodes streams; always talk to
  // it (never dial the host directly) so the surface gets the picture.
  const bool surface_mode =
      !g.surface_path.empty() && std::getenv("MOLINK_CORE_TOKEN") != nullptr;
  std::thread([host, port, token, proxy, target_is_id, target_utf8,
               generation, surface_mode] {
    auto* client = new molink::net::session_client();
    molink::net::session_endpoint endpoint;
    endpoint.host = host;
    endpoint.port = port;
    endpoint.token = token;
    endpoint.direct_session = !proxy;
    if (target_is_id) {
      endpoint.host.clear();
      endpoint.port = 0;
      endpoint.target = target_utf8;
    }
    std::string error;
    bool ok = false;
    native_log(L"connect: target=" + std::wstring(host.begin(), host.end()) +
               L" proxy=" + (proxy ? L"1" : L"0") +
               L" id=" + (target_is_id ? L"1" : L"0") +
               L" surface_env=" + (surface_mode ? L"1" : L"0") +
               L" core_started=" + (g.local_core_started ? L"1" : L"0"));
    auto* result = new connect_result();
    result->client = client;
    result->generation = generation;
    if (proxy) {
      if (!g.local_core_started) {
        int core_port = 0;
        std::string client_token;
        std::wstring surface;
        if (!start_local_core(&core_port, &client_token, &surface, &error)) {
          result->error = wide_of(error);
          if (!PostMessageW(g.window, WM_APP_CONNECT_DONE, 0,
                            reinterpret_cast<LPARAM>(result))) {
            delete result->client;
            delete result;
          }
          return;
        }
        if (!surface.empty()) {
          auto* path = new std::wstring(surface);
          if (!PostMessageW(g.window, WM_APP_SURFACE_READY, 0,
                            reinterpret_cast<LPARAM>(path))) {
            delete path;
          }
        }
      }
      client->set_request_tiles(false);
      // The local core publishes the decoded picture on the shared surface;
      // without this the hello claimed a tile-only viewer and the host never
      // produced a stream (the proxy branch used to leave this at false, so a
      // `--proxy`/`--core` session stayed blank while input worked).
      if (!g.surface_path.empty()) {
        client->set_stream_decoding(g.core_stream_decoders);
      }
      endpoint.client_token = g.local_core_token;
      ok = client->start("127.0.0.1", g.local_core_port, endpoint, &g.stats,
                         &error);
    } else {
      // Prefer the installed local core (127.0.0.1:28418): it resolves device
      // IDs, picks the P2P/relay transport and proxies the session. A direct
      // host:port connection still works when the core is not running.
      molink::net::session_endpoint proxied = endpoint;
      proxied.direct_session = false;
      // An already-running core can publish decoded frames on its shared
      // surface; MOLINK_CORE_TOKEN (from its ready line) is then required by
      // /ws/direct, and tiles are pointless because the surface wins.
      const char* core_token = std::getenv("MOLINK_CORE_TOKEN");
      if (core_token != nullptr && *core_token != '\0') {
        proxied.client_token = core_token;
      }
      client->set_stream_decoding(surface_mode && g.core_stream_decoders);
      client->set_request_tiles(!surface_mode);
      if (surface_mode) {
        // The core decodes AV1/VP9/VP8 into its surface; any target must be
        // dialled through the core, not directly.
        ok = client->start("127.0.0.1", local_core_port(), proxied, &g.stats,
                           &error);
        if (!ok && error.empty()) {
          error = "local core is not running; the surface needs it";
        }
      } else if (!target_is_id) {
        // Prefer the local core: it decodes VP9/VP8/AV1 into the shared
        // surface, which is what lets this viewer use the video streams the
        // web offers. A direct dial only carries tiles (the menu then has
        // nothing but 平铺); it stays as the fallback so a host:port still
        // works when the core is missing or cannot reach the host.
        bool core_ready = !g.force_direct && g.local_core_started;
        if (!core_ready && !g.force_direct) {
          int core_port = 0;
          std::string client_token;
          std::wstring surface;
          std::string core_error;
          if (start_local_core(&core_port, &client_token, &surface,
                               &core_error)) {
            core_ready = true;
            if (!surface.empty()) {
              auto* path = new std::wstring(surface);
              if (!PostMessageW(g.window, WM_APP_SURFACE_READY, 0,
                                reinterpret_cast<LPARAM>(path))) {
                delete path;
              }
            }
          }
        }
        g.core_connect = core_ready;
        if (core_ready) {
          client->set_stream_decoding(g.core_stream_decoders);
          client->set_request_tiles(false);
          proxied.client_token = g.local_core_token;
          // The core we started listens on its ephemeral ready-line port;
          // local_core_port() is only the MOLINK_CORE_PORT override.
          const int core_port_use =
              g.local_core_port > 0 ? g.local_core_port : local_core_port();
          ok = client->start("127.0.0.1", core_port_use, proxied, &g.stats,
                             &error);
        }
        if (!ok) {
          // Direct dial: the host's own answer (wrong code, locked) reaches
          // the tab instead of being swallowed by the core's relay switch.
          delete client;
          client = new molink::net::session_client();
          result->client = client;
          client->set_stream_decoding(false);
          client->set_request_tiles(true);
          error.clear();
          molink::net::session_endpoint direct = endpoint;
          direct.direct_session = true;
          direct.target.clear();
          ok = client->start(host, port, direct, &g.stats, &error);
          if (!ok) {
            error.clear();
            ok = client->start("127.0.0.1", local_core_port(), proxied,
                               &g.stats, &error);
          }
        }
      } else {
        // Device IDs only resolve through the local core's ID-server lookup.
        // After the core path fails before the welcome, fall back to the
        // host's published address directly: that is the same /ws/session
        // endpoint the web page uses, so it does not depend on the core's
        // P2P/relay transports. It carries tiles (the shell decodes them).
        const std::wstring lookup_path =
            L"/api/directory/device?id=" +
            std::wstring(target_utf8.begin(), target_utf8.end());
        std::string lookup;
        int lookup_status = 0;
        bool direct_ok = false;
        // Only after the core path failed before the welcome (the retry sets
        // force_direct): the direct dial is tiles-only, so it must not take
        // precedence over the core's stream decoding.
        if (g.force_direct &&
            local_api_request(L"GET", lookup_path, &lookup, &lookup_status) &&
            !lookup.empty()) {
          const std::string address = json_text(lookup, "address");
          const int port = json_int(lookup, "port", 0);
          if (!address.empty() && port > 0) {
            client->set_stream_decoding(false);
            client->set_request_tiles(true);
            molink::net::session_endpoint direct = endpoint;
            direct.host = address;
            direct.port = port;
            direct.direct_session = true;
            direct.target.clear();
            native_log(L"connect: direct " + wide_of(address) + L":" +
                       std::to_wstring(port));
            direct_ok = client->start(address, port, direct, &g.stats, &error);
          }
        }
        if (direct_ok) {
          g.core_connect = false;
          ok = true;
        } else {
          delete client;
          client = new molink::net::session_client();
          result->client = client;
          // The core decodes the stream and publishes the shared surface, so
          // the hello must advertise those decoders: without this the host
          // treats the viewer as tiles-only and a stream-mode host shows a
          // blank picture.
          client->set_stream_decoding(g.core_stream_decoders);
          client->set_request_tiles(false);
          proxied.client_token = g.local_core_token;
          g.core_connect = true;
          ok = client->start("127.0.0.1", local_core_port(), proxied, &g.stats,
                             &error);
          if (!ok) error = "local core is not running; device IDs need it";
        }
      }
    }
    if (!ok && !error.empty()) result->error = wide_of(error);
    if (!PostMessageW(g.window, WM_APP_CONNECT_DONE, ok ? 1 : 0,
                      reinterpret_cast<LPARAM>(result))) {
      delete result->client;
      delete result;
    }
  }).detach();
}

/// Shared by the ConnectCard button and the Enter key.
void start_home_connect() {
  if (g.connecting) {
    show_toast(L"正在连接中…");
    return;
  }
  if (g.address_text.empty()) {
    g.connect_error = L"请输入有效的设备 ID 或地址";
    show_toast(g.connect_error);
    return;
  }
  g.connect_error.clear();
  // A manual connect supersedes any automatic retry cycle.
  g.reconnecting = false;
  g.session_accepted = false;
  g.retry_attempt = 0;
  g.retry_at = 0;
  start_connect(g.address_text, g.code_text);
  show_toast(g.code_text.empty() ? L"已发送连接请求，等待对方确认"
                                 : L"正在连接 " + g.address_text + L" …");
  g.focus_field = 0;
}

std::wstring recents_path() {
  wchar_t buffer[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH) == 0) {
    return std::wstring();
  }
  std::wstring dir = std::wstring(buffer) + L"\\moLink";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\ui-recents.txt";
}

void save_recents() {
  const std::wstring path = recents_path();
  if (path.empty()) return;
  std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!file) return;
  for (const app_state::recent_entry& entry : g.recents) {
    std::wstring label = entry.label, target = entry.target;
    for (wchar_t& c : label) if (c == L'\t' || c == L'\n') c = L' ';
    for (wchar_t& c : target) if (c == L'\t' || c == L'\n') c = L' ';
    std::wstring token = entry.token;
    for (wchar_t& c : token) if (c == L'\t' || c == L'\n') c = L' ';
    std::wstring note = entry.note;
    for (wchar_t& c : note) if (c == L'\t' || c == L'\n') c = L' ';
    std::wstring avatar = entry.avatar;
    for (wchar_t& c : avatar) if (c == L'\t' || c == L'\n') c = L' ';
    file << utf8_of(label) << '\t' << utf8_of(target) << '\t'
         << utf8_of(entry.platform) << '\t' << entry.at_ms << '\t'
         << (entry.online ? 1 : 0) << '\t' << utf8_of(token) << '\t'
         << (entry.saved ? 1 : 0) << '\t' << utf8_of(note) << '\t'
         << utf8_of(avatar) << '\n';
  }
}

void load_recents() {
  g.recents.clear();
  const std::wstring path = recents_path();
  if (path.empty()) return;
  std::ifstream file(path.c_str(), std::ios::binary);
  if (!file) return;
  std::string line;
  while (std::getline(file, line) && g.recents.size() < 8) {
    // label, target, platform, at_ms, online, token, saved, note, avatar
    // (columns were added over time; older rows still load).
    std::string parts[9];
    size_t index = 0;
    for (char c : line) {
      if (c == '\t' && index < 8) {
        ++index;
      } else {
        parts[index] += c;
      }
    }
    app_state::recent_entry entry;
    entry.label = wide_of(parts[0]);
    entry.target = wide_of(parts[1]);
    entry.platform = parts[2].empty() ? L"windows" : wide_of(parts[2]);
    entry.at_ms = std::strtoull(parts[3].c_str(), nullptr, 10);
    entry.online = parts[4] == "1";
    entry.token = wide_of(parts[5]);
    entry.saved = parts[6] == "1";
    entry.note = wide_of(parts[7]);
    entry.avatar = wide_of(parts[8]);
    if (entry.target.empty()) continue;
    // Older ui-recents.txt files (and older builds) could hold the same
    // target more than once; keep only the newest row for each target.
    bool duplicate = false;
    for (const app_state::recent_entry& existing : g.recents) {
      if (existing.target == entry.target) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) g.recents.push_back(entry);
  }
  if (g.recents.size() > 8) g.recents.resize(8);
}

void remember_recent(const std::wstring& label, const std::wstring& target,
                     const std::wstring& token, bool online) {
  if (target.empty()) return;
  app_state::recent_entry entry;
  entry.label = label.empty() ? target : label;
  entry.target = target;
  entry.token = token;
  // An access-request connection has no code; keep a code saved earlier
  // rather than wiping it.
  if (entry.token.empty()) {
    for (const app_state::recent_entry& item : g.recents) {
      if (item.target == target) {
        entry.token = item.token;
        break;
      }
    }
  }
  entry.platform = L"windows";
  entry.online = online;
  entry.at_ms = static_cast<uint64_t>(std::time(nullptr)) * 1000;
  // Re-connecting to a known host keeps its star and its custom name.
  for (const app_state::recent_entry& item : g.recents) {
    if (item.target != target) continue;
    entry.saved = item.saved;
    entry.note = item.note;
    entry.avatar = item.avatar;
    break;
  }
  g.recents.erase(
      std::remove_if(g.recents.begin(), g.recents.end(),
                     [&](const app_state::recent_entry& item) {
                       return item.target == target;
                     }),
      g.recents.end());
  g.recents.insert(g.recents.begin(), entry);
  if (g.recents.size() > 8) g.recents.resize(8);
  save_recents();
}

void mark_recent_offline(const std::wstring& target) {
  for (app_state::recent_entry& entry : g.recents) {
    if (entry.target == target) {
      entry.online = false;
      save_recents();
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Rename a recent/saved row: the web keeps a per-device note next to the
// saved access code, and the star decides whether the row is in 已保存.
// Drawn as an in-app modal: creating a second top-level window from this D2D
// UI failed under Wine with error 1400.
// ---------------------------------------------------------------------------

void rename_trim(std::wstring* text) {
  while (!text->empty() && (text->front() == L' ' || text->front() == L'\t')) {
    text->erase(text->begin());
  }
  while (!text->empty() && (text->back() == L' ' || text->back() == L'\t')) {
    text->pop_back();
  }
}

/// Preset avatar key -> colour (the web's AVATAR_PRESETS).
D2D1_COLOR_F avatar_tint(const std::wstring& avatar) {
  if (avatar == L"preset:teal") return D2D1::ColorF(0x2dd4bf);
  if (avatar == L"preset:blue") return D2D1::ColorF(0x60a5fa);
  if (avatar == L"preset:purple") return D2D1::ColorF(0xa78bfa);
  if (avatar == L"preset:amber") return D2D1::ColorF(0xfbbf24);
  if (avatar == L"preset:rose") return D2D1::ColorF(0xfb7185);
  if (avatar == L"preset:slate") return D2D1::ColorF(0x94a3b8);
  return g.theme != nullptr ? g.theme->accent : D2D1::ColorF(0x0f9488);
}

std::wstring avatar_initial(const std::wstring& name) {
  for (wchar_t c : name) {
    if (c == L' ' || c == L'\t') continue;
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
    return std::wstring(1, c);
  }
  return L"?";
}

void close_rename_dialog() {
  g.rename_open = false;
  g.rename_index = -1;
  g.focus_field = 0;
  clear_text_selection();
  InvalidateRect(g.window, nullptr, FALSE);
}

void apply_rename() {
  if (!g.rename_open) return;
  if (g.rename_index >= 0 &&
      g.rename_index < static_cast<int>(g.recents.size())) {
    std::wstring note = g.rename_text;
    rename_trim(&note);
    g.recents[g.rename_index].note = note;
    save_recents();
    show_toast(note.empty() ? L"已恢复设备名称" : L"已重命名");
  }
  close_rename_dialog();
}

void open_rename_dialog(int index) {
  if (index < 0 || index >= static_cast<int>(g.recents.size())) return;
  g.rename_index = index;
  g.rename_text = g.recents[index].note.empty() ? g.recents[index].label
                                                : g.recents[index].note;
  g.rename_caret = g.rename_text.size();
  g.rename_open = true;
  g.focus_field = 20;
  clear_text_selection();
  InvalidateRect(g.window, nullptr, FALSE);
}

void rename_click(float x, float y) {
  if (inside(g.hit_rename_ok, x, y)) {
    apply_rename();
    return;
  }
  if (inside(g.hit_rename_cancel, x, y)) {
    close_rename_dialog();
    return;
  }
  if (inside(g.hit_rename_field, x, y)) {
    g.focus_field = 20;
    g.rename_caret = g.rename_text.size();
    return;
  }
  // A click on the dimmed background cancels, like a modal sheet.
  close_rename_dialog();
}

void draw_rename_modal(float w, float h) {
  if (!g.rename_open || g.target == nullptr) return;
  ID2D1SolidColorBrush* scrim = nullptr;
  if (SUCCEEDED(g.target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f),
                                                &scrim)) &&
      scrim != nullptr) {
    g.target->FillRectangle(D2D1::RectF(0, 0, w, h), scrim);
    scrim->Release();
  }
  const float width = 380;
  const float height = 190;
  const D2D1_RECT_F card =
      D2D1::RectF((w - width) / 2, (h - height) / 2, (w + width) / 2,
                  (h + height) / 2);
  fill_round(g.target, card, 14, g.theme->surface);
  stroke_round(g.target, card, 14, g.theme->line);
  text(g.target, g.heading, L"重命名设备",
       D2D1::RectF(card.left + 20, card.top + 12, card.right - 20,
                   card.top + 40),
       g.theme->ink);
  g.hit_rename_field = D2D1::RectF(card.left + 20, card.top + 52,
                                   card.right - 20, card.top + 90);
  fill_round(g.target, g.hit_rename_field, 8, g.theme->panel);
  stroke_round(g.target, g.hit_rename_field, 8,
               g.focus_field == 20 ? g.theme->accent : g.theme->line);
  text(g.target, g.small_left, g.rename_text,
       D2D1::RectF(card.left + 32, card.top + 52, card.right - 32,
                   card.top + 90),
       g.rename_text.empty() ? g.theme->muted : g.theme->ink);
  if (g.focus_field == 20 && g.caret_on) {
    const float cx =
        card.left + 32 +
        measure_text(g.small_left, g.rename_text.substr(0, g.rename_caret));
    line(g.target, cx, card.top + 57, cx, card.top + 85, g.theme->accent, 1.5f);
  }
  g.hit_rename_ok = D2D1::RectF(card.right - 20 - 84, card.bottom - 46,
                                card.right - 20, card.bottom - 14);
  fill_round(g.target, g.hit_rename_ok, 8, g.theme->accent);
  text(g.target, g.small, L"保存", g.hit_rename_ok, g.theme->on_accent);
  g.hit_rename_cancel = D2D1::RectF(card.right - 20 - 84 - 8 - 84,
                                    card.bottom - 46, card.right - 20 - 92,
                                    card.bottom - 14);
  fill_round(g.target, g.hit_rename_cancel, 8, g.theme->panel);
  stroke_round(g.target, g.hit_rename_cancel, 8, g.theme->line);
  text(g.target, g.small, L"取消", g.hit_rename_cancel, g.theme->ink);
  text(g.target, g.tiny, L"清空可恢复主机名",
       D2D1::RectF(card.left + 20, card.bottom - 40, card.right - 220,
                   card.bottom - 18),
       g.theme->muted);
}

/// Position in the current tab -> index into g.recents.
int recent_index_at(int position) {
  int seen = 0;
  for (int i = 0; i < static_cast<int>(g.recents.size()); ++i) {
    if (g.device_tab == 0 || (g.device_tab == 1 && g.recents[i].saved)) {
      if (seen == position) return i;
      ++seen;
    }
  }
  return -1;
}

bool recent_click(float x, float y) {
  for (int i = 0; i < 4; ++i) {
    const int idx = recent_index_at(i);
    if (idx < 0) continue;
    if (inside(g.hit_recent_forget[i], x, y)) {
      g.recents.erase(g.recents.begin() + idx);
      save_recents();
      show_toast(L"已从最近连接中移除");
      InvalidateRect(g.window, nullptr, FALSE);
      return true;
    }
    if (inside(g.hit_recent_star[i], x, y)) {
      g.recents[idx].saved = !g.recents[idx].saved;
      save_recents();
      show_toast(g.recents[idx].saved ? L"已保存到「已保存」"
                                      : L"已取消保存");
      InvalidateRect(g.window, nullptr, FALSE);
      return true;
    }
    if (inside(g.hit_recent_edit[i], x, y)) {
      show_toast(L"正在打开重命名…");
      open_rename_dialog(idx);
      return true;
    }
  }
  for (int i = 0; i < 4; ++i) {
    const int idx = recent_index_at(i);
    if (idx < 0) continue;
    for (int j = 0; j < 3; ++j) {
      if (inside(g.hit_recent_action[i][j], x, y)) {
        g.address_text = g.recents[idx].target;
        g.code_text = g.recents[idx].token;
        g.intent = j;
        g.focus_field = 0;
        clear_text_selection();
        start_home_connect();
        return true;
      }
    }
  }
  for (int i = 0; i < 4; ++i) {
    const int idx = recent_index_at(i);
    if (idx < 0) continue;
    if (inside(g.hit_recent_card[i], x, y)) {
      const ULONGLONG now = GetTickCount64();
      const bool double_click =
          g.last_recent_click == i &&
          now - g.last_recent_click_ms <= GetDoubleClickTime();
      g.last_recent_click = i;
      g.last_recent_click_ms = now;
      // Web's prefill: fill the address and saved code, then focus the code
      // field so the current code can be typed (or Enter connects).
      g.address_text = g.recents[idx].target;
      g.code_text = g.recents[idx].token;
      g.focus_field = 2;
      g.code_caret = g.code_text.size();
      g.address_caret = g.address_text.size();
      g.connect_error.clear();
      clear_text_selection();
      if (double_click) {
        // 双击直接连：no code typed means the host asks the person at the
        // machine to approve, which is what a saved access-request row means.
        start_home_connect();
      } else {
        show_toast(L"已填入连接框，双击直接连接");
      }
      InvalidateRect(g.window, nullptr, FALSE);
      return true;
    }
  }
  return false;
}

/// The web renders `error.<code>` strings; map the host's codes to the same
/// user-facing Chinese text instead of showing "code: message" raw.
std::wstring friendly_session_error(const std::wstring& code,
                                    const std::wstring& raw) {
  if (code == L"invalid_code" || code == L"unauthorized") {
    return L"验证码错误或已失效";
  }
  if (code == L"locked") return L"主机已锁定，暂时无法连接";
  if (code == L"access_denied") return L"对方拒绝了本次连接请求";
  if (code == L"too_many_attempts") return L"尝试次数过多，请稍后再试";
  if (code == L"ip_banned") return L"当前 IP 已被禁止连接";
  if (code == L"display_mode_busy") return L"主机当前无法切换显示模式";
  if (code == L"display_mode_failed") return L"显示模式切换失败";
  return raw.empty() ? L"主机返回了错误" : raw;
}

bool auth_error_code(const std::wstring& code) {
  return code == L"invalid_code" || code == L"unauthorized";
}

/// Banner actions: with a wrong code, retry with the inline field; otherwise
/// reconnect with what we already had (the web's "重新连接").
void retry_session_connect() {
  if (g.session == nullptr) return;
  const bool needs_code = auth_error_code(g.session->last_error_code());
  if (needs_code && g.retry_code.empty()) {
    g.focus_field = 11;
    show_toast(L"请输入验证码后重试");
    InvalidateRect(g.window, nullptr, FALSE);
    return;
  }
  if (g.connect_target.empty()) return;
  g.address_text = g.connect_target;
  if (needs_code) {
    g.code_text = g.retry_code;
  } else if (g.code_text.empty()) {
    g.code_text = g.connect_code;
  }
  clear_text_selection();
  g.focus_field = 0;
  start_home_connect();
}

/// Click routing for the session error bar. Returns true when consumed.
bool session_banner_click(float x, float y) {
  if (g.session == nullptr || g.session->authenticated() ||
      g.connect_error.empty()) {
    return false;
  }
  if (inside(g.hit_retry_input, x, y)) {
    g.focus_field = 11;
    g.retry_caret = caret_for_field(11, x);
    g.sel_anchor = g.retry_caret;
    clear_text_selection();
    g.text_selecting = true;
    if (g.window != nullptr) SetCapture(g.window);
    return true;
  }
  if (inside(g.hit_retry_btn, x, y)) {
    retry_session_connect();
    return true;
  }
  return false;
}

std::wstring relative_time(uint64_t at_ms) {
  if (at_ms == 0) return std::wstring();
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr)) * 1000;
  const uint64_t diff = now > at_ms ? (now - at_ms) / 1000 : 0;
  if (diff < 60) return L"刚刚";
  if (diff < 3600) return std::to_wstring(diff / 60) + L" 分钟前";
  if (diff < 86400) return std::to_wstring(diff / 3600) + L" 小时前";
  if (diff < 172800) return L"昨天";
  return std::to_wstring(diff / 86400) + L" 天前";
}

std::wstring local_clipboard_text() {
  std::wstring text;
  if (!OpenClipboard(g.window)) return text;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data != nullptr) {
    const auto* raw = static_cast<const wchar_t*>(GlobalLock(data));
    if (raw != nullptr) {
      text = raw;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return text;
}

void show_toast(const std::wstring& text);

void clipboard_get() {
  session_send("{\"type\":\"clipboard\",\"action\":\"get\"}");
  show_toast(L"已请求远端剪贴板");
}

void clipboard_set() {
  const std::wstring text = local_clipboard_text();
  if (text.empty()) {
    show_toast(L"本地剪贴板为空");
    return;
  }
  session_send("{\"type\":\"clipboard\",\"action\":\"set\",\"text\":\"" +
               json_escape(text) + "\"}");
  show_toast(L"已发送到远端剪贴板");
}

void show_toast(const std::wstring& text) {
  g.toast_text = text;
  g.toast_until = GetTickCount64() + 2500;
}

void send_quality() {
  if (g.session == nullptr) return;
  g.session->send_quality(kPresetModes[g.preset], g.fps_value, g.qp,
                          g.bitrate_kbps, rate_mode_name(g.rate_mode),
                          format_name(g.format_index));
}

void menu_apply(int which, int index) {
  if (which == 0) {
    g.fps_index = index;
    // 自动 keeps the value the host reports; the others set it directly.
    if (index > 0) g.fps_value = (index == 1 ? 15 : (index == 2 ? 30 : 60));
    send_quality();
    return;
  }
  if (which == 1) {
    g.codec_index = index;
    static const char* codecs[] = {"auto", "tiles", "h264", "vp9", "vp8",
                                   "av1"};
    session_send("{\"type\":\"stream-codec\",\"value\":\"" +
                 std::string(codecs[index]) + "\"}");
    return;
  }
  if (which == 10) {
    g.settings_lock = index;
    return;
  }
  if (which == 11) {
    g.fps_index = index;
    if (index > 0) g.fps_value = (index == 1 ? 15 : (index == 2 ? 30 : 60));
    send_quality();
    return;
  }
  if (which == 2) {
    g.res_index = index;
    int width = 0, height = 0;
    if (swscanf(kResolutionChoices[index], L"%dx%d", &width, &height) == 2 &&
        width > 0 && height > 0) {
      session_send("{\"type\":\"display-mode\",\"width\":" +
                   std::to_string(width) + ",\"height\":" +
                   std::to_string(height) + "}");
    }
    return;
  }
  if (which == 3) {
    g.format_index = index;
    send_quality();
    return;
  }
  if (which == 4) {
    if (index == 0) {
      g.clipboard_on = !g.clipboard_on;
    } else if (index == 1) {
      clipboard_get();
    } else {
      clipboard_set();
    }
    return;
  }
  if (which == 5) {
    if (index == 0) {
      g.chat_open = true;
      g.files_open = false;
      g.focus_field = 3;
      g.chat_caret = g.chat_input.size();
      g.chat_unread = 0;
      g.open_menu = -1;
      g.quality_open = false;
    } else if (index == 1) {
      g.files_open = true;
      g.chat_open = false;
      if (g.focus_field == 3) g.focus_field = 0;
      g.open_menu = -1;
      g.quality_open = false;
    } else if (index == 2) {
      g.control_on = !g.control_on;
    } else {
      session_send("{\"type\":\"input\",\"k\":\"sas\"}");
      show_toast(L"已发送 Ctrl+Alt+Del");
    }
    return;
  }
  if (index == 0) {
    g.mode = g.theme == &kLight ? app_state::theme_mode::dark
                                : app_state::theme_mode::light;
    save_theme_mode();
  } else {
    g.settings_open = true;
    g.open_menu = -1;
    g.quality_open = false;
    g.focus_field = 0;
  }
}

std::wstring quality_label() {
  return quality_custom() ? L"自定义" : kPresets[g.preset];
}

std::wstring fps_label() {
  return g.fps_index >= 0 ? kFpsChoices[g.fps_index]
                          : std::to_wstring(g.fps_value) + L" fps";
}

std::wstring codec_label() {
  if (g.codec_index > 0) return kCodecChoices[g.codec_index];
  return g.stats.codec.empty() || g.stats.codec == L"-" ? L"自动"
                                                        : g.stats.codec;
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

constexpr uint32_t kSurfaceMagic = 0x5255534d;
struct surface_header {
  uint32_t magic, version, width, height, stride, format;
  uint64_t sequence, timestamp_us;
  uint32_t slot_bytes, reserved[5];
};

/// Opens the core's frame surface. Returns false and logs why on failure.
bool surface_open(const std::wstring& path) {
  // The old log lived at a developer's absolute path, so a failed surface map
  // on a customer machine left no trace at all. Use the shell's own session
  // log, next to the connect/drop diagnostics.
  auto note = [](const char* text) {
    native_log(L"surface: " + wide_of(text));
  };
  for (int attempt = 0; attempt < 20; ++attempt) {
    g.surface_file = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g.surface_file != INVALID_HANDLE_VALUE) break;
    note("CreateFile failed, retrying");
    Sleep(100);
  }
  if (g.surface_file == INVALID_HANDLE_VALUE) {
    note("CreateFile gave up");
    return false;
  }
  note("file open");
  g.surface_mapping = CreateFileMappingW(g.surface_file, nullptr, PAGE_READONLY,
                                         0, 0, nullptr);
  if (g.surface_mapping == nullptr) {
    note("CreateFileMapping failed");
    return false;
  }
  g.surface_base =
      static_cast<uint8_t*>(MapViewOfFile(g.surface_mapping, FILE_MAP_READ, 0, 0,
                                          0));
  note(g.surface_base != nullptr ? "mapped" : "MapViewOfFile failed");
  return g.surface_base != nullptr;
}

uint64_t surface_sequence_now() {
  if (g.surface_base == nullptr) return 0;
  const auto* header = reinterpret_cast<const surface_header*>(g.surface_base);
  if (header->magic != kSurfaceMagic) return 0;
  // The shell maps the surface read-only (FILE_MAP_READ). An interlocked
  // compare-exchange is a *write* even with equal operands, so it faults on
  // that mapping. Use the same acquire-ordered plain read as the Linux shell;
  // the core's interlocked store keeps the published value coherent, and a
  // torn 64-bit read can only pick the other double-buffer slot for one poll,
  // which the next frame corrects.
  const uint64_t sequence = header->sequence;
  std::atomic_thread_fence(std::memory_order_acquire);
  return sequence;
}

/// Copies the newest published frame into the D2D bitmap.
void surface_poll() {
  if (g.surface_base == nullptr || g.target == nullptr) return;
  auto* header = reinterpret_cast<surface_header*>(g.surface_base);
  if (header->magic != kSurfaceMagic || header->width == 0) return;
  // Read-only mapping: a plain acquire-ordered load (see surface_sequence_now).
  const uint64_t sequence = header->sequence;
  std::atomic_thread_fence(std::memory_order_acquire);
  if (g.surface_hold) {
    // Still the previous session's picture: wait for the core to publish for
    // the session we just started. A tile-only host never publishes, so this
    // hold keeps its tiles visible instead of an old remote desktop.
    if (sequence == g.surface_hold_seq) return;
    g.surface_hold = false;
  }
  if (sequence == g.surface_seq) return;
  // Copying a 1920x1080 frame plus the scaled draw is expensive under a
  // software rasterizer; cap it near 30 fps instead of once per 16 ms tick.
  const ULONGLONG now = GetTickCount64();
  if (g.surface_copy_at != 0 && now - g.surface_copy_at < 30) return;
  g.surface_copy_at = now;
  const uint8_t* pixels = g.surface_base + 64 +
                          static_cast<size_t>(sequence & 1) * header->slot_bytes;
  if (g.surface_bitmap == nullptr || g.surface_w != header->width ||
      g.surface_h != header->height) {
    if (g.surface_bitmap != nullptr) g.surface_bitmap->Release();
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const HRESULT hr = g.target->CreateBitmap(
        D2D1::SizeU(header->width, header->height), pixels,
        header->width * 4, props, &g.surface_bitmap);
    if (FAILED(hr) && std::getenv("MOLINK_WS_LOG") != nullptr) {
      std::fprintf(stderr, "surface bitmap create failed hr=0x%08lx\n",
                   static_cast<unsigned long>(hr));
    }
    g.surface_w = header->width;
    g.surface_h = header->height;
  } else {
    g.surface_bitmap->CopyFromMemory(nullptr, pixels, header->width * 4);
  }
  g.surface_seq = sequence;
  g.surface_at = now;
  // The shell never sees the stream frames themselves; a surface publish is
  // its proof that the session has a picture. Without this the session-client
  // bootstrap asks the host for one forever (it only tracks tile decodes).
  if (g.session != nullptr) g.session->note_surface_frame();
  // The watermark shows the delivered frame rate; surface frames count too.
  g.stats.note_frame(GetTickCount64());
  if (g.session != nullptr) g.session->note_displayed_frame();
  // Repaint for every new surface frame. Without this the picture only jumps
  // when some unrelated hover/toast invalidates the window, which reads as
  // stutter even though frames are arriving.
  if (g.window != nullptr) InvalidateRect(g.window, nullptr, FALSE);
}

/// Loads an image through WIC and keeps it as a D2D bitmap for the avatar.
bool load_avatar(const std::wstring& path) {
  IWICImagingFactory* wic = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      wic == nullptr) {
    return false;
  }
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;
  bool ok = SUCCEEDED(wic->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(
                frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom));
  if (ok && g.target != nullptr) {
    UINT width = 0;
    UINT height = 0;
    converter->GetSize(&width, &height);
    ID2D1Bitmap* bitmap = nullptr;
    if (width > 0 && height > 0) {
      // Crop to a centred square and bake the 16 px (at 58 px display size)
      // rounded corners into the alpha. Wine's D2D ignores geometric layer
      // masks, so baking the mask keeps the avatar round everywhere.
      const UINT side = (std::min)(width, height);
      const UINT off_x = (width - side) / 2;
      const UINT off_y = (height - side) / 2;
      std::vector<uint8_t> source(static_cast<size_t>(width) * height * 4);
      std::vector<uint8_t> square(static_cast<size_t>(side) * side * 4);
      if (SUCCEEDED(converter->CopyPixels(
              nullptr, width * 4, static_cast<UINT>(source.size()),
              source.data()))) {
        for (UINT y = 0; y < side; ++y) {
          std::memcpy(square.data() + static_cast<size_t>(y) * side * 4,
                      source.data() +
                          (static_cast<size_t>(y + off_y) * width + off_x) * 4,
                      static_cast<size_t>(side) * 4);
        }
        const float radius = side * (16.0f / 58.0f);
        const float radius_sq = radius * radius;
        for (UINT y = 0; y < side; ++y) {
          for (UINT x = 0; x < side; ++x) {
            const bool left = x < radius;
            const bool right = x > side - radius;
            const bool top = y < radius;
            const bool bottom = y > side - radius;
            if ((!left && !right) || (!top && !bottom)) continue;
            const float ccx = left ? radius : side - radius;
            const float ccy = top ? radius : side - radius;
            const float dx = static_cast<float>(x) - ccx;
            const float dy = static_cast<float>(y) - ccy;
            if (dx * dx + dy * dy > radius_sq) {
              uint8_t* pixel =
                  square.data() + (static_cast<size_t>(y) * side + x) * 4;
              pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
            }
          }
        }
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        g.target->CreateBitmap(D2D1::SizeU(side, side), square.data(),
                               side * 4, props, &bitmap);
      }
    }
    if (bitmap != nullptr) {
      if (g.avatar_bitmap != nullptr) g.avatar_bitmap->Release();
      g.avatar_bitmap = bitmap;
      g.avatar_path = path;
    } else {
      ok = false;
    }
  }
  if (converter != nullptr) converter->Release();
  if (frame != nullptr) frame->Release();
  if (decoder != nullptr) decoder->Release();
  wic->Release();
  return ok;
}

std::wstring avatar_store_path() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD size = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
  if (size == 0) return std::wstring();
  std::wstring dir = std::wstring(buffer) + L"\\moLink";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\ui-avatar.bin";
}

void save_avatar_path(const std::wstring& path) {
  const std::wstring store = avatar_store_path();
  if (store.empty()) return;
  FILE* file = _wfopen(store.c_str(), L"wb");
  if (file == nullptr) return;
  const size_t length = path.size();
  std::fwrite(&length, sizeof(length), 1, file);
  std::fwrite(path.data(), sizeof(wchar_t), length, file);
  std::fclose(file);
}

bool load_saved_avatar() {
  const std::wstring store = avatar_store_path();
  if (store.empty()) return false;
  FILE* file = _wfopen(store.c_str(), L"rb");
  if (file == nullptr) return false;
  size_t length = 0;
  bool ok = false;
  if (std::fread(&length, sizeof(length), 1, file) == 1 && length > 0 &&
      length < 1024) {
    std::wstring path(length, L'\0');
    if (std::fread(path.data(), sizeof(wchar_t), length, file) == length) {
      ok = load_avatar(path);
    }
  }
  std::fclose(file);
  return ok;
}

/// The web keeps the toolbar pin in localStorage; the native shell uses the
/// same per-user preference in HKCU.
constexpr wchar_t kUiKey[] = L"Software\\moLink\\Ui";

bool load_pinned() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, kUiKey, L"ToolbarPinned",
                      RRF_RT_REG_DWORD, nullptr, &value, &size) ==
             ERROR_SUCCESS &&
         value != 0;
}

int load_theme_mode() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kUiKey, L"ThemeMode", RRF_RT_REG_DWORD,
                   nullptr, &value, &size) == ERROR_SUCCESS &&
      value <= 2) {
    return static_cast<int>(value);
  }
  return 0;
}

void save_theme_mode() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUiKey, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  const DWORD value = static_cast<DWORD>(g.mode);
  RegSetValueExW(key, L"ThemeMode", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

int load_close_behavior() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kUiKey, L"WindowCloseBehavior",
                   RRF_RT_REG_DWORD, nullptr, &value, &size) ==
          ERROR_SUCCESS &&
      value <= 2) {
    return static_cast<int>(value);
  }
  return 0;
}

void save_close_behavior(int behavior) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUiKey, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  const DWORD value = static_cast<DWORD>(behavior);
  RegSetValueExW(key, L"WindowCloseBehavior", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Notification area (the web shell's "minimize to tray" closes to the tray).
// ---------------------------------------------------------------------------

bool add_tray_icon() {
  if (g.tray_added) return true;
  if (g.tray_icon == nullptr) {
    g.tray_icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (g.tray_icon == nullptr) {
      g.tray_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
  }
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = WM_APP_TRAY;
  data.hIcon = g.tray_icon;
  wcscpy_s(data.szTip, L"moLink");
  if (Shell_NotifyIconW(NIM_ADD, &data)) {
    g.tray_added = true;
    return true;
  }
  return false;
}

void remove_tray_icon() {
  if (!g.tray_added) return;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &data);
  g.tray_added = false;
}

void show_main_window() {
  ShowWindow(g.window, SW_SHOW);
  if (IsIconic(g.window)) ShowWindow(g.window, SW_RESTORE);
  SetForegroundWindow(g.window);
  remove_tray_icon();
}

/// Hide the window and leave the app running in the notification area, like
/// the web shell's close-to-tray action (the session keeps running).
void minimize_to_tray() {
  // Without a notification area there would be no way back: keep the window
  // on the taskbar instead of hiding it forever.
  if (add_tray_icon()) {
    ShowWindow(g.window, SW_HIDE);
  } else {
    ShowWindow(g.window, SW_MINIMIZE);
  }
}

void show_tray_menu() {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return;
  AppendMenuW(menu, MF_STRING, 1, L"显示 moLink");
  AppendMenuW(menu, MF_STRING, 2, L"退出");
  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(g.window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, g.window,
                 nullptr);
  DestroyMenu(menu);
}

void save_pinned(bool pinned) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUiKey, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  const DWORD value = pinned ? 1 : 0;
  RegSetValueExW(key, L"ToolbarPinned", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

bool pick_avatar(HWND window) {
  wchar_t path[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = window;
  ofn.lpstrFilter = L"图片 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) return false;
  if (!load_avatar(path)) return false;
  save_avatar_path(path);
  return true;
}

/// Frame shared by every toolbar button: 1 px line border, panel tint on
/// hover/open and the web's accent-mixed hover border. `id` selects the
/// 140 ms hover transition.
void button_frame(ID2D1RenderTarget* t, const palette& th,
                  const D2D1_RECT_F& r, float radius, int id, bool hovered,
                  bool open) {
  const float h = hover_lerp(id, hovered || open);
  fill_round(t, r, radius,
             D2D1::ColorF(th.panel.r, th.panel.g, th.panel.b, h));
  stroke_round(t, r, radius, th.line);
  if (h > 0.01f) {
    stroke_round(t, r, radius,
                 D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b,
                              0.45f * h));
  }
  if (flash_active(hovered)) {
    fill_round(t, r, radius,
               D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.16f));
  }
}

/// Web `.btn.btn-quiet`: transparent fill, 1 px line border, radius 9.6.
/// `accent` is the web's `text-accent` state; disabled halves the opacity.
void quiet_icon_button(ID2D1RenderTarget* t, const palette& th,
                       const D2D1_RECT_F& r, int id, icon glyph, bool accent,
                       bool hovered, bool disabled = false) {
  const float h = hover_lerp(id, hovered && !disabled);
  button_frame(t, th, r, 9.6f, id, hovered, false);
  D2D1_COLOR_F color = accent ? th.accent : mix_color(th.muted, th.ink, h);
  if (disabled) {
    color = th.muted;
    color.a = 0.5f;
  }
  draw_icon(t, glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2, color);
}

/// Web `.btn.btn-primary`: accent fill, on-accent glyph (watermark on).
void primary_icon_button(ID2D1RenderTarget* t, const palette& th,
                         const D2D1_RECT_F& r, int id, icon glyph,
                         bool hovered) {
  const float h = hover_lerp(id, hovered);
  const D2D1_COLOR_F bg = mix_color(th.accent, th.ink, 0.12f * h);
  fill_round(t, r, 9.6f, bg);
  if (flash_active(hovered)) {
    fill_round(t, r, 9.6f, D2D1::ColorF(1, 1, 1, 0.14f));
  }
  draw_icon(t, glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2,
            th.on_accent);
}

/// Web `.btn.btn-quiet` carrying an icon and a label (the quality trigger).
void quiet_label_button(ID2D1RenderTarget* t, const palette& th,
                        IDWriteTextFormat* small_left, const D2D1_RECT_F& r,
                        int id, icon glyph, const std::wstring& label,
                        bool hovered, bool open) {
  const float h = hover_lerp(id, hovered || open);
  button_frame(t, th, r, 9.6f, id, hovered, open);
  draw_icon(t, glyph, r.left + 11, (r.top + r.bottom) / 2,
            mix_color(th.muted, th.ink, h));
  text(t, small_left, label,
       D2D1::RectF(r.left + 20, r.top, r.right - 4, r.bottom), th.ink);
}

/// Compact toolbar group button: icon plus chevron, opening a menu.
void menu_button(ID2D1RenderTarget* t, const palette& th,
                 const D2D1_RECT_F& r, int id, icon glyph, bool hovered,
                 bool open) {
  const float h = hover_lerp(id, hovered || open);
  button_frame(t, th, r, 9.6f, id, hovered, open);
  const float cy = (r.top + r.bottom) / 2;
  draw_icon(t, glyph, r.left + 12, cy, mix_color(th.muted, th.ink, h));
  draw_icon(t, icon::chevron_down, r.right - 10, cy, th.muted);
}

/// Toolbar display-mode button (适应 / 1:1 / 拉伸). Active is `.btn-primary`,
/// exactly like the web's display-mode group.
void display_button(ID2D1RenderTarget* t, const palette& th,
                    IDWriteTextFormat* small, IDWriteTextFormat* small_bold,
                    const D2D1_RECT_F& r, int id, const std::wstring& label,
                    bool active, bool hovered) {
  if (active) {
    fill_round(t, r, 9.6f, th.accent);
    if (flash_active(hovered)) {
      fill_round(t, r, 9.6f, D2D1::ColorF(1, 1, 1, 0.14f));
    }
    text(t, small_bold, label, r, th.on_accent);
    return;
  }
  const float h = hover_lerp(id, hovered);
  button_frame(t, th, r, 9.6f, id, hovered, false);
  text(t, small, label, r, mix_color(th.muted, th.ink, h));
}

/// Web `.chip`: pill, line border, panel tint, 11 px muted text.
void toolbar_chip(ID2D1RenderTarget* t, const palette& th,
                  IDWriteTextFormat* small, const D2D1_RECT_F& r,
                  const std::wstring& label) {
  fill_round(t, r, (r.bottom - r.top) / 2, th.panel);
  stroke_round(t, r, (r.bottom - r.top) / 2, th.line);
  text(t, small, label, r, th.muted);
}

/// Select chip: optional leading icon, label, trailing chevron.
void select_chip(ID2D1RenderTarget* t, const palette& th,
                 IDWriteTextFormat* small_left, const D2D1_RECT_F& r,
                 icon glyph, const std::wstring& label, bool hovered,
                 bool open, float radius) {
  (void)hovered;  // web selects have no hover tint; only the open state shows
  fill_round(t, r, radius, th.panel);
  stroke_round(t, r, radius, open ? th.accent : th.line);
  float left = r.left + 8;
  if (glyph != icon::none) {
    draw_icon(t, glyph, r.left + 12, (r.top + r.bottom) / 2, th.muted);
    left = r.left + 22;
  }
  text(t, small_left, label, D2D1::RectF(left, r.top, r.right - 16, r.bottom),
       th.ink);
  draw_icon(t, icon::chevron_down, r.right - 9, (r.top + r.bottom) / 2,
            th.muted);
}

void volume_slider(ID2D1RenderTarget* t, const palette& th,
                   const D2D1_RECT_F& r, float value) {
  const float cy = (r.top + r.bottom) / 2;
  const float left = r.left + 4, right = r.right - 4;
  fill_round(t, D2D1::RectF(left, cy - 2, right, cy + 2), 2, th.line_soft);
  const float knob = left + (right - left) * clampf(value, 0.0f, 1.0f);
  fill_round(t, D2D1::RectF(left, cy - 2, knob, cy + 2), 2, th.accent_strong);
  fill_round(t, D2D1::RectF(knob - 4, cy - 4, knob + 4, cy + 4), 4, th.ink);
}

/// Menu panel hanging under a select chip. The field rect is stored when the
/// menu opens, so the list stays put while the pointer moves over it.
D2D1_RECT_F menu_rect(const D2D1_RECT_F& field, int which, int count,
                      float window_w) {
  const bool action = which >= 4 && which < 10;
  const float width = action ? 200.0f
                             : (std::max)(field.right - field.left, 132.0f);
  float left = field.left;
  if (left + width > window_w - 8) left = window_w - 8 - width;
  if (left < 8) left = 8;
  return D2D1::RectF(left, field.bottom + 6, left + width,
                     field.bottom + 6 + 8 + count * 30);
}

int menu_hit(const D2D1_RECT_F& field, int which, int count, float window_w,
             float x, float y) {
  const D2D1_RECT_F menu = menu_rect(field, which, count, window_w);
  for (int i = 0; i < count; ++i) {
    const D2D1_RECT_F item = D2D1::RectF(menu.left + 4, menu.top + 4 + i * 30,
                                         menu.right - 4, menu.top + 34 + i * 30);
    if (inside(item, x, y)) return i;
  }
  return -1;
}

void draw_menu(ID2D1RenderTarget* t, const palette& th,
               IDWriteTextFormat* small_left, float window_w) {
  if (g.open_menu < 0) return;
  const int count = menu_count(g.open_menu);
  const D2D1_RECT_F menu = menu_rect(g.menu_field, g.open_menu, count, window_w);
  const float radius =
      (g.open_menu >= 4 && g.open_menu < 10) ? 6.0f : 10.0f;
  shadow(t, th, menu, radius);
  fill_round(t, menu, radius, th.surface);
  stroke_round(t, menu, radius, th.line);
  for (int i = 0; i < count; ++i) {
    const D2D1_RECT_F item = D2D1::RectF(menu.left + 4, menu.top + 4 + i * 30,
                                         menu.right - 4, menu.top + 34 + i * 30);
    const bool hovered = inside(item, g.hover_x, g.hover_y);
    const bool current = menu_current(g.open_menu) == i;
    const bool active = current || menu_active(g.open_menu, i);
    const bool disabled = menu_disabled(g.open_menu, i);
    if (current) {
      fill_round(t, item, 8, th.accent_soft);
    } else if (g.open_menu >= 4 && g.open_menu < 10) {
      const float h =
          hover_lerp(hv_menu_item0 + i, hovered && !disabled);
      fill_round(t, item, 8,
                 D2D1::ColorF(th.panel.r, th.panel.g, th.panel.b, h));
    } else if (hovered && !disabled) {
      fill_round(t, item, 8, th.panel);
    }
    D2D1_COLOR_F color = active ? th.accent : th.ink;
    if (disabled) color = th.muted;
    float text_left = item.left + 10;
    if (g.open_menu >= 4 && g.open_menu < 10) {
      icon glyph = icon::none;
      if (g.open_menu == 4) {
        glyph = i == 0 ? (g.clipboard_on ? icon::clipboard_check
                                         : icon::clipboard_x)
                       : (i == 1 ? icon::copy : icon::paste);
      } else if (g.open_menu == 5) {
        glyph = i == 0 ? icon::chat
                       : (i == 1 ? icon::folder
                                 : (i == 2
                                        ? (g.control_on ? icon::hand
                                                        : icon::pointer)
                                        : icon::keyboard));
      } else {
        glyph = i == 0 ? (g.theme == &kLight ? icon::moon : icon::sun)
                       : icon::settings;
      }
      if (glyph != icon::none) {
        draw_icon(t, glyph, item.left + 14,
                  (item.top + item.bottom) / 2, color);
        text_left = item.left + 28;
      }
    }
    text(t, g.open_menu >= 4 && g.open_menu < 10 &&
                     g.menu_text != nullptr
                ? g.menu_text
                : small_left,
         menu_label(g.open_menu, i),
         D2D1::RectF(text_left, item.top, item.right - 8, item.bottom), color);
  }
}

/// Range slider track used by the quality popover.
void range_track(ID2D1RenderTarget* t, const palette& th,
                 const D2D1_RECT_F& r, float fraction, bool reversed) {
  const float cy = (r.top + r.bottom) / 2;
  fill_round(t, D2D1::RectF(r.left, cy - 2, r.right, cy + 2), 2, th.line_soft);
  const float knob = r.left + (r.right - r.left) * clampf(fraction, 0, 1);
  // The web's quality slider is direction:rtl, so its fill runs from the
  // right-hand start to the thumb.
  if (reversed) {
    fill_round(t, D2D1::RectF(knob, cy - 2, r.right, cy + 2), 2,
               th.accent_strong);
  } else {
    fill_round(t, D2D1::RectF(r.left, cy - 2, knob, cy + 2), 2,
               th.accent_strong);
  }
  fill_round(t, D2D1::RectF(knob - 4, cy - 4, knob + 4, cy + 4), 4, th.ink);
}

/// Web QualityPanel as a fixed popover: 316 px wide, preset grid, rate-mode
/// switch, three sliders, image format and reset.
void draw_quality_panel(float window_w, float window_h) {
  if (!g.quality_open) return;
  const float width = 316;
  const float height = 282;
  const float left =
      clampf(g.hit_quality.left, 8, (std::max)(8.0f, window_w - width - 8));
  float top = g.hit_quality.bottom + 6;
  if (top + height > window_h - 8) top = window_h - 8 - height;
  const D2D1_RECT_F panel = D2D1::RectF(left, top, left + width, top + height);
  g.qp_panel = panel;
  shadow(g.target, *g.theme, panel, 8);
  fill_round(g.target, panel, 8, g.theme->surface);
  stroke_round(g.target, panel, 8, g.theme->line);

  const float x = panel.left + 10;
  const float inner = width - 20;
  float y = panel.top + 10;

  // Header: 画质 + close.
  text(g.target, g.small_bold, L"画质",
       D2D1::RectF(x, y, x + 120, y + 24), g.theme->ink);
  g.qp_close = D2D1::RectF(panel.right - 34, y, panel.right - 10, y + 24);
  quiet_icon_button(g.target, *g.theme, g.qp_close, hv_qp_close, icon::close,
                    false, inside(g.qp_close, g.hover_x, g.hover_y));
  y += 24 + 8;

  // Presets: 4-column grid, quiet buttons, the current one accent text.
  const float gap = 4;
  const float cell = (inner - gap * 3) / 4;
  for (int i = 0; i < 7; ++i) {
    const int col = i % 4, row = i / 4;
    const D2D1_RECT_F r =
        D2D1::RectF(x + col * (cell + gap), y + row * (24 + gap),
                    x + col * (cell + gap) + cell, y + row * (24 + gap) + 24);
    const bool active = !quality_custom() && g.preset == i;
    const float h = hover_lerp(hv_qp_preset0 + i, inside(r, g.hover_x, g.hover_y));
    fill_round(g.target, r, 9.6f,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, h));
    stroke_round(g.target, r, 9.6f, g.theme->line);
    text(g.target, g.tiny, kPresets[i], r,
         active ? g.theme->accent
                : mix_color(g.theme->ink, g.theme->ink, h));
    g.qp_preset[i] = r;
  }
  y += 24 + gap + 24 + 8;

  // Rate mode: 自动 / 恒定质量 / 恒定码率.
  text(g.target, g.small_left, L"码率模式",
       D2D1::RectF(x, y, x + 56, y + 24), g.theme->muted);
  const wchar_t* rates[] = {L"自动", L"恒定质量", L"恒定码率"};
  const float rate_w = (inner - 56 - gap * 3) / 3;
  for (int i = 0; i < 3; ++i) {
    const float rx = x + 56 + gap + i * (rate_w + gap);
    const D2D1_RECT_F r = D2D1::RectF(rx, y, rx + rate_w, y + 24);
    const bool active = g.rate_mode == i;
    const float h = hover_lerp(hv_qp_rate0 + i, inside(r, g.hover_x, g.hover_y));
    fill_round(g.target, r, 9.6f,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, h));
    stroke_round(g.target, r, 9.6f, g.theme->line);
    text(g.target, g.tiny, rates[i], r,
         active ? g.theme->accent
                : mix_color(g.theme->ink, g.theme->ink, h));
    g.qp_rate[i] = r;
  }
  y += 24 + 8;

  // Sliders: quality (reversed, as the web's RTL range), bitrate and fps.
  struct slider_row {
    const wchar_t* name;
    float fraction;
    std::wstring value;
  };
  const slider_row rows[3] = {
      {L"画质", 1.0f - g.qp / 63.0f,
       g.qp == 0 ? L"跟随档位" : std::to_wstring(g.qp)},
      {L"码率", g.bitrate_kbps / 20000.0f,
       g.bitrate_kbps == 0 ? L"跟随档位"
                           : std::to_wstring(g.bitrate_kbps) + L" kbps"},
      {L"帧率", (g.fps_value - 1) / 59.0f,
       std::to_wstring(g.fps_value) + L" fps"},
  };
  for (int i = 0; i < 3; ++i) {
    text(g.target, g.small_left, rows[i].name,
         D2D1::RectF(x, y, x + 56, y + 18), g.theme->muted);
    const D2D1_RECT_F track =
        D2D1::RectF(x + 64, y + 2, panel.right - 10 - 88, y + 16);
    range_track(g.target, *g.theme, track, rows[i].fraction, i == 0);
    text(g.target, g.small_right, rows[i].value,
         D2D1::RectF(panel.right - 10 - 80, y, panel.right - 10, y + 18),
         g.theme->ink);
    g.qp_slider[i] = track;
    y += 18 + (i == 1 ? 6 : 8);
  }

  // Image format select.
  text(g.target, g.small_left, L"图像格式",
       D2D1::RectF(x, y, x + 56, y + 24), g.theme->muted);
  g.qp_format = D2D1::RectF(x + 64, y, panel.right - 10, y + 24);
  select_chip(g.target, *g.theme, g.small_left, g.qp_format, icon::none,
              kFormats[g.format_index], inside(g.qp_format, g.hover_x, g.hover_y),
              g.open_menu == 3, 6);
  y += 24 + 8;

  // Reset knobs (the web's RotateCcw button).
  g.qp_reset = D2D1::RectF(x, y, panel.right - 10, y + 24);
  const float reset_h = hover_lerp(hv_qp_reset,
                                  inside(g.qp_reset, g.hover_x, g.hover_y));
  fill_round(g.target, g.qp_reset, 9.6f,
             D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                          g.theme->panel.b, reset_h));
  stroke_round(g.target, g.qp_reset, 9.6f, g.theme->line);
  draw_icon(g.target, icon::rotate, g.qp_reset.left + 10,
            (g.qp_reset.top + g.qp_reset.bottom) / 2, g.theme->muted);
  text(g.target, g.tiny, L"重置",
       D2D1::RectF(g.qp_reset.left + 20, g.qp_reset.top,
                   g.qp_reset.right, g.qp_reset.bottom),
       g.theme->ink);
}

/// Click inside (or outside) the quality popover. Returns true when consumed.
bool quality_panel_click(float x, float y) {
  if (!g.quality_open) return false;
  if (inside(g.hit_quality, x, y)) {
    g.quality_open = false;
    g.open_menu = -1;
    return true;
  }
  if (!inside(g.qp_panel, x, y)) {
    g.quality_open = false;
    g.open_menu = -1;
    return false;  // let the click reach the picture / toolbar
  }
  if (inside(g.qp_close, x, y)) {
    g.quality_open = false;
    return true;
  }
  if (inside(g.qp_reset, x, y)) {
    g.qp = 0;
    g.bitrate_kbps = 0;
    g.rate_mode = 0;
    g.format_index = 0;
    send_quality();
    return true;
  }
  for (int i = 0; i < 7; ++i) {
    if (inside(g.qp_preset[i], x, y)) {
      g.preset = i;
      g.qp = 0;
      g.bitrate_kbps = 0;
      g.rate_mode = 0;
      g.format_index = 0;
      send_quality();
      return true;
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (inside(g.qp_rate[i], x, y)) {
      g.rate_mode = i;
      send_quality();
      return true;
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (inside(g.qp_slider[i], x, y)) {
      g.dragging_slider = i;
      if (g.window != nullptr) SetCapture(g.window);
      // Set the value immediately, then drag from here.
      const float fraction = clampf(
          (x - g.qp_slider[i].left) /
              (g.qp_slider[i].right - g.qp_slider[i].left), 0.0f, 1.0f);
      if (i == 0) {
        g.qp = static_cast<int>((1.0f - fraction) * 63.0f + 0.5f);
      } else if (i == 1) {
        g.bitrate_kbps = static_cast<int>(fraction * 20000.0f + 0.5f);
      } else {
        g.fps_value = 1 + static_cast<int>(fraction * 59.0f + 0.5f);
        g.fps_index = -1;
      }
      return true;
    }
  }
  if (inside(g.qp_format, x, y)) {
    g.open_menu = g.open_menu == 3 ? -1 : 3;
    g.menu_field = g.qp_format;
    return true;
  }
  return true;
}

void draw_picture(const D2D1_RECT_F& stage) {
  // Whichever source produced the newest picture wins. A session can switch
  // between the stream surface and tile frames (or a tile host can follow a
  // streamed one), and the older bitmap must not cover the live one.
  // The margin keeps a mixed session (stream + tile frames for the static
  // parts) on the surface instead of flickering between the two sources,
  // while a surface that stopped updating loses to the tiles after a beat.
  ID2D1Bitmap* bitmap = g.surface_bitmap;
  if (g.stream_bitmap != nullptr &&
      (bitmap == nullptr || g.stream_at > g.surface_at + 300)) {
    bitmap = g.stream_bitmap;
  }
  g.picture_rect = stage;
  if (bitmap == nullptr) {
    const bool live = g.session != nullptr && g.session->live();
    const bool authenticated = live && g.session->authenticated();
    const bool pending = live && g.session->access_pending();
    std::wstring label = L"远程画面（共享内存直绘）";
    D2D1_COLOR_F color = g.theme->muted;
    if (g.reconnecting || (g.connecting && g.tab == 1)) {
      label = g.reconnecting ? L"连接已断开，正在重连…" : L"正在连接…";
      color = D2D1::ColorF(0xf59e0b);
    } else if (pending) {
      label = L"已发送连接请求，等待主机确认…";
      color = D2D1::ColorF(0xf59e0b);
    } else if (authenticated) {
      label = L"已连接 " + g.session_label + L" · 等待画面";
      color = g.theme->accent;
    } else if (live) {
      label = L"已连接，正在认证…";
      color = g.theme->muted;
    }
    text(g.target, g.small, label,
         D2D1::RectF(stage.left, (stage.top + stage.bottom) / 2 - 12,
                     stage.right, (stage.top + stage.bottom) / 2 + 12),
         color);
    return;
  }
  const D2D1_SIZE_F src = bitmap->GetSize();
  const float sw = src.width, sh = src.height;
  const float dw = stage.right - stage.left;
  const float dh = stage.bottom - stage.top;
  if (sw <= 0 || sh <= 0) return;
  ID2D1RoundedRectangleGeometry* clip = nullptr;
  if (!g.immersive) {
    g.factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(stage, 12, 12),
                                              &clip);
  }
  const D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters(
      D2D1::InfiniteRect(), clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  g.target->PushLayer(params, nullptr);
  if (g.display_mode == 1) {
    // 1:1: pixels at their native size from the top-left corner.
    g.picture_rect = D2D1::RectF(stage.left, stage.top, stage.left + sw,
                                 stage.top + sh);
    g.target->DrawBitmap(bitmap, g.picture_rect);
  } else if (g.display_mode == 2) {
    // 拉伸: fill the stage, cropping the longer axis.
    const float scale = (std::max)(dw / sw, dh / sh);
    const float cw = dw / scale, ch = dh / scale;
    const D2D1_RECT_F source =
        D2D1::RectF((sw - cw) / 2, (sh - ch) / 2, (sw + cw) / 2, (sh + ch) / 2);
    g.target->DrawBitmap(bitmap, stage, 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
    g.picture_rect = stage;
  } else {
    // 适应: contain, letterboxed.
    const float scale = (std::min)(dw / sw, dh / sh);
    const float fw = sw * scale, fh = sh * scale;
    g.picture_rect =
        D2D1::RectF(stage.left + (dw - fw) / 2, stage.top + (dh - fh) / 2,
                    stage.left + (dw + fw) / 2, stage.top + (dh + fh) / 2);
    g.target->DrawBitmap(bitmap, g.picture_rect);
  }
  g.target->PopLayer();
  if (clip != nullptr) clip->Release();
}

void dark_chip(ID2D1RenderTarget* t, const palette& th,
               const D2D1_RECT_F& r, const std::wstring& label, bool accent) {
  fill_round(t, r, 6, D2D1::ColorF(0, 0, 0, 0.65f));
  stroke_round(t, r, 6,
               accent ? D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.4f)
                      : D2D1::ColorF(1, 1, 1, 0.15f));
  text(t, g.tiny, label, r,
       accent ? th.accent : D2D1::ColorF(1, 1, 1, 0.85f));
}

/// The web's diagnostics overlay: chips along the picture's top-left and a
/// stats bar in the bottom-right corner.
std::wstring session_path_label();
std::wstring session_protocol_label();

void draw_watermark(const D2D1_RECT_F& stage) {
  if (!g.watermark) return;
  // The floating toolbar shares the picture's top edge; drop the chips below
  // it so the transport/resolution chips stay readable.
  const float top = stage.top + (g.bar_open ? 48.0f : 12.0f);
  float x = stage.left + 12;
  struct chip_spec {
    std::wstring label;
    bool accent;
  };
  const std::wstring codec = codec_label();
  std::wstring codec_upper = codec;
  for (wchar_t& c : codec_upper) {
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
  }
  // Actual transport the session settled on (the old chip was a fixed
  // "LAN · UDP · H2" placeholder). H2 there meant an HTTP/2 WebSocket
  // (RFC 8441), not a UDP path.
  std::wstring transport = session_path_label();
  const std::wstring transport_protocol = session_protocol_label();
  if (!transport_protocol.empty()) transport += L" · " + transport_protocol;
  const chip_spec specs[4] = {
      {g.stats.resolution, false},
      {codec_upper, false},
      {kPresets[g.preset], true},
      {transport, false},
  };
  for (const chip_spec& spec : specs) {
    const float w = text_width(spec.label, 10.0f) + 16;
    dark_chip(g.target, *g.theme, D2D1::RectF(x, top, x + w, top + 21),
              spec.label, spec.accent);
    x += w + 6;
  }

  // Bottom-right stats bar: fps, latency, bandwidth, encoding, transport.
  // The web watermark shows what the viewer actually painted, not the target
  // selected in the toolbar.
  const int measured_fps = static_cast<int>(g.stats.fps + 0.5);
  const std::wstring fps = std::to_wstring(measured_fps) + L" fps";
  const std::wstring latency = std::to_wstring(g.stats.latency_ms) + L" ms";
  // Bandwidth in bytes per second: KB/s up to a megabyte, then MB/s (the
  // old "kbps" figure never moved and used bits, not bytes).
  const double bytes_per_s = static_cast<double>(g.stats.kbps) * 125.0;
  wchar_t rate_buffer[32];
  if (bytes_per_s >= 1024.0 * 1024.0) {
    std::swprintf(rate_buffer, 32, L"%.1f MB/s", bytes_per_s / (1024.0 * 1024.0));
  } else {
    std::swprintf(rate_buffer, 32, L"%.0f KB/s", bytes_per_s / 1024.0);
  }
  const std::wstring rate = rate_buffer;
  const std::wstring link = session_path_label() + L"/" +
                            session_protocol_label();
  // The top-left chip is the *configured* codec (AUTO / a pinned choice);
  // this bar shows what the host is really sending right now (TILES / VP9 /
  // VP8 / AV1 / H.264).
  const std::wstring live_codec =
      g.session != nullptr ? g.session->live_codec_name() : std::wstring();
  std::vector<std::wstring> labels = {fps, latency, rate, quality_label()};
  std::vector<bool> accents = {true, false, false, true};
  std::vector<bool> faded = {false, false, false, false};
  if (!live_codec.empty()) {
    labels.push_back(live_codec);
    accents.push_back(true);
    faded.push_back(false);
  }
  labels.push_back(link);
  accents.push_back(false);
  faded.push_back(true);
  const int count = static_cast<int>(labels.size());
  std::vector<float> widths(count, 0.0f);
  float total = 12;
  for (int i = 0; i < count; ++i) {
    widths[i] = text_width(labels[i], 10.0f);
    total += widths[i] + (i > 0 ? 8 : 0);
  }
  const float bar_h = 22;
  const D2D1_RECT_F bar = D2D1::RectF(stage.right - 12 - total,
                                     stage.bottom - 12 - bar_h,
                                     stage.right - 12, stage.bottom - 12);
  fill_round(g.target, bar, 6, D2D1::ColorF(0, 0, 0, 0.65f));
  stroke_round(g.target, bar, 6, D2D1::ColorF(1, 1, 1, 0.15f));
  float bx = bar.left + 6;
  for (int i = 0; i < count; ++i) {
    D2D1_COLOR_F color = accents[i]
                             ? g.theme->accent
                             : (faded[i]
                                    ? D2D1::ColorF(g.theme->muted.r,
                                                   g.theme->muted.g,
                                                   g.theme->muted.b, 0.6f)
                                    : D2D1::ColorF(1, 1, 1, 0.8f));
    text(g.target, g.tiny, labels[i],
         D2D1::RectF(bx, bar.top, bx + widths[i] + 2, bar.bottom), color);
    bx += widths[i] + 8;
  }
}

void draw_ball(const D2D1_RECT_F& stage) {
  if (!g.ball_placed) {
    g.ball_x = stage.left + (stage.right - stage.left) / 2 - 18;
    g.ball_y = stage.top + 12;
    g.ball_placed = true;
  }
  g.ball_x = clampf(g.ball_x, stage.left + 4, stage.right - 40);
  g.ball_y = clampf(g.ball_y, stage.top + 4, stage.bottom - 40);
  const D2D1_RECT_F ball =
      D2D1::RectF(g.ball_x, g.ball_y, g.ball_x + 40, g.ball_y + 40);
  g.hit_ball = ball;
  const bool ball_hover = inside(ball, g.hover_x, g.hover_y);
  note_tip(2000, L"快速操作", ball, ball_hover);
  const float h = hover_lerp(hv_ball, ball_hover);
  shadow(g.target, *g.theme, ball, 20);
  // Web ball: bg-surface/90, border-line, icon muted -> ink on hover.
  fill_round(g.target, ball, 20,
             D2D1::ColorF(g.theme->surface.r, g.theme->surface.g,
                          g.theme->surface.b, 0.90f));
  stroke_round(g.target, ball, 20, g.theme->line);
  draw_icon(g.target, icon::settings, ball.left + 20, ball.top + 20,
            mix_color(g.theme->muted, g.theme->ink, h));
  if (g.chat_unread > 0) {
    const D2D1_COLOR_F danger =
        g.theme == &kLight ? D2D1::ColorF(0xc0392b) : D2D1::ColorF(0xff8f87);
    fill_round(g.target,
               D2D1::RectF(ball.right - 10, ball.top + 4, ball.right - 2,
                           ball.top + 12),
               4, danger);
  }
}

std::wstring upper_wide(std::wstring text) {
  for (wchar_t& c : text) {
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
  }
  return text;
}

std::wstring session_path_label() {
  if (g.session == nullptr) return L"LAN";
  const std::wstring& path = g.session->path_label();
  if (path.empty()) return L"LAN";
  if (path == L"direct") return L"DIRECT";
  if (path == L"p2p") return L"P2P";
  if (path == L"relay") return L"RELAY";
  if (path == L"webrtc") return L"WEBRTC";
  return upper_wide(path);
}

std::wstring session_protocol_label() {
  if (g.session == nullptr) return L"UDP";
  const std::wstring& protocol = g.session->protocol_label();
  if (protocol.empty()) return L"UDP";
  if (protocol == L"websocket") return L"WS";
  if (protocol == L"http2") return L"HTTP/2";
  if (protocol == L"http3") return L"HTTP/3";
  if (protocol == L"quic") return L"QUIC";
  if (protocol == L"udp") return L"UDP";
  if (protocol == L"tcp") return L"TCP";
  return upper_wide(protocol);
}

void draw_bar(const D2D1_RECT_F& stage) {
  const float stage_w = stage.right - stage.left;
  // Fullscreen adds the session chip; keep the compact controls if possible
  // rather than dropping to the minimal tier just for the chip.
  const float limit = stage_w - 24;
  auto fits = [&](int tier, bool chip) {
    return bar_width(bar_layout(tier, chip)) <= limit;
  };
  bool with_chip = g.immersive;
  int tier = 2;
  if (!fits(2, with_chip)) {
    if (with_chip && fits(2, false)) {
      with_chip = false;
    } else {
      tier = 1;
      if (!fits(1, with_chip)) {
        if (with_chip && fits(1, false)) {
          with_chip = false;
        } else {
          tier = 0;
        }
      }
    }
  }
  const std::vector<bar_slot> slots = bar_layout(tier, with_chip);
  const float width = bar_width(slots);
  const float height = 38;
  g.bar_anchor_x = stage.left + (stage_w - width) / 2;
  g.bar_anchor_y = stage.top + 12;
  const float left = clampf(g.bar_anchor_x + g.bar_dx, stage.left + 4,
                            stage.right - 4 - width);
  const float top = clampf(g.bar_anchor_y + g.bar_dy, stage.top + 4,
                           stage.bottom - 4 - height);
  const D2D1_RECT_F bar = D2D1::RectF(left, top, left + width, top + height);
  g.hit_bar = bar;
  shadow(g.target, *g.theme, bar, height / 2);
  // Web quick bar: bg-surface/95 with a line border.
  fill_round(g.target, bar, height / 2,
             D2D1::ColorF(g.theme->surface.r, g.theme->surface.g,
                          g.theme->surface.b, 0.95f));
  stroke_round(g.target, bar, height / 2, g.theme->line);
  const float cy = (bar.top + bar.bottom) / 2;
  float x = bar.left + 6;
  for (const bar_slot& slot : slots) {
    const D2D1_RECT_F r = D2D1::RectF(x, cy - 14, x + slot.width, cy + 14);
    const bool hovered = inside(r, g.hover_x, g.hover_y);
    const wchar_t* tip = nullptr;
    switch (slot.kind) {
      case bk_pin: tip = g.pinned ? L"取消固定" : L"固定"; break;
      case bk_collapse: tip = L"收起"; break;
      case bk_fullscreen: tip = g.immersive ? L"退出全屏" : L"全屏"; break;
      case bk_display_fit: tip = L"适应"; break;
      case bk_display_actual: tip = L"1:1"; break;
      case bk_display_fill: tip = L"拉伸"; break;
      case bk_fps: tip = L"帧率"; break;
      case bk_quality: tip = L"画质"; break;
      case bk_codec: tip = L"编码"; break;
      case bk_watermark: tip = L"诊断水印"; break;
      case bk_resolution: tip = L"分辨率"; break;
      case bk_clip_toggle:
        tip = g.clipboard_on ? L"关闭剪贴板" : L"开启剪贴板";
        break;
      case bk_clip_get: tip = L"获取远端剪贴板"; break;
      case bk_clip_set: tip = L"发送到远端剪贴板"; break;
      case bk_audio: tip = L"声音"; break;
      case bk_volume: tip = L"音量"; break;
      case bk_chat: tip = L"聊天"; break;
      case bk_files: tip = L"文件"; break;
      case bk_control: tip = g.control_on ? L"停止控制" : L"开始控制"; break;
      case bk_sas: tip = L"Ctrl+Alt+Del"; break;
      case bk_theme: tip = L"切换主题"; break;
      case bk_settings: tip = L"设置"; break;
      case bk_clip_menu: tip = L"剪贴板"; break;
      case bk_tools_menu: tip = L"工具"; break;
      case bk_appearance_menu: tip = L"外观"; break;
      case bk_disconnect: tip = L"断开连接"; break;
      default: break;
    }
    if (tip != nullptr) note_tip(slot.kind, tip, r, hovered);
    switch (slot.kind) {
      case bk_pin:
        quiet_icon_button(g.target, *g.theme, r, hv_pin,
                          g.pinned ? icon::pin_off : icon::pin, g.pinned,
                          hovered);
        g.hit_pin = r;
        break;
      case bk_collapse:
        quiet_icon_button(g.target, *g.theme, r, hv_collapse, icon::close, false,
                          hovered);
        g.hit_collapse = r;
        break;
      case bk_session_chip:
        toolbar_chip(g.target, *g.theme, g.tiny, r, g.session_label);
        break;
      case bk_fullscreen:
        quiet_icon_button(g.target, *g.theme, r, hv_fullscreen,
                          g.immersive ? icon::minimize : icon::fullscreen,
                          false, hovered);
        g.hit_fullscreen = r;
        break;
      case bk_separator:
        line(g.target, x + slot.width / 2, cy - 9, x + slot.width / 2, cy + 9,
             g.theme->line, 1.0f);
        break;
      case bk_display_fit:
      case bk_display_actual:
      case bk_display_fill: {
        const int mode = slot.kind - bk_display_fit;
        display_button(g.target, *g.theme, g.small, g.small_bold, r,
                       hv_display0 + mode, kDisplayModes[mode],
                       g.display_mode == mode, hovered);
        g.hit_display[mode] = r;
        break;
      }
      case bk_fps:
        select_chip(g.target, *g.theme, g.small_left, r, icon::activity,
                    fps_label(), hovered, g.open_menu == 0, 6);
        g.hit_fps = r;
        break;
      case bk_quality:
        quiet_label_button(g.target, *g.theme, g.small_left, r, hv_quality,
                           icon::gauge, quality_label(), hovered,
                           g.quality_open);
        g.hit_quality = r;
        break;
      case bk_codec:
        select_chip(g.target, *g.theme, g.small_left, r, icon::cpu,
                    codec_label(), hovered, g.open_menu == 1, 6);
        g.hit_codec = r;
        break;
      case bk_watermark:
        if (g.watermark) {
          primary_icon_button(g.target, *g.theme, r, hv_watermark,
                              icon::activity, hovered);
        } else {
          quiet_icon_button(g.target, *g.theme, r, hv_watermark,
                            icon::activity, false, hovered);
        }
        g.hit_watermark = r;
        break;
      case bk_resolution:
        select_chip(g.target, *g.theme, g.small_left, r, icon::monitor,
                    kResolutionChoices[g.res_index], hovered, g.open_menu == 2,
                    6);
        g.hit_resolution = r;
        break;
      case bk_clip_toggle:
        quiet_icon_button(g.target, *g.theme, r, hv_clip_toggle,
                          g.clipboard_on ? icon::clipboard_check
                                         : icon::clipboard_x,
                          g.clipboard_on, hovered);
        g.hit_clip_toggle = r;
        break;
      case bk_clip_get:
        quiet_icon_button(g.target, *g.theme, r, hv_clip_get, icon::copy, false,
                          hovered, !g.clipboard_on);
        g.hit_clip_get = r;
        break;
      case bk_clip_set:
        quiet_icon_button(g.target, *g.theme, r, hv_clip_set, icon::paste, false,
                          hovered, !g.clipboard_on);
        g.hit_clip_set = r;
        break;
      case bk_audio:
        quiet_icon_button(g.target, *g.theme, r, hv_audio,
                          g.audio_on ? icon::volume : icon::volume_off,
                          g.audio_on, hovered);
        g.hit_audio = r;
        break;
      case bk_volume:
        volume_slider(g.target, *g.theme, r, g.audio_volume);
        g.hit_volume = r;
        break;
      case bk_chat:
        quiet_icon_button(g.target, *g.theme, r, hv_chat, icon::chat,
                          g.chat_open, hovered);
        g.hit_chat = r;
        break;
      case bk_files:
        quiet_icon_button(g.target, *g.theme, r, hv_files, icon::folder,
                          g.files_open, hovered);
        g.hit_files = r;
        break;
      case bk_control:
        if (g.control_on) {
          const float h = hover_lerp(hv_control, hovered);
          fill_round(g.target, r, 9.6f, g.theme->accent_soft);
          stroke_round(g.target, r, 9.6f,
                       mix_color(g.theme->accent, g.theme->ink, 0.12f * h));
          draw_icon(g.target, icon::hand, (r.left + r.right) / 2,
                    (r.top + r.bottom) / 2, g.theme->accent);
        } else {
          quiet_icon_button(g.target, *g.theme, r, hv_control, icon::pointer,
                            false, hovered);
        }
        g.hit_control = r;
        break;
      case bk_sas:
        quiet_icon_button(g.target, *g.theme, r, hv_sas, icon::keyboard, false,
                          hovered);
        g.hit_sas = r;
        break;
      case bk_theme:
        quiet_icon_button(g.target, *g.theme, r, hv_theme,
                          g.theme == &kLight ? icon::moon : icon::sun, false,
                          hovered);
        g.hit_tool_theme = r;
        break;
      case bk_settings:
        quiet_icon_button(g.target, *g.theme, r, hv_settings, icon::settings,
                          false, hovered);
        g.hit_tool_settings = r;
        break;
      case bk_transport:
        toolbar_chip(g.target, *g.theme, g.tiny, r,
                     session_path_label() + L"·" + session_protocol_label());
        g.hit_transport = r;
        break;
      case bk_kbps:
        toolbar_chip(g.target, *g.theme, g.tiny, r,
                     std::to_wstring(g.stats.kbps) + L" kbps");
        g.hit_kbps = r;
        break;
      case bk_latency:
        toolbar_chip(g.target, *g.theme, g.tiny, r,
                     std::to_wstring(g.stats.latency_ms) + L" ms");
        g.hit_latency = r;
        break;
      case bk_path:
        toolbar_chip(g.target, *g.theme, g.tiny, r, L"LAN");
        g.hit_path = r;
        break;
      case bk_protocol:
        toolbar_chip(g.target, *g.theme, g.tiny, r, L"UDP");
        g.hit_protocol = r;
        break;
      case bk_clip_menu:
        menu_button(g.target, *g.theme, r, hv_clip_menu,
                    g.clipboard_on ? icon::clipboard_check : icon::clipboard_x,
                    hovered, g.open_menu == 4);
        g.hit_clip_menu = r;
        break;
      case bk_tools_menu:
        menu_button(g.target, *g.theme, r, hv_tools_menu, icon::wrench, hovered,
                    g.open_menu == 5);
        g.hit_tools_menu = r;
        break;
      case bk_appearance_menu:
        menu_button(g.target, *g.theme, r, hv_appearance_menu,
                    g.theme == &kLight ? icon::moon : icon::sun, hovered,
                    g.open_menu == 6);
        g.hit_appearance_menu = r;
        break;
      case bk_disconnect:
        quiet_icon_button(g.target, *g.theme, r, hv_disconnect, icon::logout,
                          false, hovered);
        g.hit_disconnect = r;
        break;
    }
    x += slot.width + 2;
  }
}

/// Clears every toolbar hit rect. The layout changes with the window width
/// (compact/full) and with toggles (audio hides the volume slider), so stale
/// rects from the previous paint must never accept clicks.
void clear_toolbar_hits() {
  const D2D1_RECT_F none = D2D1::RectF(0, 0, 0, 0);
  g.hit_ball = g.hit_bar = none;
  g.hit_pin = g.hit_collapse = g.hit_fullscreen = none;
  g.hit_display[0] = g.hit_display[1] = g.hit_display[2] = none;
  g.hit_fps = g.hit_quality = g.hit_codec = g.hit_watermark = none;
  g.hit_resolution = g.hit_clip_toggle = g.hit_clip_get = none;
  g.hit_clip_set = g.hit_audio = g.hit_volume = g.hit_chat = none;
  g.hit_files = g.hit_control = g.hit_sas = g.hit_tool_theme = none;
  g.hit_tool_settings = g.hit_path = g.hit_protocol = none;
  g.hit_clip_menu = g.hit_tools_menu = g.hit_appearance_menu = none;
  g.hit_transport = g.hit_kbps = g.hit_latency = g.hit_disconnect = none;
}

void draw_toolbar(const D2D1_RECT_F& stage) {
  clear_toolbar_hits();
  if (!g.bar_open) {
    draw_ball(stage);
    return;
  }
  draw_bar(stage);
}

/// True when pointer/keyboard events belong to the remote host rather than
/// the local toolbar.
bool remote_input_active() {
  return g.control_on && g.session != nullptr && g.session->live() &&
         g.session->authenticated() && g.tab == 1 && !g.settings_open &&
         g.focus_field == 0;
}

void send_remote_move(float x, float y) {
  if (!g.session || !g.session->live()) return;
  const D2D1_RECT_F r = g.picture_rect;
  if (r.right <= r.left || r.bottom <= r.top) return;
  const float nx = (x - r.left) / (r.right - r.left);
  const float ny = (y - r.top) / (r.bottom - r.top);
  if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f) return;
  char buffer[192];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\":\"input\",\"k\":\"move\",\"x\":%.5f,\"y\":%.5f}",
                nx, ny);
  g.session->send_json(buffer);
}

void send_remote_button(int button, bool down) {
  if (!g.session || !g.session->live()) return;
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\":\"input\",\"k\":\"button\",\"button\":%d,\"down\":%s}",
                button, down ? "true" : "false");
  g.session->send_json(buffer);
}

void release_remote_buttons() {
  for (int button = 1; button <= 3; ++button) {
    if ((g.remote_buttons & (1 << button)) != 0) {
      send_remote_button(button, false);
    }
  }
  g.remote_buttons = 0;
  g.remote_mouse_down = false;
}

void close_session_tab() {
  // Cancel any in-flight connect (including a reconnect attempt) so its
  // worker cannot re-open the tab after the user closed it.
  ++g.connect_generation;
  g.connecting = false;
  g.reconnecting = false;
  g.session_accepted = false;
  g.retry_attempt = 0;
  g.retry_at = 0;
  g.tab = 0;
  g.session_open = false;
  g.immersive = false;
  g.bar_dx = g.bar_dy = 0;
  clear_text_selection();
  release_remote_buttons();
  mark_recent_offline(g.connect_target);
  if (g.session != nullptr) {
    g.session->stop();
    g.session.reset();
  }
  g.session_was_live = false;
}

void send_remote_wheel(float dx, float dy) {
  if (!g.session || !g.session->live()) return;
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\":\"input\",\"k\":\"wheel\",\"dx\":%.2f,\"dy\":%.2f}",
                dx, dy);
  g.session->send_json(buffer);
}

/// Maps a Windows virtual key to the browser `KeyboardEvent.code` string the
/// remote core expects (KeyA, Digit1, ArrowLeft, ControlLeft, ...).
std::string browser_key_code(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') {
    return std::string("Key") + static_cast<char>(vk);
  }
  if (vk >= '0' && vk <= '9') {
    return std::string("Digit") + static_cast<char>(vk);
  }
  switch (vk) {
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_SPACE: return "Space";
    case VK_LEFT: return "ArrowLeft";
    case VK_RIGHT: return "ArrowRight";
    case VK_UP: return "ArrowUp";
    case VK_DOWN: return "ArrowDown";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_SHIFT:
    case VK_LSHIFT: return "ShiftLeft";
    case VK_RSHIFT: return "ShiftRight";
    case VK_CONTROL:
    case VK_LCONTROL: return "ControlLeft";
    case VK_RCONTROL: return "ControlRight";
    case VK_MENU:
    case VK_LMENU: return "AltLeft";
    case VK_RMENU: return "AltRight";
    case VK_LWIN: return "MetaLeft";
    case VK_RWIN: return "MetaRight";
    case VK_CAPITAL: return "CapsLock";
    case VK_NUMLOCK: return "NumLock";
    case VK_SCROLL: return "ScrollLock";
    case VK_PAUSE: return "Pause";
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_OEM_MINUS: return "Minus";
    case VK_OEM_PLUS: return "Equal";
    case VK_OEM_4: return "BracketLeft";
    case VK_OEM_6: return "BracketRight";
    case VK_OEM_5: return "Backslash";
    case VK_OEM_1: return "Semicolon";
    case VK_OEM_7: return "Quote";
    case VK_OEM_COMMA: return "Comma";
    case VK_OEM_PERIOD: return "Period";
    case VK_OEM_2: return "Slash";
    case VK_OEM_3: return "Backquote";
    case VK_ADD: return "NumpadAdd";
    case VK_SUBTRACT: return "NumpadSubtract";
    case VK_MULTIPLY: return "NumpadMultiply";
    case VK_DIVIDE: return "NumpadDivide";
    case VK_DECIMAL: return "NumpadDecimal";
    default: break;
  }
  if (vk >= VK_F1 && vk <= VK_F24) {
    return std::string("F") + std::to_string(vk - VK_F1 + 1);
  }
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
    return std::string("Numpad") + static_cast<char>('0' + (vk - VK_NUMPAD0));
  }
  return std::string();
}

void send_remote_key(WPARAM vk, bool down) {
  if (!g.session || !g.session->live() || !g.control_on) return;
  const std::string code = browser_key_code(vk);
  if (code.empty()) return;
  g.session->send_json("{\"type\":\"input\",\"k\":\"key\",\"code\":\"" + code +
                       "\",\"down\":" + (down ? "true" : "false") + "}");
}

/// Click routing for the session toolbar. Returns true when the click was
/// consumed; the caller repaints on any true result.
bool session_click(float x, float y) {
  const float window_w = g.target != nullptr ? g.target->GetSize().width : 0;
  // An open menu takes the click first: pick a row, or just close.
  if (g.open_menu >= 0) {
    const int count = menu_count(g.open_menu);
    const int picked =
        menu_hit(g.menu_field, g.open_menu, count, window_w, x, y);
    if (picked >= 0) {
      if (!menu_disabled(g.open_menu, picked)) {
        menu_apply(g.open_menu, picked);
      }
      g.open_menu = -1;
      return true;
    }
    const bool on_field = inside(g.menu_field, x, y);
    g.open_menu = -1;
    if (on_field) return true;  // pressing the same chip closes its menu
  }
  if (g.quality_open) {
    if (quality_panel_click(x, y)) return true;
  }
  if (remote_input_active() && inside(g.picture_rect, x, y) &&
      !inside(g.hit_bar, x, y) && !inside(g.hit_ball, x, y)) {
    send_remote_move(x, y);
    send_remote_button(1, true);
    g.remote_buttons |= 1 << 1;
    g.remote_mouse_down = true;
    if (g.window != nullptr) SetCapture(g.window);
    if (!g.pinned) g.bar_open = false;
    return true;
  }
  if (!g.bar_open) {
    if (inside(g.hit_ball, x, y)) {
      g.dragging_ball = true;
      g.ball_moved = false;
      g.drag_start_x = x;
      g.drag_start_y = y;
      g.drag_dx = x - g.ball_x;
      g.drag_dy = y - g.ball_y;
      if (g.window != nullptr) SetCapture(g.window);
      return true;
    }
    return false;
  }
  // Pin and collapse.
  if (inside(g.hit_pin, x, y)) {
    g.pinned = !g.pinned;
    save_pinned(g.pinned);
    return true;
  }
  if (inside(g.hit_collapse, x, y)) {
    g.bar_open = false;
    g.quality_open = false;
    g.open_menu = -1;
    return true;
  }
  if (inside(g.hit_fullscreen, x, y)) {
    g.immersive = !g.immersive;
    g.bar_dx = g.bar_dy = 0;
    return true;
  }
  for (int i = 0; i < 3; ++i) {
    if (inside(g.hit_display[i], x, y)) {
      g.display_mode = i;
      if (!g.pinned) g.bar_open = false;
      return true;
    }
  }
  struct select_hit {
    const D2D1_RECT_F* rect;
    int which;
  };
  const select_hit selects[] = {
      {&g.hit_fps, 0}, {&g.hit_codec, 1}, {&g.hit_resolution, 2},
  };
  for (const select_hit& select : selects) {
    if (inside(*select.rect, x, y)) {
      g.open_menu = g.open_menu == select.which ? -1 : select.which;
      g.menu_field = *select.rect;
      return true;
    }
  }
  if (inside(g.hit_quality, x, y)) {
    g.quality_open = !g.quality_open;
    g.open_menu = -1;
    return true;
  }
  struct menu_hit_button {
    const D2D1_RECT_F* rect;
    int which;
  };
  const menu_hit_button menu_buttons[] = {
      {&g.hit_clip_menu, 4},
      {&g.hit_tools_menu, 5},
      {&g.hit_appearance_menu, 6},
  };
  for (const menu_hit_button& button : menu_buttons) {
    if (inside(*button.rect, x, y)) {
      g.open_menu = g.open_menu == button.which ? -1 : button.which;
      g.menu_field = *button.rect;
      g.quality_open = false;
      return true;
    }
  }
  if (inside(g.hit_watermark, x, y)) {
    g.watermark = !g.watermark;
    return true;
  }
  if (inside(g.hit_audio, x, y)) {
    g.audio_on = !g.audio_on;
    session_send(g.audio_on ? "{\"type\":\"audio\",\"action\":\"start\"}"
                            : "{\"type\":\"audio\",\"action\":\"stop\"}");
    return true;
  }
  if (inside(g.hit_volume, x, y)) {
    const float left = g.hit_volume.left + 4;
    const float right = g.hit_volume.right - 4;
    g.audio_volume = clampf((x - left) / (right - left), 0.0f, 1.0f);
    g.dragging_volume = true;
    if (g.window != nullptr) SetCapture(g.window);
    return true;
  }
  if (inside(g.hit_clip_toggle, x, y)) {
    g.clipboard_on = !g.clipboard_on;
    return true;
  }
  if (inside(g.hit_clip_get, x, y)) {
    clipboard_get();
    return true;
  }
  if (inside(g.hit_clip_set, x, y)) {
    clipboard_set();
    return true;
  }
  if (inside(g.hit_sas, x, y)) {
    session_send("{\"type\":\"input\",\"k\":\"sas\"}");
    show_toast(L"已发送 Ctrl+Alt+Del");
    return true;
  }
  if (inside(g.hit_transport, x, y) || inside(g.hit_kbps, x, y) ||
      inside(g.hit_latency, x, y)) {
    return true;
  }
  if (inside(g.hit_chat, x, y)) {
    g.chat_open = !g.chat_open;
    if (g.chat_open) {
      g.files_open = false;
      g.focus_field = 3;
      g.chat_caret = g.chat_input.size();
      g.chat_unread = 0;
      g.open_menu = -1;
      g.quality_open = false;
    } else if (g.focus_field == 3) {
      g.focus_field = 0;
    }
    return true;
  }
  if (inside(g.hit_files, x, y)) {
    g.files_open = !g.files_open;
    if (g.files_open) {
      g.chat_open = false;
      if (g.focus_field == 3) g.focus_field = 0;
      g.open_menu = -1;
      g.quality_open = false;
      if (g.files_tab == 1) request_files_browse();
    }
    return true;
  }
  if (inside(g.hit_control, x, y)) {
    g.control_on = !g.control_on;
    if (!g.control_on) release_remote_buttons();
    return true;
  }
  if (inside(g.hit_tool_theme, x, y)) {
    // Same toggle as the header square: flip light and dark.
    g.mode = g.theme == &kLight ? app_state::theme_mode::dark
                                : app_state::theme_mode::light;
    save_theme_mode();
    return true;
  }
  if (inside(g.hit_tool_settings, x, y)) {
    g.settings_open = true;
    g.open_menu = -1;
    g.quality_open = false;
    g.focus_field = 0;
    return true;
  }
  if (inside(g.hit_disconnect, x, y)) {
    g.tab = 0;
    g.session_open = false;
    g.immersive = false;
    g.bar_dx = g.bar_dy = 0;
    release_remote_buttons();
    mark_recent_offline(g.connect_target);
    if (g.session != nullptr) {
      g.session->stop();
      g.session.reset();
      g.session_was_live = false;
    }
    return true;
  }
  if (inside(g.hit_bar, x, y)) {
    // Empty part of the bar: drag the whole bar (the ball drags too).
    g.dragging_bar = true;
    g.bar_moved = false;
    g.drag_start_x = x;
    g.drag_start_y = y;
    g.drag_dx = x - g.hit_bar.left;
    g.drag_dy = y - g.hit_bar.top;
    if (g.window != nullptr) SetCapture(g.window);
    return true;
  }
  if (!g.pinned) {
    // Like the web: a click on the picture collapses the unpinned bar.
    g.bar_open = false;
    g.quality_open = false;
    g.open_menu = -1;
  } else {
  }
  return true;
}

/// Title-bar window button: 32x28, radius 6. Close uses the danger tint.
void window_button(ID2D1RenderTarget* t, const palette& th,
                   const D2D1_RECT_F& r, int id, icon glyph, bool danger,
                   bool hovered) {
  const float h = hover_lerp(id, hovered);
  const D2D1_COLOR_F danger_color =
      th.bg.r < 0.5f ? D2D1::ColorF(0xff8f87) : D2D1::ColorF(0xc0392b);
  const float cx = (r.left + r.right) / 2;
  const float cy = (r.top + r.bottom) / 2;
  if (danger) {
    fill_round(t, r, 6,
               D2D1::ColorF(danger_color.r, danger_color.g, danger_color.b,
                            0.15f * h));
    draw_icon(t, glyph, cx, cy, mix_color(th.muted, danger_color, h));
  } else {
    fill_round(t, r, 6, D2D1::ColorF(th.panel.r, th.panel.g, th.panel.b, h));
    draw_icon(t, glyph, cx, cy, mix_color(th.muted, th.ink, h));
  }
  if (flash_active(hovered)) {
    fill_round(t, r, 6, D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b,
                                     0.16f));
  }
}

/// Header 32x32 square: quiet by default, panel on hover, accent-mixed border.
void header_square(ID2D1RenderTarget* t, const palette& th,
                   const D2D1_RECT_F& r, int id, icon glyph, bool hovered) {
  const float h = hover_lerp(id, hovered);
  fill_round(t, r, 6, D2D1::ColorF(th.panel.r, th.panel.g, th.panel.b, h));
  stroke_round(t, r, 6, th.line_soft);
  draw_icon(t, glyph, r.left + 16, r.top + 16, mix_color(th.muted, th.ink, h));
  if (flash_active(hovered)) {
    fill_round(t, r, 6, D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b,
                                     0.16f));
  }
}

/// Web TabBar pill: accent-soft when active, panel on hover otherwise.
void nav_tab(ID2D1RenderTarget* t, const palette& th,
             IDWriteTextFormat* small, const D2D1_RECT_F& r,
             const std::wstring& label, bool active, int id, bool hovered) {
  const float h = hover_lerp(id, hovered);
  const D2D1_COLOR_F color = active ? th.accent : mix_color(th.muted, th.ink, h);
  if (active) {
    fill_round(t, r, 8, th.accent_soft);
  } else {
    fill_round(t, r, 8, D2D1::ColorF(th.panel.r, th.panel.g, th.panel.b, h));
  }
  // Web TabBar: px-3 (12), a 14 px icon, gap-1.5 (6), then the 12 px label.
  draw_icon(t, icon::house, r.left + 19, (r.top + r.bottom) / 2, color);
  text(t, small, label,
       D2D1::RectF(r.left + 32, r.top, r.right - 12, r.bottom), color);
  if (flash_active(hovered)) {
    fill_round(t, r, 8, D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b,
                                     0.16f));
  }
}

/// Home filter chip (最近/已保存/...): pill, accent-soft when selected.
[[maybe_unused]] void filter_chip(ID2D1RenderTarget* t, const palette& th,
                 IDWriteTextFormat* small, const D2D1_RECT_F& r,
                 const std::wstring& label, bool active, int id,
                 bool hovered) {
  const float h = hover_lerp(id, hovered);
  fill_round(t, r, 8, active ? th.accent_soft
                             : D2D1::ColorF(th.panel.r, th.panel.g,
                                            th.panel.b, h));
  stroke_round(t, r, 8, active ? th.accent : th.line_soft);
  text(t, small, label, r,
       active ? th.accent : mix_color(th.muted, th.ink, h));
  if (flash_active(hovered)) {
    fill_round(t, r, 8, D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b,
                                     0.16f));
  }
}

/// After the hover has rested for 600 ms, draw the web's `title` tooltip.
void draw_tooltip(const D2D1_SIZE_F& size) {
  // Tooltips belong to the topmost surface: while a modal or a drawer is
  // under the pointer, the control hidden behind it must not leak one.
  if (g.ctx_field != 0 || g.settings_open ||
      (g.chat_open && inside(g.hit_chat_panel, g.hover_x, g.hover_y)) ||
      (g.files_open && inside(g.hit_files_panel, g.hover_x, g.hover_y))) {
    g.tip_id = -1;
    g.tip_shown = false;
    return;
  }
  if (g.tip_candidate < 0) {
    g.tip_id = -1;
    g.tip_shown = false;
    return;
  }
  if (g.tip_candidate != g.tip_id) {
    g.tip_id = g.tip_candidate;
    g.tip_since = GetTickCount64();
    g.tip_shown = false;
    return;
  }
  if (!g.tip_shown) return;
  const float x = (g.tip_rect.left + g.tip_rect.right) / 2;
  tooltip(g.target, *g.theme, g.small, x, g.tip_rect.bottom, g.tip_text, size);
}

void draw_toast(const D2D1_SIZE_F& size) {
  if (g.toast_text.empty() || GetTickCount64() >= g.toast_until) return;
  toast_state state{g.toast_text, g.toast_until};
  toast(g.target, *g.theme, g.small, size, state, GetTickCount64());
}

void draw_overlays(float w, float h);
void draw_close_prompt(float w, float h);
void draw_ctx_menu();
void dashed_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                  const D2D1_COLOR_F& color, float width);

/// The web paints the canvas as `linear-gradient(bg, bg-soft)` plus two
/// radial glows (index.css). Reproducing it matters most in the light theme,
/// where a flat fill made the layout read differently from the web.
void draw_background(float w, float h) {
  if (g.target == nullptr || g.theme == nullptr) return;
  // Dark already matches the web; only the light canvas needed the gradient.
  if (g.theme->ink.r > 0.5f) return;
  // The gradients are rebuilt only when the target, size or theme changes:
  // creating three stop collections and brushes on every animation frame was
  // part of the settings open/close stutter.
  static ID2D1RenderTarget* cached_target = nullptr;
  static float cached_w = -1.0f;
  static float cached_h = -1.0f;
  static const palette* cached_theme = nullptr;
  static ID2D1LinearGradientBrush* linear = nullptr;
  static ID2D1RadialGradientBrush* glows[2] = {nullptr, nullptr};
  auto clear_cache = [&]() {
    if (linear != nullptr) { linear->Release(); linear = nullptr; }
    for (ID2D1RadialGradientBrush*& brush : glows) {
      if (brush != nullptr) { brush->Release(); brush = nullptr; }
    }
  };
  if (g.target != cached_target || w != cached_w || h != cached_h ||
      g.theme != cached_theme) {
    clear_cache();
    cached_target = g.target;
    cached_w = w;
    cached_h = h;
    cached_theme = g.theme;
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f;
    stops[0].color = g.theme->bg;
    stops[1].position = 1.0f;
    stops[1].color = g.theme->bg_soft;
    ID2D1GradientStopCollection* collection = nullptr;
    if (SUCCEEDED(g.target->CreateGradientStopCollection(stops, 2,
                                                         &collection)) &&
        collection != nullptr) {
      const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES linear_props =
          D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                              D2D1::Point2F(0, h));
      g.target->CreateLinearGradientBrush(linear_props, collection, &linear);
      struct glow_spec {
        float cx, cy, rx, ry;
        D2D1_COLOR_F color;
      };
      const glow_spec specs[2] = {
          {w * 0.08f, h * -0.12f, w * 0.60f, h * 0.40f, g.theme->glow1},
          {w * 1.02f, h * 1.08f, w * 0.50f, h * 0.34f, g.theme->glow2},
      };
      for (int i = 0; i < 2; ++i) {
        D2D1_COLOR_F transparent = specs[i].color;
        transparent.a = 0.0f;
        D2D1_GRADIENT_STOP glow_stops[2];
        glow_stops[0].position = 0.0f;
        glow_stops[0].color = specs[i].color;
        glow_stops[1].position = 0.60f;
        glow_stops[1].color = transparent;
        ID2D1GradientStopCollection* glow_collection = nullptr;
        if (FAILED(g.target->CreateGradientStopCollection(glow_stops, 2,
                                                          &glow_collection)) ||
            glow_collection == nullptr) {
          continue;
        }
        D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES radial_props{};
        radial_props.center = D2D1::Point2F(specs[i].cx, specs[i].cy);
        radial_props.radiusX = specs[i].rx;
        radial_props.radiusY = specs[i].ry;
        g.target->CreateRadialGradientBrush(radial_props, glow_collection,
                                            &glows[i]);
        glow_collection->Release();
      }
      collection->Release();
    }
  }
  if (linear != nullptr) {
    g.target->FillRectangle(D2D1::RectF(0, 0, w, h), linear);
  }
  for (ID2D1RadialGradientBrush* glow : glows) {
    if (glow != nullptr) {
      g.target->FillRectangle(D2D1::RectF(0, 0, w, h), glow);
    }
  }
}

void paint(HWND window) {
  g.theme = resolve_theme();
  if (g.target == nullptr) return;
  const D2D1_SIZE_F size = g.target->GetSize();
  const float w = size.width;
  const float h = size.height;
  const auto paint_started = std::chrono::steady_clock::now();
  g.target->BeginDraw();
  g.target->Clear(g.theme->bg);
  draw_background(w, h);
  for (float& target : g_hover_target) target = 0.0f;
  g.tip_candidate = -1;

  // Web chrome is two rows: a 40 px title bar and an app header below it.
  // Fullscreen (the toolbar's expand button) hides both, like the web's
  // immersive mode.
  if (!g.immersive) {
    const D2D1_RECT_F title_bar = D2D1::RectF(0, 0, w, 40);
    // Web `.titlebar` is bg-surface/85 with a line-soft bottom border.
    fill_round(g.target, title_bar, 0,
               D2D1::ColorF(g.theme->surface.r, g.theme->surface.g,
                            g.theme->surface.b, 0.85f));
    line(g.target, 0, 40, w, 40, g.theme->line_soft, 1.0f);
    // Product icon at the top-left, which a browser title bar cannot show.
    if (g.title_icon != nullptr) {
      g.target->DrawBitmap(g.title_icon, D2D1::RectF(12, 10, 32, 30), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      fill_round(g.target, D2D1::RectF(12, 10, 32, 30), 5,
                 g.theme->accent_strong);
      draw_icon(g.target, icon::monitor, 22, 20, g.theme->on_accent);
    }
    // Window buttons: 32x28 at y=6, radius 6 (measured x=1052/1088/1124 at 1164).
    const float bx = w - 112;
    const D2D1_RECT_F buttons[3] = {
        D2D1::RectF(bx, 6, bx + 32, 34),
        D2D1::RectF(bx + 36, 6, bx + 68, 34),
        D2D1::RectF(bx + 72, 6, bx + 104, 34),
    };
    const wchar_t* window_tips[3] = {L"最小化", L"全屏", L"关闭"};
    for (int i = 0; i < 3; ++i) {
      note_tip(1000 + i, window_tips[i], buttons[i],
               inside(buttons[i], g.hover_x, g.hover_y));
      window_button(g.target, *g.theme, buttons[i], hv_win0 + i,
                    i == 0 ? icon::chevron_down
                           : (i == 1 ? icon::fullscreen : icon::close),
                    i == 2, inside(buttons[i], g.hover_x, g.hover_y));
      g.hit_win[i] = buttons[i];
    }
    g.hit_modal_close = buttons[2];

    // App header: logo+name, the fixed 首页 tab, a new-session icon, the
    // session tabs, then the theme and settings squares at the right edge.
    const float header_y = 49;
    fill_round(g.target, D2D1::RectF(16, header_y + 3, 28, header_y + 15), 5,
               g.theme->accent_strong);
    text(g.target, g.body, L"moLink",
         D2D1::RectF(36, header_y, 110, header_y + 30), g.theme->ink);

    const D2D1_RECT_F home_tab = D2D1::RectF(127, header_y, 195, header_y + 30);
    note_tip(1003, L"首页", home_tab, inside(home_tab, g.hover_x, g.hover_y));
    nav_tab(g.target, *g.theme, g.label12, home_tab, L"首页", g.tab == 0,
            hv_home_tab, inside(home_tab, g.hover_x, g.hover_y));
    g.hit_tab[0] = home_tab;

    const D2D1_RECT_F plus = D2D1::RectF(201, header_y, 229, header_y + 30);
    note_tip(1004, L"新建会话", plus, inside(plus, g.hover_x, g.hover_y));
    {
      const float h = hover_lerp(hv_plus, inside(plus, g.hover_x, g.hover_y));
      fill_round(g.target, plus, 8,
                 D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                              g.theme->panel.b, h));
      draw_icon(g.target, icon::plus, (plus.left + plus.right) / 2,
                (plus.top + plus.bottom) / 2,
                mix_color(g.theme->muted, g.theme->ink, h));
    }
    g.hit_tab[1] = plus;

    // Session tab (one remote per tab; a session list arrives with the real
    // shell). Status dot + name + close, 190x30 like the web.
    if (g.session_open) {
    const D2D1_RECT_F session_tab =
        D2D1::RectF(237, header_y, 427, header_y + 30);
    note_tip(1005, L"会话", session_tab,
             inside(session_tab, g.hover_x, g.hover_y));
    const bool session_active = g.tab == 1;
    const float tab_h = hover_lerp(hv_session_tab,
                                   inside(session_tab, g.hover_x, g.hover_y));
    fill_round(g.target, session_tab, 8,
               session_active
                   ? g.theme->accent_soft
                   : D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                  g.theme->panel.b, tab_h));
    if (session_active) {
      stroke_round(g.target, session_tab, 8, g.theme->accent);
    }
    // Web TabBar order: status dot, TLS badge, then the remote's name.
    const D2D1_COLOR_F tab_ink =
        session_active ? g.theme->accent
                       : mix_color(g.theme->muted, g.theme->ink, tab_h);
    const bool session_live = g.session != nullptr && g.session->live();
    const bool session_failed = g.session != nullptr && !session_live;
    // A policy error (wrong code, denied access) keeps the socket open on
    // some transports; the tab still belongs in the web's red error state.
    const bool session_denied =
        g.session != nullptr && !g.session->authenticated() &&
        !g.session->last_error().empty();
    D2D1_COLOR_F dot_color = g.theme->accent_strong;
    const bool waiting_access =
        g.session != nullptr && g.session->access_pending() &&
        !g.session->authenticated();
    if (session_denied) {
      dot_color = g.theme == &kLight ? D2D1::ColorF(0xc0392b)
                                     : D2D1::ColorF(0xff8f87);
    } else if (g.connecting || waiting_access) {
      // animate-pulse: a warn dot breathing while the socket is opening.
      const float phase =
          0.5f + 0.5f * std::sin(static_cast<float>(GetTickCount64()) / 220.0f);
      dot_color = D2D1::ColorF(0xf59e0b, 0.45f + 0.55f * phase);
    } else if (!session_live && session_failed) {
      dot_color = g.theme == &kLight ? D2D1::ColorF(0xc0392b)
                                     : D2D1::ColorF(0xff8f87);
    } else if (!session_live) {
      dot_color = D2D1::ColorF(g.theme->muted.r, g.theme->muted.g,
                               g.theme->muted.b, 0.60f);
    }
    fill_round(g.target,
               D2D1::RectF(254, header_y + 12, 260, header_y + 18), 3,
               dot_color);
    float label_x = 266;
    if (g.session != nullptr) {
      const std::wstring security = g.session->security();
      if (!security.empty()) {
        icon glyph = icon::shield_off;
        D2D1_COLOR_F color = D2D1::ColorF(g.theme->muted.r, g.theme->muted.g,
                                          g.theme->muted.b, 0.70f);
        const wchar_t* tip = L"非加密通讯";
        if (security == L"e2e") {
          glyph = icon::shield_check;
          color = D2D1::ColorF(0x0ea5e9);
          tip = L"端到端加密（直连证书）";
        } else if (security == L"relay") {
          glyph = icon::shield_check;
          color = D2D1::ColorF(0xf59e0b);
          tip = L"中继 TLS（服务端加密，非端到端）";
        }
        const D2D1_RECT_F shield_rect =
            D2D1::RectF(266, header_y + 4, 278, header_y + 26);
        draw_icon(g.target, glyph, 272, header_y + 15, color);
        note_tip(1100, tip, shield_rect,
                 inside(shield_rect, g.hover_x, g.hover_y));
        label_x = 284;
      }
    }
    const std::wstring tab_label = elide_text(
        g.label12, g.session_label, (std::max)(24.0f, 399 - label_x - 4));
    text(g.target, g.label12, tab_label,
         D2D1::RectF(label_x, header_y, 399, header_y + 30), tab_ink);
    // The close glyph lights up red on its own hover.
    const D2D1_RECT_F tab_close =
        D2D1::RectF(401, header_y + 4, 423, header_y + 26);
    note_tip(1006, L"关闭会话", tab_close,
             inside(tab_close, g.hover_x, g.hover_y));
    const float close_h =
        hover_lerp(hv_session_close, inside(tab_close, g.hover_x, g.hover_y));
    const D2D1_COLOR_F danger_color =
        g.theme == &kLight ? D2D1::ColorF(0xc0392b) : D2D1::ColorF(0xff8f87);
    if (close_h > 0.01f) {
      fill_round(g.target, tab_close, 4,
                 D2D1::ColorF(danger_color.r, danger_color.g, danger_color.b,
                              0.15f * close_h));
    }
    draw_icon(g.target, icon::close, 412, header_y + 15,
              mix_color(g.theme->muted, danger_color, close_h));
    g.hit_session_close = tab_close;
    g.hit_tab[2] = session_tab;
    } else {
      g.hit_session_close = D2D1::RectF(0, 0, 0, 0);
      g.hit_tab[2] = D2D1::RectF(0, 0, 0, 0);
    }

    // Right edge: theme square and settings square (32x32, radius 6).
    const D2D1_RECT_F theme_sq = D2D1::RectF(w - 76, header_y, w - 44,
                                             header_y + 32);
    const D2D1_RECT_F settings_sq = D2D1::RectF(w - 40, header_y, w - 8,
                                                header_y + 32);
    // Show the theme the button switches to, like the web (sun on dark).
    note_tip(1007, L"切换主题", theme_sq,
             inside(theme_sq, g.hover_x, g.hover_y));
    note_tip(1008, L"设置", settings_sq,
             inside(settings_sq, g.hover_x, g.hover_y));
    header_square(g.target, *g.theme, theme_sq, hv_header_theme,
                  g.theme == &kLight ? icon::moon : icon::sun,
                  inside(theme_sq, g.hover_x, g.hover_y));
    header_square(g.target, *g.theme, settings_sq, hv_header_settings,
                  icon::settings, inside(settings_sq, g.hover_x, g.hover_y));
    g.hit_theme_sq = theme_sq;
    g.hit_settings = settings_sq;
  }

  if (g.tab == 0) {
    // Web SimpleHomeView: SelfCard + ConnectCard, then NoticePanel +
    // DeviceList, then the status bar. The page scrolls inside the area
    // below the header.
    const float content_w = (std::min)(w - 32, 1280.0f);
    const float px = (w - content_w) / 2;
    const float gap = 12;
    // Web `lg:` breakpoint: below it the cards stack in one column.
    const bool stacked = content_w < 1000.0f;
    const float self_w = stacked ? content_w : (content_w - gap) * 4 / 12;
    const float connect_w = stacked ? content_w : content_w - gap - self_w;
    const float notice_w = stacked ? content_w : (content_w - gap) * 3 / 12;
    const float list_w = stacked ? content_w : content_w - gap - notice_w;
    const float row1_h = 240;
    const float row2_h = 240;
    const float content_top = 91;
    const float content_h =
        stacked ? row1_h * 4 + gap * 4 + 30 + 16
                : row1_h + gap + row2_h + gap + 30 + 16;
    g.home_view = D2D1::RectF(0, 79, w, h);
    const float view_h = h - 79;
    g.home_content_h = content_h;
    g.home_max_scroll = (std::max)(0.0f, content_h - view_h + 8);
    g.home_scroll = clampf(g.home_scroll, 0.0f, g.home_max_scroll);

    for (int i = 0; i < 4; ++i) g.hit_dev_tab[i] = D2D1::RectF(0, 0, 0, 0);
    for (int i = 0; i < 3; ++i) g.hit_mode[i] = D2D1::RectF(0, 0, 0, 0);
    g.hit_self_avatar = g.hit_self_name = g.hit_copy_id = D2D1::RectF(0, 0, 0, 0);
    g.hit_code_eye = g.hit_code_copy = g.hit_code_refresh = D2D1::RectF(0, 0, 0, 0);
    g.hit_code_single = g.hit_code_multi = g.hit_more_settings =
        D2D1::RectF(0, 0, 0, 0);
    g.hit_connect_input_home = g.hit_connect_send = g.hit_connect_code_home =
        g.hit_connect_code_eye = g.hit_connect_btn = g.hit_relay =
            D2D1::RectF(0, 0, 0, 0);
    g.hit_notice_update = g.hit_view_cards = g.hit_view_list =
        D2D1::RectF(0, 0, 0, 0);

    auto card = [&](const D2D1_RECT_F& r) {
      shadow(g.target, *g.theme, r, 14);
      fill_round(g.target, r, 16, g.theme->surface);
      stroke_round(g.target, r, 16, g.theme->line);
    };
    auto chip_at = [&](float cx, float cy, const std::wstring& label,
                       bool accent) {
      const float cw = measure_text(g.tiny, label) + 14;
      const D2D1_RECT_F r = D2D1::RectF(cx, cy, cx + cw, cy + 20);
      fill_round(g.target, r, 10,
                 accent ? g.theme->accent_soft
                        : D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                       g.theme->panel.b, 0.55f));
      stroke_round(g.target, r, 10, accent ? g.theme->accent : g.theme->line);
      text(g.target, g.tiny, label, r,
           accent ? g.theme->accent : g.theme->muted);
      return cw;
    };
    auto mini = [&](const D2D1_RECT_F& r, icon glyph, bool hovered) {
      if (hovered) fill_round(g.target, r, 6, g.theme->panel);
      draw_icon(g.target, glyph, (r.left + r.right) / 2,
                (r.top + r.bottom) / 2, hovered ? g.theme->ink : g.theme->muted);
    };

    g.target->PushAxisAlignedClip(g.home_view,
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float sy = content_top - g.home_scroll;

    // ---- SelfCard ----
    const D2D1_RECT_F self = D2D1::RectF(px, sy, px + self_w, sy + row1_h);
    card(self);
    const D2D1_RECT_F avatar = D2D1::RectF(px + 12, sy + 12, px + 70, sy + 70);
    g.hit_self_avatar = avatar;
    // Web SelfCard: rounded-2xl + `hover:scale-[1.03]` with the 140 ms
    // transition. Scale the picture, the placeholder and the pencil badge
    // together around the tile's centre.
    const float avatar_h =
        hover_lerp(hv_avatar, inside(avatar, g.hover_x, g.hover_y));
    const float avatar_scale = 1.0f + 0.03f * avatar_h;
    const float avatar_cx = (avatar.left + avatar.right) / 2;
    const float avatar_cy = (avatar.top + avatar.bottom) / 2;
    const float avatar_half = 29.0f * avatar_scale;
    const D2D1_RECT_F shown = D2D1::RectF(
        avatar_cx - avatar_half, avatar_cy - avatar_half,
        avatar_cx + avatar_half, avatar_cy + avatar_half);
    fill_round(g.target, shown, 16.0f * avatar_scale, g.theme->accent_soft);
    if (g.avatar_bitmap != nullptr) {
      const D2D1_SIZE_F src = g.avatar_bitmap->GetSize();
      const float box = avatar_half * 2.0f;
      const float fit = (std::min)(box / src.width, box / src.height);
      const float dw = src.width * fit, dh = src.height * fit;
      const D2D1_RECT_F area = D2D1::RectF(
          avatar_cx - dw / 2, avatar_cy - dh / 2, avatar_cx + dw / 2,
          avatar_cy + dh / 2);
      g.target->DrawBitmap(g.avatar_bitmap, area, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      draw_icon(g.target, icon::monitor, avatar_cx, avatar_cy,
                g.theme->accent);
    }
    const float badge_scale = avatar_scale;
    const D2D1_RECT_F pencil_badge = D2D1::RectF(
        shown.right - 20.0f * badge_scale,
        shown.bottom - 20.0f * badge_scale,
        shown.right + 2.0f * badge_scale,
        shown.bottom + 2.0f * badge_scale);
    fill_round(g.target, pencil_badge, 11.0f * badge_scale, g.theme->surface);
    stroke_round(g.target, pencil_badge, 11.0f * badge_scale, g.theme->line);
    draw_icon(g.target, icon::pencil,
              (pencil_badge.left + pencil_badge.right) / 2,
              (pencil_badge.top + pencil_badge.bottom) / 2, g.theme->muted);

    const float name_x = avatar.right + 12;
    text(g.target, g.name16, g.set_device_name,
         D2D1::RectF(name_x, sy + 8, px + self_w - 12, sy + 32),
         g.theme->ink);
    const float name_w = measure_text(g.name16, g.set_device_name);
    draw_icon(g.target, icon::pencil, name_x + name_w + 8, sy + 20,
              g.theme->muted);
    g.hit_self_name = D2D1::RectF(name_x, sy + 8, name_x + name_w + 16, sy + 32);

    float chip_x = name_x;
    chip_x += chip_at(chip_x, sy + 36, L"windows", false) + 6;
    chip_at(chip_x, sy + 36, g.id_status, g.id_online);

    text(g.target, g.mono,
         g.device_id.empty() ? std::wstring(L"—") : g.device_id,
         D2D1::RectF(name_x, sy + 62, px + self_w - 44, sy + 88),
         g.theme->ink);
    const D2D1_RECT_F copy_id = D2D1::RectF(
        px + self_w - 38, sy + 63, px + self_w - 14, sy + 87);
    mini(copy_id, icon::copy, inside(copy_id, g.hover_x, g.hover_y));
    g.hit_copy_id = copy_id;
    if (!g.device_ip.empty()) {
      text(g.target, g.mono_small, g.device_ip,
           D2D1::RectF(name_x, sy + 90, px + self_w - 12, sy + 104),
           g.theme->muted);
    }

    // Verification-code box.
    const D2D1_RECT_F code_box =
        D2D1::RectF(px + 12, sy + 108, px + self_w - 12, sy + 212);
    fill_round(g.target, code_box, 12,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.55f));
    stroke_round(g.target, code_box, 12, g.theme->line);
    draw_icon(g.target, icon::shield_check, code_box.left + 16,
              code_box.top + 14, g.theme->accent);
    text(g.target, g.small_left, L"验证码",
         D2D1::RectF(code_box.left + 26, code_box.top + 4, code_box.right - 96,
                     code_box.top + 24),
         g.theme->muted);
    const float ib_x = code_box.right - 86;
    const D2D1_RECT_F code_eye = D2D1::RectF(ib_x, code_box.top + 4, ib_x + 24,
                                              code_box.top + 28);
    mini(code_eye, g.code_visible ? icon::eye_off : icon::eye,
         inside(code_eye, g.hover_x, g.hover_y));
    const D2D1_RECT_F code_copy =
        D2D1::RectF(ib_x + 26, code_box.top + 4, ib_x + 50, code_box.top + 28);
    mini(code_copy, icon::copy, inside(code_copy, g.hover_x, g.hover_y));
    const D2D1_RECT_F code_refresh =
        D2D1::RectF(ib_x + 52, code_box.top + 4, ib_x + 76, code_box.top + 28);
    mini(code_refresh, icon::rotate,
         inside(code_refresh, g.hover_x, g.hover_y));
    g.hit_code_eye = code_eye;
    g.hit_code_copy = code_copy;
    g.hit_code_refresh = code_refresh;

    const std::wstring shown_code =
        g.code_visible
            ? (g.device_code.empty() ? std::wstring(L"—") : g.device_code)
            : std::wstring(L"••••••");
    text(g.target, g.mono, shown_code,
         D2D1::RectF(code_box.left + 12, code_box.top + 30,
                     code_box.right - 120, code_box.top + 60),
         g.theme->ink);
    const D2D1_RECT_F single =
        D2D1::RectF(code_box.right - 102, code_box.top + 34,
                    code_box.right - 58, code_box.top + 56);
    const D2D1_RECT_F multi =
        D2D1::RectF(code_box.right - 54, code_box.top + 34,
                    code_box.right - 10, code_box.top + 56);
    const bool single_active = !g.auth_multi;
    fill_round(g.target, single, 11,
               single_active ? g.theme->accent_soft
                             : D2D1::ColorF(g.theme->panel.r,
                                            g.theme->panel.g,
                                            g.theme->panel.b,
                                            inside(single, g.hover_x, g.hover_y)
                                                ? 1.0f
                                                : 0.0f));
    text(g.target, g.tiny, L"单次", single,
         single_active ? g.theme->accent : g.theme->muted);
    fill_round(g.target, multi, 11,
               !single_active ? g.theme->accent_soft
                              : D2D1::ColorF(g.theme->panel.r,
                                             g.theme->panel.g,
                                             g.theme->panel.b,
                                             inside(multi, g.hover_x, g.hover_y)
                                                 ? 1.0f
                                                 : 0.0f));
    text(g.target, g.tiny, L"多次", multi,
         !single_active ? g.theme->accent : g.theme->muted);
    g.hit_code_single = single;
    g.hit_code_multi = multi;
    if (g.code_budget_known) {
      const int total =
          g.auth_multi ? (g.code_uses > 0 ? g.code_uses : 20) : 1;
      const std::wstring uses = L"剩余 " +
                                std::to_wstring(g.code_remaining) + L" / " +
                                std::to_wstring(total) + L" 次";
      text(g.target, g.tiny, uses,
           D2D1::RectF(code_box.left + 12, code_box.top + 66,
                       code_box.left + 16 + measure_text(g.tiny, uses),
                       code_box.top + 86),
           g.theme->muted);
    }

    const D2D1_RECT_F more =
        D2D1::RectF(px + 12, sy + 218, px + self_w - 12, sy + 236);
    g.hit_more_settings = more;
    draw_icon(g.target, icon::server, (more.left + more.right) / 2 - 30,
              (more.top + more.bottom) / 2, g.theme->muted);
    text(g.target, g.tiny, L"更多设置",
         D2D1::RectF(more.left + 22, more.top, more.right, more.bottom),
         inside(more, g.hover_x, g.hover_y) ? g.theme->ink
                                            : g.theme->muted);

    // ---- ConnectCard ----
    const D2D1_RECT_F conn =
        stacked ? D2D1::RectF(px, sy + row1_h + gap, px + content_w,
                              sy + row1_h + gap + row1_h)
                : D2D1::RectF(px + self_w + gap, sy,
                              px + self_w + gap + connect_w, sy + row1_h);
    card(conn);
    const float cx = conn.left;
    const float conny = conn.top;
    text(g.target, g.subtitle, L"连接设备",
         D2D1::RectF(cx + 12, conny + 8, cx + 220, conny + 28), g.theme->ink);
    text(g.target, g.tiny, L"输入设备 ID 或地址，端口默认 28418",
         D2D1::RectF(cx + 12, conny + 30, cx + 320, conny + 44),
         g.theme->muted);

    // Session-intent segment at the top right.
    const float seg_w = 250;
    const D2D1_RECT_F seg =
        D2D1::RectF(conn.right - 12 - seg_w, conny + 8, conn.right - 12, conny + 34);
    fill_round(g.target, seg, 17,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.70f));
    stroke_round(g.target, seg, 17, g.theme->line);
    const wchar_t* mode_labels[3] = {L"远程桌面", L"文件传输", L"聊天"};
    const icon mode_icons[3] = {icon::monitor, icon::upload, icon::chat};
    for (int i = 0; i < 3; ++i) {
      const float mw = (seg_w - 4) / 3;
      const D2D1_RECT_F r =
          D2D1::RectF(seg.left + 2 + i * mw, seg.top + 2,
                      seg.left + 2 + (i + 1) * mw, seg.bottom - 2);
      const bool active = g.intent == i;
      if (active) {
        fill_round(g.target, r, 15, g.theme->accent_soft);
      } else if (inside(r, g.hover_x, g.hover_y)) {
        fill_round(g.target, r, 15,
                   D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                g.theme->panel.b, 1.0f));
      }
      const float tw = measure_text(g.small_left, mode_labels[i]);
      const float total = tw + 18;
      const float tx = r.left + (r.right - r.left - total) / 2;
      draw_icon(g.target, mode_icons[i], tx + 6, (r.top + r.bottom) / 2,
                active ? g.theme->accent : g.theme->muted);
      text(g.target, g.small_left, mode_labels[i],
           D2D1::RectF(tx + 15, r.top, r.right, r.bottom),
           active ? g.theme->accent : g.theme->muted);
      g.hit_mode[i] = r;
    }

    // Big target input with the inline send button.
    const D2D1_RECT_F input =
        D2D1::RectF(cx + 12, conny + 54, conn.right - 12, conny + 102);
    // Web `.field`: panel -> surface on focus, accent-mixed border and the
    // 3 px accent-soft focus ring.
    const float input_hover =
        hover_lerp(hv_home_address, inside(input, g.hover_x, g.hover_y));
    fill_round(g.target, input, 12,
               g.focus_field == 1 ? g.theme->surface : g.theme->panel);
    stroke_round(g.target, input, 12,
                 g.focus_field == 1
                     ? mix_color(g.theme->line, g.theme->accent, 0.65f)
                     : mix_color(g.theme->line, g.theme->accent,
                                 0.22f * input_hover));
    if (g.focus_field == 1) {
      stroke_round(g.target,
                   D2D1::RectF(input.left - 3, input.top - 3, input.right + 3,
                               input.bottom + 3),
                   15.0f, g.theme->accent_soft, 3.0f);
    }
    const bool address_empty = g.address_text.empty();
    if (g.sel_field == 1 && g.sel_start < g.sel_end) {
      const float x0 = input.left + 12;
      const float xa = x0 + measure_text(
                                 g.mono_input,
                                 g.address_text.substr(0, g.sel_start));
      const float xb = x0 + measure_text(
                                 g.mono_input,
                                 g.address_text.substr(0, g.sel_end));
      fill_round(g.target,
                 D2D1::RectF(xa, input.top + 8, xb, input.bottom - 8), 2,
                 D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                              g.theme->accent.b, 0.22f));
    }
    text(g.target, g.mono_input,
         address_empty ? L"设备 ID 或 IP[:端口]" : g.address_text,
         D2D1::RectF(input.left + 12, input.top, input.right - 48,
                     input.bottom),
         address_empty ? g.theme->muted : g.theme->ink);
    if (g.focus_field == 1 && g.caret_on) {
      const float caret_x =
          input.left + 12 +
          measure_text(g.mono_input,
                       g.address_text.substr(0, g.address_caret));
      line(g.target, caret_x, input.top + 14, caret_x, input.bottom - 14,
           g.theme->accent, 1.5f);
    }
    g.hit_connect_input_home = input;
    g.hit_connect_send =
        D2D1::RectF(input.right - 42, input.top + 6, input.right - 6,
                    input.bottom - 6);
    fill_round(g.target, g.hit_connect_send, 8,
               g.connecting
                   ? D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                                  g.theme->accent.b, 0.55f)
                   : (inside(g.hit_connect_send, g.hover_x, g.hover_y)
                          ? mix_color(g.theme->accent, g.theme->ink, 0.12f)
                          : g.theme->accent));
    draw_icon(g.target, icon::send,
              (g.hit_connect_send.left + g.hit_connect_send.right) / 2,
              (g.hit_connect_send.top + g.hit_connect_send.bottom) / 2,
              g.theme->on_accent);

    // Code row.
    const D2D1_RECT_F code =
        D2D1::RectF(cx + 12, conny + 112, cx + 372, conny + 146);
    fill_round(g.target, code, 12,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.50f));
    stroke_round(g.target, code, 12, g.theme->line);
    draw_icon(g.target, icon::key, code.left + 16, (code.top + code.bottom) / 2,
              g.theme->muted);
    const bool code_empty = g.code_text.empty();
    const std::wstring code_shown =
        code_empty ? L"验证码（可选）"
                   : (g.connect_code_visible
                          ? g.code_text
                          : std::wstring(g.code_text.size(), L'•'));
    if (g.sel_field == 2 && g.sel_start < g.sel_end) {
      const float x0 = code.left + 30;
      const float xa = x0 + measure_text(g.small_left,
                                         code_shown.substr(0, g.sel_start));
      const float xb = x0 + measure_text(g.small_left,
                                         code_shown.substr(0, g.sel_end));
      fill_round(g.target,
                 D2D1::RectF(xa, code.top + 8, xb, code.bottom - 8), 2,
                 D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                              g.theme->accent.b, 0.22f));
    }
    text(g.target, g.small_left, code_shown,
         D2D1::RectF(code.left + 30, code.top, code.right - 32, code.bottom),
         code_empty ? g.theme->muted : g.theme->ink);
    if (g.focus_field == 2 && g.caret_on) {
      const float caret_x =
          code.left + 30 +
          measure_text(g.small_left, code_shown.substr(0, g.code_caret));
      line(g.target, caret_x, code.top + 8, caret_x, code.bottom - 8,
           g.theme->accent, 1.5f);
    }
    g.hit_connect_code_home = code;
    g.hit_connect_code_eye =
        D2D1::RectF(code.right - 28, code.top + 5, code.right - 6,
                    code.bottom - 5);
    const float eye_hover =
        hover_lerp(hv_home_code_eye,
                   inside(g.hit_connect_code_eye, g.hover_x, g.hover_y));
    if (eye_hover > 0.01f) {
      fill_round(g.target, g.hit_connect_code_eye, 6,
                 D2D1::ColorF(g.theme->panel_strong.r, g.theme->panel_strong.g,
                              g.theme->panel_strong.b, eye_hover));
    }
    draw_icon(g.target, g.connect_code_visible ? icon::eye_off : icon::eye,
              code.right - 17, (code.top + code.bottom) / 2,
              mix_color(g.theme->muted, g.theme->ink, eye_hover));
    note_tip(1200,
             g.connect_code_visible ? L"隐藏验证码" : L"显示验证码",
             g.hit_connect_code_eye,
             inside(g.hit_connect_code_eye, g.hover_x, g.hover_y));
    text(g.target, g.tiny,
         code_empty ? L"不填验证码将发送请求，由对方确认"
                    : L"填写验证码将直接连接",
         D2D1::RectF(cx + 384, conny + 118, conn.right - 12, conny + 134),
         g.theme->muted);

    // Force relay checkbox.
    const D2D1_RECT_F relay_box =
        D2D1::RectF(cx + 14, conny + 154, cx + 26, conny + 166);
    fill_round(g.target, relay_box, 3,
               g.force_relay ? g.theme->accent : g.theme->panel);
    stroke_round(g.target, relay_box, 3,
                 g.force_relay ? g.theme->accent : g.theme->line);
    if (g.force_relay) {
      draw_icon(g.target, icon::check, relay_box.left + 6, relay_box.top + 6,
                g.theme->on_accent);
    }
    text(g.target, g.tiny, L"强制中继（不打洞）",
         D2D1::RectF(cx + 32, conny + 152, cx + 180, conny + 168),
         g.theme->muted);
    g.hit_relay = D2D1::RectF(cx + 10, conny + 148, cx + 190, conny + 172);

    // Connect button and port hint.
    g.hit_connect_btn =
        D2D1::RectF(cx + 12, conny + 176, conn.right - 12, conny + 212);
    const bool needs_code = g.code_text.empty();
    const D2D1_COLOR_F connect_bg =
        g.connecting
            ? D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                           g.theme->accent.b, 0.55f)
            : (inside(g.hit_connect_btn, g.hover_x, g.hover_y)
                   ? mix_color(g.theme->accent, g.theme->ink, 0.12f)
                   : g.theme->accent);
    fill_round(g.target, g.hit_connect_btn, 10, connect_bg);
    const std::wstring connect_label =
        g.connecting ? L"正在连接…"
                     : (needs_code ? L"发送连接请求" : L"连接");
    const float cw_text = measure_text(g.small_bold, connect_label);
    draw_icon(g.target, icon::send,
              (g.hit_connect_btn.left + g.hit_connect_btn.right) / 2 -
                  cw_text / 2 - 16,
              (g.hit_connect_btn.top + g.hit_connect_btn.bottom) / 2,
              g.theme->on_accent);
    text(g.target, g.small_bold, connect_label, g.hit_connect_btn,
         g.theme->on_accent);
    {
      const bool has_error = !g.connect_error.empty();
      const D2D1_COLOR_F danger =
          g.theme == &kLight ? D2D1::ColorF(0xc0392b)
                             : D2D1::ColorF(0xff8f87);
      text(g.target, g.tiny,
           has_error ? g.connect_error
                     : L"提示：输入对方设备 ID，或 局域网地址:端口（默认 28418）",
           D2D1::RectF(cx + 12, conny + 218, conn.right - 12, conny + 232),
           has_error ? danger : g.theme->muted);
    }

    // ---- NoticePanel + DeviceList ----
    const float row2 =
        stacked ? conn.bottom + gap : sy + row1_h + gap;
    const D2D1_RECT_F notice =
        stacked ? D2D1::RectF(px, row2, px + content_w, row2 + row2_h)
                : D2D1::RectF(px, row2, px + notice_w, row2 + row2_h);
    card(notice);
    draw_icon(g.target, icon::shield, notice.left + 18, notice.top + 17,
              g.theme->accent);
    text(g.target, g.heading, L"通知",
         D2D1::RectF(notice.left + 28, notice.top + 8, notice.right - 12,
                     notice.top + 28),
         g.theme->ink);
    const D2D1_RECT_F srv_box =
        D2D1::RectF(notice.left + 10, notice.top + 34, notice.right - 10,
                    notice.top + 66);
    fill_round(g.target, srv_box, 12,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.40f));
    stroke_round(g.target, srv_box, 12, g.theme->line_soft);
    draw_icon(g.target, icon::server, srv_box.left + 14,
              (srv_box.top + srv_box.bottom) / 2, g.theme->accent);
    text(g.target, g.small_left, g.id_status,
         D2D1::RectF(srv_box.left + 26, srv_box.top, srv_box.right - 8,
                     srv_box.bottom),
         g.theme->ink);
    // Same update state as 设置 → 关于: checking a release is not the same as
    // opening the download page. The banner only offers 立即更新 once a newer
    // tag is known.
    std::wstring notice_text;
    const wchar_t* notice_button = L"检查更新";
    switch (g.update_state) {
      case 1:
        notice_text = L"正在检查更新…";
        notice_button = L"检查中…";
        break;
      case 2:
        notice_text = L"已是最新版本（v" + std::wstring(kNativeVersion) + L"）";
        notice_button = L"已是最新";
        break;
      case 3:
        notice_text = L"发现新版本 " + g.update_latest;
        notice_button = L"立即更新";
        break;
      default:
        notice_text = L"当前版本 v" + std::wstring(kNativeVersion);
        break;
    }
    const D2D1_RECT_F update =
        D2D1::RectF(notice.left + 10, notice.top + 74, notice.right - 10,
                    notice.top + 142);
    fill_round(g.target, update, 12,
               D2D1::ColorF(g.theme->accent_soft.r, g.theme->accent_soft.g,
                            g.theme->accent_soft.b, 0.55f));
    stroke_round(g.target, update, 12,
                 D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                              g.theme->accent.b, 0.40f));
    text(g.target, g.small_left, notice_text,
         D2D1::RectF(update.left + 10, update.top + 8, update.right - 10,
                     update.top + 26),
         g.update_state == 3 ? g.theme->accent : g.theme->muted);
    g.hit_notice_update =
        D2D1::RectF(update.left + 10, update.top + 32, update.left + 90,
                    update.top + 56);
    fill_round(g.target, g.hit_notice_update, 8, g.theme->accent);
    draw_icon(g.target, icon::arrow_up_right, g.hit_notice_update.left + 12,
              (g.hit_notice_update.top + g.hit_notice_update.bottom) / 2,
              g.theme->on_accent);
    text(g.target, g.tiny, notice_button,
         D2D1::RectF(g.hit_notice_update.left + 22, g.hit_notice_update.top,
                     g.hit_notice_update.right, g.hit_notice_update.bottom),
         g.theme->on_accent);
    const D2D1_RECT_F nat =
        D2D1::RectF(notice.left + 10, notice.top + 150, notice.right - 10,
                    notice.bottom - 10);
    stroke_round(g.target, nat, 12, g.theme->line_soft);
    draw_icon(g.target, icon::info, nat.left + 16, nat.top + 16,
              g.theme->accent);
    text(g.target, g.tiny_wrap,
         L"远程桌面需要对方网络可达；NAT 或防火墙后的设备可能显示在线但无法"
         L"直连。",
         D2D1::RectF(nat.left + 28, nat.top + 8, nat.right - 8,
                     nat.bottom - 6),
         g.theme->muted);

    const D2D1_RECT_F list =
        stacked ? D2D1::RectF(px, row2 + row2_h + gap, px + content_w,
                              row2 + row2_h * 2 + gap)
                : D2D1::RectF(px + notice_w + gap, row2,
                              px + notice_w + gap + list_w, row2 + row2_h);
    card(list);
    // Tabs header.
    const float tab_y = list.top + 9;
    const wchar_t* tabs[4] = {L"最近", L"已保存", L"局域网", L"联网"};
    float tx = list.left + 10;
    for (int i = 0; i < 4; ++i) {
      std::wstring tab_label = tabs[i];
      if (i == 0 && !g.recents.empty()) {
        tab_label += L" " + std::to_wstring(g.recents.size());
      }
      const float tw = measure_text(g.label12, tab_label) + 20;
      const D2D1_RECT_F r = D2D1::RectF(tx, tab_y, tx + tw, tab_y + 26);
      const bool active = g.device_tab == i;
      if (active) {
        fill_round(g.target, r, 13, g.theme->accent_soft);
      } else if (inside(r, g.hover_x, g.hover_y)) {
        fill_round(g.target, r, 13, g.theme->panel);
      }
      text(g.target, g.label12, tab_label, r,
           active ? g.theme->accent : g.theme->muted);
      g.hit_dev_tab[i] = r;
      tx += tw + 2;
    }
    const D2D1_RECT_F grid_btn =
        D2D1::RectF(list.right - 70, list.top + 8, list.right - 42,
                    list.top + 36);
    const D2D1_RECT_F list_btn =
        D2D1::RectF(list.right - 40, list.top + 8, list.right - 12,
                    list.top + 36);
    if (g.history_view == 0) fill_round(g.target, grid_btn, 6, g.theme->panel);
    draw_icon(g.target, icon::grid, grid_btn.left + 14, grid_btn.top + 14,
              g.history_view == 0 ? g.theme->ink : g.theme->muted);
    if (g.history_view == 1) fill_round(g.target, list_btn, 6, g.theme->panel);
    draw_icon(g.target, icon::list, list_btn.left + 14, list_btn.top + 14,
              g.history_view == 1 ? g.theme->ink : g.theme->muted);
    g.hit_view_cards = grid_btn;
    g.hit_view_list = list_btn;
    line(g.target, list.left, list.top + 44, list.right, list.top + 44,
         g.theme->line_soft, 1.0f);
    const D2D1_RECT_F empty =
        D2D1::RectF(list.left + 10, list.top + 54, list.right - 10,
                    list.bottom - 10);
    for (int i = 0; i < 4; ++i) {
      g.hit_recent_card[i] = D2D1::RectF(0, 0, 0, 0);
      g.hit_recent_forget[i] = D2D1::RectF(0, 0, 0, 0);
      g.hit_recent_star[i] = D2D1::RectF(0, 0, 0, 0);
      g.hit_recent_edit[i] = D2D1::RectF(0, 0, 0, 0);
      for (int j = 0; j < 3; ++j) {
        g.hit_recent_action[i][j] = D2D1::RectF(0, 0, 0, 0);
      }
    }
    // 最近 shows everything; 已保存 shows only starred rows (the web's
    // savedDevices list). Positions index this vector, not g.recents.
    std::vector<int> visible;
    for (int i = 0; i < static_cast<int>(g.recents.size()); ++i) {
      if (g.device_tab == 0 || (g.device_tab == 1 && g.recents[i].saved)) {
        visible.push_back(i);
      }
    }
    const int recent_count =
        (std::min)(4, static_cast<int>(visible.size()));
    if ((g.device_tab == 0 || g.device_tab == 1) && recent_count > 0) {
      if (g.history_view == 0) {
        const float card_gap = 8.0f;
        const float card_h = 78.0f;
        const float card_w = (empty.right - empty.left - card_gap) / 2;
        for (int i = 0; i < recent_count; ++i) {
          const int row = i / 2, col = i % 2;
          const D2D1_RECT_F card = D2D1::RectF(
              empty.left + col * (card_w + card_gap),
              empty.top + row * (card_h + card_gap),
              empty.left + col * (card_w + card_gap) + card_w,
              empty.top + row * (card_h + card_gap) + card_h);
          hover_lerp(hv_recent_card0 + i, inside(card, g.hover_x, g.hover_y));
          const float hover = g_hover_value[hv_recent_card0 + i];
          // Row actions appear on hover, like the web's group-hover buttons.
          const float reveal = hover;
          auto faded = [reveal](const D2D1_COLOR_F& c) {
            return fade_alpha(c, reveal);
          };
          fill_round(g.target, card, 12,
                     D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                  g.theme->panel.b, 0.35f + 0.25f * hover));
          stroke_round(g.target, card, 12,
                       mix_color(g.theme->line, g.theme->accent,
                                 0.40f * hover));
          const D2D1_RECT_F avatar =
              D2D1::RectF(card.left + 10, card.top + 10, card.left + 50,
                          card.top + 50);
          // Avatar: the host's preset colour + initial once the welcome told
          // us; the plain monitor icon until then (web parity).

          const int idx = visible[i];
          const app_state::recent_entry& entry = g.recents[idx];
          const std::wstring display_name =
              entry.note.empty() ? entry.label : entry.note;
          bool avatar_drawn = false;
          if (entry.avatar.rfind(L"file:", 0) == 0) {
            ID2D1Bitmap* picture =
                avatar_bitmap(g.target, entry.avatar.substr(5));
            ID2D1BitmapBrush* brush = nullptr;
            if (picture != nullptr &&
                SUCCEEDED(g.target->CreateBitmapBrush(picture, &brush)) &&
                brush != nullptr) {
              brush->SetExtendModeX(D2D1_EXTEND_MODE_CLAMP);
              brush->SetExtendModeY(D2D1_EXTEND_MODE_CLAMP);
              const D2D1_SIZE_F size = picture->GetSize();
              if (size.width > 0.0f && size.height > 0.0f) {
                brush->SetTransform(
                    D2D1::Matrix3x2F::Scale(
                        (avatar.right - avatar.left) / size.width,
                        (avatar.bottom - avatar.top) / size.height) *
                    D2D1::Matrix3x2F::Translation(avatar.left, avatar.top));
                // A bitmap brush in a rounded rect keeps the picture's
                // corners round without a layer mask.
                g.target->FillRoundedRectangle(
                    D2D1::RoundedRect(avatar, 10, 10), brush);
                avatar_drawn = true;
              }
              brush->Release();
            }
          }
          if (!avatar_drawn && entry.avatar.empty()) {
            fill_round(g.target, avatar, 12, g.theme->accent_soft);
            draw_icon(g.target, icon::monitor, card.left + 30, card.top + 30,
                      g.theme->accent);
          } else if (!avatar_drawn) {
            const D2D1_COLOR_F tint = avatar_tint(entry.avatar);
            fill_round(g.target, avatar, 12, fade_alpha(tint, 0.16f));
            const std::wstring initial = avatar_initial(display_name);
            const float iw = measure_text(g.name16, initial);
            text(g.target, g.name16, initial,
                 D2D1::RectF((avatar.left + avatar.right) / 2 - iw / 2 - 2,
                             avatar.top,
                             (avatar.left + avatar.right) / 2 + iw / 2 + 2,
                             avatar.bottom + 1),
                 tint);
          }
          const D2D1_COLOR_F dot =
              entry.online ? g.theme->accent_strong
                           : D2D1::ColorF(g.theme->muted.r, g.theme->muted.g,
                                          g.theme->muted.b, 0.5f);
          fill_round(g.target,
                     D2D1::RectF(card.left + 59, card.top + 16,
                                 card.left + 66, card.top + 23),
                     4, dot);
          text(g.target, g.body_medium, display_name,
               D2D1::RectF(card.left + 70, card.top + 8,
                           card.right - 100, card.top + 28),
               g.theme->ink);
          text(g.target, g.mono_small, entry.target,
               D2D1::RectF(card.left + 70, card.top + 27,
                           card.right - 100, card.top + 43),
               g.theme->muted);
          text(g.target, g.small_left,
               entry.platform + L" · " + relative_time(entry.at_ms),
               D2D1::RectF(card.left + 70, card.top + 45,
                           card.right - 100, card.top + 61),
               g.theme->muted);
          // Star (save to 已保存) and rename, like the web's saved devices.
          const D2D1_RECT_F star =
              D2D1::RectF(card.right - 62, card.top + 7, card.right - 38,
                          card.top + 31);
          const D2D1_RECT_F edit =
              D2D1::RectF(card.right - 90, card.top + 7, card.right - 66,
                          card.top + 31);
          if (inside(star, g.hover_x, g.hover_y)) {
            fill_round(g.target, star, 6, faded(g.theme->panel));
          }
          if (entry.saved) {
            draw_solid_star(g.target, (star.left + star.right) / 2,
                            (star.top + star.bottom) / 2, 6.5f,
                            fade_alpha(D2D1::ColorF(0xffc02e), reveal));
          } else {
            draw_icon(g.target, icon::star, (star.left + star.right) / 2,
                      (star.top + star.bottom) / 2, faded(g.theme->muted));
          }
          note_tip(1500 + i, entry.saved ? L"取消保存" : L"保存到已保存",
                   star, inside(star, g.hover_x, g.hover_y));
          g.hit_recent_star[i] = star;
          if (inside(edit, g.hover_x, g.hover_y)) {
            fill_round(g.target, edit, 6, faded(g.theme->panel));
          }
          draw_icon(g.target, icon::pencil, (edit.left + edit.right) / 2,
                    (edit.top + edit.bottom) / 2, faded(g.theme->muted));
          note_tip(1600 + i, L"重命名", edit,
                   inside(edit, g.hover_x, g.hover_y));
          g.hit_recent_edit[i] = edit;
          const icon actions[3] = {icon::monitor, icon::folder, icon::chat};
          for (int j = 0; j < 3; ++j) {
            const D2D1_RECT_F r = D2D1::RectF(
                card.right - 122 + j * 28, card.top + 27,
                card.right - 98 + j * 28, card.top + 51);
            if (inside(r, g.hover_x, g.hover_y)) {
              fill_round(g.target, r, 6, faded(g.theme->panel));
            }
            draw_icon(g.target, actions[j], (r.left + r.right) / 2,
                      (r.top + r.bottom) / 2, faded(g.theme->muted));
            static const wchar_t* action_tips[3] = {
                L"远程桌面", L"文件传输", L"聊天"};
            note_tip(1300 + i * 3 + j, action_tips[j], r,
                     inside(r, g.hover_x, g.hover_y));
            g.hit_recent_action[i][j] = r;
          }
          // Forget (web Trash2): removes the row from the list.
          const D2D1_RECT_F forget =
              D2D1::RectF(card.right - 34, card.top + 7, card.right - 10,
                          card.top + 31);
          if (inside(forget, g.hover_x, g.hover_y)) {
            fill_round(g.target, forget, 6,
                       fade_alpha(D2D1::ColorF(0xff8f87, 0.14f), reveal));
          }
          draw_icon(g.target, icon::trash, (forget.left + forget.right) / 2,
                    (forget.top + forget.bottom) / 2,
                    fade_alpha(inside(forget, g.hover_x, g.hover_y)
                                   ? (g.theme == &kLight
                                          ? D2D1::ColorF(0xc0392b)
                                          : D2D1::ColorF(0xff8f87))
                                   : g.theme->muted,
                               reveal));
          note_tip(1400 + i, L"从最近连接中移除", forget,
                   inside(forget, g.hover_x, g.hover_y));
          g.hit_recent_forget[i] = forget;
          g.hit_recent_card[i] = card;
        }
      } else {
        const float row_h = 34.0f;
        for (int i = 0; i < recent_count; ++i) {
          const D2D1_RECT_F row = D2D1::RectF(
              empty.left, empty.top + i * row_h, empty.right,
              empty.top + (i + 1) * row_h);
          if (i > 0) {
            line(g.target, row.left, row.top, row.right, row.top,
                 g.theme->line_soft, 1.0f);
          }
          if (inside(row, g.hover_x, g.hover_y)) {
            fill_round(g.target, row, 6, g.theme->panel);
          }
          const int idx = visible[i];
          const app_state::recent_entry& entry = g.recents[idx];
          const std::wstring display_name =
              entry.note.empty() ? entry.label : entry.note;
          hover_lerp(hv_recent_card0 + i, inside(row, g.hover_x, g.hover_y));
          const float reveal = g_hover_value[hv_recent_card0 + i];
          auto faded = [reveal](const D2D1_COLOR_F& c) {
            return fade_alpha(c, reveal);
          };
          const D2D1_RECT_F avatar_box =
              D2D1::RectF(row.left + 2, row.top + 5, row.left + 26,
                          row.top + 29);
          bool avatar_drawn = false;
          if (entry.avatar.rfind(L"file:", 0) == 0) {
            ID2D1Bitmap* picture =
                avatar_bitmap(g.target, entry.avatar.substr(5));
            ID2D1BitmapBrush* brush = nullptr;
            if (picture != nullptr &&
                SUCCEEDED(g.target->CreateBitmapBrush(picture, &brush)) &&
                brush != nullptr) {
              brush->SetExtendModeX(D2D1_EXTEND_MODE_CLAMP);
              brush->SetExtendModeY(D2D1_EXTEND_MODE_CLAMP);
              const D2D1_SIZE_F size = picture->GetSize();
              if (size.width > 0.0f && size.height > 0.0f) {
                brush->SetTransform(
                    D2D1::Matrix3x2F::Scale(
                        (avatar_box.right - avatar_box.left) / size.width,
                        (avatar_box.bottom - avatar_box.top) / size.height) *
                    D2D1::Matrix3x2F::Translation(avatar_box.left,
                                                  avatar_box.top));
                g.target->FillRoundedRectangle(
                    D2D1::RoundedRect(avatar_box, 6, 6), brush);
                avatar_drawn = true;
              }
              brush->Release();
            }
          }
          if (!avatar_drawn && entry.avatar.empty()) {
            fill_round(g.target, avatar_box, 8, g.theme->accent_soft);
            draw_icon(g.target, icon::monitor, row.left + 14, row.top + 17,
                      g.theme->accent);
          } else if (!avatar_drawn) {
            const D2D1_COLOR_F tint = avatar_tint(entry.avatar);
            fill_round(g.target, avatar_box, 8, fade_alpha(tint, 0.16f));
            text(g.target, g.small, avatar_initial(display_name), avatar_box,
                 tint);
          }
          text(g.target, g.label12, display_name,
               D2D1::RectF(row.left + 34, row.top, row.left + 220,
                           row.bottom),
               g.theme->ink);
          text(g.target, g.mono_small, entry.target,
               D2D1::RectF(row.left + 224, row.top, row.left + 500,
                           row.bottom),
               g.theme->muted);
          text(g.target, g.small_left, relative_time(entry.at_ms),
               D2D1::RectF(row.left + 510, row.top, row.left + 620,
                           row.bottom),
               g.theme->muted);
          const D2D1_RECT_F star =
              D2D1::RectF(row.right - 178, row.top + 5, row.right - 154,
                          row.top + 29);
          const D2D1_RECT_F edit =
              D2D1::RectF(row.right - 150, row.top + 5, row.right - 126,
                          row.top + 29);
          if (inside(star, g.hover_x, g.hover_y)) {
            fill_round(g.target, star, 6, faded(g.theme->panel_strong));
          }
          if (entry.saved) {
            draw_solid_star(g.target, (star.left + star.right) / 2,
                            (star.top + star.bottom) / 2, 6.5f,
                            fade_alpha(D2D1::ColorF(0xffc02e), reveal));
          } else {
            draw_icon(g.target, icon::star, (star.left + star.right) / 2,
                      (star.top + star.bottom) / 2, faded(g.theme->muted));
          }
          note_tip(1500 + i, entry.saved ? L"取消保存" : L"保存到已保存",
                   star, inside(star, g.hover_x, g.hover_y));
          g.hit_recent_star[i] = star;
          if (inside(edit, g.hover_x, g.hover_y)) {
            fill_round(g.target, edit, 6, faded(g.theme->panel_strong));
          }
          draw_icon(g.target, icon::pencil, (edit.left + edit.right) / 2,
                    (edit.top + edit.bottom) / 2, faded(g.theme->muted));
          note_tip(1600 + i, L"重命名", edit,
                   inside(edit, g.hover_x, g.hover_y));
          g.hit_recent_edit[i] = edit;
          const icon actions[3] = {icon::monitor, icon::folder, icon::chat};
          for (int j = 0; j < 3; ++j) {
            const D2D1_RECT_F r = D2D1::RectF(
                row.right - 122 + j * 28, row.top + 5,
                row.right - 98 + j * 28, row.top + 29);
            if (inside(r, g.hover_x, g.hover_y)) {
              fill_round(g.target, r, 6, faded(g.theme->panel_strong));
            }
            draw_icon(g.target, actions[j], (r.left + r.right) / 2,
                      (r.top + r.bottom) / 2, faded(g.theme->muted));
            g.hit_recent_action[i][j] = r;
          }
          const D2D1_RECT_F forget =
              D2D1::RectF(row.right - 34, row.top + 5, row.right - 10,
                          row.top + 29);
          if (inside(forget, g.hover_x, g.hover_y)) {
            fill_round(g.target, forget, 6,
                       fade_alpha(D2D1::ColorF(0xff8f87, 0.14f), reveal));
          }
          draw_icon(g.target, icon::trash, (forget.left + forget.right) / 2,
                    (forget.top + forget.bottom) / 2,
                    fade_alpha(inside(forget, g.hover_x, g.hover_y)
                                   ? (g.theme == &kLight
                                          ? D2D1::ColorF(0xc0392b)
                                          : D2D1::ColorF(0xff8f87))
                                   : g.theme->muted,
                               reveal));
          note_tip(1400 + i, L"从最近连接中移除", forget,
                   inside(forget, g.hover_x, g.hover_y));
          g.hit_recent_forget[i] = forget;
          g.hit_recent_card[i] = row;
        }
      }
    } else {
      const wchar_t* empty_texts[4] = {
          L"还没有最近连接。", L"点击星标把设备保存到这里。",
          L"没有发现局域网设备；可在安全设置中开启局域网发现。",
          L"还没有通过 ID 服务器连接过的设备。"};
      fill_round(g.target, empty, 12,
                 D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                              g.theme->panel.b, 0.30f));
      dashed_round(g.target, empty, 12, g.theme->line, 1.0f);
      text(g.target, g.body_center, empty_texts[g.device_tab],
           D2D1::RectF(empty.left + 12,
                       (empty.top + empty.bottom) / 2 - 12, empty.right - 12,
                       (empty.top + empty.bottom) / 2 + 12),
           g.theme->muted);
    }

    // ---- Status bar ----
    const float status_y =
        stacked ? list.bottom + 12 : row2 + row2_h + 12;
    line(g.target, px, status_y, px + content_w, status_y, g.theme->line_soft,
         1.0f);
    const float status_cy = status_y + 15;
    draw_icon(g.target, icon::server, px + 8, status_cy, g.theme->muted);
    fill_round(g.target, D2D1::RectF(px + 18, status_cy - 3, px + 24,
                                     status_cy + 3),
               3, g.theme->accent_strong);
    text(g.target, g.tiny, g.id_status,
         D2D1::RectF(px + 28, status_y + 5, px + 220, status_y + 25),
         g.theme->muted);
    draw_icon(g.target, icon::activity, px + 236, status_cy, g.theme->muted);
    text(g.target, g.tiny, L"连接状态：未连接",
         D2D1::RectF(px + 246, status_y + 5, px + 420, status_y + 25),
         g.theme->muted);
    text(g.target, g.mono_small, kNativeVersion,
         D2D1::RectF(px + content_w - 72, status_y + 5, px + content_w,
                     status_y + 25),
         g.theme->muted);

    g.target->PopAxisAlignedClip();

    // Home scrollbar.
    if (g.home_max_scroll > 0.5f) {
      const float track_top = 89;
      const float track_bottom = h - 10;
      const float track_h = track_bottom - track_top;
      const D2D1_RECT_F track =
          D2D1::RectF(w - 10, track_top, w - 6, track_bottom);
      fill_round(g.target, track, 2, g.theme->line_soft);
      const float thumb_h =
          (std::max)(24.0f, track_h * view_h / g.home_content_h);
      const float thumb_y =
          track_top + (track_h - thumb_h) *
                          (g.home_scroll / g.home_max_scroll);
      fill_round(g.target,
                 D2D1::RectF(track.left, thumb_y, track.right,
                             thumb_y + thumb_h),
                 2, g.theme->line);
    }

    draw_overlays(w, h);
    draw_tooltip(size);
    draw_toast(size);
    draw_close_prompt(w, h);
    draw_ctx_menu();
    g.target->EndDraw();
    if (std::getenv("MOLINK_PAINT_LOG") != nullptr) {
      const auto finished = std::chrono::steady_clock::now();
      std::fprintf(stderr, "paint %.2f ms\n",
                   std::chrono::duration<double, std::milli>(finished -
                                                             paint_started)
                       .count());
    }
    return;
  }

  // Session page: the picture fills the area below the header, and the web's
  // floating ball / top toolbar sits over it. Both are drawn here so their
  // hit rects always match what is on screen.
  g.stage = g.immersive
                ? D2D1::RectF(0, 0, w, h)
                : D2D1::RectF(16, 95, w - 16, h - 16);
  fill_round(g.target, g.stage, g.immersive ? 0 : 12,
             D2D1::ColorF(0x05070a));
  stroke_round(g.target, g.stage, g.immersive ? 0 : 12, g.theme->line);
  draw_picture(g.stage);
  draw_watermark(g.stage);
  // Remote pointer feedback: the web does not paint the host cursor by
  // default, and neither does this shell unless 设置 → 显示 asks for it.
  if (g.disp_remote_cursor && g.session != nullptr && g.session->live() &&
      g.session->authenticated() && g.session->cursor_visible() &&
      g.session->screen_width() > 0 && g.session->screen_height() > 0 &&
      g.picture_rect.right > g.picture_rect.left) {
    const float sw = static_cast<float>(g.session->screen_width());
    const float sh = static_cast<float>(g.session->screen_height());
    const float px = g.picture_rect.left +
                     (g.session->cursor_x() / sw) *
                         (g.picture_rect.right - g.picture_rect.left);
    const float py = g.picture_rect.top +
                     (g.session->cursor_y() / sh) *
                         (g.picture_rect.bottom - g.picture_rect.top);
    if (inside(g.picture_rect, px, py)) {
      const D2D1_COLOR_F outline = D2D1::ColorF(0, 0, 0, 0.85f);
      const D2D1_COLOR_F fill = D2D1::ColorF(1, 1, 1);
      auto arrow = [&](const D2D1_COLOR_F& color, float width) {
        line(g.target, px, py, px, py + 15, color, width);
        line(g.target, px, py, px + 10, py + 10, color, width);
        line(g.target, px + 10, py + 10, px + 4, py + 11, color, width);
        line(g.target, px + 4, py + 11, px + 2, py + 16, color, width);
      };
      arrow(outline, 3.0f);
      arrow(fill, 1.4f);
    }
  }
  // Keep the last frame visible and mark it, like the web's disconnected
  // overlay, instead of leaving the stage blank during a retry cycle.
  if (g.reconnecting) {
    const std::wstring label = L"连接已断开，正在重连…";
    const float cw = measure_text(g.tiny, label) + 24;
    const float cy = (g.stage.top + g.stage.bottom) / 2;
    const D2D1_RECT_F chip = D2D1::RectF(
        (g.stage.left + g.stage.right) / 2 - cw / 2, cy - 12,
        (g.stage.left + g.stage.right) / 2 + cw / 2, cy + 12);
    fill_round(g.target, chip, 8, D2D1::ColorF(0, 0, 0, 0.66f));
    stroke_round(g.target, chip, 8, D2D1::ColorF(0xf59e0b, 0.45f));
    text(g.target, g.tiny, label, chip, D2D1::ColorF(0xf59e0b));
  }
  g.hit_retry_input = g.hit_retry_btn = D2D1::RectF(0, 0, 0, 0);
  if (g.session != nullptr && !g.session->authenticated() &&
      !g.connect_error.empty()) {
    // Web SessionView error bar: the reason on the left, plus either an
    // inline access-code field (invalid code) or a reconnect button.
    const D2D1_COLOR_F danger =
        g.theme == &kLight ? D2D1::ColorF(0xc0392b)
                           : D2D1::ColorF(0xff8f87);
    // The chat/file drawers are overlays; keep the error bar in the picture
    // column like the web's flex layout so the drawer never hides its retry
    // controls.
    float content_right = g.stage.right - 16;
    float drawer_w = 0;
    if (g.chat_open && g.chat_anim > 0.01f) {
      drawer_w = (std::max)(drawer_w, 340.0f * g.chat_anim);
    }
    if (g.files_open && g.files_anim > 0.01f) {
      drawer_w = (std::max)(drawer_w, 380.0f * g.files_anim);
    }
    if (drawer_w > 0) {
      content_right = (std::min)(content_right, w - drawer_w - 16);
    }
    if (content_right < g.stage.left + 220) {
      content_right = g.stage.right - 16;
    }
    const D2D1_RECT_F banner = D2D1::RectF(
        g.stage.left + 16, g.stage.bottom - 58, content_right,
        g.stage.bottom - 14);
    fill_round(g.target, banner, 10,
               D2D1::ColorF(danger.r, danger.g, danger.b, 0.14f));
    stroke_round(g.target, banner, 10,
                 D2D1::ColorF(danger.r, danger.g, danger.b, 0.45f));
    const bool needs_code = auth_error_code(g.session->last_error_code());
    float text_right = banner.right - 14;
    if (needs_code) {
      g.hit_retry_btn = D2D1::RectF(banner.right - 12 - 60, banner.top + 8,
                                    banner.right - 12, banner.bottom - 8);
      g.hit_retry_input =
          D2D1::RectF(g.hit_retry_btn.left - 8 - 132, banner.top + 8,
                      g.hit_retry_btn.left - 8, banner.bottom - 8);
      text_right = g.hit_retry_input.left - 12;
    } else {
      g.hit_retry_btn = D2D1::RectF(banner.right - 12 - 84, banner.top + 8,
                                    banner.right - 12, banner.bottom - 8);
      text_right = g.hit_retry_btn.left - 12;
    }
    text(g.target, g.small_left, g.connect_error,
         D2D1::RectF(banner.left + 14, banner.top, text_right, banner.bottom),
         danger);
    if (needs_code) {
      const D2D1_RECT_F& field = g.hit_retry_input;
      fill_round(g.target, field, 7, g.theme->surface);
      stroke_round(g.target, field, 7,
                   g.focus_field == 11
                       ? mix_color(g.theme->line, danger, 0.65f)
                       : g.theme->line);
      const float x0 = field.left + 10;
      if (g.focus_field == 11 && g.sel_field == 11 &&
          g.sel_start != g.sel_end) {
        const size_t a = (std::min)(g.sel_start, g.sel_end);
        const size_t b = (std::max)(g.sel_start, g.sel_end);
        const float xa = x0 + measure_text(g.small_left,
                                          g.retry_code.substr(0, a));
        const float xb = x0 + measure_text(g.small_left,
                                          g.retry_code.substr(0, b));
        fill_round(g.target, D2D1::RectF(xa, field.top + 5, xb,
                                         field.bottom - 5),
                   2, D2D1::ColorF(danger.r, danger.g, danger.b, 0.25f));
      }
      text(g.target, g.small_left,
           g.retry_code.empty() ? L"验证码" : g.retry_code,
           D2D1::RectF(x0, field.top, field.right - 8, field.bottom),
           g.retry_code.empty() ? g.theme->muted : g.theme->ink);
      if (g.focus_field == 11 && g.caret_on) {
        const float caret_x =
            x0 + measure_text(g.small_left,
                              g.retry_code.substr(0, g.retry_caret));
        line(g.target, caret_x, field.top + 6, caret_x, field.bottom - 6,
             danger, 1.5f);
      }
      const bool enabled = !g.retry_code.empty();
      fill_round(g.target, g.hit_retry_btn, 7,
                 enabled ? g.theme->accent_soft
                         : D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                        g.theme->panel.b, 0.6f));
      text(g.target, g.label12, L"连接", g.hit_retry_btn,
           enabled ? g.theme->accent : g.theme->muted);
    } else {
      fill_round(g.target, g.hit_retry_btn, 7, g.theme->accent_soft);
      text(g.target, g.label12, L"重新连接", g.hit_retry_btn,
           g.theme->accent);
    }
  }
  draw_toolbar(g.stage);
  draw_quality_panel(w, h);
  draw_menu(g.target, *g.theme, g.small_left, w);
  draw_overlays(w, h);
  draw_tooltip(size);
  draw_toast(size);
  draw_close_prompt(w, h);
  draw_ctx_menu();

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
  g_dpi_base = static_cast<float>(system_dpi_for(window));
  if (g_dpi_base <= 0) g_dpi_base = 96.0f;
  g_dpi = ui_dpi_for(window);
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
  make(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, &g.small_bold);
  make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, false, &g.small_right);
  make(10.0f, DWRITE_FONT_WEIGHT_NORMAL, true, &g.tiny);
  make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, false, &g.menu_text);
  make(12.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, &g.heading);
  make(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, &g.name16);
  make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, false, &g.label12);
  make(13.0f, DWRITE_FONT_WEIGHT_MEDIUM, false, &g.body_medium);
  make(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, &g.subtitle);
  g.write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            20.0f, L"zh-CN", &g.mono);
  g.write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            11.0f, L"zh-CN", &g.mono_small);
  g.write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            14.0f, L"zh-CN", &g.mono_input);
  if (g.mono != nullptr) {
    g.mono->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.mono->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.mono->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  if (g.mono_small != nullptr) {
    g.mono_small->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.mono_small->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.mono_small->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  if (g.mono_input != nullptr) {
    g.mono_input->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.mono_input->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.mono_input->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  make(10.0f, DWRITE_FONT_WEIGHT_NORMAL, false, &g.tiny_wrap);
  if (g.tiny_wrap != nullptr) {
    g.tiny_wrap->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tiny_wrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g.tiny_wrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  }
  if (g.small_right != nullptr) {
    g.small_right->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
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
  // The web sets -webkit-font-smoothing: antialiased; grayscale text avoids
  // the coloured ClearType fringes that do not match the browser rendering.
  g.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  // Product icon for the native title bar; the web header cannot show the
  // exe icon, the client can. Resource id 1 is embedded by ui_kit_demo.rc.
  {
    // Load a large frame and let D2D scale it down: the title bar draws at
    // 20 DIP (40 px at 200 %), so a 32 px source turned blurry next to the
    // browser shell's icon.
    HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 128, 128,
        LR_DEFAULTCOLOR));
    if (icon != nullptr) {
      IWICImagingFactory* wic = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&wic))) &&
          wic != nullptr) {
        IWICBitmap* bitmap = nullptr;
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bitmap)) &&
            bitmap != nullptr &&
            SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            converter != nullptr &&
            SUCCEEDED(converter->Initialize(
                bitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
          // CreateBitmapFromHICON returns a format D2D rejects; convert to
          // premultiplied BGRA first (the same path the avatar loader uses).
          g.target->CreateBitmapFromWicBitmap(converter, nullptr,
                                              &g.title_icon);
        }
        if (converter != nullptr) converter->Release();
        if (bitmap != nullptr) bitmap->Release();
        wic->Release();
      }
      DestroyIcon(icon);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Web-derived overlays: the settings modal, chat drawer and file drawer.
// ---------------------------------------------------------------------------

std::wstring base_name(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

uint64_t file_bytes(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
         data.nFileSizeLow;
}

std::wstring format_bytes(uint64_t bytes) {
  if (bytes >= (1ull << 20)) {
    return std::to_wstring(bytes >> 20) + L" MB";
  }
  if (bytes >= (1ull << 10)) {
    return std::to_wstring(bytes >> 10) + L" KB";
  }
  return std::to_wstring(bytes) + L" B";
}

std::wstring default_download_dir() {
  wchar_t profile[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) {
    return std::wstring();
  }
  return std::wstring(profile) + L"\\Downloads\\moLink";
}

void configure_session(molink::net::session_client* client) {
  if (client == nullptr) return;
  if (!g.download_dir.empty()) client->set_download_dir(g.download_dir);
  // A core token + a shared surface mean the local core decodes the video and
  // publishes it: ask the host for the AV1 stream instead of tiles.
  if (surface_stream_mode()) {
    client->set_stream_decoding(true);
    client->set_request_tiles(false);
  }
}

/// Web `sendFiles`: offer each picked/dropped file to the host. The transfer
/// list is fed by session_client::file_transfers().
void add_transfer(const std::wstring& path) {
  if (path.empty()) return;
  if (g.session == nullptr || !g.session->live()) {
    show_toast(L"请先连接会话后再发送文件");
    return;
  }
  const std::wstring name = base_name(path);
  if (!g.session->file_offer(name, path)) {
    show_toast(L"无法发送 " + name);
    return;
  }
  g.files_open = true;
  g.chat_open = false;
  g.files_tab = 0;
}

void request_files_browse() {
  if (g.session == nullptr || !g.session->live()) return;
  g.files_loading = true;
  g.files_error.clear();
  g.session->file_browse(g.files_path);
}

/// Pumps the file protocol and turns host events into drawer state and
/// toasts, once per UI timer tick.
void handle_file_events() {
  if (g.session == nullptr) return;
  g.session->pump_files();
  for (const molink::net::file_event& event : g.session->take_file_events()) {
    if (event.action == L"listing") {
      g.files_loading = false;
      if (!event.ok) {
        g.files_error =
            event.message.empty() ? L"无法读取主机文件夹" : event.message;
      } else {
        g.files_entries = event.entries;
        if (!event.listing_path.empty()) g.files_path = event.listing_path;
        if (!event.listing_root.empty()) g.files_root = event.listing_root;
        g.files_scroll = 0;
        g.files_error.clear();
      }
      InvalidateRect(g.window, nullptr, FALSE);
    } else if (event.action == L"sending") {
      // The host started sending a file: show the receive tab like the web.
      g.files_open = true;
      g.chat_open = false;
      g.files_tab = 1;
      show_toast(L"开始接收 " + event.name);
      InvalidateRect(g.window, nullptr, FALSE);
    } else if (event.action == L"downloaded") {
      show_toast(L"已保存到 " + event.path);
      InvalidateRect(g.window, nullptr, FALSE);
    } else if (event.action == L"done") {
      show_toast(L"已发送 " + event.name);
      InvalidateRect(g.window, nullptr, FALSE);
    } else if (event.action == L"reject" || event.action == L"error" ||
               event.action == L"cancel" || event.action == L"cancelled") {
      show_toast(event.message.empty() ? L"文件传输失败" : event.message);
      InvalidateRect(g.window, nullptr, FALSE);
    }
  }
}

bool pick_files(HWND window) {
  std::vector<wchar_t> buffer(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = window;
  ofn.lpstrFilter = L"所有文件\0*.*\0";
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
              OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return false;
  // Multi-select returns "dir\0name1\0name2\0\0"; a single file is one path.
  const wchar_t* first = buffer.data();
  const size_t first_len = wcslen(first);
  const wchar_t* second = first + first_len + 1;
  if (*second == L'\0') {
    add_transfer(first);
  } else {
    const std::wstring dir = first;
    for (const wchar_t* name = second; *name != L'\0';
         name += wcslen(name) + 1) {
      add_transfer(dir + L"\\" + name);
    }
  }
  return true;
}

void send_chat() {
  std::wstring text = g.chat_input;
  const size_t begin = text.find_first_not_of(L" \t\r\n");
  if (begin == std::wstring::npos) return;
  const size_t end = text.find_last_not_of(L" \t\r\n");
  text = text.substr(begin, end - begin + 1);
  if (text.empty()) return;
  g.chat_messages.push_back({text, true});
  session_send("{\"type\":\"chat\",\"action\":\"send\",\"text\":\"" +
               json_escape(text) + "\"}");
  g.chat_input.clear();
  g.chat_caret = 0;
}

void dashed_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                  const D2D1_COLOR_F& color, float width);

/// Wrapped 11 px hint text. Returns the y below the last line.
float settings_hint(float x, float y, float width, const std::wstring& text_in) {
  const float line_h = 16.0f;
  std::wstring line;
  float cx = 0;
  for (wchar_t ch : text_in) {
    if (ch == L'\n') {
      text(g.target, g.small_left, line,
           D2D1::RectF(x, y, x + width, y + line_h), g.theme->muted);
      y += line_h;
      line.clear();
      cx = 0;
      continue;
    }
    const float w = ch < 128 ? 6.0f : 11.0f;
    if (!line.empty() && cx + w > width) {
      text(g.target, g.small_left, line,
           D2D1::RectF(x, y, x + width, y + line_h), g.theme->muted);
      y += line_h;
      line.clear();
      cx = 0;
    }
    line += ch;
    cx += w;
  }
  if (!line.empty()) {
    text(g.target, g.small_left, line,
         D2D1::RectF(x, y, x + width, y + line_h), g.theme->muted);
    y += line_h;
  }
  return y;
}

void settings_head(icon glyph, const wchar_t* title, float x, float y) {
  draw_icon(g.target, glyph, x + 7, y + 9, g.theme->accent);
  text(g.target, g.heading, title,
       D2D1::RectF(x + 18, y, x + 460, y + 20), g.theme->ink);
}

void settings_switch(const D2D1_RECT_F& r, bool on, int id) {
  const float h = hover_lerp(id, inside(r, g.hover_x, g.hover_y));
  toggle(g.target, *g.theme, r, on);
  if (h > 0.01f) {
    stroke_round(g.target, r, 11,
                 D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                              g.theme->accent.b, h),
                 1.0f);
  }
}

float settings_toggle_row(float x, float y, float width, const wchar_t* title,
                          const std::wstring& hint, bool on, int id,
                          D2D1_RECT_F* out) {
  text(g.target, g.heading, title,
       D2D1::RectF(x, y, x + width - 60, y + 18), g.theme->ink);
  float yy = y + 20;
  if (!hint.empty()) yy = settings_hint(x, yy, width - 64, hint) + 2;
  const D2D1_RECT_F tr =
      D2D1::RectF(x + width - 44, y + 2, x + width, y + 24);
  settings_switch(tr, on, id);
  if (out != nullptr) *out = tr;
  return (std::max)(yy, y + 28) + 10;
}

void settings_segment(float x, float y, float width,
                      const wchar_t* const* labels, const icon* icons,
                      int count, int selected, D2D1_RECT_F* out, int id0) {
  const float height = 36.0f;
  fill_round(g.target, D2D1::RectF(x, y, x + width, y + height), 8,
             g.theme->panel);
  stroke_round(g.target, D2D1::RectF(x, y, x + width, y + height), 8,
               g.theme->line);
  const float gap = 4.0f;
  const float bw = (width - 8 - gap * (count - 1)) / count;
  for (int i = 0; i < count; ++i) {
    const D2D1_RECT_F r =
        D2D1::RectF(x + 4 + i * (bw + gap), y + 4,
                    x + 4 + i * (bw + gap) + bw, y + 32);
    const bool active = selected == i;
    const float hover = hover_lerp(id0 + i, inside(r, g.hover_x, g.hover_y));
    if (active) {
      fill_round(g.target, r, 6, g.theme->surface);
      stroke_round(g.target, r, 6, g.theme->line_soft);
    }
    const D2D1_COLOR_F color =
        active ? g.theme->accent : mix_color(g.theme->muted, g.theme->ink, hover);
    if (icons != nullptr && icons[i] != icon::none) {
      const float tw = text_width(labels[i], 11.0f);
      const float total = tw + 19.0f;
      const float tx = r.left + (r.right - r.left - total) / 2;
      draw_icon(g.target, icons[i], tx + 6.0f, (r.top + r.bottom) / 2, color);
      text(g.target, g.small_left, labels[i],
           D2D1::RectF(tx + 15, r.top, r.right, r.bottom), color);
    } else {
      text(g.target, g.small, labels[i], r, color);
    }
    if (out != nullptr) out[i] = r;
  }
}

D2D1_RECT_F settings_select(float x, float y, float width,
                            const std::wstring& label, bool open, int id) {
  const D2D1_RECT_F r = D2D1::RectF(x, y, x + width, y + 36);
  const float hover = hover_lerp(id, inside(r, g.hover_x, g.hover_y));
  fill_round(g.target, r, 6, g.theme->panel);
  stroke_round(g.target, r, 6,
               open ? g.theme->accent
                    : mix_color(g.theme->line, g.theme->accent,
                                0.22f * hover));
  text(g.target, g.small_left, label,
       D2D1::RectF(x + 12, y, r.right - 30, y + 36), g.theme->ink);
  draw_icon(g.target, icon::chevron_down, r.right - 13, y + 18,
            g.theme->muted);
  return r;
}

D2D1_RECT_F settings_input(float x, float y, float width,
                           const std::wstring& value, const wchar_t* hint_text,
                           bool focused, size_t caret, int id) {
  const D2D1_RECT_F r = D2D1::RectF(x, y, x + width, y + 36);
  const float hover = hover_lerp(id, inside(r, g.hover_x, g.hover_y));
  fill_round(g.target, r, 6, g.theme->panel);
  stroke_round(g.target, r, 6,
               focused ? g.theme->accent
                       : mix_color(g.theme->line, g.theme->accent,
                                   0.22f * hover));
  const bool empty = value.empty();
  text(g.target, g.small_left, empty ? hint_text : value,
       D2D1::RectF(x + 12, y, r.right - 8, y + 36),
       empty ? g.theme->muted : g.theme->ink);
  if (focused && g.caret_on) {
    const float caret_x =
        x + 12 + measure_text(g.small_left, value.substr(0, caret));
    line(g.target, caret_x, y + 8, caret_x, y + 28, g.theme->accent, 1.5f);
  }
  return r;
}

void settings_button(const D2D1_RECT_F& r, const wchar_t* label, bool primary,
                     int id, bool disabled = false) {
  const bool hover = inside(r, g.hover_x, g.hover_y) && !disabled;
  const float h = hover_lerp(id, hover);
  if (primary) {
    const D2D1_COLOR_F bg = mix_color(g.theme->accent, g.theme->ink, 0.12f * h);
    fill_round(g.target, r, 9.6f, disabled ? g.theme->panel : bg);
    text(g.target, g.small_bold, label, r,
         disabled ? g.theme->muted : g.theme->on_accent);
    return;
  }
  button_frame(g.target, *g.theme, r, 9.6f, id, hover, false);
  text(g.target, g.small, label, r,
       disabled ? D2D1::ColorF(g.theme->muted.r, g.theme->muted.g,
                               g.theme->muted.b, 0.5f)
                : mix_color(g.theme->muted, g.theme->ink, h));
}

void settings_quality_card(const D2D1_RECT_F& r, const wchar_t* title,
                           const std::wstring& desc, bool selected, int id) {
  const float hover = hover_lerp(id, inside(r, g.hover_x, g.hover_y));
  fill_round(g.target, r, 8,
             selected ? D2D1::ColorF(g.theme->accent_soft.r,
                                     g.theme->accent_soft.g,
                                     g.theme->accent_soft.b, 0.60f)
                      : D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                     g.theme->panel.b, 0.40f + 0.35f * hover));
  stroke_round(g.target, r, 8,
               selected ? D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                                       g.theme->accent.b, 0.4f)
                        : g.theme->line);
  const float cx = r.left + 17, cy = r.top + 17;
  if (selected) {
    fill_round(g.target, D2D1::RectF(cx - 7, cy - 7, cx + 7, cy + 7), 7,
               g.theme->accent);
    line(g.target, cx - 4, cy, cx - 1, cy + 3, g.theme->on_accent, 1.5f);
    line(g.target, cx - 1, cy + 3, cx + 4, cy - 3, g.theme->on_accent, 1.5f);
  } else {
    stroke_round(g.target, D2D1::RectF(cx - 7, cy - 7, cx + 7, cy + 7), 7,
                 g.theme->line);
  }
  text(g.target, g.heading, title,
       D2D1::RectF(r.left + 32, r.top + 5, r.right - 10, r.top + 25),
       selected ? g.theme->accent : g.theme->ink);
  text(g.target, g.small_left, desc,
       D2D1::RectF(r.left + 32, r.top + 24, r.right - 10, r.top + 44),
       g.theme->muted);
}

/// Settings dialog, copied from the web's centered `SettingsPanel`: header,
/// a 176 px section nav and preferences on the right.
void draw_settings_modal(float w, float h) {
  if (!g.settings_open && g.settings_anim <= 0.01f) return;
  const float e = ease_out(clampf(g.settings_anim, 0.0f, 1.0f));
  fill_round(g.target, D2D1::RectF(0, 0, w, h), 0,
             D2D1::ColorF(0, 0, 0, 0.35f * e));
  const float width = (std::min)(768.0f, w - 32);
  const float height = (std::min)(560.0f, h - 32);
  const float scale = 0.96f + 0.04f * e;
  const D2D1_RECT_F panel =
      D2D1::RectF((w - width * scale) / 2, (h - height * scale) / 2,
                  (w + width * scale) / 2, (h + height * scale) / 2);
  g.hit_set_panel = panel;
  shadow(g.target, *g.theme, panel, 16);
  fill_round(g.target, panel, 16, g.theme->surface);
  stroke_round(g.target, panel, 16, g.theme->line);

  // Header: settings icon + title + close.
  draw_icon(g.target, icon::settings, panel.left + 22, panel.top + 24,
            g.theme->accent);
  text(g.target, g.name16, L"设置",
       D2D1::RectF(panel.left + 36, panel.top + 6, panel.right - 60,
                   panel.top + 42),
       g.theme->ink);
  g.hit_set_close =
      D2D1::RectF(panel.right - 44, panel.top + 8, panel.right - 12,
                  panel.top + 40);
  quiet_icon_button(g.target, *g.theme, g.hit_set_close, hv_set_close,
                    icon::close, false,
                    inside(g.hit_set_close, g.hover_x, g.hover_y));
  line(g.target, panel.left, panel.top + 48, panel.right, panel.top + 48,
       g.theme->line_soft, 1.0f);

  // Section navigation, mirroring the web's six SettingsPanel tabs.
  const wchar_t* sections[6] = {L"通用", L"安全", L"网络",
                                L"显示", L"服务", L"关于"};
  const icon section_icons[6] = {icon::settings, icon::shield_check,
                                 icon::globe, icon::monitor, icon::server,
                                 icon::info};
  const float nav_w = 176;
  fill_round(g.target,
             D2D1::RectF(panel.left + 1, panel.top + 49,
                         panel.left + nav_w, panel.bottom - 1),
             0, D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                             g.theme->panel.b, 0.30f));
  line(g.target, panel.left + nav_w, panel.top + 49, panel.left + nav_w,
       panel.bottom, g.theme->line_soft, 1.0f);
  for (int i = 0; i < 6; ++i) {
    const D2D1_RECT_F r =
        D2D1::RectF(panel.left + 12, panel.top + 56 + i * 36,
                    panel.left + nav_w - 12, panel.top + 88 + i * 36);
    const bool active = g.settings_section == i;
    const bool hovered = inside(r, g.hover_x, g.hover_y);
    const float hh = hover_lerp(hv_set_nav0 + i, hovered);
    fill_round(g.target, r, 8,
               active ? g.theme->accent_soft
                      : D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                     g.theme->panel.b, hh));
    draw_icon(g.target, section_icons[i], r.left + 14,
              (r.top + r.bottom) / 2,
              active ? g.theme->accent
                     : mix_color(g.theme->muted, g.theme->ink, hh));
    text(g.target, g.label12, sections[i],
         D2D1::RectF(r.left + 28, r.top, r.right - 8, r.bottom),
         active ? g.theme->accent
                : mix_color(g.theme->muted, g.theme->ink, hh));
    g.hit_set_nav[i] = r;
  }

  // Content pane: clipped and scrollable, like the web's overflow-y-auto.
  g.settings_view = D2D1::RectF(panel.left + nav_w + 1, panel.top + 49,
                                panel.right - 1, panel.bottom - 1);
  const float x = panel.left + nav_w + 20;
  const float right = panel.right - 24;
  const float cw = right - x;
  const float content_top = panel.top + 64;
  const float view_h = g.settings_view.bottom - g.settings_view.top;
  g.settings_scroll = clampf(g.settings_scroll, 0.0f, g.settings_max_scroll);
  float y = content_top - g.settings_scroll;

  for (int i = 0; i < 2; ++i) {
    g.hit_seg_theme[i] = D2D1::RectF(0, 0, 0, 0);
    g.hit_seg_lang[i] = D2D1::RectF(0, 0, 0, 0);
    g.hit_sec_toggle[i] = D2D1::RectF(0, 0, 0, 0);
  }
  for (int i = 0; i < 3; ++i) g.hit_disp_toggle[i] = D2D1::RectF(0, 0, 0, 0);
  for (int i = 0; i < 6; ++i) g.hit_sec_toggle[i] = D2D1::RectF(0, 0, 0, 0);
  for (int i = 0; i < 3; ++i) g.hit_seg_video[i] = D2D1::RectF(0, 0, 0, 0);
  for (int i = 0; i < 8; ++i) g.hit_quality_card[i] = D2D1::RectF(0, 0, 0, 0);
  for (int i = 0; i < 5; ++i) g.hit_service_btn[i] = D2D1::RectF(0, 0, 0, 0);
  g.hit_lock_field = g.hit_fps_field = D2D1::RectF(0, 0, 0, 0);
  g.hit_dev_name = g.hit_dev_apply = g.hit_long_term = g.hit_long_apply =
      g.hit_stun = D2D1::RectF(0, 0, 0, 0);
  g.hit_tcp = g.hit_udp = g.hit_ports_save = D2D1::RectF(0, 0, 0, 0);
  g.hit_server_toggle = g.hit_server_url = g.hit_server_invite =
      g.hit_server_save = g.hit_server_default = D2D1::RectF(0, 0, 0, 0);
  g.hit_update_btn = g.hit_repo_btn = g.hit_site_btn = g.hit_star_btn =
      D2D1::RectF(0, 0, 0, 0);

  g.target->PushAxisAlignedClip(g.settings_view,
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  const int section_now = g.settings_section;

  if (section_now == 0) {
    // General: theme, auto-lock, language (web PreferencesControls).
    settings_head(icon::sun, L"主题", x, y);
    y += 24;
    const wchar_t* theme_labels[2] = {L"亮色", L"暗色"};
    const icon theme_icons[2] = {icon::sun, icon::moon};
    settings_segment(x, y, cw, theme_labels, theme_icons, 2,
                     g.theme == &kLight ? 0 : 1, g.hit_seg_theme,
                     hv_set_seg_theme0);
    y += 36 + 20;

    settings_head(icon::rotate, L"自动锁定", x, y);
    y += 22;
    y = settings_hint(x, y, cw,
                      L"在没有任何远程会话且长时间无操作后锁定界面。") +
        6;
    static const wchar_t* locks[6] = {L"30 秒", L"1 分钟", L"2 分钟",
                                      L"5 分钟", L"15 分钟", L"从不"};
    g.hit_lock_field = settings_select(
        x, y, cw, locks[g.settings_lock], g.open_menu == 10,
        hv_set_field_lock);
    y += 36 + 20;

    settings_head(icon::globe, L"语言", x, y);
    y += 24;
    const wchar_t* lang_labels[2] = {L"English", L"中文"};
    settings_segment(x, y, cw, lang_labels, nullptr, 2, g.settings_lang,
                     g.hit_seg_lang, hv_set_seg_lang0);
    y += 36;
  } else if (section_now == 1) {
    // Security: toggles card, device name and long-term code.
    settings_head(icon::shield, L"安全设置", x, y);
    y += 24;
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + cw, y + 316);
    fill_round(g.target, card, 8,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.60f));
    stroke_round(g.target, card, 8, g.theme->line);
    float cy = y + 12;
    const float inner = cw - 24;
    cy = settings_toggle_row(
        x + 12, cy, inner, L"允许局域网 / IP 访问",
        L"立即生效；关闭时会同时断开已连接的远程会话。", g.sec_allow_remote,
        hv_set_toggle0, &g.hit_sec_toggle[0]);
    cy = settings_toggle_row(
        x + 12, cy, inner, L"允许浏览器客户端",
        L"允许普通浏览器（非桌面壳）加载 Web UI。", g.sec_allow_web,
        hv_set_toggle1, &g.hit_sec_toggle[1]);
    cy = settings_toggle_row(
        x + 12, cy, inner, L"局域网发现",
        L"通过组播发现设备；必须先允许局域网访问。", g.sec_discovery,
        hv_set_toggle2, &g.hit_sec_toggle[2]);
    cy = settings_toggle_row(
        x + 12, cy, inner, L"允许免密远程请求",
        L"对方没有验证码时可以发出请求，每次仍需你手动同意。默认开启。",
        g.sec_access_requests, hv_set_toggle3, &g.hit_sec_toggle[3]);
    cy = settings_toggle_row(
        x + 12, cy, inner, L"远程会话加密（ID 服务器）",
        L"默认开启，修改后需要重启 moLink 才能切换监听模式。经 ID "
        L"服务器/中继的会话目前只加密外层 WSS。",
        g.sec_encrypt_remote, hv_set_toggle6, &g.hit_sec_toggle[4]);
    settings_toggle_row(
        x + 12, cy, inner, L"局域网会话加密",
        L"默认关闭，修改后需要重启 moLink 才能切换监听模式。开启后局域网"
        L"连接使用自签 HTTPS 加密，对端需要接受证书。",
        g.sec_encrypt_lan, hv_set_toggle7, &g.hit_sec_toggle[5]);
    y = card.bottom + 20;

    // 视频模式: the web's PreferencesControls three-way switch. Tiles keeps a
    // still desktop near zero traffic (only changed cells move); stream is for
    // big fast motion; mixed is the default.
    text(g.target, g.small_left, L"视频模式",
         D2D1::RectF(x, y, x + cw, y + 16), g.theme->muted);
    y += 18;
    static const wchar_t* video_labels[3] = {L"视频流", L"混合", L"瓦片（静态）"};
    settings_segment(x, y, cw, video_labels, nullptr, 3, g.video_mode,
                     g.hit_seg_video, hv_set_video0);
    y += 38;
    y = settings_hint(
            x, y, cw,
            L"瓦片只传变化的格子，静止桌面几乎不耗流量；视频流适合全屏快速"
            L"运动；混合按画面变化自动切换。主机全局生效。") +
        14;

    text(g.target, g.small_left, L"STUN 服务器",
         D2D1::RectF(x, y, x + cw, y + 16), g.theme->muted);
    y += 18;
    g.hit_stun = settings_input(x, y, cw, g.stun_server,
                                L"stun.miwifi.com:3478",
                                g.focus_field == 10, g.set_caret_stun,
                                hv_set_field_stun);
    y += 36;
    y = settings_hint(
            x, y, cw,
            L"用于跨 NAT 打洞。留空使用内置 stun.miwifi.com:3478；填 "
            L"none 关闭 STUN。") +
        12;

    text(g.target, g.heading, L"设备名称",
         D2D1::RectF(x, y, x + cw, y + 18), g.theme->ink);
    y += 22;
    g.hit_dev_name = settings_input(
        x, y, cw - 96, g.set_device_name, L"设备名称",
        g.focus_field == 4, g.set_caret_dev, hv_set_field_dev);
    g.hit_dev_apply = D2D1::RectF(x + cw - 84, y, x + cw, y + 36);
    settings_button(g.hit_dev_apply, L"应用", false, hv_set_btn_dev);
    y += 36 + 18;

    text(g.target, g.heading, L"长期授权码",
         D2D1::RectF(x, y, x + cw, y + 18), g.theme->ink);
    y += 20;
    y = settings_hint(x, y, cw, L"至少 8 位，必须同时包含字母和数字。") + 4;
    g.hit_long_term = settings_input(
        x, y, cw - 96, g.set_long_term, L"新的长期授权码",
        g.focus_field == 7, g.set_caret_long, hv_set_field_long);
    g.hit_long_apply = D2D1::RectF(x + cw - 84, y, x + cw, y + 36);
    settings_button(g.hit_long_apply, L"应用", false, hv_set_btn_long);
    y += 36;
  } else if (section_now == 2) {
    // Network ports.
    settings_head(icon::globe, L"网络端口", x, y);
    y += 24;
    const float col = (cw - 8) / 2;
    text(g.target, g.small_left, L"网页 / WS（TCP）",
         D2D1::RectF(x, y, x + col, y + 16), g.theme->muted);
    text(g.target, g.small_left, L"UDP 数据",
         D2D1::RectF(x + col + 8, y, right, y + 16), g.theme->muted);
    y += 18;
    g.hit_tcp = settings_input(x, y, col, g.set_tcp, L"28418",
                               g.focus_field == 5, g.set_caret_tcp,
                               hv_set_field_tcp);
    g.hit_udp = settings_input(x + col + 8, y, col, g.set_udp, L"28422",
                               g.focus_field == 6, g.set_caret_udp,
                               hv_set_field_udp);
    y += 36 + 12;
    g.hit_ports_save = D2D1::RectF(x, y, x + 108, y + 32);
    settings_button(g.hit_ports_save, L"保存端口", false, hv_set_btn_ports);
    y += 32 + 12;
    y = settings_hint(
            x, y, cw,
            L"端口段：网页/WS 28418  HTTP/3 28419  QUIC 数据 28420  "
            L"TCP 数据 28421  UDP 数据 28422。这里只改网页/WS 与 UDP "
            L"数据端口；预留传输由 /api/transports 宣告。") +
        16;
    line(g.target, x, y, x + cw, y, g.theme->line_soft, 1.0f);
    y += 16;

    settings_head(icon::server, L"服务端", x, y);
    y += 24;
    y = settings_hint(
            x, y, cw,
            L"把本机注册到 moLink Server，其他人就可以通过设备 ID 找到"
            L"这台电脑。") +
        6;
    y = settings_toggle_row(
        x, y, cw, L"启用远程目录",
        L"关闭后只保留局域网 / IP 连接，本机不再注册到 moLink Server。",
        g.server_enabled, hv_set_toggle8, &g.hit_server_toggle);
    text(g.target, g.small_left, L"服务端地址",
         D2D1::RectF(x, y, x + cw, y + 16), g.theme->muted);
    y += 18;
    g.hit_server_url = settings_input(x, y, cw, g.server_url,
                                      L"https://...", g.focus_field == 8,
                                      g.set_caret_url, hv_set_field_url);
    y += 36 + 12;
    text(g.target, g.small_left, L"注册邀请码",
         D2D1::RectF(x, y, x + cw, y + 16), g.theme->muted);
    y += 18;
    g.hit_server_invite = settings_input(
        x, y, cw, g.server_invite, L"服务端开启邀请码时必填",
        g.focus_field == 9, g.set_caret_invite, hv_set_field_invite);
    y += 36 + 12;
    g.hit_server_save = D2D1::RectF(x, y, x + 120, y + 32);
    settings_button(g.hit_server_save, L"保存并注册", true,
                    hv_set_btn_server_save);
    g.hit_server_default = D2D1::RectF(x + 128, y, x + 248, y + 32);
    settings_button(g.hit_server_default, L"使用内置目录", false,
                    hv_set_btn_server_default);
    y += 32 + 12;
    std::wstring server_state = L"状态：" + g.id_status;
    if (!g.server_enabled) {
      server_state = L"状态：已关闭远程目录";
    } else if (g.id_registered) {
      server_state = L"状态：已注册 · " +
                     std::wstring(g.id_online ? L"在线" : L"离线") +
                     L" · ID " +
                     (g.device_id.empty() ? std::wstring(L"—") : g.device_id);
    }
    text(g.target, g.small_left, server_state,
         D2D1::RectF(x, y, x + cw, y + 20), g.theme->muted);
    y += 24;
  } else if (section_now == 3) {
    // Display: watermark, auto audio, default frame rate and quality.
    y = settings_toggle_row(
        x, y, cw, L"连接质量水印",
        L"在会话角落显示帧率，延迟和带宽，默认关闭。", g.watermark,
        hv_set_toggle4, &g.hit_disp_toggle[0]);
    y = settings_toggle_row(
        x, y, cw, L"连接后自动播放声音",
        L"会话建立后自动开始转发主机声音。", g.disp_auto_audio,
        hv_set_toggle5, &g.hit_disp_toggle[1]);
    y = settings_toggle_row(
        x, y, cw, L"显示远程主机光标",
        L"在被控端画面上绘制主机自己的鼠标指针（虚拟绘制）。默认关闭，"
        L"只显示本机光标。", g.disp_remote_cursor, hv_set_toggle9,
        &g.hit_disp_toggle[2]);

    settings_head(icon::activity, L"平均帧率", x, y);
    y += 22;
    y = settings_hint(
            x, y, cw,
            L"主机和链路跟得上时按这个帧率推送画面；CPU 编码不过来或网络"
            L"传不过来时会自动降帧，恢复后逐步回到设定值。自适应跟随画质"
            L"档位：极速 5-25  流畅/平衡 10-25  清晰/超清/无损 15  "
            L"游戏 30-60。") +
        6;
    static const wchar_t* fps_labels[4] = {
        L"自适应（按画质档位和负载）", L"15 帧", L"30 帧", L"60 帧"};
    const int fps_sel = g.fps_index >= 0 ? g.fps_index : 0;
    g.hit_fps_field = settings_select(x, y, cw, fps_labels[fps_sel],
                                      g.open_menu == 11, hv_set_field_fps);
    y += 36 + 20;

    settings_head(icon::gauge, L"默认画质", x, y);
    y += 22;
    y = settings_hint(
            x, y, cw,
            L"选择会话默认使用的压缩档位；自适应会结合延迟与链路自动切换"
            L"档位。帧率由上面的平均帧率设置控制。") +
        6;
    static const wchar_t* quality_titles[8] = {
        L"自适应", L"极速", L"流畅", L"平衡",
        L"清晰", L"超清", L"无损", L"游戏"};
    static const wchar_t* quality_desc[8] = {
        L"推荐。根据延迟，带宽和画面变化自动选择画质档位。",
        L"黑白画面，最低带宽，适合弱网和远程运维。",
        L"16 位色 + 快速压缩，低带宽下更流畅。",
        L"无损色块 + 自适应压缩，日常桌面最稳妥。",
        L"优先画质，加大压缩力度，占用更多 CPU。",
        L"超清：原始像素不做压缩，适合高速局域网。",
        L"无损：原始像素不做压缩，画质最好但带宽最高。",
        L"游戏：16 位色 + 快速压缩，优先画面更新。"};
    for (int i = 0; i < 8; ++i) {
      const D2D1_RECT_F card =
          D2D1::RectF(x, y, x + cw, y + 58);
      settings_quality_card(card, quality_titles[i], quality_desc[i],
                            g.settings_quality == i, hv_set_quality0 + i);
      g.hit_quality_card[i] = card;
      y += 58 + 6;
    }
  } else if (section_now == 4) {
    // Windows service.
    settings_head(icon::server, L"后台服务", x, y);
    y += 24;
    const std::wstring state =
        !g.service_installed ? L"未安装"
                             : (g.service_running ? L"运行中" : L"已停止");
    text(g.target, g.small_left, L"状态：" + state,
         D2D1::RectF(x, y, x + cw, y + 20), g.theme->ink);
    y += 26;
    if (!g.service_installed) {
      g.hit_service_btn[0] = D2D1::RectF(x, y, x + 120, y + 32);
      settings_button(g.hit_service_btn[0], L"安装服务", true,
                      hv_set_btn_service0);
      y += 32 + 14;
    } else {
      g.hit_service_btn[1] = D2D1::RectF(x, y, x + 96, y + 32);
      settings_button(g.hit_service_btn[1],
                      g.service_running ? L"停止" : L"启动",
                      !g.service_running, hv_set_btn_service1);
      g.hit_service_btn[2] = D2D1::RectF(x + 104, y, x + 200, y + 32);
      settings_button(g.hit_service_btn[2], L"重启", false,
                      hv_set_btn_service2);
      g.hit_service_btn[3] = D2D1::RectF(x + 208, y, x + 304, y + 32);
      settings_button(g.hit_service_btn[3], L"卸载", false,
                      hv_set_btn_service3);
      y += 32 + 14;
    }
    y = settings_hint(
        x, y, cw,
        L"安装后启动/停止不需要提权；安装/卸载会弹出 UAC。") + 4;
  } else {
    // About, copied from the web AboutPanel. The mark is the product icon,
    // not a kit glyph: the web panel shows the real logo.
    if (ID2D1Bitmap* logo = about_logo(g.target); logo != nullptr) {
      g.target->DrawBitmap(
          logo, D2D1::RectF(x, y, x + 44, y + 44), 1.0f,
          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      fill_round(g.target, D2D1::RectF(x, y, x + 44, y + 44), 12,
                 g.theme->accent_strong);
      draw_icon(g.target, icon::monitor, x + 22, y + 22, g.theme->on_accent);
    }
    text(g.target, g.title, L"moLink",
         D2D1::RectF(x + 56, y, x + 260, y + 24), g.theme->ink);
    text(g.target, g.small_left, L"远程桌面 · 原生预览版",
         D2D1::RectF(x + 56, y + 22, x + 360, y + 44), g.theme->muted);
    g.hit_star_btn = D2D1::RectF(x + cw - 92, y + 6, x + cw, y + 34);
    settings_button(g.hit_star_btn, L"GitHub", false, hv_set_btn_star);
    y += 56;

    const float row_h = 40;
    const D2D1_RECT_F repo = D2D1::RectF(x, y, x + cw, y + row_h);
    fill_round(g.target, repo, 8,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.50f));
    stroke_round(g.target, repo, 8, g.theme->line);
    draw_icon(g.target, icon::folder, x + 18, y + row_h / 2, g.theme->muted);
    text(g.target, g.small_left, L"代码仓库",
         D2D1::RectF(x + 30, y, x + 300, y + row_h), g.theme->muted);
    g.hit_repo_btn = D2D1::RectF(x + cw - 90, y + 4, x + cw - 8, y + 36);
    text(g.target, g.small, L"打开", g.hit_repo_btn, g.theme->accent);
    y += row_h + 6;

    const D2D1_RECT_F site = D2D1::RectF(x, y, x + cw, y + row_h);
    fill_round(g.target, site, 8,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.50f));
    stroke_round(g.target, site, 8, g.theme->line);
    draw_icon(g.target, icon::globe, x + 18, y + row_h / 2, g.theme->muted);
    text(g.target, g.small_left, L"官方网站",
         D2D1::RectF(x + 30, y, x + 300, y + row_h), g.theme->muted);
    g.hit_site_btn = D2D1::RectF(x + cw - 150, y + 4, x + cw - 8, y + 36);
    text(g.target, g.small, L"ssvr.top/molink", g.hit_site_btn,
         g.theme->accent);
    y += row_h + 6;

    const D2D1_RECT_F license = D2D1::RectF(x, y, x + cw, y + row_h);
    fill_round(g.target, license, 8,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.50f));
    stroke_round(g.target, license, 8, g.theme->line);
    draw_icon(g.target, icon::shield_check, x + 18, y + row_h / 2,
              g.theme->muted);
    text(g.target, g.small_left, L"许可证",
         D2D1::RectF(x + 30, y, x + 300, y + row_h), g.theme->muted);
    text(g.target, g.small_right, L"MIT",
         D2D1::RectF(x + cw - 80, y, x + cw - 12, y + row_h),
         g.theme->ink);
    y += row_h + 12;

    const D2D1_RECT_F star_card =
        D2D1::RectF(x, y, x + cw, y + 92);
    fill_round(g.target, star_card, 12,
               D2D1::ColorF(g.theme->accent_soft.r, g.theme->accent_soft.g,
                            g.theme->accent_soft.b, 0.40f));
    stroke_round(g.target, star_card, 12,
                 D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                              g.theme->accent.b, 0.30f));
    draw_icon(g.target, icon::star, x + 20, y + 24, g.theme->accent);
    text(g.target, g.small_left,
         L"如果 moLink 对你有帮助，点个 Star 支持一下，也能让更多人发现它。",
         D2D1::RectF(x + 34, y + 10, x + cw - 14, y + 44),
         g.theme->ink);
    g.hit_star_btn = D2D1::RectF(x + 14, y + 48, x + 122, y + 78);
    settings_button(g.hit_star_btn, L"去点 Star", true, hv_set_btn_star);
    y += 92 + 12;

    const D2D1_RECT_F update_card =
        D2D1::RectF(x, y, x + cw, y + 70);
    fill_round(g.target, update_card, 8,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.50f));
    stroke_round(g.target, update_card, 8, g.theme->line);
    std::wstring update_detail;
    const wchar_t* update_button = L"检查更新";
    switch (g.update_state) {
      case 1:
        update_detail = L"正在检查…";
        update_button = L"检查中…";
        break;
      case 2:
        update_detail =
            L"已是最新版本（v" + std::wstring(kNativeVersion) + L"）";
        update_button = L"已是最新";
        break;
      case 3:
        update_detail = L"发现新版本 " + g.update_latest + L"（当前 v" +
                        std::wstring(kNativeVersion) + L"）";
        update_button = L"立即更新";
        break;
      default:
        update_detail = L"当前版本 v" + std::wstring(kNativeVersion);
        break;
    }
    text(g.target, g.heading, L"检查更新",
         D2D1::RectF(x + 12, y + 8, x + 300, y + 28), g.theme->ink);
    text(g.target, g.small_left, update_detail,
         D2D1::RectF(x + 12, y + 30, x + 340, y + 52), g.theme->muted);
    g.hit_update_btn = D2D1::RectF(x + cw - 110, y + 18, x + cw - 12,
                                   y + 52);
    settings_button(g.hit_update_btn, update_button,
                    g.update_state == 3, hv_set_btn_update);
    y += 70 + 12;

    text(g.target, g.heading, L"隐私声明",
         D2D1::RectF(x, y, x + cw, y + 20), g.theme->ink);
    y += 24;
    y = settings_hint(
            x, y, cw,
            L"moLink 不收集使用统计或遥测数据。设备目录只保存设备 ID 和"
            L"名称，以及在线状态和建立连接所需的地址与端口；远程画面，输入，"
            L"剪贴板和文件内容在设备之间加密传输（直连或经中继），不会保存"
            L"到目录服务器。验证码，聊天记录和本机设置只保存在本机的配置目录中。") +
        4;
  }

  g.target->PopAxisAlignedClip();
  g.settings_content_h = (y + g.settings_scroll) - content_top + 20;
  g.settings_max_scroll =
      (std::max)(0.0f, g.settings_content_h - view_h + 8);
  if (g.settings_scroll > g.settings_max_scroll) {
    g.settings_scroll = g.settings_max_scroll;
  }

  // Scrollbar (only when the section is taller than the view).
  if (g.settings_max_scroll > 0.5f) {
    const float track_top = g.settings_view.top + 10;
    const float track_bottom = g.settings_view.bottom - 10;
    const float track_h = track_bottom - track_top;
    const D2D1_RECT_F track =
        D2D1::RectF(panel.right - 10, track_top, panel.right - 6,
                    track_bottom);
    fill_round(g.target, track, 2, g.theme->line_soft);
    const float thumb_h =
        (std::max)(24.0f, track_h * view_h / g.settings_content_h);
    const float thumb_y =
        track_top + (track_h - thumb_h) *
                        (g.settings_scroll / g.settings_max_scroll);
    fill_round(g.target,
               D2D1::RectF(track.left, thumb_y, track.right,
                           thumb_y + thumb_h),
               2, g.theme->line);
  }
  if (g.open_menu >= 10) {
    draw_menu(g.target, *g.theme, g.small_left, w);
  }
}

void apply_settings_quality(int index) {
  g.settings_quality = index;
  static const char* modes[8] = {"auto",   "turbo", "smooth", "balanced",
                                 "quality", "ultra", "lossless", "game"};
  if (g.session != nullptr) {
    g.session->send_quality(modes[index], g.fps_value, 0, 0, "auto", "preset");
  }
  // Keep the toolbar preset in step for the seven presets it exposes.
  static const int preset_of[8] = {6, 0, 1, 2, 3, 4, 5, 2};
  g.preset = preset_of[index];
}

void settings_click(float x, float y) {
  if (g.open_menu >= 10) {
    const float window_w = g.target != nullptr ? g.target->GetSize().width : 0;
    const int count = menu_count(g.open_menu);
    const int picked =
        menu_hit(g.menu_field, g.open_menu, count, window_w, x, y);
    if (picked >= 0) {
      menu_apply(g.open_menu, picked);
      g.open_menu = -1;
      return;
    }
    g.open_menu = -1;
  }
  if (!inside(g.hit_set_panel, x, y)) {
    g.settings_open = false;
    g.focus_field = 0;
    return;
  }
  if (inside(g.hit_set_close, x, y)) {
    g.settings_open = false;
    g.focus_field = 0;
    return;
  }
  for (int i = 0; i < 6; ++i) {
    if (inside(g.hit_set_nav[i], x, y)) {
      if (g.settings_section != i) {
        g.settings_section = i;
        g.settings_scroll = 0;
        g.open_menu = -1;
      }
      return;
    }
  }

  switch (g.settings_section) {
    case 0:
      for (int i = 0; i < 2; ++i) {
        if (inside(g.hit_seg_theme[i], x, y)) {
          g.mode = i == 0 ? app_state::theme_mode::light
                          : app_state::theme_mode::dark;
          save_theme_mode();
          return;
        }
        if (inside(g.hit_seg_lang[i], x, y)) {
          g.settings_lang = i;
          show_toast(L"语言设置已保存（重启后生效）");
          return;
        }
      }
      if (inside(g.hit_lock_field, x, y)) {
        g.open_menu = g.open_menu == 10 ? -1 : 10;
        g.menu_field = g.hit_lock_field;
        return;
      }
      break;
    case 1:
      for (int i = 0; i < 3; ++i) {
        if (inside(g.hit_seg_video[i], x, y)) {
          static const char* modes[] = {"stream", "mixed", "tiles"};
          if (g.session == nullptr || !g.session->live()) {
            show_toast(L"请先连接会话再切换视频模式");
          } else if (g.session->send_video_mode(modes[i])) {
            g.video_mode = i;
            show_toast(i == 2 ? L"已切换到瓦片（静态）模式"
                              : (i == 0 ? L"已切换到视频流模式"
                                        : L"已切换到混合模式"));
          } else {
            show_toast(L"视频模式切换失败");
          }
          return;
        }
      }
      for (int i = 0; i < 6; ++i) {
        if (inside(g.hit_sec_toggle[i], x, y)) {
          if (i == 0) g.sec_allow_remote = !g.sec_allow_remote;
          if (i == 1) g.sec_allow_web = !g.sec_allow_web;
          if (i == 2) g.sec_discovery = !g.sec_discovery;
          if (i == 3) g.sec_access_requests = !g.sec_access_requests;
          if (i == 4) g.sec_encrypt_remote = !g.sec_encrypt_remote;
          if (i == 5) g.sec_encrypt_lan = !g.sec_encrypt_lan;
          // Write-through: these are the host's own settings, not shell
          // preferences, and the web panel persists them the same way.
          std::wstring error;
          if (!save_host_settings(&error)) show_toast(error);
          return;
        }
      }
      if (inside(g.hit_stun, x, y)) {
        g.focus_field = 10;
        g.set_caret_stun = g.stun_server.size();
        return;
      }
      if (inside(g.hit_dev_name, x, y)) {
        g.focus_field = 4;
        g.set_caret_dev = g.set_device_name.size();
        return;
      }
      if (inside(g.hit_dev_apply, x, y)) {
        show_toast(g.set_device_name.empty() ? L"设备名称不能为空"
                                             : L"已保存。");
        return;
      }
      if (inside(g.hit_long_term, x, y)) {
        g.focus_field = 7;
        g.set_caret_long = g.set_long_term.size();
        return;
      }
      if (inside(g.hit_long_apply, x, y)) {
        bool letter = false, digit = false;
        for (wchar_t c : g.set_long_term) {
          if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) {
            letter = true;
          }
          if (c >= L'0' && c <= L'9') digit = true;
        }
        show_toast(g.set_long_term.size() >= 8 && letter && digit
                       ? L"已保存。"
                       : L"至少 8 位，必须同时包含字母和数字。");
        return;
      }
      break;
    case 2:
      if (inside(g.hit_tcp, x, y)) {
        g.focus_field = 5;
        g.set_caret_tcp = g.set_tcp.size();
        return;
      }
      if (inside(g.hit_udp, x, y)) {
        g.focus_field = 6;
        g.set_caret_udp = g.set_udp.size();
        return;
      }
      if (inside(g.hit_ports_save, x, y)) {
        const int tcp = _wtoi(g.set_tcp.c_str());
        const int udp = _wtoi(g.set_udp.c_str());
        show_toast(tcp >= 1 && tcp <= 65535 && udp >= 1 && udp <= 65535
                       ? L"端口已保存，重启后生效"
                       : L"端口无效");
        return;
      }
      if (inside(g.hit_server_toggle, x, y)) {
        g.server_enabled = !g.server_enabled;
        return;
      }
      if (inside(g.hit_server_url, x, y)) {
        g.focus_field = 8;
        g.set_caret_url = g.server_url.size();
        return;
      }
      if (inside(g.hit_server_invite, x, y)) {
        g.focus_field = 9;
        g.set_caret_invite = g.server_invite.size();
        return;
      }
      if (inside(g.hit_server_save, x, y)) {
        show_toast(L"已保存。");
        return;
      }
      if (inside(g.hit_server_default, x, y)) {
        g.server_url = L"https://molink.ssvr.top";
        show_toast(L"已使用内置目录");
        return;
      }
      break;
    case 3:
      for (int i = 0; i < 3; ++i) {
        if (inside(g.hit_disp_toggle[i], x, y)) {
          if (i == 0) g.watermark = !g.watermark;
          if (i == 1) g.disp_auto_audio = !g.disp_auto_audio;
          if (i == 2) g.disp_remote_cursor = !g.disp_remote_cursor;
          return;
        }
      }
      if (inside(g.hit_fps_field, x, y)) {
        g.open_menu = g.open_menu == 11 ? -1 : 11;
        g.menu_field = g.hit_fps_field;
        return;
      }
      for (int i = 0; i < 8; ++i) {
        if (inside(g.hit_quality_card[i], x, y)) {
          apply_settings_quality(i);
          return;
        }
      }
      break;
    case 4:
      if (inside(g.hit_service_btn[0], x, y)) {
        g.service_installed = true;
        g.service_running = false;
        show_toast(L"服务已安装");
        return;
      }
      if (inside(g.hit_service_btn[1], x, y)) {
        g.service_running = !g.service_running;
        return;
      }
      if (inside(g.hit_service_btn[2], x, y)) {
        g.service_running = true;
        show_toast(L"服务已重启");
        return;
      }
      if (inside(g.hit_service_btn[3], x, y)) {
        g.service_installed = false;
        g.service_running = false;
        show_toast(L"服务已卸载");
        return;
      }
      break;
    case 5:
      if (inside(g.hit_update_btn, x, y)) {
        if (g.update_state == 3) {
          // Only after a newer tag was found: the native shell updates from
          // the native asset on the release page; the WebView2 bundle and the
          // .deb are a different product.
          open_url(kNativeReleasesUrl);
          show_toast(L"已打开下载页：请选择 moLink-native 资产");
        } else if (g.update_state != 1) {
          start_update_check();
        }
        return;
      }
      if (inside(g.hit_repo_btn, x, y)) {
        open_url(kRepoUrl);
        return;
      }
      if (inside(g.hit_site_btn, x, y)) {
        open_url(kSiteUrl);
        return;
      }
      if (inside(g.hit_star_btn, x, y)) {
        open_url(kRepoUrl);
        return;
      }
      break;
    default:
      break;
  }
}

/// Chat drawer, copied from the web/// Chat drawer, copied from the web's ChatPanel: 340 px, right edge, header
/// with the remote label, empty state and a message composer.
void draw_chat_drawer(float w, float h) {
  if (!g.chat_open && g.chat_anim <= 0.01f) return;
  const float e = clampf(g.chat_anim, 0.0f, 1.0f);
  const float width = 340;
  const float top = g.immersive ? 0.0f : 79.0f;
  const float left = w - width * e;
  const D2D1_RECT_F panel = D2D1::RectF(left, top, left + width, h);
  g.hit_chat_panel = panel;
  shadow(g.target, *g.theme, panel, 16);
  fill_round(g.target, panel, 0,
             D2D1::ColorF(g.theme->surface.r, g.theme->surface.g,
                          g.theme->surface.b, 0.95f));
  line(g.target, panel.left, panel.top, panel.left, panel.bottom,
       g.theme->line, 1.0f);
  line(g.target, panel.left, panel.top + 44, panel.right, panel.top + 44,
       g.theme->line_soft, 1.0f);
  draw_icon(g.target, icon::chat, panel.left + 18, panel.top + 22,
            g.theme->accent);
  text(g.target, g.body_medium, L"聊天",
       D2D1::RectF(panel.left + 34, panel.top + 6, panel.left + 80,
                   panel.top + 38),
       g.theme->ink);
  toolbar_chip(g.target, *g.theme, g.tiny,
               D2D1::RectF(panel.left + 84, panel.top + 12,
                           panel.left + 84 +
                               measure_text(g.tiny, g.session_label) + 16,
                           panel.top + 32),
               g.session_label);
  g.hit_chat_close =
      D2D1::RectF(panel.right - 40, panel.top + 8, panel.right - 8,
                  panel.top + 36);
  quiet_icon_button(g.target, *g.theme, g.hit_chat_close, hv_chat_close,
                    icon::close, false,
                    inside(g.hit_chat_close, g.hover_x, g.hover_y));

  // Composer at the bottom.
  const float input_top = panel.bottom - 52;
  g.hit_chat_send = D2D1::RectF(panel.right - 44, input_top + 6,
                                panel.right - 12, input_top + 38);
  primary_icon_button(g.target, *g.theme, g.hit_chat_send, hv_chat_send,
                      icon::send, inside(g.hit_chat_send, g.hover_x, g.hover_y));
  g.hit_chat_input = D2D1::RectF(panel.left + 12, input_top + 6,
                                 panel.right - 52, input_top + 38);
  fill_round(g.target, g.hit_chat_input, 8, g.theme->panel);
  stroke_round(g.target, g.hit_chat_input, 8,
               g.focus_field == 3 ? g.theme->accent : g.theme->line);
  const bool empty_input = g.chat_input.empty();
  if (g.sel_field == 3 && g.sel_start < g.sel_end && !empty_input) {
    const float x0 = g.hit_chat_input.left + 10;
    const float xa = x0 + measure_text(
                               g.small_left,
                               g.chat_input.substr(0, g.sel_start));
    const float xb = x0 + measure_text(
                               g.small_left,
                               g.chat_input.substr(0, g.sel_end));
    fill_round(g.target,
               D2D1::RectF(xa, g.hit_chat_input.top + 8, xb,
                           g.hit_chat_input.bottom - 8),
               2, D2D1::ColorF(g.theme->accent.r, g.theme->accent.g,
                               g.theme->accent.b, 0.22f));
  }
  text(g.target, g.small_left,
       empty_input ? L"输入消息…" : g.chat_input,
       D2D1::RectF(g.hit_chat_input.left + 10, g.hit_chat_input.top,
                   g.hit_chat_input.right - 8, g.hit_chat_input.bottom),
       empty_input ? g.theme->muted : g.theme->ink);
  if (g.focus_field == 3 && g.caret_on) {
    const float caret_x =
        g.hit_chat_input.left + 10 +
        measure_text(g.small_left, g.chat_input.substr(0, g.chat_caret));
    line(g.target, caret_x, g.hit_chat_input.top + 6, caret_x,
         g.hit_chat_input.bottom - 6, g.theme->accent, 1.5f);
  }

  // Message list (or the web's dashed empty state).
  const float list_top = panel.top + 52;
  const float list_bottom = input_top - 8;
  if (g.chat_messages.empty()) {
    const D2D1_RECT_F empty =
        D2D1::RectF(panel.left + 12, list_top + 4, panel.right - 12,
                    list_top + 96);
    fill_round(g.target, empty, 12,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, 0.35f));
    dashed_round(g.target, empty, 12, g.theme->line_soft, 1.0f);
    text(g.target, g.body_center, L"暂无消息",
         D2D1::RectF(empty.left, empty.top + 20, empty.right,
                     empty.top + 48),
         g.theme->muted);
    text(g.target, g.small, L"发送一条消息开始聊天",
         D2D1::RectF(empty.left, empty.top + 48, empty.right,
                     empty.top + 72),
         g.theme->muted);
  } else {
    // Show the newest messages that fit, bottom-aligned.
    const float row_h = 30;
    const int capacity =
        (std::max)(1, static_cast<int>((list_bottom - list_top) / row_h));
    const int count = static_cast<int>(g.chat_messages.size());
    const int start = (std::max)(0, count - capacity);
    float y = list_top + 2;
    for (int i = start; i < count; ++i) {
      const app_state::chat_msg& msg = g.chat_messages[i];
      const float text_w = text_width(msg.text, 11.0f);
      const float bubble_w = (std::min)(250.0f, text_w + 20);
      const D2D1_RECT_F bubble =
          msg.mine ? D2D1::RectF(panel.right - 12 - bubble_w, y,
                                 panel.right - 12, y + 24)
                   : D2D1::RectF(panel.left + 12, y,
                                 panel.left + 12 + bubble_w, y + 24);
      fill_round(g.target, bubble, 8,
                 msg.mine ? g.theme->accent_soft : g.theme->panel);
      text(g.target, g.small_left, msg.text,
           D2D1::RectF(bubble.left + 8, bubble.top, bubble.right - 8,
                       bubble.bottom),
           msg.mine ? g.theme->accent : g.theme->ink);
      y += row_h;
    }
  }
}

void chat_click(float x, float y) {
  if (inside(g.hit_chat_close, x, y)) {
    g.chat_open = false;
    if (g.focus_field == 3) g.focus_field = 0;
    return;
  }
  if (inside(g.hit_chat_send, x, y)) {
    send_chat();
    return;
  }
  if (inside(g.hit_chat_input, x, y)) {
    g.focus_field = 3;
    g.chat_caret = caret_for_field(3, x);
    g.sel_anchor = g.chat_caret;
    clear_text_selection();
    g.text_selecting = true;
    if (g.window != nullptr) SetCapture(g.window);
  }
}

/// File drawer, copied from the web's FileTransferPanel: 380 px, right edge,
/// drop zone and a transfer list.
void draw_files_drawer(float w, float h) {
  if (!g.files_open && g.files_anim <= 0.01f) return;
  const float e = clampf(g.files_anim, 0.0f, 1.0f);
  const float width = 380;
  const float top = g.immersive ? 0.0f : 79.0f;
  const float left = w - width * e;
  const D2D1_RECT_F panel = D2D1::RectF(left, top, left + width, h);
  g.hit_files_panel = panel;
  shadow(g.target, *g.theme, panel, 16);
  fill_round(g.target, panel, 0,
             D2D1::ColorF(g.theme->surface.r, g.theme->surface.g,
                          g.theme->surface.b, 0.95f));
  line(g.target, panel.left, panel.top, panel.left, panel.bottom,
       g.theme->line, 1.0f);
  line(g.target, panel.left, panel.top + 44, panel.right, panel.top + 44,
       g.theme->line_soft, 1.0f);
  draw_icon(g.target, icon::folder, panel.left + 18, panel.top + 22,
            g.theme->accent);
  text(g.target, g.body_medium, L"文件",
       D2D1::RectF(panel.left + 34, panel.top + 6, panel.left + 90,
                   panel.top + 38),
       g.theme->ink);
  g.hit_files_close =
      D2D1::RectF(panel.right - 40, panel.top + 8, panel.right - 8,
                  panel.top + 36);
  quiet_icon_button(g.target, *g.theme, g.hit_files_close, hv_files_close,
                    icon::close, false,
                    inside(g.hit_files_close, g.hover_x, g.hover_y));

  // 发送 / 接收 tabs (the web's FileTransferPanel tabs).
  const wchar_t* tab_labels[2] = {L"发送", L"接收"};
  for (int i = 0; i < 2; ++i) {
    const D2D1_RECT_F tab =
        D2D1::RectF(panel.left + 16 + i * 82, panel.top + 50,
                    panel.left + 16 + i * 82 + 76, panel.top + 76);
    const bool active = g.files_tab == i;
    const bool hovered = inside(tab, g.hover_x, g.hover_y);
    fill_round(g.target, tab, 13,
               active ? g.theme->accent_soft
                      : D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                                     g.theme->panel.b, hovered ? 1.0f : 0.0f));
    text(g.target, g.label12, tab_labels[i], tab,
         active ? g.theme->accent
                : (hovered ? g.theme->ink : g.theme->muted));
    g.hit_files_tab[i] = tab;
  }

  // Bottom transfer list (both tabs), fed by the real file protocol.
  std::vector<molink::net::file_transfer_state> transfers;
  if (g.session != nullptr) transfers = g.session->file_transfers();
  const int shown_transfers =
      (std::min)(3, static_cast<int>(transfers.size()));
  const float list_h = shown_transfers * 40 + 8;
  const float content_top = panel.top + 88;
  const float content_bottom = panel.bottom - 8 - list_h;

  if (g.files_tab == 0) {
    // Drop zone / picker.
    const float zone_h = (std::max)(90.0f, content_bottom - content_top - 12);
    const D2D1_RECT_F zone =
        D2D1::RectF(panel.left + 16, content_top, panel.right - 16,
                    content_top + (std::min)(zone_h, 124.0f));
    g.hit_files_drop = zone;
    g.hit_files_pick = zone;
    const bool zone_hover = inside(zone, g.hover_x, g.hover_y);
    fill_round(g.target, zone, 12,
               D2D1::ColorF(g.theme->panel.r, g.theme->panel.g,
                            g.theme->panel.b, zone_hover ? 0.75f : 0.40f));
    dashed_round(g.target, zone, 12,
                 zone_hover ? g.theme->accent : g.theme->line_soft, 1.0f);
    draw_icon(g.target, icon::folder, (zone.left + zone.right) / 2,
              zone.top + 40, g.theme->muted);
    text(g.target, g.body_center, L"拖放文件到此处",
         D2D1::RectF(zone.left, zone.top + 56, zone.right, zone.top + 82),
         g.theme->ink);
    text(g.target, g.small, L"或点击选择文件",
         D2D1::RectF(zone.left, zone.top + 80, zone.right, zone.top + 102),
         g.theme->muted);
  } else {
    // Host browser: up + path + refresh, then the folder listing.
    const D2D1_RECT_F up =
        D2D1::RectF(panel.left + 16, content_top, panel.left + 42,
                    content_top + 26);
    const D2D1_RECT_F refresh =
        D2D1::RectF(panel.right - 42, content_top, panel.right - 16,
                    content_top + 26);
    g.hit_files_up = up;
    g.hit_files_refresh = refresh;
    for (const D2D1_RECT_F* button : {&up, &refresh}) {
      if (inside(*button, g.hover_x, g.hover_y)) {
        fill_round(g.target, *button, 6, g.theme->panel);
      }
    }
    draw_icon(g.target, icon::arrow_up_right, up.left + 13, up.top + 13,
              g.theme->muted);
    draw_icon(g.target, icon::rotate, refresh.left + 13, refresh.top + 13,
              g.theme->muted);
    std::wstring shown_path =
        g.files_path.empty() ? L"主机主目录" : g.files_path;
    if (shown_path.size() > 34) {
      shown_path = L"…" + shown_path.substr(shown_path.size() - 32);
    }
    text(g.target, g.small_left, shown_path,
         D2D1::RectF(up.right + 8, content_top, refresh.left - 8,
                     content_top + 26),
         g.theme->muted);

    float ry = content_top + 32;
    const int capacity = (std::max)(
        1, (std::min)(12, static_cast<int>((content_bottom - ry) / 28)));
    const int total = static_cast<int>(g.files_entries.size());
    g.files_scroll = (std::max)(
        0, (std::min)(g.files_scroll, (std::max)(0, total - capacity)));
    for (int i = 0; i < 12; ++i) g.hit_files_entry[i] = D2D1::RectF(0, 0, 0, 0);
    if (total == 0) {
      text(g.target, g.small, g.files_loading ? L"正在读取主机文件夹…"
                           : g.files_error.empty()
                               ? L"文件夹为空（点击 ↻ 读取）"
                               : g.files_error,
           D2D1::RectF(panel.left + 16, ry, panel.right - 16, ry + 24),
           g.files_error.empty() ? g.theme->muted
                                 : (g.theme == &kLight
                                        ? D2D1::ColorF(0xc0392b)
                                        : D2D1::ColorF(0xff8f87)));
    } else {
      for (int k = 0; k < capacity && g.files_scroll + k < total; ++k) {
        const molink::net::file_listing_entry& entry =
            g.files_entries[g.files_scroll + k];
        const D2D1_RECT_F row =
            D2D1::RectF(panel.left + 10, ry, panel.right - 10, ry + 26);
        if (inside(row, g.hover_x, g.hover_y)) {
          fill_round(g.target, row, 6, g.theme->panel);
        }
        draw_icon(g.target, entry.dir ? icon::folder : icon::clipboard,
                  panel.left + 24, ry + 13,
                  entry.dir ? g.theme->accent : g.theme->muted);
        text(g.target, g.small_left, entry.name,
             D2D1::RectF(panel.left + 38, ry, panel.right - 80, ry + 26),
             g.theme->ink);
        if (!entry.dir) {
          text(g.target, g.small_right, format_bytes(entry.size),
               D2D1::RectF(panel.right - 76, ry, panel.right - 16, ry + 26),
               g.theme->muted);
        }
        g.hit_files_entry[k] = row;
        ry += 28;
      }
    }
  }

  // Transfers: name, direction, progress.
  float ty = content_bottom + 6;
  int transfer_slot = 0;
  for (int s = 0; s < 3; ++s) g.hit_files_cancel[s] = D2D1::RectF(0, 0, 0, 0);
  for (int i = static_cast<int>(transfers.size()) - shown_transfers;
       i < static_cast<int>(transfers.size()); ++i) {
    if (i < 0) continue;
    const int slot = transfer_slot++;
    const molink::net::file_transfer_state& item = transfers[i];
    const D2D1_COLOR_F danger =
        g.theme == &kLight ? D2D1::ColorF(0xc0392b)
                           : D2D1::ColorF(0xff8f87);
    draw_icon(g.target, item.download ? icon::arrow_up_right : icon::upload,
              panel.left + 24, ty + 10,
              item.state == 3 ? danger : g.theme->accent);
    text(g.target, g.small_left, item.name,
         D2D1::RectF(panel.left + 38, ty - 2, panel.right - 90, ty + 18),
         g.theme->ink);
    std::wstring status;
    if (item.state == 2) {
      status = L"已完成";
    } else if (item.state == 3) {
      status = L"失败";
    } else if (item.size > 0) {
      status = std::to_wstring(item.sent * 100 / item.size) + L"%";
    } else {
      status = L"等待";
    }
    const bool active = item.state == 0 || item.state == 1;
    const float status_right = active ? panel.right - 38 : panel.right - 16;
    text(g.target, g.small_right, status,
         D2D1::RectF(panel.right - 104, ty - 2, status_right, ty + 18),
         item.state == 3 ? danger : g.theme->muted);
    if (active && slot < 3) {
      const D2D1_RECT_F cancel =
          D2D1::RectF(panel.right - 32, ty, panel.right - 14, ty + 18);
      if (inside(cancel, g.hover_x, g.hover_y)) {
        fill_round(g.target, cancel, 4,
                   D2D1::ColorF(danger.r, danger.g, danger.b, 0.15f));
      }
      draw_icon(g.target, icon::close, cancel.left + 9, cancel.top + 9,
                inside(cancel, g.hover_x, g.hover_y) ? danger
                                                     : g.theme->muted);
      g.hit_files_cancel[slot] = cancel;
    }
    const float bar_left = panel.left + 38;
    const float bar_right = panel.right - 16;
    const float fraction = item.size > 0
                               ? clampf(static_cast<float>(item.sent) /
                                            static_cast<float>(item.size),
                                        0.0f, 1.0f)
                               : 0.0f;
    fill_round(g.target,
               D2D1::RectF(bar_left, ty + 24, bar_right, ty + 28), 2,
               g.theme->line_soft);
    fill_round(g.target,
               D2D1::RectF(bar_left, ty + 24,
                           bar_left + (bar_right - bar_left) * fraction,
                           ty + 28),
               2, item.state == 3 ? danger : g.theme->accent_strong);
    ty += 40;
  }
}

/// Joins a host path and a listing entry (the host accepts both slashes).
std::wstring join_host_path(const std::wstring& base,
                            const std::wstring& name) {
  if (base.empty()) return name;
  const wchar_t last = base.back();
  if (last == L'/' || last == L'\\') return base + name;
  return base + L"/" + name;
}

void files_click(float x, float y, HWND window) {
  if (inside(g.hit_files_close, x, y)) {
    g.files_open = false;
    return;
  }
  if (g.session != nullptr) {
    const std::vector<molink::net::file_transfer_state> transfers =
        g.session->file_transfers();
    const int shown =
        (std::min)(3, static_cast<int>(transfers.size()));
    const int start = static_cast<int>(transfers.size()) - shown;
    for (int k = 0; k < 3; ++k) {
      if (!inside(g.hit_files_cancel[k], x, y)) continue;
      const int index = start + k;
      if (index >= 0 && index < static_cast<int>(transfers.size())) {
        g.session->file_cancel(transfers[index].id);
      }
      return;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (inside(g.hit_files_tab[i], x, y)) {
      g.files_tab = i;
      if (i == 1 && g.session != nullptr && g.session->live()) {
        request_files_browse();
      }
      return;
    }
  }
  if (g.files_tab == 0) {
    if (inside(g.hit_files_pick, x, y)) {
      pick_files(window);
    }
    return;
  }
  if (inside(g.hit_files_refresh, x, y)) {
    request_files_browse();
    return;
  }
  if (inside(g.hit_files_up, x, y)) {
    if (!g.files_path.empty()) {
      const size_t slash = g.files_path.find_last_of(L"/\\");
      if (slash == std::wstring::npos) {
        g.files_path.clear();
      } else {
        g.files_path = g.files_path.substr(0, slash);
        if (g.files_path.empty()) g.files_path = L"/";
      }
    }
    request_files_browse();
    return;
  }
  if (g.session == nullptr || !g.session->live()) return;
  for (int k = 0; k < 12; ++k) {
    if (!inside(g.hit_files_entry[k], x, y)) continue;
    const int index = g.files_scroll + k;
    if (index < 0 || index >= static_cast<int>(g.files_entries.size())) return;
    const molink::net::file_listing_entry& entry = g.files_entries[index];
    if (entry.dir) {
      g.files_path = join_host_path(g.files_path, entry.name);
      request_files_browse();
    } else {
      g.session->file_request(join_host_path(g.files_path, entry.name));
      show_toast(L"正在请求 " + entry.name);
    }
    return;
  }
}

void set_clipboard_text(const std::wstring& text) {
  if (!OpenClipboard(g.window)) return;
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (mem != nullptr) {
    void* dst = GlobalLock(mem);
    if (dst != nullptr) {
      memcpy(dst, text.c_str(), bytes);
      GlobalUnlock(mem);
      SetClipboardData(CF_UNICODETEXT, mem);
    }
  }
  CloseClipboard();
}

/// Dashed rounded border, used by the web's empty-state and drop zones.
void dashed_round(ID2D1RenderTarget* t, const D2D1_RECT_F& r, float radius,
                  const D2D1_COLOR_F& color, float width) {
  ID2D1StrokeStyle* style = nullptr;
  if (g.factory == nullptr) return;
  g.factory->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(
          D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
          D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f),
      nullptr, 0, &style);
  ID2D1SolidColorBrush* brush = nullptr;
  if (FAILED(t->CreateSolidColorBrush(color, &brush)) || brush == nullptr) {
    if (style != nullptr) style->Release();
    return;
  }
  t->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush, width,
                          style);
  brush->Release();
  if (style != nullptr) style->Release();
}

void draw_close_prompt(float w, float h) {
  if (!g.close_prompt) return;
  fill_round(g.target, D2D1::RectF(0, 0, w, h), 0,
             D2D1::ColorF(0, 0, 0, 0.40f));
  const float pw = 330, ph = 196;
  const D2D1_RECT_F panel =
      D2D1::RectF((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
  g.hit_close_panel = panel;
  shadow(g.target, *g.theme, panel, 12);
  fill_round(g.target, panel, 12, g.theme->surface);
  stroke_round(g.target, panel, 12, g.theme->line_soft);
  text(g.target, g.body_medium, L"关闭窗口",
       D2D1::RectF(panel.left + 16, panel.top + 12, panel.right - 16,
                   panel.top + 36),
       g.theme->ink);
  text(g.target, g.tiny_wrap,
       L"当前有 1 个远程画面标签页。要关闭会话标签页，还是把窗口最小化到"
       L"托盘（会话继续）？",
       D2D1::RectF(panel.left + 16, panel.top + 40, panel.right - 16,
                   panel.top + 92),
       g.theme->muted);
  g.hit_close_remember = D2D1::RectF(panel.left + 16, panel.top + 96,
                                     panel.right - 16, panel.top + 116);
  const D2D1_RECT_F box = D2D1::RectF(panel.left + 16, panel.top + 98,
                                      panel.left + 30, panel.top + 112);
  fill_round(g.target, box, 3,
             g.close_remember ? g.theme->accent : g.theme->panel);
  stroke_round(g.target, box, 3,
               g.close_remember ? g.theme->accent : g.theme->line);
  if (g.close_remember) {
    draw_icon(g.target, icon::check, panel.left + 23, panel.top + 105,
              g.theme->on_accent);
  }
  text(g.target, g.small_left, L"记住我的选择，下次不再提示",
       D2D1::RectF(panel.left + 36, panel.top + 96, panel.right - 16,
                   panel.top + 116),
       g.theme->muted);

  const float by = panel.bottom - 40;
  const float bh = 28;
  const float right = panel.right - 16;
  g.hit_close_tray = D2D1::RectF(right - 104, by, right, by + bh);
  g.hit_close_tab = D2D1::RectF(right - 104 - 8 - 88, by,
                                right - 104 - 8, by + bh);
  g.hit_close_cancel = D2D1::RectF(right - 104 - 8 - 88 - 8 - 52, by,
                                   right - 104 - 8 - 88 - 8, by + bh);
  auto prompt_button = [&](const D2D1_RECT_F& r, const wchar_t* label,
                           bool primary) {
    const bool hovered = inside(r, g.hover_x, g.hover_y);
    if (primary) {
      fill_round(g.target, r, 6, g.theme->accent_soft);
      text(g.target, g.label12, label, r, g.theme->accent);
    } else {
      if (hovered) fill_round(g.target, r, 6, g.theme->panel);
      text(g.target, g.label12, label, r,
           hovered ? g.theme->ink : g.theme->muted);
    }
  };
  prompt_button(g.hit_close_cancel, L"取消", false);
  prompt_button(g.hit_close_tab, L"关闭标签页", false);
  prompt_button(g.hit_close_tray, L"最小化到托盘", true);
}

bool close_prompt_click(float x, float y) {
  if (!g.close_prompt) return false;
  if (!inside(g.hit_close_panel, x, y)) {
    g.close_prompt = false;
    return true;
  }
  if (inside(g.hit_close_remember, x, y)) {
    g.close_remember = !g.close_remember;
    return true;
  }
  if (inside(g.hit_close_cancel, x, y)) {
    g.close_prompt = false;
    return true;
  }
  if (inside(g.hit_close_tab, x, y)) {
    if (g.close_remember) save_close_behavior(2);
    g.close_prompt = false;
    close_session_tab();
    return true;
  }
  if (inside(g.hit_close_tray, x, y)) {
    if (g.close_remember) save_close_behavior(1);
    g.close_prompt = false;
    minimize_to_tray();
    return true;
  }
  return true;
}

void draw_overlays(float w, float h) {
  if (g.tab == 1 || g.immersive) {
    draw_chat_drawer(w, h);
    draw_files_drawer(w, h);
  }
  draw_settings_modal(w, h);
  draw_rename_modal(w, h);
}

std::wstring* focus_text(int field) {
  switch (field) {
    case 1: return &g.address_text;
    case 2: return &g.code_text;
    case 3: return &g.chat_input;
    case 4: return &g.set_device_name;
    case 5: return &g.set_tcp;
    case 6: return &g.set_udp;
    case 7: return &g.set_long_term;
    case 8: return &g.server_url;
    case 9: return &g.server_invite;
    case 10: return &g.stun_server;
    case 11: return &g.retry_code;
    case 20: return &g.rename_text;
    default: return nullptr;
  }
}

size_t* focus_caret(int field) {
  switch (field) {
    case 1: return &g.address_caret;
    case 2: return &g.code_caret;
    case 3: return &g.chat_caret;
    case 4: return &g.set_caret_dev;
    case 5: return &g.set_caret_tcp;
    case 6: return &g.set_caret_udp;
    case 7: return &g.set_caret_long;
    case 8: return &g.set_caret_url;
    case 9: return &g.set_caret_invite;
    case 10: return &g.set_caret_stun;
    case 11: return &g.retry_caret;
    case 20: return &g.rename_caret;
    default: return nullptr;
  }
}

bool text_selection(std::wstring* target, size_t* first, size_t* last) {
  if (target == nullptr || g.sel_field != g.focus_field ||
      g.sel_start == g.sel_end) {
    return false;
  }
  *first = (std::min)(g.sel_start, g.sel_end);
  *last = (std::max)(g.sel_start, g.sel_end);
  return true;
}

void clear_text_selection() {
  g.sel_field = -1;
  g.sel_start = g.sel_end = 0;
}

/// Caret index nearest to x for the home address/code and chat fields.
size_t caret_for_field(int field, float x) {
  std::wstring text;
  IDWriteTextFormat* format = g.small_left;
  float x0 = 0;
  if (field == 1) {
    text = g.address_text;
    format = g.mono_input;
    x0 = g.hit_connect_input_home.left + 12;
  } else if (field == 2) {
    text = g.connect_code_visible ? g.code_text
                                  : std::wstring(g.code_text.size(), L'•');
    x0 = g.hit_connect_code_home.left + 30;
  } else if (field == 3) {
    text = g.chat_input;
    x0 = g.hit_chat_input.left + 10;
  } else if (field == 11) {
    text = g.retry_code;
    x0 = g.hit_retry_input.left + 10;
  } else {
    return 0;
  }
  size_t best = 0;
  float best_distance = 1e9f;
  for (size_t i = 0; i <= text.size(); ++i) {
    const float width = measure_text(format, text.substr(0, i));
    const float distance = std::fabs(x0 + width - x);
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Text-field context menu: 剪切 / 复制 / 粘贴 / 全选. The web gets these from
// the browser's native menu; the Direct2D client has to draw its own.
// ---------------------------------------------------------------------------

void close_ctx_menu() { g.ctx_field = 0; }

void open_ctx_menu(int field, float x, float y) {
  if (focus_text(field) == nullptr) return;
  if (g.focus_field != field) {
    g.focus_field = field;
    clear_text_selection();
  }
  const bool selected = g.sel_field == field && g.sel_start != g.sel_end;
  g.ctx_field = field;
  g.ctx_cut = selected;
  g.ctx_copy = selected;
  g.ctx_paste = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
  const D2D1_SIZE_F size =
      g.target != nullptr ? g.target->GetSize() : D2D1::SizeF(800, 600);
  const float mw = 132, item_h = 28, mh = item_h * 4 + 10;
  float mx = x + 4, my = y + 4;
  mx = clampf(mx, 8, (std::max)(8.0f, size.width - 8 - mw));
  my = clampf(my, 8, (std::max)(8.0f, size.height - 8 - mh));
  g.ctx_panel = D2D1::RectF(mx, my, mx + mw, my + mh);
  for (int i = 0; i < 4; ++i) {
    g.hit_ctx[i] = D2D1::RectF(mx + 5, my + 5 + i * item_h, mx + mw - 5,
                               my + 5 + (i + 1) * item_h);
  }
}

/// True when x falls inside the field's current selection (so a right-click
/// can keep it for 复制 instead of collapsing to a caret).
bool selection_contains_point(int field, float x) {
  if (g.sel_field != field || g.sel_start == g.sel_end) return false;
  std::wstring shown;
  IDWriteTextFormat* format = g.small_left;
  float x0 = 0;
  if (field == 1) {
    shown = g.address_text;
    format = g.mono_input;
    x0 = g.hit_connect_input_home.left + 12;
  } else if (field == 2) {
    shown = g.connect_code_visible ? g.code_text
                                   : std::wstring(g.code_text.size(), L'•');
    x0 = g.hit_connect_code_home.left + 30;
  } else if (field == 3) {
    shown = g.chat_input;
    x0 = g.hit_chat_input.left + 10;
  } else if (field == 11) {
    shown = g.retry_code;
    x0 = g.hit_retry_input.left + 10;
  } else {
    return false;
  }
  const size_t first = (std::min)(g.sel_start, g.sel_end);
  const size_t last = (std::max)(g.sel_start, g.sel_end);
  const float xa = x0 + measure_text(format, shown.substr(0, first));
  const float xb = x0 + measure_text(format, shown.substr(0, last));
  return x >= xa - 2 && x <= xb + 2;
}

void ctx_copy_selection(bool cut) {
  std::wstring* text = focus_text(g.ctx_field);
  size_t* caret = focus_caret(g.ctx_field);
  size_t first = 0, last = 0;
  if (text == nullptr || !text_selection(text, &first, &last)) return;
  set_clipboard_text(text->substr(first, last - first));
  if (cut && caret != nullptr) {
    text->erase(first, last - first);
    *caret = first;
    clear_text_selection();
  }
}

void ctx_paste() {
  std::wstring* text = focus_text(g.ctx_field);
  size_t* caret = focus_caret(g.ctx_field);
  if (text == nullptr || caret == nullptr) return;
  size_t first = 0, last = 0;
  if (text_selection(text, &first, &last)) {
    text->erase(first, last - first);
    *caret = first;
    clear_text_selection();
  }
  const size_t limit = g.ctx_field == 3 ? 500 : 64;
  if (!OpenClipboard(g.window)) return;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data != nullptr) {
    const auto* clip = static_cast<const wchar_t*>(GlobalLock(data));
    if (clip != nullptr) {
      for (const wchar_t* c = clip; *c != L'\0' && text->size() < limit; ++c) {
        if (*c >= 32 && *c != 127) {
          text->insert(text->begin() + static_cast<long>(*caret), *c);
          ++*caret;
        }
      }
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
}

void ctx_select_all() {
  std::wstring* text = focus_text(g.ctx_field);
  size_t* caret = focus_caret(g.ctx_field);
  if (text == nullptr || caret == nullptr) return;
  g.sel_field = g.ctx_field;
  g.sel_anchor = 0;
  g.sel_start = 0;
  g.sel_end = text->size();
  *caret = text->size();
}

void ctx_apply(int index) {
  switch (index) {
    case 0: ctx_copy_selection(true); break;
    case 1: ctx_copy_selection(false); break;
    case 2: ctx_paste(); break;
    case 3: ctx_select_all(); break;
    default: break;
  }
  InvalidateRect(g.window, nullptr, FALSE);
}

void draw_ctx_menu() {
  if (g.ctx_field == 0) return;
  shadow(g.target, *g.theme, g.ctx_panel, 12);
  fill_round(g.target, g.ctx_panel, 10, g.theme->surface);
  stroke_round(g.target, g.ctx_panel, 10, g.theme->line_soft);
  const wchar_t* labels[4] = {L"剪切", L"复制", L"粘贴", L"全选"};
  const bool enabled[4] = {g.ctx_cut, g.ctx_copy, g.ctx_paste, true};
  for (int i = 0; i < 4; ++i) {
    const D2D1_RECT_F& r = g.hit_ctx[i];
    const bool hovered = inside(r, g.hover_x, g.hover_y);
    if (hovered && enabled[i]) fill_round(g.target, r, 6, g.theme->panel);
    text(g.target, g.label12, labels[i],
         D2D1::RectF(r.left + 10, r.top, r.right, r.bottom),
         enabled[i] ? (hovered ? g.theme->ink : g.theme->ink)
                    : g.theme->muted);
  }
}

/// Which cursor the web would show over this point: pointer over controls,
/// move over the ball/bar background, text cursor in the home form fields.
LPCWSTR cursor_at(float x, float y) {
  const D2D1_RECT_F none = D2D1::RectF(0, 0, 0, 0);
  auto in = [&](const D2D1_RECT_F& r) { return inside(r, x, y); };
  (void)none;
  if (g.ctx_field != 0) {
    const bool enabled[4] = {g.ctx_cut, g.ctx_copy, g.ctx_paste, true};
    for (int i = 0; i < 4; ++i) {
      if (in(g.hit_ctx[i]) && enabled[i]) return IDC_HAND;
    }
    return IDC_ARROW;
  }
  if (g.close_prompt) {
    if (in(g.hit_close_remember) || in(g.hit_close_cancel) ||
        in(g.hit_close_tab) || in(g.hit_close_tray)) {
      return IDC_HAND;
    }
    return IDC_ARROW;
  }
  if (!g.immersive) {
    for (int i = 0; i < 3; ++i) {
      if (in(g.hit_win[i])) return IDC_HAND;
    }
    if (in(g.hit_tab[0]) || in(g.hit_tab[1]) || in(g.hit_tab[2]) ||
        in(g.hit_session_close) || in(g.hit_theme_sq) || in(g.hit_settings)) {
      return IDC_HAND;
    }
  }
  if (g.tab == 0 && !g.settings_open) {
    if (in(g.hit_connect_send) || in(g.hit_connect_code_eye)) return IDC_HAND;
    if (in(g.hit_connect_input_home) && !in(g.hit_connect_send)) {
      return IDC_IBEAM;
    }
    if (in(g.hit_connect_code_home) && !in(g.hit_connect_code_eye)) {
      return IDC_IBEAM;
    }
    if (in(g.hit_self_avatar) || in(g.hit_self_name) || in(g.hit_copy_id) ||
        in(g.hit_code_eye) || in(g.hit_code_copy) || in(g.hit_code_refresh) ||
        in(g.hit_code_single) || in(g.hit_code_multi) ||
        in(g.hit_more_settings) || in(g.hit_connect_send) ||
        in(g.hit_connect_code_eye) || in(g.hit_connect_btn) ||
        in(g.hit_relay) || in(g.hit_notice_update) || in(g.hit_view_cards) ||
        in(g.hit_view_list)) {
      return IDC_HAND;
    }
    for (int i = 0; i < 3; ++i) {
      if (in(g.hit_mode[i])) return IDC_HAND;
    }
    if (g.device_tab == 0) {
      for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) {
          if (in(g.hit_recent_action[i][j])) return IDC_HAND;
        }
        if (in(g.hit_recent_forget[i])) return IDC_HAND;
        if (in(g.hit_recent_star[i])) return IDC_HAND;
        if (in(g.hit_recent_edit[i])) return IDC_HAND;
        if (in(g.hit_recent_card[i])) return IDC_HAND;
      }
    }
    for (int i = 0; i < 4; ++i) {
      if (in(g.hit_dev_tab[i])) return IDC_HAND;
    }
  }
  if (g.settings_open) {
    if (in(g.hit_set_close)) return IDC_HAND;
    for (int i = 0; i < 6; ++i) {
      if (in(g.hit_set_nav[i])) return IDC_HAND;
    }
    if (g.open_menu >= 10 &&
        in(menu_rect(g.menu_field, g.open_menu, menu_count(g.open_menu),
                     g.target != nullptr ? g.target->GetSize().width : 0))) {
      return IDC_HAND;
    }
    switch (g.settings_section) {
      case 0:
        for (int i = 0; i < 2; ++i) {
          if (in(g.hit_seg_theme[i]) || in(g.hit_seg_lang[i])) return IDC_HAND;
        }
        if (in(g.hit_lock_field)) return IDC_HAND;
        break;
      case 1:
        for (int i = 0; i < 3; ++i) {
          if (in(g.hit_seg_video[i])) return IDC_HAND;
        }
        for (int i = 0; i < 6; ++i) {
          if (in(g.hit_sec_toggle[i])) return IDC_HAND;
        }
        if (in(g.hit_dev_name) || in(g.hit_long_term) || in(g.hit_stun)) {
          return IDC_IBEAM;
        }
        if (in(g.hit_dev_apply) || in(g.hit_long_apply)) return IDC_HAND;
        break;
      case 2:
        if (in(g.hit_tcp) || in(g.hit_udp) || in(g.hit_server_url) ||
            in(g.hit_server_invite)) {
          return IDC_IBEAM;
        }
        if (in(g.hit_ports_save) || in(g.hit_server_toggle) ||
            in(g.hit_server_save) || in(g.hit_server_default)) {
          return IDC_HAND;
        }
        break;
      case 3:
        for (int i = 0; i < 3; ++i) {
          if (in(g.hit_disp_toggle[i])) return IDC_HAND;
        }
        if (in(g.hit_fps_field)) return IDC_HAND;
        for (int i = 0; i < 8; ++i) {
          if (in(g.hit_quality_card[i])) return IDC_HAND;
        }
        break;
      case 4:
        for (int i = 0; i < 5; ++i) {
          if (in(g.hit_service_btn[i])) return IDC_HAND;
        }
        break;
      case 5:
        if (in(g.hit_update_btn) || in(g.hit_repo_btn) ||
            in(g.hit_site_btn) || in(g.hit_star_btn)) {
          return IDC_HAND;
        }
        break;
      default:
        break;
    }
    return IDC_ARROW;
  }
  if (g.tab == 1 || g.immersive) {
    if (in(g.hit_retry_input)) return IDC_IBEAM;
    if (in(g.hit_retry_btn)) return IDC_HAND;
    if (g.chat_open && in(g.hit_chat_panel)) {
      if (in(g.hit_chat_close) || in(g.hit_chat_send)) return IDC_HAND;
      if (in(g.hit_chat_input)) return IDC_IBEAM;
      return IDC_ARROW;
    }
    if (g.files_open && in(g.hit_files_panel)) {
      if (in(g.hit_files_close) || in(g.hit_files_pick) ||
          in(g.hit_files_drop)) {
        return IDC_HAND;
      }
      return IDC_ARROW;
    }
    if (g.open_menu >= 0 &&
        in(menu_rect(g.menu_field, g.open_menu, menu_count(g.open_menu),
                     g.target != nullptr ? g.target->GetSize().width : 0))) {
      return IDC_HAND;
    }
    if (g.quality_open) {
      if (in(g.qp_close) || in(g.qp_preset[0]) || in(g.qp_rate[0]) ||
          in(g.qp_slider[0]) || in(g.qp_format) || in(g.qp_reset)) {
        return IDC_HAND;
      }
    }
    if (!g.bar_open) {
      if (in(g.hit_ball)) return IDC_SIZEALL;
      return IDC_ARROW;
    }
    const D2D1_RECT_F controls[] = {
        g.hit_pin, g.hit_collapse, g.hit_fullscreen, g.hit_display[0],
        g.hit_display[1], g.hit_display[2], g.hit_fps, g.hit_quality,
        g.hit_codec, g.hit_watermark, g.hit_resolution, g.hit_clip_toggle,
        g.hit_clip_get, g.hit_clip_set, g.hit_audio, g.hit_volume,
        g.hit_chat, g.hit_files, g.hit_control, g.hit_sas, g.hit_tool_theme,
        g.hit_tool_settings, g.hit_path, g.hit_protocol, g.hit_clip_menu,
        g.hit_tools_menu, g.hit_appearance_menu, g.hit_disconnect,
        g.hit_transport};
    for (const D2D1_RECT_F& r : controls) {
      if (in(r)) return IDC_HAND;
    }
    if (in(g.hit_bar)) return IDC_SIZEALL;
  }
  return IDC_ARROW;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g.window = window;
      if (!create_resources(window)) return -1;
      if (g.rename_test && !g.recents.empty()) open_rename_dialog(0);
      if (!g.surface_path.empty()) surface_open(g.surface_path);
      if (!g.session_host.empty() && g.session_port > 0) {
        // --session: go through the normal connect path so the CLI gets the
        // same local-core/surface handling (and stream codecs) as a click.
        g.address_text =
            std::wstring(g.session_host.begin(), g.session_host.end()) + L":" +
            std::to_wstring(g.session_port);
        g.code_text.assign(g.session_code.begin(), g.session_code.end());
        PostMessageW(window, WM_APP_START_CONNECT, 0, 0);
      }
      load_saved_avatar();
      DragAcceptFiles(window, TRUE);
      apply_rounded(window);
      SetTimer(window, 1, 16, nullptr);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      const float scale = (g_dpi_base > 0 ? g_dpi_base : 96.0f) / 96.0f;
      info->ptMinTrackSize.x = static_cast<LONG>(760 * scale);
      info->ptMinTrackSize.y = static_cast<LONG>(520 * scale);
      return 0;
    }
    case WM_CLOSE:
      if (g.quitting || !g.session_open) {
        DestroyWindow(window);
        return 0;
      }
      if (g.close_behavior == 1) {
        minimize_to_tray();
        return 0;
      }
      if (g.close_behavior == 2) {
        close_session_tab();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      close_ctx_menu();
      g.close_prompt = true;
      g.close_remember = false;
      InvalidateRect(window, nullptr, FALSE);
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
      // A modal covers the window: nothing behind it may become a caption
      // drag, otherwise its own header buttons (y < 40) are dead.
      if (g.settings_open || g.close_prompt) return HTCLIENT;
      if (left && top) return HTTOPLEFT;
      if (right && top) return HTTOPRIGHT;
      if (left && bottom) return HTBOTTOMLEFT;
      if (right && bottom) return HTBOTTOMRIGHT;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      // The tab strip doubles as the title bar.
      // Only the 40 px title bar drags the window. Returning HTCAPTION for the
      // whole chrome band swallowed every click on the tabs, the theme square
      // and the settings square (they never reached WM_LBUTTONDOWN).
      // Immersive mode hides the title bar, so nothing drags there.
      if (!g.immersive &&
          client.y < static_cast<int>(40 * g_dpi / 96.0f)) {
        // The three window buttons sit inside the drag strip: they must get
        // WM_LBUTTONDOWN, not become a caption drag.
        const float lx = static_cast<float>(client.x) * 96.0f / g_dpi;
        const float ly = static_cast<float>(client.y) * 96.0f / g_dpi;
        for (int i = 0; i < 3; ++i) {
          if (inside(g.hit_win[i], lx, ly)) return HTCLIENT;
        }
        return HTCAPTION;
      }
      return base;
    }
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT screen{};
        GetCursorPos(&screen);
        ScreenToClient(window, &screen);
        const float x = static_cast<float>(screen.x) * 96.0f / g_dpi;
        const float y = static_cast<float>(screen.y) * 96.0f / g_dpi;
        SetCursor(LoadCursorW(nullptr, cursor_at(x, y)));
        return TRUE;
      }
      break;
    case WM_NCLBUTTONDBLCLK:
      if (wparam == HTCAPTION) {
        ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
        return 0;
      }
      break;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
      const float x = static_cast<float>(LOWORD(lparam)) * 96.0f / g_dpi;
      const float y = static_cast<float>(HIWORD(lparam)) * 96.0f / g_dpi;
      if (message == WM_RBUTTONDOWN) {
        if (g.ctx_field != 0) {
          close_ctx_menu();
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        int field = 0;
        if (g.tab == 0 && !g.settings_open && !g.close_prompt) {
          if (inside(g.hit_connect_input_home, x, y) &&
              !inside(g.hit_connect_send, x, y)) {
            field = 1;
          } else if (inside(g.hit_connect_code_home, x, y) &&
                     !inside(g.hit_connect_code_eye, x, y)) {
            field = 2;
          }
        } else if ((g.tab == 1 || g.immersive) && !g.settings_open) {
          if (g.chat_open && inside(g.hit_chat_input, x, y)) {
            field = 3;
          } else if (inside(g.hit_retry_input, x, y)) {
            field = 11;
          }
        }
        if (field != 0) {
          if (!selection_contains_point(field, x)) {
            size_t* caret = focus_caret(field);
            const size_t index = caret_for_field(field, x);
            if (caret != nullptr) *caret = index;
            clear_text_selection();
            g.sel_anchor = index;
          }
          open_ctx_menu(field, x, y);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      if (remote_input_active() && inside(g.picture_rect, x, y) &&
          !inside(g.hit_bar, x, y) && !inside(g.hit_ball, x, y)) {
        send_remote_move(x, y);
        const int button = message == WM_RBUTTONDOWN ? 3 : 2;
        send_remote_button(button, true);
        g.remote_buttons |= 1 << button;
        g.remote_mouse_down = true;
        if (g.window != nullptr) SetCapture(g.window);
        return 0;
      }
      break;
    }
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
      const int button = message == WM_RBUTTONUP ? 3 : 2;
      if ((g.remote_buttons & (1 << button)) != 0) {
        send_remote_button(button, false);
        g.remote_buttons &= ~(1 << button);
        if (g.remote_buttons == 0) {
          g.remote_mouse_down = false;
          ReleaseCapture();
        }
        return 0;
      }
      break;
    }
    case WM_MOUSEHWHEEL: {
      if (remote_input_active()) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &screen);
        const float x = static_cast<float>(screen.x) * 96.0f / g_dpi;
        const float y = static_cast<float>(screen.y) * 96.0f / g_dpi;
        if (inside(g.picture_rect, x, y) && !inside(g.hit_bar, x, y)) {
          const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
          send_remote_wheel(delta / 120.0f, 0);
          return 0;
        }
      }
      break;
    }
    case WM_MOUSEWHEEL:
      if (g.ctx_field != 0) {
        close_ctx_menu();
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g.files_open && g.files_tab == 1) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &screen);
        const float x = static_cast<float>(screen.x) * 96.0f / g_dpi;
        const float y = static_cast<float>(screen.y) * 96.0f / g_dpi;
        if (inside(g.hit_files_panel, x, y)) {
          const int delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120;
          g.files_scroll = (std::max)(0, g.files_scroll - delta * 3);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      if (remote_input_active()) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &screen);
        const float x = static_cast<float>(screen.x) * 96.0f / g_dpi;
        const float y = static_cast<float>(screen.y) * 96.0f / g_dpi;
        if (inside(g.picture_rect, x, y) && !inside(g.hit_bar, x, y)) {
          const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
          send_remote_wheel(0, -delta / 120.0f);
          return 0;
        }
      }
      if (g.settings_open) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        g.settings_scroll =
            clampf(g.settings_scroll - (delta / 120.0f) * 56.0f, 0.0f,
                   g.settings_max_scroll);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.tab == 0) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        g.home_scroll =
            clampf(g.home_scroll - (delta / 120.0f) * 56.0f, 0.0f,
                   g.home_max_scroll);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_TIMER: {
      // Registration happens a moment after the core binds, and the id can
      // change when the directory is unreachable, so poll the identity (not
      // just at startup). Two loopback GETs; a missing core refuses at once.
      if (GetTickCount64() >= g.id_refresh_at) {
        g.id_refresh_at = GetTickCount64() + 5000;
        // The host core is a child process. Windows can end it (a crash, or a
        // security suite killing it) while this shell stays open; without a
        // watchdog the machine then silently disappears from the directory and
        // every remote attempt fails. Restart it with a small backoff.
        if (g.local_core_process != nullptr &&
            WaitForSingleObject(g.local_core_process, 0) == WAIT_OBJECT_0) {
          CloseHandle(g.local_core_process);
          g.local_core_process = nullptr;
          g.local_core_started = false;
          g.local_core_port = 0;
          g.local_core_token.clear();
          g.local_api_token.clear();
          native_log(L"core watchdog: the host core exited");
        }
        if (g.local_core_process == nullptr && !g.local_core_started &&
            GetTickCount64() >= g.core_restart_at) {
          g.core_restart_at = GetTickCount64() + 10000;
          int core_port = 0;
          std::string client_token;
          std::wstring surface;
          std::string core_error;
          if (start_local_core(&core_port, &client_token, &surface,
                               &core_error)) {
            if (!surface.empty()) g.surface_path = surface;
            native_log(L"core watchdog: restarted on port " +
                       std::to_wstring(core_port));
          } else {
            native_log(L"core watchdog: restart failed: " +
                       wide_of(core_error));
          }
        }
        load_device_identity();
      }
      surface_poll();
      if (g.session != nullptr) {
        g.session->tick(GetTickCount64());
        const std::wstring& host_name = g.session->device_name();
        if (!host_name.empty() && host_name != g.session_label) {
          g.session_label = host_name;
          InvalidateRect(window, nullptr, FALSE);
        }
        if (g.session->access_pending() && !g.access_notice_shown) {
          g.access_notice_shown = true;
          show_toast(L"已发送连接请求，等待对方确认");
          InvalidateRect(window, nullptr, FALSE);
        }
        const std::wstring& host_error = g.session->last_error();
        if (!host_error.empty() && host_error != g.seen_session_error) {
          g.seen_session_error = host_error;
          const std::wstring text = friendly_session_error(
              g.session->last_error_code(), host_error);
          g.connect_error = text;
          show_toast(text);
          InvalidateRect(window, nullptr, FALSE);
        }
        if (g.session->authenticated()) {
          g.session_accepted = true;
          {
            // The video path is host-global; the settings switch mirrors the
            // mode the host actually runs (another viewer may have changed it).
            const std::wstring& mode = g.session->video_mode();
            if (mode == L"stream") g.video_mode = 0;
            else if (mode == L"mixed") g.video_mode = 1;
            else if (mode == L"tiles") g.video_mode = 2;
          }
          {
            // Cache the host's avatar for this device. Uploaded pictures are
            // stored as a marker (the data URL would bloat the recents file
            // and WIC cannot decode the web's WebP here); the initial circle
            // stands in for those.
            const std::wstring& avatar = g.session->avatar();
            if (!avatar.empty()) {
              std::wstring stored = avatar;
              if (avatar.rfind(L"data:", 0) == 0) {
                // Uploaded picture (WebP): decode once into the avatar cache
                // and remember the file, not the 30 KiB data URL.
                std::string utf8(avatar.size() * 3, '\0');
                const int bytes = WideCharToMultiByte(
                    CP_UTF8, 0, avatar.data(), static_cast<int>(avatar.size()),
                    utf8.data(), static_cast<int>(utf8.size()), nullptr,
                    nullptr);
                utf8.resize(bytes > 0 ? static_cast<size_t>(bytes) : 0);
                const std::wstring path = avatar_import_data_url(utf8);
                stored = path.empty() ? L"data:" : (L"file:" + path);
              }
              for (app_state::recent_entry& entry : g.recents) {
                if (entry.target != g.connect_target) continue;
                if (entry.avatar != stored) {
                  entry.avatar = stored;
                  save_recents();
                  InvalidateRect(window, nullptr, FALSE);
                }
                break;
              }
            }
          }
          if (g.reconnecting) {
            // The replacement socket made it through hello: the retry cycle
            // is over.
            g.reconnecting = false;
            g.retry_attempt = 0;
            g.retry_at = 0;
            native_log(L"reconnected: target=" + g.connect_target);
            show_toast(L"已重新连接 " + g.session_label);
            InvalidateRect(window, nullptr, FALSE);
          }
          if (!g.connect_error.empty()) {
            g.connect_error.clear();
            InvalidateRect(window, nullptr, FALSE);
          }
          if (!g.intent_applied) {
            g.intent_applied = true;
            remember_recent(g.session_label, g.connect_target, g.connect_code,
                            true);
            if (g.intent == 1) {
              g.files_open = true;
              g.chat_open = false;
            } else if (g.intent == 2) {
              g.chat_open = true;
              g.files_open = false;
              g.focus_field = 3;
              g.chat_caret = g.chat_input.size();
            }
            InvalidateRect(window, nullptr, FALSE);
          }
          if (!g.send_after_connect.empty() && !g.send_after_done) {
            g.send_after_done = true;
            add_transfer(g.send_after_connect);
            g.send_after_connect.clear();
          }
        }
        for (const std::string& message : g.session->take_chat()) {
          g.chat_messages.push_back({wide_of(message), false});
          if (!g.chat_open) ++g.chat_unread;
          InvalidateRect(window, nullptr, FALSE);
        }
        for (const std::string& clip : g.session->take_clipboard()) {
          if (g.clipboard_on) {
            set_clipboard_text(wide_of(clip));
            show_toast(L"已接收远端剪贴板");
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        int fw = 0, fh = 0;
        uint64_t fseq = 0;
        if (g.session->copy_frame(g.stream_pixels, &fw, &fh, &fseq,
                                  g.stream_seq) &&
            g.target != nullptr) {
          g.stream_seq = fseq;
          if (g.stream_bitmap == nullptr || g.stream_w != static_cast<uint32_t>(fw) ||
              g.stream_h != static_cast<uint32_t>(fh)) {
            if (g.stream_bitmap != nullptr) g.stream_bitmap->Release();
            const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_IGNORE));
            g.target->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(fw),
                                               static_cast<UINT32>(fh)),
                                   g.stream_pixels.data(),
                                   static_cast<UINT32>(fw) * 4, props,
                                   &g.stream_bitmap);
            g.stream_w = static_cast<uint32_t>(fw);
            g.stream_h = static_cast<uint32_t>(fh);
          } else {
            g.stream_bitmap->CopyFromMemory(
                nullptr, g.stream_pixels.data(), static_cast<UINT32>(fw) * 4);
          }
          g.stream_at = GetTickCount64();
          InvalidateRect(window, nullptr, FALSE);
        }
        if (g.session->live()) {
          g.session_was_live = true;
        } else if (g.session_was_live) {
          g.session_was_live = false;
          mark_recent_offline(g.connect_target);
          // An accepted session that drops reconnects quietly (the web's
          // immediate retry then 1 s / 1 s / once a minute). A policy error
          // or a never-accepted request is final: the banner explains it.
          const bool retryable =
              g.session_accepted && g.session->last_error().empty();
          if (!g.session_accepted && g.core_connect && !g.force_direct) {
            // The core session never reached the host (probe/relay failure):
            // retry once with a direct dial so the user still gets tiles.
            g.force_direct = true;
            g.core_connect = false;
            native_log(L"core session failed before welcome; retrying direct");
            g.reconnecting = true;
            g.retry_attempt = 0;
            g.retry_at = GetTickCount64() + 500;
            show_toast(L"通过本机 core 连接失败，改用直连（平铺）");
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          native_log(L"session dropped: target=" + g.connect_target +
                     L" code=" + g.session->last_error_code() +
                     L" close=" + g.session->close_detail() +
                     L" error=" + g.session->last_error() +
                     (retryable ? L" -> reconnecting" : L" -> stopped"));
          if (retryable) {
            g.reconnecting = true;
            g.retry_attempt = 0;
            g.retry_at = GetTickCount64();
            show_toast(L"连接已断开，正在重连…");
          } else {
            show_toast(L"会话已断开");
          }
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      const ULONGLONG now = GetTickCount64();
      if (g.reconnecting && g.retry_at != 0 && now >= g.retry_at &&
          !g.connecting) {
        g.retry_at = 0;
        if (g.session != nullptr) {
          g.session->stop();
          g.session.reset();
        }
        start_connect(g.connect_target, g.connect_code);
        InvalidateRect(window, nullptr, FALSE);
      }
      handle_file_events();
      if (now - g.caret_tick >= 500) {
        g.caret_tick = now;
        g.caret_on = !g.caret_on;
        if (g.focus_field != 0) InvalidateRect(window, nullptr, FALSE);
      }
      if (advance_hover()) InvalidateRect(window, nullptr, FALSE);
      if (g.tip_id >= 0 && !g.tip_shown && now - g.tip_since >= 600) {
        g.tip_shown = true;
        InvalidateRect(window, nullptr, FALSE);
      }
      if (!g.toast_text.empty() && GetTickCount64() < g.toast_until) {
        InvalidateRect(window, nullptr, FALSE);
      }
      auto step_anim = [&](float& value, bool target) {
        const float goal = target ? 1.0f : 0.0f;
        if (std::fabs(value - goal) <= 0.001f) return;
        value += (goal - value) * 0.18f;
        if (std::fabs(goal - value) < 0.01f) value = goal;
        InvalidateRect(window, nullptr, FALSE);
      };
      const bool on_session = g.tab == 1 || g.immersive;
      step_anim(g.settings_anim, g.settings_open);
      step_anim(g.chat_anim, g.chat_open && on_session);
      step_anim(g.files_anim, g.files_open && on_session);
      if (g.connecting) InvalidateRect(window, nullptr, FALSE);
      if (g.click_flash_until != 0) {
        if (now < g.click_flash_until) {
          InvalidateRect(window, nullptr, FALSE);
        } else {
          g.click_flash_until = 0;
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      return 0;
    }
    case WM_SIZE: {
      // A minimized window reports an icon size; resizing the swap chain to it
      // leaves D2D with a stale back buffer (Wine logs "holding back buffer").
      // Keep the restore size until the window is visible again.
      if (IsIconic(window)) return 0;
      RECT client{};
      GetClientRect(window, &client);
      const UINT32 width = static_cast<UINT32>((std::max)(0L, client.right - client.left));
      const UINT32 height = static_cast<UINT32>((std::max)(0L, client.bottom - client.top));
      if (width == 0 || height == 0) return 0;
      const float dpi = ui_dpi_for(window);
      if (g.target != nullptr) {
        const D2D1_SIZE_U pixel = g.target->GetPixelSize();
        if (pixel.width != width || pixel.height != height) {
          g.target->Resize(D2D1::SizeU(width, height));
        }
        if (std::fabs(dpi - g_dpi) > 0.5f || pixel.width == 0) {
          g.target->SetDpi(dpi, dpi);
        }
      }
      g_dpi = dpi;
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
      const float x = static_cast<float>(LOWORD(lparam)) * 96.0f / g_dpi;
      const float y = static_cast<float>(HIWORD(lparam)) * 96.0f / g_dpi;
      if (!g.tracking_leave) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        if (TrackMouseEvent(&track)) g.tracking_leave = true;
      }
      if (g.text_selecting && (g.focus_field == 1 || g.focus_field == 2 ||
                               g.focus_field == 3)) {
        size_t* caret = focus_caret(g.focus_field);
        const size_t index = caret_for_field(g.focus_field, x);
        if (caret != nullptr) *caret = index;
        if (index != g.sel_anchor) {
          g.sel_field = g.focus_field;
          g.sel_start = (std::min)(g.sel_anchor, index);
          g.sel_end = (std::max)(g.sel_anchor, index);
        } else {
          g.sel_field = -1;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.dragging_ball) {
        // A drag moves the ball; a press without movement opens the bar,
        // exactly like the web's pointer handler.
        if (std::hypot(x - g.drag_start_x, y - g.drag_start_y) > 5.0f) {
          g.ball_moved = true;
        }
        if (g.ball_moved) {
          g.ball_x = clampf(x - g.drag_dx, g.stage.left + 4,
                            g.stage.right - 40);
          g.ball_y = clampf(y - g.drag_dy, g.stage.top + 4,
                            g.stage.bottom - 40);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.dragging_bar) {
        if (std::hypot(x - g.drag_start_x, y - g.drag_start_y) > 4.0f) {
          g.bar_moved = true;
        }
        if (g.bar_moved) {
          const float bar_w = g.hit_bar.right - g.hit_bar.left;
          const float bar_h = g.hit_bar.bottom - g.hit_bar.top;
          const float left = clampf(x - g.drag_dx, g.stage.left + 4,
                                    g.stage.right - 4 - bar_w);
          const float top = clampf(y - g.drag_dy, g.stage.top + 4,
                                   g.stage.bottom - 4 - bar_h);
          g.bar_dx = left - g.bar_anchor_x;
          g.bar_dy = top - g.bar_anchor_y;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.dragging_volume) {
        const float left = g.hit_volume.left + 4;
        const float right = g.hit_volume.right - 4;
        g.audio_volume = clampf((x - left) / (right - left), 0.0f, 1.0f);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.dragging_slider >= 0) {
        const D2D1_RECT_F track = g.qp_slider[g.dragging_slider];
        const float fraction = clampf(
            (x - track.left) / (track.right - track.left), 0.0f, 1.0f);
        if (g.dragging_slider == 0) {
          g.qp = static_cast<int>((1.0f - fraction) * 63.0f + 0.5f);
        } else if (g.dragging_slider == 1) {
          g.bitrate_kbps = static_cast<int>(fraction * 20000.0f + 0.5f);
        } else {
          g.fps_value = 1 + static_cast<int>(fraction * 59.0f + 0.5f);
          g.fps_index = -1;
        }
        send_quality();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (remote_input_active() && inside(g.picture_rect, x, y) &&
          !inside(g.hit_bar, x, y) &&
          !(g.chat_open && inside(g.hit_chat_panel, x, y)) &&
          !(g.files_open && inside(g.hit_files_panel, x, y))) {
        send_remote_move(x, y);
      }
      g.hover_x = x;
      g.hover_y = y;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSELEAVE:
      g.tracking_leave = false;
      g.hover_x = g.hover_y = -1;
      // A tooltip must not outlive the pointer leaving the window.
      g.tip_candidate = -1;
      g.tip_id = -1;
      g.tip_shown = false;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDBLCLK: {
      const float x = static_cast<float>(LOWORD(lparam)) * 96.0f / g_dpi;
      const float y = static_cast<float>(HIWORD(lparam)) * 96.0f / g_dpi;
      int field = 0;
      if (g.tab == 0 && !g.settings_open && !g.close_prompt) {
        if (inside(g.hit_connect_input_home, x, y)) {
          field = 1;
        } else if (inside(g.hit_connect_code_home, x, y)) {
          field = 2;
        }
      } else if ((g.tab == 1 || g.immersive) && !g.settings_open &&
                 !g.close_prompt) {
        if (g.chat_open && inside(g.hit_chat_input, x, y)) {
          field = 3;
        } else if (inside(g.hit_retry_input, x, y)) {
          field = 11;
        }
      }
      if (field != 0) {
        std::wstring* target = focus_text(field);
        size_t* caret = focus_caret(field);
        g.focus_field = field;
        g.ctx_field = field;
        ctx_select_all();
        if (target != nullptr && caret != nullptr) *caret = target->size();
        g.ctx_field = 0;
        g.text_selecting = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      // A double-click over the remote picture arrives as WM_LBUTTONDBLCLK
      // instead of a second WM_LBUTTONDOWN: without forwarding a press here
      // the host only sees one click and double-clicks never register.
      if ((g.tab == 1 || g.immersive) && !g.settings_open &&
          !g.close_prompt && remote_input_active() &&
          inside(g.picture_rect, x, y) && !inside(g.hit_bar, x, y) &&
          !inside(g.hit_ball, x, y) &&
          !(g.chat_open && inside(g.hit_chat_panel, x, y)) &&
          !(g.files_open && inside(g.hit_files_panel, x, y))) {
        send_remote_move(x, y);
        send_remote_button(1, true);
        g.remote_buttons |= 1 << 1;
        g.remote_mouse_down = true;
        if (g.window != nullptr) SetCapture(g.window);
        if (!g.pinned) g.bar_open = false;
        return 0;
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const float x = static_cast<float>(LOWORD(lparam)) * 96.0f / g_dpi;
      const float y = static_cast<float>(HIWORD(lparam)) * 96.0f / g_dpi;
      SetFocus(window);
      g.click_flash_until = GetTickCount64() + 130;
      if (g.ctx_field != 0) {
        const bool on_panel = inside(g.ctx_panel, x, y);
        int picked = -1;
        for (int i = 0; i < 4; ++i) {
          if (inside(g.hit_ctx[i], x, y)) {
            picked = i;
            break;
          }
        }
        const int field = g.ctx_field;
        if (picked >= 0) {
          const bool allowed = picked == 0   ? g.ctx_cut
                               : picked == 1 ? g.ctx_copy
                               : picked == 2 ? g.ctx_paste
                                             : true;
          if (allowed) {
            g.ctx_field = field;
            ctx_apply(picked);
          }
          close_ctx_menu();
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        close_ctx_menu();
        InvalidateRect(window, nullptr, FALSE);
        if (on_panel) return 0;
      }
      if (g.close_prompt) {
        close_prompt_click(x, y);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.rename_open) {
        rename_click(x, y);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.settings_open) {
        settings_click(x, y);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.tab == 1 || g.immersive) {
        if (g.chat_open && inside(g.hit_chat_panel, x, y)) {
          chat_click(x, y);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (g.files_open && inside(g.hit_files_panel, x, y)) {
          files_click(x, y, window);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
      }
      if (!g.immersive && inside(g.hit_win[0], x, y)) {
        ShowWindow(window, SW_MINIMIZE);
        return 0;
      }
      if (!g.immersive && inside(g.hit_win[1], x, y)) {
        if (!g.window_fullscreen) {
          g.saved_placement.length = sizeof(WINDOWPLACEMENT);
          GetWindowPlacement(window, &g.saved_placement);
          MONITORINFO monitor{};
          monitor.cbSize = sizeof(monitor);
          GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                          &monitor);
          SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
          SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left,
                       monitor.rcMonitor.top,
                       monitor.rcMonitor.right - monitor.rcMonitor.left,
                       monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                       SWP_FRAMECHANGED);
          g.window_fullscreen = true;
        } else {
          SetWindowLongPtrW(window, GWL_STYLE,
                            WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX |
                                WS_MAXIMIZEBOX | WS_VISIBLE);
          SetWindowPlacement(window, &g.saved_placement);
          g.window_fullscreen = false;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (!g.immersive && inside(g.hit_win[2], x, y)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
      }
      if (!g.immersive && inside(g.hit_tab[0], x, y)) {
        g.focus_field = 0;
        g.tab = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (!g.immersive && inside(g.hit_tab[1], x, y)) {
        // The web's plus returns to the home tab to start a session there.
        g.tab = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (!g.immersive && g.session_open &&
          inside(g.hit_session_close, x, y)) {
        close_session_tab();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (!g.immersive && g.session_open && inside(g.hit_tab[2], x, y)) {
        g.tab = 1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (!g.immersive && inside(g.hit_theme_sq, x, y)) {
        // Cycle 自动 -> 亮色 -> 暗色.
        g.mode = g.mode == app_state::theme_mode::automatic
                     ? app_state::theme_mode::light
                     : (g.mode == app_state::theme_mode::light
                            ? app_state::theme_mode::dark
                            : app_state::theme_mode::automatic);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (!g.immersive && inside(g.hit_settings, x, y)) {
        g.settings_open = true;
        g.open_menu = -1;
        g.quality_open = false;
        g.focus_field = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.tab == 1 || g.immersive) {
        if (g.focus_field == 11 &&
            !inside(g.hit_retry_input, x, y) &&
            !inside(g.hit_retry_btn, x, y)) {
          g.focus_field = 0;
        }
        if (session_banner_click(x, y) || session_click(x, y)) {
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      if (g.tab == 0) {
        if (inside(g.hit_self_avatar, x, y)) {
          g.focus_field = 0;
          if (pick_avatar(window)) InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_self_name, x, y)) {
          g.settings_open = true;
          g.settings_section = 1;
          g.focus_field = 4;
          g.set_caret_dev = g.set_device_name.size();
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_copy_id, x, y)) {
          if (g.device_id.empty()) {
            show_toast(L"ID 尚未就绪：本机核心没有响应");
          } else {
            set_clipboard_text(g.device_id);
            show_toast(L"已复制 ID");
          }
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_code_eye, x, y)) {
          g.code_visible = !g.code_visible;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_code_copy, x, y)) {
          if (g.device_code.empty()) {
            show_toast(L"验证码尚未就绪：本机核心没有响应");
          } else {
            set_clipboard_text(g.device_code);
            show_toast(L"已复制验证码");
          }
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_code_refresh, x, y)) {
          // Rotate the host's real code through the core instead of drawing
          // a random number that no client could ever authenticate with.
          std::wstring error;
          if (refresh_device_code(&error)) {
            show_toast(L"验证码已更换");
          } else {
            show_toast(error.empty() ? L"验证码更换失败" : error);
          }
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_code_single, x, y) ||
            inside(g.hit_code_multi, x, y)) {
          g.auth_multi = inside(g.hit_code_multi, x, y);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (inside(g.hit_more_settings, x, y)) {
          g.settings_open = true;
          g.settings_section = 0;
          g.focus_field = 0;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        for (int i = 0; i < 3; ++i) {
          if (inside(g.hit_mode[i], x, y)) {
            g.intent = i;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
        auto caret_for_x = [](const std::wstring& text, float x0,
                              float click_x, IDWriteTextFormat* format) {
          size_t best = 0;
          float best_distance = 1e9f;
          for (size_t i = 0; i <= text.size(); ++i) {
            const float width = measure_text(format, text.substr(0, i));
            const float distance = std::fabs(x0 + width - click_x);
            if (distance < best_distance) {
              best_distance = distance;
              best = i;
            }
          }
          return best;
        };
        if (inside(g.hit_connect_send, x, y)) {
          start_home_connect();
        } else if (inside(g.hit_connect_input_home, x, y)) {
          g.focus_field = 1;
          g.connect_error.clear();
          g.address_caret = caret_for_x(
              g.address_text, g.hit_connect_input_home.left + 12, x,
              g.mono_input);
          g.sel_anchor = g.address_caret;
          clear_text_selection();
          g.text_selecting = true;
          if (g.window != nullptr) SetCapture(g.window);
        } else if (inside(g.hit_connect_btn, x, y)) {
          start_home_connect();
        } else if (inside(g.hit_connect_code_eye, x, y)) {
          g.connect_code_visible = !g.connect_code_visible;
        } else if (inside(g.hit_connect_code_home, x, y)) {
          g.focus_field = 2;
          g.connect_error.clear();
          const std::wstring shown =
              g.connect_code_visible
                  ? g.code_text
                  : std::wstring(g.code_text.size(), L'•');
          g.code_caret = caret_for_x(
              shown, g.hit_connect_code_home.left + 30, x, g.small_left);
          g.sel_anchor = g.code_caret;
          clear_text_selection();
          g.text_selecting = true;
          if (g.window != nullptr) SetCapture(g.window);
        } else if (inside(g.hit_relay, x, y)) {
          g.force_relay = !g.force_relay;
        } else if (inside(g.hit_notice_update, x, y)) {
          if (g.update_state == 3) {
            open_url(kNativeReleasesUrl);
            show_toast(L"已打开下载页：请选择 moLink-native 资产");
          } else if (g.update_state != 1) {
            start_update_check();
          }
        } else if (inside(g.hit_view_cards, x, y)) {
          g.history_view = 0;
        } else if (inside(g.hit_view_list, x, y)) {
          g.history_view = 1;
        } else if ((g.device_tab == 0 || g.device_tab == 1) &&
                   recent_click(x, y)) {
          // handled by the recent-device card / quick action
        } else {
          bool tab_hit = false;
          for (int i = 0; i < 4; ++i) {
            if (inside(g.hit_dev_tab[i], x, y)) {
              g.device_tab = i;
              tab_hit = true;
              break;
            }
          }
          if (!tab_hit) {
            g.focus_field = 0;
            clear_text_selection();
          }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      return 0;
    }
    case WM_SYSKEYDOWN:
      if (remote_input_active() && wparam != VK_F11) {
        send_remote_key(wparam, true);
        return 0;
      }
      break;
    case WM_SYSKEYUP:
      if (remote_input_active() && wparam != VK_F11) {
        send_remote_key(wparam, false);
        return 0;
      }
      break;
    case WM_KEYUP:
      if (remote_input_active() && wparam != VK_F11) {
        send_remote_key(wparam, false);
        return 0;
      }
      break;
    case WM_CHAR: {
      if (remote_input_active()) return 0;
      const wchar_t ch = static_cast<wchar_t>(wparam);
      if (g.focus_field == 3) {
        if (ch >= 32 && ch != 127 && g.chat_input.size() < 500) {
          g.chat_input.insert(g.chat_input.begin() +
                                  static_cast<long>(g.chat_caret),
                              ch);
          ++g.chat_caret;
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      if (g.focus_field != 0 && ch >= 32 && ch != 127) {
        std::wstring* target = focus_text(g.focus_field);
        size_t* caret = focus_caret(g.focus_field);
        const bool numeric = g.focus_field == 5 || g.focus_field == 6;
        const size_t limit =
            numeric ? 5 : (g.focus_field == 8 ? 200 : 64);
        if (target != nullptr && caret != nullptr &&
            (!numeric || (ch >= L'0' && ch <= L'9'))) {
          if (g.sel_field == g.focus_field && g.sel_start != g.sel_end) {
            const size_t first = (std::min)(g.sel_start, g.sel_end);
            const size_t last = (std::max)(g.sel_start, g.sel_end);
            target->erase(first, last - first);
            *caret = first;
            clear_text_selection();
          }
          if (target->size() < limit) {
            target->insert(target->begin() + static_cast<long>(*caret), ch);
            ++*caret;
          }
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      break;
    }
    case WM_KEYDOWN: {
      if (g.rename_open) {
        if (wparam == VK_RETURN) {
          apply_rename();
        } else if (wparam == VK_ESCAPE) {
          close_rename_dialog();
        } else if (wparam == VK_BACK) {
          if (g.rename_caret > 0) {
            g.rename_text.erase(g.rename_text.begin() +
                                static_cast<long>(g.rename_caret - 1));
            --g.rename_caret;
          }
          InvalidateRect(window, nullptr, FALSE);
        } else if (wparam == VK_LEFT) {
          if (g.rename_caret > 0) --g.rename_caret;
        } else if (wparam == VK_RIGHT) {
          if (g.rename_caret < g.rename_text.size()) ++g.rename_caret;
        } else if (wparam == VK_HOME) {
          g.rename_caret = 0;
        } else if (wparam == VK_END) {
          g.rename_caret = g.rename_text.size();
        }
        return 0;
      }
      if (g.close_prompt) {
        if (wparam == VK_ESCAPE) {
          g.close_prompt = false;
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      if (g.ctx_field != 0 && wparam == VK_ESCAPE) {
        close_ctx_menu();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if ((g.focus_field == 1 || g.focus_field == 2) &&
          wparam == VK_RETURN) {
        start_home_connect();
        return 0;
      }
      if (g.focus_field == 11 && wparam == VK_RETURN) {
        retry_session_connect();
        return 0;
      }
      if (remote_input_active() && wparam != VK_F11) {
        send_remote_key(wparam, true);
        return 0;
      }
      // Esc unwinds one layer at a time, like the web: menu, then toolbar,
      // then immersive mode. F11 toggles immersive directly.
      if (wparam == VK_ESCAPE) {
        if (g.settings_open) {
          g.settings_open = false;
          g.focus_field = 0;
        } else if (g.open_menu >= 0) {
          g.open_menu = -1;
        } else if (g.bar_open) {
          g.bar_open = false;
          g.quality_open = false;
          g.open_menu = -1;
        } else if (g.immersive) {
          g.immersive = false;
        } else if (g.focus_field != 0) {
          g.focus_field = 0;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (wparam == VK_F11) {
        g.immersive = !g.immersive;
        g.bar_dx = g.bar_dy = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.focus_field == 3) {
        if (wparam == VK_BACK) {
          if (g.chat_caret > 0) {
            g.chat_input.erase(g.chat_input.begin() +
                                    static_cast<long>(g.chat_caret - 1));
            --g.chat_caret;
          }
        } else if (wparam == VK_LEFT) {
          if (g.chat_caret > 0) --g.chat_caret;
        } else if (wparam == VK_RIGHT) {
          if (g.chat_caret < g.chat_input.size()) ++g.chat_caret;
        } else if (wparam == VK_HOME) {
          g.chat_caret = 0;
        } else if (wparam == VK_END) {
          g.chat_caret = g.chat_input.size();
        } else if (wparam == VK_RETURN) {
          send_chat();
        } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                   (wparam == 'A' || wparam == 'a')) {
          g.sel_field = 3;
          g.sel_anchor = 0;
          g.sel_start = 0;
          g.sel_end = g.chat_input.size();
          g.chat_caret = g.chat_input.size();
        } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                   (wparam == 'C' || wparam == 'c' || wparam == 'X' ||
                    wparam == 'x')) {
          if (g.sel_field == 3 && g.sel_start != g.sel_end) {
            const size_t first = (std::min)(g.sel_start, g.sel_end);
            const size_t last = (std::max)(g.sel_start, g.sel_end);
            set_clipboard_text(g.chat_input.substr(first, last - first));
            if (wparam == 'X' || wparam == 'x') {
              g.chat_input.erase(first, last - first);
              g.chat_caret = first;
              clear_text_selection();
            }
          }
        } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                   (wparam == 'V' || wparam == 'v')) {
          if (OpenClipboard(window)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (data != nullptr) {
              const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
              if (text != nullptr) {
                for (const wchar_t* c = text;
                     *c != L'\0' && g.chat_input.size() < 500; ++c) {
                  if (*c >= 32 && *c != 127) {
                    g.chat_input.insert(
                        g.chat_input.begin() +
                            static_cast<long>(g.chat_caret),
                        *c);
                    ++g.chat_caret;
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
      if (g.focus_field == 0) break;
      {
        std::wstring* target_ptr = focus_text(g.focus_field);
        size_t* caret_ptr = focus_caret(g.focus_field);
        if (target_ptr == nullptr || caret_ptr == nullptr) break;
        std::wstring& target = *target_ptr;
        size_t& caret = *caret_ptr;
        const bool numeric = g.focus_field == 5 || g.focus_field == 6;
        const size_t limit =
            numeric ? 5 : (g.focus_field == 8 ? 200 : 64);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        size_t sel_a = 0;
        size_t sel_b = 0;
        const bool selected = text_selection(target_ptr, &sel_a, &sel_b);
        auto erase_selection = [&]() {
          if (!selected) return;
          target.erase(sel_a, sel_b - sel_a);
          caret = sel_a;
          clear_text_selection();
        };
        if (wparam == VK_BACK) {
          if (selected) {
            erase_selection();
          } else if (caret > 0) {
            target.erase(target.begin() + static_cast<long>(caret - 1));
            --caret;
          }
        } else if (wparam == VK_DELETE) {
          if (selected) {
            erase_selection();
          } else if (caret < target.size()) {
            target.erase(target.begin() + static_cast<long>(caret));
          }
        } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
          const bool left = wparam == VK_LEFT;
          if (selected && !shift) {
            caret = left ? sel_a : sel_b;
            clear_text_selection();
          } else if (shift) {
            if (!selected) {
              g.sel_field = g.focus_field;
              g.sel_anchor = caret;
            }
            if (left) {
              if (caret > 0) --caret;
            } else if (caret < target.size()) {
              ++caret;
            }
            g.sel_start = (std::min)(g.sel_anchor, caret);
            g.sel_end = (std::max)(g.sel_anchor, caret);
          } else {
            if (left) {
              if (caret > 0) --caret;
            } else if (caret < target.size()) {
              ++caret;
            }
            clear_text_selection();
          }
        } else if (wparam == VK_HOME || wparam == VK_END) {
          const size_t destination = wparam == VK_HOME ? 0 : target.size();
          if (shift) {
            if (!selected) {
              g.sel_field = g.focus_field;
              g.sel_anchor = caret;
            }
            caret = destination;
            g.sel_start = (std::min)(g.sel_anchor, caret);
            g.sel_end = (std::max)(g.sel_anchor, caret);
          } else {
            caret = destination;
            clear_text_selection();
          }
        } else if (wparam == VK_ESCAPE) {
          g.focus_field = 0;
          clear_text_selection();
        } else if (ctrl && (wparam == 'A' || wparam == 'a')) {
          g.sel_field = g.focus_field;
          g.sel_anchor = 0;
          g.sel_start = 0;
          g.sel_end = target.size();
          caret = target.size();
        } else if (ctrl && (wparam == 'C' || wparam == 'c' ||
                            wparam == 'X' || wparam == 'x')) {
          if (selected) {
            set_clipboard_text(target.substr(sel_a, sel_b - sel_a));
          }
          if (wparam == 'X' || wparam == 'x') erase_selection();
        } else if (ctrl && (wparam == 'V' || wparam == 'v')) {
          erase_selection();
          if (OpenClipboard(window)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (data != nullptr) {
              const auto* text =
                  static_cast<const wchar_t*>(GlobalLock(data));
              if (text != nullptr) {
                for (const wchar_t* c = text;
                     *c != L'\0' && target.size() < limit; ++c) {
                  const bool ok =
                      *c >= 32 && *c != 127 &&
                      (!numeric || (*c >= L'0' && *c <= L'9'));
                  if (ok) {
                    target.insert(target.begin() + static_cast<long>(caret),
                                  *c);
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
    }
    case WM_LBUTTONUP:
      if (g.text_selecting) {
        g.text_selecting = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.remote_mouse_down) {
        send_remote_button(1, false);
        g.remote_buttons &= ~(1 << 1);
        g.remote_mouse_down = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (g.dragging_ball) {
        const bool moved = g.ball_moved;
        g.dragging_ball = false;
        g.ball_moved = false;
        ReleaseCapture();
        // A click (no meaningful movement) opens the bar.
        if (!moved) g.bar_open = true;
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g.dragging_bar) {
        g.dragging_bar = false;
        g.bar_moved = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g.dragging_volume) {
        g.dragging_volume = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g.dragging_slider >= 0) {
        g.dragging_slider = -1;
        ReleaseCapture();
        send_quality();
        InvalidateRect(window, nullptr, FALSE);
      }
      if (g.connect_pressed) {
        g.connect_pressed = false;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_APP_START_CONNECT:
      start_home_connect();
      return 0;
    case WM_APP_UPDATE_RESULT: {
      auto* result = reinterpret_cast<update_result*>(lparam);
      if (result == nullptr) return 0;
      if (result->state == 3) {
        g.update_state = 3;
        g.update_latest = result->latest;
        show_toast(L"发现新版本 " + result->latest);
      } else if (result->state == 2) {
        g.update_state = 2;
        g.update_latest = result->latest;
        show_toast(L"已是最新版本");
      } else {
        g.update_state = 0;
        show_toast(L"检查更新失败，请检查网络后重试");
      }
      delete result;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_APP_SURFACE_READY: {
      auto* path = reinterpret_cast<std::wstring*>(lparam);
      if (path != nullptr) {
        if (g.surface_base == nullptr && surface_open(*path)) {
          show_toast(L"本机核心画面已连接");
        }
        g.surface_path = *path;
        delete path;
      }
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_APP_CONNECT_DONE: {
      auto* result = reinterpret_cast<connect_result*>(lparam);
      if (result == nullptr) return 0;
      if (result->generation != g.connect_generation.load()) {
        // The tab was closed (or another connect started) meanwhile.
        delete result->client;
        delete result;
        return 0;
      }
      g.connecting = false;
      if (wparam != 0 && result->client != nullptr) {
        const bool was_reconnecting = g.reconnecting;
        g.session.reset(result->client);
        configure_session(g.session.get());
        g.session_was_live = false;
        g.retry_attempt = 0;
        g.retry_at = 0;
        g.tab = 1;
        g.session_open = true;
        g.focus_field = 0;
        g.connect_error.clear();
        if (was_reconnecting) {
          // Socket is back but hello has not been answered yet; the toast
          // waits for `welcome` so "已重新连接" means the session really is.
          delete result;
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        g.reconnecting = false;
        show_toast(L"已连接 " + g.session_label);
      } else if (g.reconnecting) {
        // A transient failure while reconnecting: stay quiet and try again.
        delete result->client;
        ++g.retry_attempt;
        const ULONGLONG delay = g.retry_attempt <= 2 ? 1000 : 10000;
        g.retry_at = GetTickCount64() + delay;
      } else {
        delete result->client;
        g.connect_error =
            result->error.empty()
                ? L"无法建立连接，请检查 IP:端口、验证码和主机核心"
                : result->error;
        show_toast(g.connect_error);
      }
      delete result;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wparam);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        wchar_t path[1024] = {};
        if (DragQueryFileW(drop, i, path, 1024) > 0) add_transfer(path);
      }
      DragFinish(drop);
      g.files_open = true;
      g.files_tab = 0;
      g.chat_open = false;
      g.focus_field = 0;
      g.open_menu = -1;
      g.quality_open = false;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == 1) {
        show_main_window();
        return 0;
      }
      if (LOWORD(wparam) == 2) {
        g.quitting = true;
        DestroyWindow(window);
        return 0;
      }
      break;
    case WM_APP_TRAY:
      if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK ||
          lparam == WM_LBUTTONDOWN) {
        show_main_window();
      } else if (lparam == WM_RBUTTONUP) {
        show_tray_menu();
      }
      return 0;
    case WM_DESTROY:
      remove_tray_icon();
      if (g.tray_icon != nullptr) {
        DestroyIcon(g.tray_icon);
        g.tray_icon = nullptr;
      }
      if (g.local_core_process != nullptr) {
        TerminateProcess(g.local_core_process, 0);
        WaitForSingleObject(g.local_core_process, 2000);
        CloseHandle(g.local_core_process);
        g.local_core_process = nullptr;
      }
      if (g.instance_mutex != nullptr) {
        CloseHandle(g.instance_mutex);
        g.instance_mutex = nullptr;
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Same AppUserModelID as the WebView2 shell: without it the taskbar button
  // is not tied to the exe's large icon and Windows renders a scaled-down
  // frame next to the browser build.
  SetCurrentProcessExplicitAppUserModelID(L"com.molink.desktop");
  enable_dpi_awareness();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // Same startup as the web: a pinned toolbar opens expanded, otherwise the
  // session starts with the floating ball.
  g.pinned = load_pinned();
  g.bar_open = g.pinned;
  g.mode = static_cast<app_state::theme_mode>(load_theme_mode());
  g.close_behavior = load_close_behavior();
  // The security/video settings are the host's, not the shell's: read them
  // from the local core instead of showing hard-coded defaults. A launcher
  // that talks to an already-running core passes its ready-line token here.
  if (const char* api_token = std::getenv("MOLINK_API_TOKEN");
      api_token != nullptr && *api_token != '\0') {
    g.local_api_token = api_token;
  }
  // A launcher that already started a core (MOLINK_CORE_PORT) must also pass
  // the ready line's client token, or the shell's identity reads are gated.
  if (const char* client_token = std::getenv("MOLINK_CLIENT_TOKEN");
      client_token != nullptr && *client_token != '\0') {
    g.local_core_token = client_token;
  }
  bool standalone = false;
  for (int i = 1; i < __argc; ++i) {
    if (wcscmp(__wargv[i], L"--rename-test") == 0) {
      g.rename_test = true;
      continue;
    }
    if (wcscmp(__wargv[i], L"--session") == 0 && i + 1 < __argc) {
      const std::wstring value = __wargv[++i];
      const size_t colon = value.find(L':');
      g.session_host = std::string(value.begin(),
                                  colon == std::wstring::npos ? value.end()
                                                              : value.begin() + colon);
      g.session_port = colon == std::wstring::npos
                           ? 0
                           : _wtoi(value.c_str() + colon + 1);
    }
    if (wcscmp(__wargv[i], L"--code") == 0 && i + 1 < __argc) {
      const std::wstring value = __wargv[++i];
      g.session_code.assign(value.begin(), value.end());
    }
    if (wcscmp(__wargv[i], L"--bar") == 0 && i + 1 < __argc) {
      const wchar_t* mode = __wargv[++i];
      g.tab = 1;  // the bar states live on the session page
      if (wcscmp(mode, L"collapsed") == 0 || wcscmp(mode, L"ball") == 0) {
        g.bar_open = false;
      } else if (wcscmp(mode, L"expanded") == 0) {
        g.bar_open = true;
      } else if (wcscmp(mode, L"pinned") == 0) {
        g.bar_open = true;
        g.pinned = true;
      }
    }
    if (wcscmp(__wargv[i], L"--immersive") == 0) g.immersive = true;
    if (wcscmp(__wargv[i], L"--standalone") == 0) standalone = true;
    if (wcscmp(__wargv[i], L"--proxy") == 0) g.proxy_mode = true;
    if (wcscmp(__wargv[i], L"--send-file") == 0 && i + 1 < __argc) {
      g.send_after_connect = __wargv[++i];
    }
    if (wcscmp(__wargv[i], L"--core") == 0 && i + 1 < __argc) {
      g.core_path = __wargv[++i];
      g.proxy_mode = true;
    }
    if (wcscmp(__wargv[i], L"--surface") == 0 && i + 1 < __argc) {
      g.surface_path = __wargv[++i];
      g.tab = 1;  // open on the desktop view when a picture is attached
    }
    if (wcscmp(__wargv[i], L"--theme") == 0 && i + 1 < __argc) {
      const wchar_t* value = __wargv[++i];
      g.mode = wcscmp(value, L"light") == 0
                   ? app_state::theme_mode::light
                   : (wcscmp(value, L"dark") == 0
                          ? app_state::theme_mode::dark
                          : app_state::theme_mode::automatic);
    }
  }
  // One shell per session. Two instances would each start a host core from
  // the same configuration directory, so the directory would see the same
  // device identity heartbeating from two ports. --standalone (test builds)
  // skips both the mutex and the focus hand-off, matching the WebView2 shell.
  if (!standalone) {
    g.instance_mutex =
        CreateMutexW(nullptr, FALSE, L"Local\\moLinkShellInstance");
    if (g.instance_mutex != nullptr &&
        GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(g.instance_mutex);
      g.instance_mutex = nullptr;
      for (int attempt = 0; attempt < 50; ++attempt) {
        HWND existing = FindWindowW(L"MoLinkShell", nullptr);
        if (existing == nullptr) {
          existing = FindWindowW(L"moLinkUiKitDemo", nullptr);
        }
        if (existing != nullptr) {
          ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
          SetForegroundWindow(existing);
          break;
        }
        Sleep(100);
      }
      CoUninitialize();
      return 0;
    }
  }

  // The desktop shell owns the host core, as the WebView2 shell does: start
  // it once and keep its ready-line tokens. Without this the native build had
  // no listener and therefore no directory identity at all; the card below
  // now reads the id/code/status that this core actually got.
  const bool external_core =
      std::getenv("MOLINK_CORE_PORT") != nullptr || !g.surface_path.empty();
  if (!external_core) {
    int core_port = 0;
    std::string client_token;
    std::wstring surface;
    std::string core_error;
    if (start_local_core(&core_port, &client_token, &surface, &core_error)) {
      if (!surface.empty()) g.surface_path = surface;
      native_log(L"host core: ready on port " +
                 std::to_wstring(core_port));
    } else {
      native_log(L"host core: " + wide_of(core_error));
    }
  }
  load_host_settings();
  load_device_identity();

  if (g.session_port > 0 || !g.surface_path.empty() || g.tab == 1) {
    g.session_open = true;
  }
  load_recents();
  g.download_dir = default_download_dir();
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  // Double-click in the address/code fields selects the whole value.
  cls.style = CS_DBLCLKS;
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  // The exe's icon (resource id 1) gives the window and the taskbar button the
  // product icon. Load each size at its native resolution: hIconSm = hIcon
  // made Windows scale the 32 px frame down to 16, which is what looked
  // blurry next to the WebView2 shell.
  // Large frame for Alt-Tab / large icons, 32 px for the small one: the
  // shell scales down cleanly, while a 32 px source upscaled at 200 % DPI
  // looked soft next to the browser shell.
  cls.hIcon = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR));
  cls.hIconSm = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
  if (cls.hIcon == nullptr) {
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  }
  if (cls.hIconSm == nullptr) cls.hIconSm = cls.hIcon;
  cls.lpszClassName = L"moLinkUiKitDemo";
  RegisterClassExW(&cls);
  // Default size: the layout is designed at 1280x720 logical; 1280x800 gives
  // the home page breathing room (the old 1120x560 forced the scaled-down
  // layout and felt cramped). Clamped to the work area and centred.
  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  int window_w = 1280;
  int window_h = 800;
  const int work_w = work_area.right - work_area.left;
  const int work_h = work_area.bottom - work_area.top;
  if (work_w > 0 && window_w > work_w - 40) window_w = work_w - 40;
  if (work_h > 0 && window_h > work_h - 40) window_h = work_h - 40;
  const int window_x = work_area.left + (work_w - window_w) / 2;
  const int window_y = work_area.top + (work_h - window_h) / 2;
  HWND window = CreateWindowExW(
      0, cls.lpszClassName, L"moLink \x2014 native UI kit",
      WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE,
      window_x, window_y, window_w, window_h, nullptr, nullptr, instance,
      nullptr);
  if (window == nullptr) return 1;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
