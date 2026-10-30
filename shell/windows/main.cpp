// moLink desktop shell for Windows — Win32 + WebView2.
//
// Starts molink-core, reads its ready line, then hosts the UI served by the
// core in an Edge WebView2 control. The WebView2 loader DLL is loaded at
// runtime so the same source builds with MSVC (CI) and MinGW (local checks).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Windows 7 SP1 is still a supported shell target. Keep the import table at
// the Win7 level and resolve newer APIs dynamically (see window_dpi).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>

#include <WebView2.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

/// Native picture window over the core's shared-memory frame surface
/// (surface_window.cpp), defined outside the anonymous namespace.
int run_surface_window(const std::wstring& path);

namespace {

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
ICoreWebView2Controller* g_controller = nullptr;
ICoreWebView2* g_webview = nullptr;
HANDLE g_core_process = nullptr;
DWORD g_core_pid = 0;
bool g_smoke = false;
bool g_smoke_done = false;
int g_exit_code = 0;
bool g_fullscreen = false;
RECT g_restore_rect{};
std::wstring g_url;
std::wstring g_origin;
std::wstring g_url_override;
std::wstring g_core_host;
bool g_core_tls = false;
bool g_url_mode = false;
bool g_quitting = false;
bool g_tray_added = false;
NOTIFYICONDATAW g_tray_data{};
HMENU g_tray_menu = nullptr;
HICON g_app_icon = nullptr;
HANDLE g_instance_mutex = nullptr;

void spawn_self(const std::wstring& uri);
void toggle_fullscreen(HWND window);
void start_update(const std::wstring& url);

void fail_box(const wchar_t* message) {
  MessageBoxW(g_window, message, L"moLink", MB_ICONERROR | MB_OK);
}

bool g_browser_fallback = false;

/// WebView2 is unavailable (Windows 7 without the last 109 runtime, a broken
/// installation, or a missing loader). The core is already running, so open
/// the same URL — it carries the client token — in the default browser and
/// keep this window as the backend lifetime indicator instead of exiting.
void browser_fallback(const wchar_t* reason) {
  if (g_browser_fallback) return;
  g_browser_fallback = true;
  std::wstring note =
      L"WebView2 is unavailable. moLink was opened in your default browser.\n"
      L"Closing this window stops the moLink backend.\n\n";
  if (reason != nullptr) {
    note += reason;
    note += L"\n\n";
  }
  note += L"Install WebView2 Runtime 109 (the last version supporting Windows 7) "
          L"and restart, or open this URL in Firefox ESR 115 / Chrome 109:\n";
  note += g_url;
  if (g_window != nullptr) {
    SetWindowTextW(g_window, L"moLink (browser mode)");
  }
  // Open the browser before the modal note so the UI is usable even if
  // nobody dismisses the dialog immediately.
  if (!g_url.empty()) {
    ShellExecuteW(g_window, L"open", g_url.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
  if (g_window != nullptr) {
    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);
  }
  MessageBoxW(g_window, note.c_str(), L"moLink",
              MB_ICONINFORMATION | MB_OK);
}

// GetDpiForWindow is Windows 10 1607+. Resolve it at runtime so the same
// executable loads on Windows 7, where the static import would fail with
// "entry point not found" before WinMain runs. Fall back to the screen DPI.
UINT window_dpi(HWND window) {
  using get_dpi_fn = UINT(WINAPI*)(HWND);
  static const get_dpi_fn get_dpi = reinterpret_cast<get_dpi_fn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get_dpi != nullptr) {
    const UINT dpi = get_dpi(window);
    if (dpi != 0) return dpi;
  }
  HWND dc_window = window;
  HDC dc = GetDC(window);
  if (dc == nullptr) {
    dc_window = nullptr;
    dc = GetDC(nullptr);
  }
  UINT dpi = 96;
  if (dc != nullptr) {
    const int value = GetDeviceCaps(dc, LOGPIXELSX);
    if (value > 0) dpi = static_cast<UINT>(value);
    ReleaseDC(dc_window, dc);
  }
  return dpi;
}

std::wstring exe_dir() {
  wchar_t buffer[MAX_PATH * 4] = {};
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  if (length == 0) return std::wstring();
  fs::path path(buffer);
  return path.parent_path().wstring();
}

std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), needed);
  return out;
}

