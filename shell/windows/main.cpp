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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <WebView2.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

void fail_box(const wchar_t* message) {
  MessageBoxW(g_window, message, L"moLink", MB_ICONERROR | MB_OK);
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
  const std::string needle = std::string("\"") + key + "\":\"";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  const std::size_t end = json.find('"', pos);
  if (end == std::string::npos) return false;
  out = json.substr(pos, end - pos);
  return true;
}

bool start_core(const std::wstring& core, const std::wstring& web_root, int& port,
                std::string& token, std::wstring& error) {
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

  std::wstring command = L"\"" + core + L"\" --port 0 --print-ready --parent-pid " +
                         std::to_wstring(GetCurrentProcessId());
  if (!web_root.empty()) command += L" --web-root \"" + web_root + L"\"";

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
      !json_string(line, "token", token) || parsed_port <= 0) {
    error = L"molink-core did not report a ready port and token";
    return false;
  }
  port = static_cast<int>(parsed_port);
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
      fail_and_close(
          L"WebView2 runtime not found (install the Edge WebView2 Runtime)");
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

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      if (g_controller != nullptr) {
        RECT bounds;
        GetClientRect(window, &bounds);
        g_controller->put_Bounds(bounds);
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
      stop_core();
      PostQuitMessage(g_exit_code);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int run(HINSTANCE instance) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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
  std::wstring error;
  if (!start_core(core, web_root, port, token, error)) {
    fail_box(error.c_str());
    stop_core();
    return 1;
  }

  g_url = L"http://127.0.0.1:" + std::to_wstring(port) +
          L"/?host=127.0.0.1&port=" + std::to_wstring(port) + L"&token=" +
          widen(token);
  g_origin = L"http://127.0.0.1:" + std::to_wstring(port);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"MoLinkShell";
  window_class.hbrBackground = CreateSolidBrush(RGB(5, 13, 11));
  RegisterClassExW(&window_class);

  g_window = CreateWindowExW(0, L"MoLinkShell", L"moLink", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr,
                             nullptr, instance, nullptr);
  if (g_window == nullptr) {
    fail_box(L"could not create the window");
    stop_core();
    return 1;
  }

  const std::wstring loader = exe_dir() + L"\\WebView2Loader.dll";
  HMODULE loader_module = LoadLibraryW(loader.c_str());
  if (loader_module == nullptr) {
    loader_module = LoadLibraryW(L"WebView2Loader.dll");
  }
  if (loader_module == nullptr) {
    fail_box(L"WebView2Loader.dll not found next to moLink.exe");
    DestroyWindow(g_window);
    return 1;
  }
  using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(
      PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
  auto create_environment = reinterpret_cast<CreateEnvironmentFn>(
      GetProcAddress(loader_module, "CreateCoreWebView2EnvironmentWithOptions"));
  if (create_environment == nullptr) {
    fail_box(L"WebView2Loader.dll does not export the environment factory");
    DestroyWindow(g_window);
    return 1;
  }

  PWSTR local_app_data = nullptr;
  std::wstring user_data = L".";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                     &local_app_data))) {
    user_data = std::wstring(local_app_data) + L"\\moLink\\WebView2";
    CoTaskMemFree(local_app_data);
    std::error_code ec;
    fs::create_directories(user_data, ec);
  }

  const HRESULT created = create_environment(
      nullptr, user_data.c_str(), nullptr, new EnvironmentHandler());
  if (FAILED(created)) {
    fail_box(L"could not create the WebView2 environment");
    DestroyWindow(g_window);
    return 1;
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
  if (g_controller != nullptr) {
    g_controller->Release();
    g_controller = nullptr;
  }
  FreeLibrary(loader_module);
  CoUninitialize();
  return g_exit_code;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv != nullptr) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--smoke-test") == 0) g_smoke = true;
    }
    LocalFree(argv);
  }
  g_instance = instance;
  return run(instance);
}