bool file_exists(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

std::wstring find_core() {
  wchar_t env[MAX_PATH * 4] = {};
  const DWORD env_length = GetEnvironmentVariableW(L"MOLINK_CORE", env,
                                                   static_cast<DWORD>(std::size(env)));
  if (env_length > 0 && file_exists(env)) return env;

  const fs::path dir(exe_dir());
  std::vector<fs::path> candidates;
  if (!dir.empty()) {
    candidates.push_back(dir / "molink-core.exe");
    candidates.push_back(dir / "../lib/molink/molink-core.exe");
    candidates.push_back(dir / "../../../backend/build/molink-core.exe");
  }
  candidates.push_back("backend/build/molink-core.exe");
  candidates.push_back("../backend/build/molink-core.exe");
  for (const fs::path& candidate : candidates) {
    if (file_exists(candidate)) return candidate.lexically_normal().wstring();
  }
  return std::wstring();
}

std::wstring find_web_root() {
  wchar_t env[MAX_PATH * 4] = {};
  const DWORD env_length = GetEnvironmentVariableW(
      L"MOLINK_WEB_ROOT", env, static_cast<DWORD>(std::size(env)));
  if (env_length > 0 && file_exists(fs::path(env) / "index.html")) return env;

  const fs::path dir(exe_dir());
  std::vector<fs::path> candidates;
  if (!dir.empty()) {
    candidates.push_back(dir / "web");
    candidates.push_back(dir / "../web");
    candidates.push_back(dir / "../../../frontend/dist");
  }
  candidates.push_back("frontend/dist");
  candidates.push_back("../frontend/dist");
  for (const fs::path& candidate : candidates) {
    if (file_exists(candidate / "index.html")) {
      return candidate.lexically_normal().wstring();
    }
  }
  return std::wstring();
}

bool json_number(const std::string& json, const char* key, long& out) {
  const std::string needle = std::string("\"") + key + "\":";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  long value = 0;
  std::size_t digits = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    value = value * 10 + (json[pos] - '0');
    ++pos;
    ++digits;
  }
  if (digits == 0) return false;
  out = value;
  return true;
}

bool json_string(const std::string& json, const char* key, std::string& out) {
  const std::string needle = std::string("\"") + key + "\":";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' ||
          json[pos] == '\r')) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;
  const std::size_t end = json.find('"', pos);
  if (end == std::string::npos) return false;
  out = json.substr(pos, end - pos);
  return true;
}

/// The tray lives in the native shell, which cannot read the WebView's
/// localStorage. The page mirrors its language into the core's ui.json; fall
/// back to the Windows UI language when that file is missing or old.
bool tray_uses_chinese() {
  const char* appdata = std::getenv("APPDATA");
  if (appdata != nullptr) {
    std::ifstream in(fs::path(appdata) / "moLink" / "ui.json");
    if (in) {
      const std::string body((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      std::string language;
      if (json_string(body, "language", language) && !language.empty()) {
        return language.rfind("zh", 0) == 0;
      }
    }
  }
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

std::wstring url_host_w(const std::wstring& host) {
  if (host.empty()) return L"127.0.0.1";
  if (host.front() == L'[' && host.back() == L']') return host;
  return host.find(L':') != std::wstring::npos ? L"[" + host + L"]" : host;
}

std::wstring ui_host_w() {
  if (g_core_host.empty() || g_core_host == L"0.0.0.0") return L"127.0.0.1";
  if (g_core_host == L"::") return L"::1";
  return g_core_host;
}

std::string g_surface_path;
/// The test build runs beside an installed instance: skip the single-instance
/// focus and let the core bind a dedicated port.
bool g_standalone = false;
int g_core_port = 0;
/// DevTool/CDP port for the WebView2 environment (0 = off).
int g_debug_port = 0;

bool start_core(const std::wstring& core, const std::wstring& web_root, int& port,
                std::string& token, std::string& client_token, bool& tls,
                std::wstring& error) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) {
    error = L"CreatePipe failed";
    return false;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  startup.hStdInput = nullptr;

  std::wstring command = L"\"" + core +
                         L"\" --print-ready --parent-pid " +
                         std::to_wstring(GetCurrentProcessId());
  if (!web_root.empty()) command += L" --web-root \"" + web_root + L"\"";
  if (!g_core_host.empty()) {
    command += L" --host \"" + g_core_host + L"\"";
  }
  if (g_core_port > 0) {
    command += L" --port " + std::to_wstring(g_core_port);
  }

  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(
      nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
      nullptr, &startup, &process);
  CloseHandle(write_pipe);
  if (!started) {
    CloseHandle(read_pipe);
    error = L"could not start molink-core";
    return false;
  }
  g_core_process = process.hProcess;
  g_core_pid = process.dwProcessId;
  CloseHandle(process.hThread);

  std::string line;
  const ULONGLONG deadline = GetTickCount64() + 10000;
  bool complete = false;
  while (!complete && GetTickCount64() < deadline) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) break;
    if (available > 0) {
      char buffer[4096];
      DWORD got = 0;
      if (!ReadFile(read_pipe, buffer, static_cast<DWORD>(std::size(buffer)),
                    &got, nullptr)) {
        break;
      }
      line.append(buffer, got);
      if (line.find('\n') != std::string::npos) complete = true;
    } else {
      Sleep(100);
    }
  }
  CloseHandle(read_pipe);

  long parsed_port = 0;
  if (!complete || !json_number(line, "port", parsed_port) ||
      !json_string(line, "token", token) ||
      !json_string(line, "client_token", client_token) || parsed_port <= 0) {
    error = L"molink-core did not report a ready port and tokens";
    return false;
  }
  port = static_cast<int>(parsed_port);
  tls = line.find("\"tls\":true") != std::string::npos;
  // Set only when MOLINK_SURFACE asked the core to publish a native picture.
  json_string(line, "surface", g_surface_path);
  return true;
}

void stop_core() {
  if (g_core_process == nullptr) return;
  TerminateProcess(g_core_process, 0);
  WaitForSingleObject(g_core_process, 3000);
  CloseHandle(g_core_process);
  g_core_process = nullptr;
  g_core_pid = 0;
}

void fail_and_close(const wchar_t* message) {
  std::fwprintf(stderr, L"moLink shell: %ls\n", message);
  g_exit_code = 1;
  if (g_smoke) {
    std::printf("SHELL_SMOKE_FAIL %ls\n", message);
    std::fflush(stdout);
    g_smoke_done = true;
  }
  if (g_window != nullptr) PostMessageW(g_window, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------------------
// WebView2 handlers
// ---------------------------------------------------------------------------

class WebMessageHandler final
    : public ICoreWebView2WebMessageReceivedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == IID_ICoreWebView2WebMessageReceivedEventHandler) {
      *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* arguments) override {
    LPWSTR raw = nullptr;
    if (arguments == nullptr ||
        FAILED(arguments->TryGetWebMessageAsString(&raw)) || raw == nullptr) {
      return S_OK;
    }
    const std::wstring action(raw);
    CoTaskMemFree(raw);
    if (action == L"minimize") {
      ShowWindow(g_window, SW_MINIMIZE);
    } else if (action == L"maximize") {
      if (IsZoomed(g_window)) ShowWindow(g_window, SW_RESTORE);
      else ShowWindow(g_window, SW_MAXIMIZE);
    } else if (action == L"fullscreen") {
      toggle_fullscreen(g_window);
    } else if (action == L"fullscreen-on") {
      if (!g_fullscreen) toggle_fullscreen(g_window);
    } else if (action == L"fullscreen-off") {
      if (g_fullscreen) toggle_fullscreen(g_window);
    } else if (action.rfind(L"open-url:", 0) == 0) {
      const std::wstring url = action.substr(9);
      if (!url.empty()) {
        ShellExecuteW(g_window, L"open", url.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
      }
    } else if (action.rfind(L"update-url:", 0) == 0) {
      start_update(action.substr(11));
    } else if (action == L"drag") {
      ReleaseCapture();
      SendMessageW(g_window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    } else if (action == L"resize-top" || action == L"resize-bottom" ||
               action == L"resize-left" || action == L"resize-right" ||
               action == L"resize-tl" || action == L"resize-tr" ||
               action == L"resize-bl" || action == L"resize-br") {
      WPARAM hit = HTLEFT;
      if (action == L"resize-top") hit = HTTOP;
      else if (action == L"resize-bottom") hit = HTBOTTOM;
      else if (action == L"resize-left") hit = HTLEFT;
      else if (action == L"resize-right") hit = HTRIGHT;
      else if (action == L"resize-tl") hit = HTTOPLEFT;
      else if (action == L"resize-tr") hit = HTTOPRIGHT;
      else if (action == L"resize-bl") hit = HTBOTTOMLEFT;
      else if (action == L"resize-br") hit = HTBOTTOMRIGHT;
      ReleaseCapture();
      SendMessageW(g_window, WM_NCLBUTTONDOWN, hit, 0);
    } else if (action == L"close") {
      PostMessageW(g_window, WM_CLOSE, 0, 0);
    } else if (action == L"quit") {
      PostQuitMessage(0);
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

class NewWindowHandler final
    : public ICoreWebView2NewWindowRequestedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == IID_ICoreWebView2NewWindowRequestedEventHandler) {
      *ppv = static_cast<ICoreWebView2NewWindowRequestedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* arguments) override {
    if (arguments == nullptr) return S_OK;
    LPWSTR uri = nullptr;
    if (SUCCEEDED(arguments->get_Uri(&uri)) && uri != nullptr) {
      arguments->put_Handled(TRUE);
      spawn_self(std::wstring(uri));
      CoTaskMemFree(uri);
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

class ControllerHandler;

class NavigationHandler final
    : public ICoreWebView2NavigationCompletedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == IID_ICoreWebView2NavigationCompletedEventHandler) {
      *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2*,
      ICoreWebView2NavigationCompletedEventArgs* arguments) override {
    BOOL success = FALSE;
    if (arguments != nullptr) arguments->get_IsSuccess(&success);
    if (g_smoke) {
      if (success) {
        std::printf("SHELL_SMOKE_OK title=moLink\n");
      } else {
        std::printf("SHELL_SMOKE_FAIL navigation\n");
        g_exit_code = 1;
      }
      std::fflush(stdout);
      g_smoke_done = true;
    }
    if (!success) fail_and_close(L"the UI did not load");
    if (g_smoke) PostMessageW(g_window, WM_CLOSE, 0, 0);
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

/// Trusts the shell's own self-signed HTTPS certificate. Only the origin this
/// shell loaded is allowed; every other certificate error stays fatal.
class CertificateHandler final
    : public ICoreWebView2ServerCertificateErrorDetectedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid ==
            IID_ICoreWebView2ServerCertificateErrorDetectedEventHandler) {
      *ppv = static_cast<ICoreWebView2ServerCertificateErrorDetectedEventHandler*>(
          this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2*,
      ICoreWebView2ServerCertificateErrorDetectedEventArgs* args) override {
    if (args == nullptr) return S_OK;
    LPWSTR raw = nullptr;
    args->get_RequestUri(&raw);
    bool allow = false;
    if (raw != nullptr) {
      const std::wstring uri(raw);
      allow = !g_origin.empty() && uri.rfind(g_origin, 0) == 0;
      CoTaskMemFree(raw);
    }
    if (allow) {
      args->put_Action(
          COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW);
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

/// Grants the microphone to the locally served UI so voice calls work in the
/// Windows shell. Nothing else is auto-granted.
class PermissionHandler final
    : public ICoreWebView2PermissionRequestedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == IID_ICoreWebView2PermissionRequestedEventHandler) {
      *ppv =
          static_cast<ICoreWebView2PermissionRequestedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }

  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2*,
      ICoreWebView2PermissionRequestedEventArgs* args) override {
    if (args == nullptr) return S_OK;
    COREWEBVIEW2_PERMISSION_KIND kind =
        COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
    args->get_PermissionKind(&kind);
    if (kind != COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) return S_OK;
    LPWSTR raw = nullptr;
    args->get_Uri(&raw);
    bool local = false;
    if (raw != nullptr) {
      const std::wstring uri(raw);
      // Allow the exact origin this shell loaded, not just loopback: a core
      // bound to a LAN address is still the local UI and needs the mic.
      local = (!g_origin.empty() && uri.rfind(g_origin, 0) == 0) ||
              uri.rfind(L"http://127.0.0.1", 0) == 0 ||
              uri.rfind(L"http://localhost", 0) == 0;
      CoTaskMemFree(raw);
    }
    if (local) {
      args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

class ControllerHandler final
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid ==
            IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
      *ppv =
          static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(
              this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }

  HRESULT STDMETHODCALLTYPE Invoke(
      HRESULT result, ICoreWebView2Controller* controller) override {
    if (FAILED(result) || controller == nullptr) {
      fail_and_close(L"WebView2 controller creation failed");
      return result;
    }
    g_controller = controller;
    g_controller->AddRef();
    controller->get_CoreWebView2(&g_webview);

    RECT bounds;
    GetClientRect(g_window, &bounds);
    controller->put_Bounds(bounds);

    if (g_webview != nullptr) {
      ICoreWebView2Settings* settings = nullptr;
      if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings != nullptr) {
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->Release();
      }
      EventRegistrationToken token{};
      g_webview->add_NavigationCompleted(new NavigationHandler(), &token);
      g_webview->add_WebMessageReceived(new WebMessageHandler(), &token);
      g_webview->add_NewWindowRequested(new NewWindowHandler(), &token);
      g_webview->add_PermissionRequested(new PermissionHandler(), &token);
      // The certificate event landed on ICoreWebView2_14; older runtimes
      // (notably WebView2 109 on Windows 7) do not implement it, in which
      // case the shell simply cannot auto-trust a self-signed HTTPS UI.
      ICoreWebView2_14* modern = nullptr;
      if (SUCCEEDED(g_webview->QueryInterface(
              IID_ICoreWebView2_14, reinterpret_cast<void**>(&modern))) &&
          modern != nullptr) {
        modern->add_ServerCertificateErrorDetected(new CertificateHandler(),
                                                   &token);
        modern->Release();
      }
      g_webview->Navigate(g_url.c_str());
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

class EnvironmentHandler final
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid ==
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
      *ppv =
          static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(
              this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }

  HRESULT STDMETHODCALLTYPE Invoke(
      HRESULT result, ICoreWebView2Environment* environment) override {
    if (FAILED(result) || environment == nullptr) {
      if (g_smoke) {
        fail_and_close(
            L"WebView2 runtime not found (install the Edge WebView2 Runtime)");
        return result;
      }
      browser_fallback(L"WebView2 runtime is unavailable (install WebView2 Runtime 109)");
      return result;
    }
    environment->CreateCoreWebView2Controller(g_window, new ControllerHandler());
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

/// True when this copy came from the Inno installer. The installer writes an
/// uninstall entry with the app id; a portable extraction has none. The page
/// uses it to pick the setup or the portable artifact when updating.
bool installed_copy() {
  static const wchar_t* kKey =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
      L"{B2D9F7D4-6C38-4E7A-9A4B-0B5E8C3F1A72}_is1";
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) ==
      ERROR_SUCCESS) {
    RegCloseKey(key);
    return true;
  }
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, KEY_READ, &key) ==
      ERROR_SUCCESS) {
    RegCloseKey(key);
    return true;
  }
  return false;
}

void toggle_fullscreen(HWND window) {
  if (!g_fullscreen) {
    GetWindowRect(window, &g_restore_rect);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info);
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top,
                 SWP_FRAMECHANGED);
    g_fullscreen = true;
  } else {
    SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(window, nullptr, g_restore_rect.left, g_restore_rect.top,
                 g_restore_rect.right - g_restore_rect.left,
                 g_restore_rect.bottom - g_restore_rect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER);
    g_fullscreen = false;
  }
}


// Downloads the release installer and launches it elevated. The installer
// closes this app through Restart Manager (same session), replaces the files
// and restarts the session service. Failures are silent; the UI always offers
// a manual download as well.
void start_update(const std::wstring& url) {
  if (url.empty()) return;
  std::thread([url] {
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0) return;
    const std::wstring file = std::wstring(temp) + L"molink-update.exe";
    DeleteFileW(file.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), file.c_str(), 0,
                                  nullptr))) {
      return;
    }
    ShellExecuteW(nullptr, L"runas", file.c_str(),
                  L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS=service",
                  nullptr, SW_SHOWNORMAL);
  }).detach();
}

std::wstring exe_path() {
  wchar_t buffer[32768];
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  return length > 0 ? std::wstring(buffer, length) : std::wstring();
}

void spawn_self(const std::wstring& uri) {
  std::wstring command = L"\"" + exe_path() + L"\" --url \"" + uri + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

// The core now binds a fixed TCP port (28418 by default), so a second shell
// cannot start its own core. Instead of failing after the readiness timeout,
// focus the window of the instance that is already running. --url pop-outs
// must bypass this: they are deliberately separate shells.
bool focus_running_instance() {
  g_instance_mutex =
      CreateMutexW(nullptr, FALSE, L"Local\\moLinkShellInstance");
  if (g_instance_mutex == nullptr) return false;
  if (GetLastError() != ERROR_ALREADY_EXISTS) return false;

  HWND existing = nullptr;
  for (int attempt = 0; attempt < 100 && existing == nullptr; ++attempt) {
    existing = FindWindowW(L"MoLinkShell", L"moLink");
    if (existing == nullptr) Sleep(100);
  }
  if (existing != nullptr) {
    ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(existing);
  }
  CloseHandle(g_instance_mutex);
  g_instance_mutex = nullptr;
  return true;
}

HICON load_app_icon() {
  // Prefer the icon embedded in the executable (resource id 1); fall back to
  // the file shipped next to it for unpackaged builds.
  HICON icon = static_cast<HICON>(
      LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0,
                 0, LR_DEFAULTSIZE));
  if (icon == nullptr) {
    const std::wstring path = exe_dir() + L"\\molink.ico";
    icon = static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, 0,
                                        0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
  }
  if (icon == nullptr) icon = LoadIconW(nullptr, IDI_APPLICATION);
  return icon;
}

void add_tray_icon() {
  if (g_tray_added) return;
  if (g_app_icon == nullptr) g_app_icon = load_app_icon();
  ZeroMemory(&g_tray_data, sizeof(g_tray_data));
  g_tray_data.cbSize = sizeof(g_tray_data);
  g_tray_data.hWnd = g_window;
  g_tray_data.uID = 1;
  g_tray_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray_data.uCallbackMessage = WM_APP + 1;
  g_tray_data.hIcon = g_app_icon;
  wcscpy_s(g_tray_data.szTip, L"moLink");
  Shell_NotifyIconW(NIM_ADD, &g_tray_data);
  g_tray_added = true;
}

void remove_tray_icon() {
  if (!g_tray_added) return;
  Shell_NotifyIconW(NIM_DELETE, &g_tray_data);
  g_tray_added = false;
}

void show_main_window() {
  ShowWindow(g_window, SW_SHOW);
  SetForegroundWindow(g_window);
}

void show_tray_menu() {
  if (g_tray_menu != nullptr) DestroyMenu(g_tray_menu);
  g_tray_menu = CreatePopupMenu();
  const bool zh = tray_uses_chinese();
  AppendMenuW(g_tray_menu, MF_STRING, 1,
              zh ? L"\u663e\u793a moLink" : L"Show moLink");
  AppendMenuW(g_tray_menu, MF_STRING, 2, zh ? L"\u9000\u51fa" : L"Quit");
  POINT point;
  GetCursorPos(&point);
  SetForegroundWindow(g_window);
  TrackPopupMenu(g_tray_menu, TPM_RIGHTBUTTON, point.x, point.y, 0, g_window,
                 nullptr);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_NCCALCSIZE:
      if (wparam == TRUE) {
        if (IsZoomed(window)) {
          auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
          MONITORINFO info{};
          info.cbSize = sizeof(info);
          GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                          &info);
          params->rgrc[0] = info.rcWork;
        }
        return 0;
      }
      break;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                      &monitor);
      info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
      info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
      info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
      info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
      return 0;
    }
    case WM_NCHITTEST: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &point);
      RECT client;
      GetClientRect(window, &client);
      const UINT dpi = window_dpi(window);
      const int border = MulDiv(6, dpi, 96);
      const int title = MulDiv(40, dpi, 96);
      const int controls = MulDiv(110, dpi, 96);
      if (point.y < border) {
        if (point.x < border) return HTTOPLEFT;
        if (point.x > client.right - border) return HTTOPRIGHT;
        return HTTOP;
      }
      if (point.y > client.bottom - border) {
        if (point.x < border) return HTBOTTOMLEFT;
        if (point.x > client.right - border) return HTBOTTOMRIGHT;
        return HTBOTTOM;
      }
      if (point.x < border) return HTLEFT;
      if (point.x > client.right - border) return HTRIGHT;
      if (point.y < title && point.x < client.right - controls) {
        return HTCAPTION;
      }
      return HTCLIENT;
    }
    case WM_SIZE: {
      if (g_controller != nullptr) {
        RECT bounds;
        GetClientRect(window, &bounds);
        g_controller->put_Bounds(bounds);
      }
      // Fullscreen is a monitor-sized popup, not a zoomed window, but it must
      // be square just like a maximized one.
      if (g_fullscreen || IsZoomed(window) || IsIconic(window)) {
        SetWindowRgn(window, nullptr, TRUE);
      } else {
        RECT client;
        GetClientRect(window, &client);
        if (client.right > 0 && client.bottom > 0) {
          const int radius = MulDiv(28, window_dpi(window), 96);
          HRGN region = CreateRoundRectRgn(0, 0, client.right + 1,
                                           client.bottom + 1, radius, radius);
          SetWindowRgn(window, region, TRUE);
        }
      }
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == 1) show_main_window();
      else if (LOWORD(wparam) == 2) {
        g_quitting = true;
        DestroyWindow(window);
      }
      return 0;
    case WM_APP + 1:
      if (lparam == WM_LBUTTONDBLCLK) show_main_window();
      else if (lparam == WM_RBUTTONUP) show_tray_menu();
      return 0;
    case WM_CLOSE:
      if (g_url_mode) {
        DestroyWindow(window);
      } else {
        ShowWindow(window, SW_HIDE);
        add_tray_icon();
      }
      return 0;
    case WM_TIMER:
      if (g_smoke && !g_smoke_done) {
        std::printf("SHELL_SMOKE_FAIL timeout\n");
        std::fflush(stdout);
        g_smoke_done = true;
        g_exit_code = 1;
        DestroyWindow(window);
      }
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_F11) {
        toggle_fullscreen(window);
        return 0;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
          (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
          (wparam == 'Q' || wparam == 'q')) {
        DestroyWindow(window);
        return 0;
      }
      break;
    case WM_DESTROY:
      remove_tray_icon();
      stop_core();
      PostQuitMessage(g_exit_code);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/// Minimal ICoreWebView2EnvironmentOptions: the runtime's coclass is not in
/// the C header, so a debugging build carries its own.
class environment_options final : public ICoreWebView2EnvironmentOptions {
 public:
  explicit environment_options(std::wstring arguments)
      : arguments_(std::move(arguments)),
        // The SDK's default (WebView2EnvironmentOptions.h); the runtime
        // rejects an empty target version with E_INVALIDARG.
        version_(L"154.0.4258.31") {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    // MinGW has no __uuidof for the WebView2 interfaces (no __CRT_UUID_DECL),
    // so compare the header's IID symbol against the documented IUnknown GUID.
    static const GUID kUnknown = {0, 0, 0,
                                  {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    if (IsEqualGUID(iid, IID_ICoreWebView2EnvironmentOptions) ||
        IsEqualGUID(iid, kUnknown)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(
      LPWSTR* value) override {
    return copy_string(arguments_, value);
  }
  HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(
      LPCWSTR value) override {
    arguments_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
    return copy_string(language_, value);
  }
  HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
    language_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(
      LPWSTR* value) override {
    return copy_string(version_, value);
  }
  HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(
      LPCWSTR value) override {
    version_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(
      BOOL* allow) override {
    if (allow == nullptr) return E_POINTER;
    *allow = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(
      BOOL) override {
    return S_OK;
  }

 private:
  static HRESULT copy_string(const std::wstring& text, LPWSTR* out) {
    if (out == nullptr) return E_POINTER;
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (*out == nullptr) return E_OUTOFMEMORY;
    std::memcpy(*out, text.c_str(), bytes);
    return S_OK;
  }

  std::atomic<ULONG> references_{1};
  std::wstring arguments_;
  std::wstring language_;
  std::wstring version_;
};

int run(HINSTANCE instance) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (!g_url_mode && !g_smoke && !g_standalone && focus_running_instance()) {
    CoUninitialize();
    return 0;
  }

  if (g_url_mode) {
    g_url = g_url_override;
    g_origin = L"http://127.0.0.1:";
  } else {
  const std::wstring core = find_core();
  if (core.empty()) {
    fail_box(L"molink-core.exe not found. Build backend/ or set MOLINK_CORE.");
    return 1;
  }
  const std::wstring web_root = find_web_root();
  if (web_root.empty()) {
    fail_box(L"frontend/dist not found. Run pnpm build in frontend/.");
    return 1;
  }

  int port = 0;
  std::string token;
  std::string client_token;
  bool tls = false;
  std::wstring error;
  if (!start_core(core, web_root, port, token, client_token, tls, error)) {
    fail_box(error.c_str());
    stop_core();
    return 1;
  }

  // The core publishes to a native surface: open the window that draws it, as
  // a sibling process so the WebView keeps owning the UI.
  if (!g_surface_path.empty()) {
    std::wstring child_command = L"\"" + exe_path() + L"\" --surface \"" +
                                 widen(g_surface_path) + L"\"";
    STARTUPINFOW child_startup{};
    child_startup.cb = sizeof(child_startup);
    PROCESS_INFORMATION child{};
    if (CreateProcessW(nullptr, child_command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &child_startup, &child)) {
      CloseHandle(child.hThread);
      CloseHandle(child.hProcess);
    }
  }

  g_core_tls = tls;
  const std::wstring host = ui_host_w();
  const std::wstring authority = url_host_w(host);
  const std::wstring scheme = g_core_tls ? L"https://" : L"http://";
  g_url = scheme + authority + L":" + std::to_wstring(port) +
          L"/?host=" + host + L"&port=" + std::to_wstring(port) +
          L"&shell=1&platform=windows";
#ifdef _WIN64
  g_url += L"&arch=x64";
#else
  g_url += L"&arch=x86";
#endif
  if (!installed_copy()) g_url += L"&portable=1";
  if (!client_token.empty()) {
    g_url += L"&client=" + widen(client_token);
  }
  g_origin = scheme + authority + L":" + std::to_wstring(port);
  }

  SetCurrentProcessExplicitAppUserModelID(L"com.molink.desktop");
  g_app_icon = load_app_icon();

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = g_app_icon;
  window_class.hIconSm = g_app_icon;
  window_class.lpszClassName = L"MoLinkShell";
  window_class.hbrBackground = CreateSolidBrush(RGB(5, 13, 11));
  RegisterClassExW(&window_class);

  g_window = CreateWindowExW(0, L"MoLinkShell", L"moLink", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr,
                             nullptr, instance, nullptr);
  if (g_window == nullptr) {
    fail_box(L"could not create the window");
    stop_core();
    return 1;
  }
  SendMessageW(g_window, WM_SETICON, ICON_BIG,
               reinterpret_cast<LPARAM>(g_app_icon));
  SendMessageW(g_window, WM_SETICON, ICON_SMALL,
               reinterpret_cast<LPARAM>(g_app_icon));

  const std::wstring loader = exe_dir() + L"\\WebView2Loader.dll";
  HMODULE loader_module = LoadLibraryW(loader.c_str());
  if (loader_module == nullptr) {
    loader_module = LoadLibraryW(L"WebView2Loader.dll");
  }
  using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(
      PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
  CreateEnvironmentFn create_environment = nullptr;
  if (loader_module != nullptr) {
    create_environment = reinterpret_cast<CreateEnvironmentFn>(
        GetProcAddress(loader_module,
                       "CreateCoreWebView2EnvironmentWithOptions"));
  }

  if (loader_module == nullptr) {
    if (g_smoke) {
      fail_box(L"WebView2Loader.dll not found next to moLink.exe");
      DestroyWindow(g_window);
      return 1;
    }
    browser_fallback(L"WebView2Loader.dll is missing");
  } else if (create_environment == nullptr) {
    if (g_smoke) {
      fail_box(L"WebView2Loader.dll does not export the environment factory");
      DestroyWindow(g_window);
      return 1;
    }
    browser_fallback(L"WebView2Loader.dll does not export the environment factory");
  } else {
    PWSTR local_app_data = nullptr;
    std::wstring user_data = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &local_app_data))) {
      user_data = std::wstring(local_app_data) + L"\\moLink\\WebView2";
      CoTaskMemFree(local_app_data);
      std::error_code ec;
      fs::create_directories(user_data, ec);
    }

    // A fixed-version WebView2 runtime can sit next to the exe (the Windows 7
    // offline package). Use it instead of the Evergreen installation.
    std::wstring browser_folder;
    const std::wstring bundled = exe_dir() + L"\\WebView2Runtime";
    if (GetFileAttributesW((bundled + L"\\msedgewebview2.exe").c_str()) !=
        INVALID_FILE_ATTRIBUTES) {
      browser_folder = bundled;
    }

    // A CDP port for debugging: pass it through the environment options so
    // the runtime actually listens (the WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
    // variable is not honoured on every runtime version, and the options
    // coclass is not reachable through the C header).
    environment_options* env_options = nullptr;
    if (g_debug_port > 0) {
      env_options = new environment_options(
          L"--remote-debugging-port=" + std::to_wstring(g_debug_port) +
          L" --remote-allow-origins=*");
      env_options->AddRef();
    }
    std::fprintf(stderr, "shell: debug port=%d env_options=%p\n",
                 g_debug_port, static_cast<void*>(env_options));
    const HRESULT created = create_environment(
        browser_folder.empty() ? nullptr : browser_folder.c_str(),
        user_data.c_str(), env_options, new EnvironmentHandler());
    std::fprintf(stderr, "shell: create_environment hr=0x%08lx\n",
                 static_cast<unsigned long>(created));
    if (env_options != nullptr) env_options->Release();
    if (FAILED(created)) {
      if (g_smoke) {
        fail_box(L"could not create the WebView2 environment");
        DestroyWindow(g_window);
        return 1;
      }
      browser_fallback(L"Could not create the WebView2 environment (install WebView2 Runtime 109)");
    }
  }

  ShowWindow(g_window, SW_SHOW);
  UpdateWindow(g_window);
  if (g_smoke) SetTimer(g_window, 1, 15000, nullptr);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (g_webview != nullptr) {
    g_webview->Release();
    g_webview = nullptr;
  }
  remove_tray_icon();
  if (g_controller != nullptr) {
    g_controller->Release();
    g_controller = nullptr;
  }
  FreeLibrary(loader_module);
  if (g_instance_mutex != nullptr) {
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
  }
  CoUninitialize();
  return g_exit_code;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv != nullptr) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--smoke-test") == 0) {
        g_smoke = true;
      } else if (wcscmp(argv[i], L"--url") == 0 && i + 1 < argc) {
        g_url_override = argv[++i];
      } else if (wcscmp(argv[i], L"--host") == 0 && i + 1 < argc) {
        g_core_host = argv[++i];
      } else if (wcscmp(argv[i], L"--standalone") == 0) {
        g_standalone = true;
      } else if (wcscmp(argv[i], L"--core-port") == 0 && i + 1 < argc) {
        g_core_port = _wtoi(argv[++i]);
      } else if (wcscmp(argv[i], L"--remote-debugging-port") == 0 &&
                 i + 1 < argc) {
        g_debug_port = _wtoi(argv[++i]);
      } else if (wcscmp(argv[i], L"--surface") == 0 && i + 1 < argc) {
        const std::wstring surface_path = argv[++i];
        LocalFree(argv);
        return run_surface_window(surface_path);
      }
    }
    LocalFree(argv);
  }
  g_url_mode = !g_url_override.empty();
  g_instance = instance;
  return run(instance);
}
