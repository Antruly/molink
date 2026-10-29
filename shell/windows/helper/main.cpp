// Experimental secure-desktop helper for Windows.
//
// The LocalSystem service launches this binary in the active console session.
// It switches its thread onto the current input desktop (Winlogon for lock /
// UAC), captures BGRA frames and injects input there. The core is not wired
// to this pipe yet; treat it as experimental until validated on a real
// Windows machine.
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <sddl.h>

#include <atomic>
#include <fstream>
#include <functional>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Each pipe thread must track its own desktop: SetThreadDesktop only affects
// the calling thread, and a shared handle made the input thread believe it had
// already switched when the capture thread changed desktops.
thread_local HDESK g_desktop = nullptr;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-secure";
constexpr wchar_t kInputPipeName[] = L"\\\\.\\pipe\\molink-secure-input";
constexpr wchar_t kLogPath[] =
    L"C:\\ProgramData\\moLink\\service\\helper.log";

void log_line(const std::string& message) {
  std::ofstream out(kLogPath, std::ios::app);
  if (out) out << message << "\n";
}

// GDI virtualization: an unaware process sees a 2560x1440 screen at 150%
// scaling as 1707x960, and BitBlt returns that scaled bitmap. The helper is a
// separate process from molink-core, so the core's DPI setup does not apply
// here: without this call the session reports and streams logical pixels.
// Match molink-core: per-monitor v2, then per-monitor, then Win7 system DPI.
void enable_physical_pixels() {
  using set_dpi_ctx_fn = BOOL(WINAPI*)(void*);
  using set_dpi_awareness_fn = HRESULT(WINAPI*)(int);
  bool ok = false;
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    auto set_ctx = reinterpret_cast<set_dpi_ctx_fn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_ctx != nullptr) {
      // PER_MONITOR_AWARE_V2 = -4
      ok = set_ctx(reinterpret_cast<void*>(-4)) != FALSE;
    }
  }
  if (!ok) {
    // Load shcore explicitly: it is not mapped into every process, so
    // GetModuleHandle would miss it on Windows 8.1.
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
      auto set_awareness = reinterpret_cast<set_dpi_awareness_fn>(
          GetProcAddress(shcore, "SetProcessDpiAwareness"));
      if (set_awareness != nullptr) {
        // PROCESS_PER_MONITOR_DPI_AWARE = 2
        ok = SUCCEEDED(set_awareness(2));
      }
    }
  }
  if (!ok) SetProcessDPIAware();
}

std::string utf8(const wchar_t* text) {
  if (text == nullptr) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::string object_name(HANDLE object) {
  wchar_t name[256] = {};
  DWORD needed = 0;
  if (object == nullptr ||
      !GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name),
                                 &needed)) {
    return "?";
  }
  return utf8(name);
}

bool open_input_desktop(std::string& err) {
  log_line("start: station=" +
           object_name(GetProcessWindowStation()) + " thread-desktop=" +
           object_name(GetThreadDesktop(GetCurrentThreadId())));
  g_desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
  if (g_desktop == nullptr) {
    g_desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS |
                                               DESKTOP_WRITEOBJECTS |
                                               DESKTOP_SWITCHDESKTOP);
  }
  if (g_desktop == nullptr) {
    const DWORD code = GetLastError();
    log_line("OpenInputDesktop failed: " + std::to_string(code));
    err = "OpenInputDesktop failed";
    return false;
  }
  log_line("input desktop=" + object_name(g_desktop));
  if (!SetThreadDesktop(g_desktop)) {
    err = "SetThreadDesktop failed";
    return false;
  }
  return true;
}

bool refresh_input_desktop() {
  // GENERIC_ALL matches what other secure-desktop injectors request; the
  // smaller mask made SetThreadDesktop succeed but SendInput report access
  // denied.
  HDESK current = OpenInputDesktop(0, FALSE, GENERIC_ALL);
  if (current == nullptr) {
    current = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS |
                                            DESKTOP_WRITEOBJECTS |
                                            DESKTOP_SWITCHDESKTOP);
  }
  if (current == nullptr) {
    log_line("OpenInputDesktop failed: " + std::to_string(GetLastError()));
    return false;
  }
  if (g_desktop == nullptr) {
    g_desktop = current;
    return SetThreadDesktop(current) != 0;
  }
  wchar_t old_name[256] = {};
  wchar_t new_name[256] = {};
  DWORD needed = 0;
  GetUserObjectInformationW(g_desktop, UOI_NAME, old_name, sizeof(old_name),
                            &needed);
  GetUserObjectInformationW(current, UOI_NAME, new_name, sizeof(new_name),
                            &needed);
  bool same_name = _wcsicmp(old_name, new_name) == 0;
  if (same_name) {
    // Two different desktops can carry the same name. The lock screen and the
    // logon UI are both "Winlogon": the handle that captures the lock screen
    // shows black once the logon page takes over, while input still works.
    // Re-open the desktop periodically even when the name is unchanged so the
    // DC follows the desktop that is really in front.
    static thread_local ULONGLONG last_forced = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last_forced < 2000) {
      CloseDesktop(current);
      return true;
    }
    last_forced = now;
  }
  if (!SetThreadDesktop(current)) {
    CloseDesktop(current);
    log_line(std::string("SetThreadDesktop failed for ") + utf8(new_name) +
             ": " + std::to_string(GetLastError()));
    return false;
  }
  CloseDesktop(g_desktop);
  g_desktop = current;
  // A periodic re-open of the same-named desktop is routine; only a real
  // desktop change is worth a log line.
  if (!same_name) log_line("desktop -> " + utf8(new_name));
  return true;
}

/// Whether the console session is locked. The logon UI (after the lock screen
/// is dismissed) reports the normal "Default" desktop on some builds, so the
/// desktop name alone cannot tell the secure desktop from the user's own.
bool session_locked() {
  LPWSTR buffer = nullptr;
  DWORD bytes = 0;
  const DWORD session = WTSGetActiveConsoleSessionId();
  if (session == 0xFFFFFFFF) return false;
  if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session,
                                   WTSSessionInfoEx, &buffer, &bytes)) {
    return false;
  }
  bool locked = false;
  const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
  if (info != nullptr && info->Level == 1) {
    // WTS_SESSIONSTATE_LOCK is 0 and UNLOCK is 1.
    locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
  }
  WTSFreeMemory(buffer);
  return locked;
}

std::string desktop_json() {
  wchar_t name[256] = {};
  DWORD needed = 0;
  GetUserObjectInformationW(g_desktop, UOI_NAME, name, sizeof(name), &needed);
  HDC screen = GetDC(nullptr);
  const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSX) : 0;
  if (screen != nullptr) ReleaseDC(nullptr, screen);
  char buffer[512] = {};
  std::snprintf(buffer, sizeof(buffer),
                R"({"ok":true,"desktop":"%s","locked":%d,"width":%d,)"
                R"("height":%d,"dpi":%d})",
                utf8(name).c_str(), session_locked() ? 1 : 0,
                GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                dpi);
  return buffer;
}

// GDI binds the screen DC to the desktop the thread first uses. After a
// Winlogon <-> Default switch the old thread's DC is stale, so capture again
// on a fresh thread that binds to the current input desktop.
/// A remote viewer should never see a black screen because the monitor went to
/// sleep. Throttled: capture runs up to 8 times a second.
void ensure_display_awake() {
  static std::atomic<uint64_t> last{0};
  const uint64_t now = GetTickCount64();
  if (now - last.load() < 5000) return;
  last.store(now);
  SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
                          ES_SYSTEM_REQUIRED);
  SendNotifyMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, -1);
}

bool capture_on_thread(std::vector<uint8_t>& out, int& width, int& height,
                       std::string& err) {
  ensure_display_awake();
  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    err = "GetDC failed";
    return false;
  }
  width = GetDeviceCaps(screen, HORZRES);
  height = GetDeviceCaps(screen, VERTRES);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  if (memory == nullptr || bitmap == nullptr) {
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    ReleaseDC(nullptr, screen);
    err = "bitmap allocation failed";
    return false;
  }
  HGDIOBJ old = SelectObject(memory, bitmap);
  // Plain SRCCOPY: CAPTUREBLT forces DWM to compose for every grab, which
  // makes the host cursor blink and costs latency. DWM's composed desktop is
  // already in the screen DC on current Windows versions.
  bool blitted = BitBlt(memory, 0, 0, width, height, screen, 0, 0,
                        SRCCOPY) != 0;
  if (!blitted) {
    err = "BitBlt failed: " + std::to_string(GetLastError());
  }
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  out.resize(static_cast<std::size_t>(width) * height * 4);
  const int lines = blitted ? GetDIBits(memory, bitmap, 0, height, out.data(),
                                        &info, DIB_RGB_COLORS)
                            : 0;
  SelectObject(memory, old);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (!blitted || lines != height) {
    if (blitted && lines != height) {
      log_line("GetDIBits returned " + std::to_string(lines) + " of " +
               std::to_string(height) + " lines");
    }
    err = "BitBlt/GetDIBits failed";
    return false;
  }
  return true;
}

bool capture(std::vector<uint8_t>& out, int& width, int& height,
             std::string& err) {
  if (capture_on_thread(out, width, height, err)) return true;
  bool ok = false;
  std::string second_error;
  std::thread retry([&] {
    HDESK desktop = OpenInputDesktop(0, FALSE,
                                     DESKTOP_READOBJECTS |
                                         DESKTOP_WRITEOBJECTS |
                                         DESKTOP_SWITCHDESKTOP);
    if (desktop == nullptr) {
      second_error = "OpenInputDesktop failed";
      return;
    }
    if (!SetThreadDesktop(desktop)) {
      second_error = "SetThreadDesktop failed";
      CloseDesktop(desktop);
      return;
    }
    ok = capture_on_thread(out, width, height, second_error);
    CloseDesktop(desktop);
  });
  retry.join();
  if (!ok) {
    err = second_error.empty() ? err : second_error;
    static std::atomic<uint64_t> last_log{0};
    const uint64_t now = GetTickCount64();
    if (now - last_log.exchange(now) > 10000) {
      log_line("capture failed: " + err);
    }
  }
  return ok;
}

void send_key(WORD vk, bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  if (!down) input.ki.dwFlags = KEYEVENTF_KEYUP;
  if (SendInput(1, &input, sizeof(input)) != 1) {
    log_line("SendInput key " + std::to_string(vk) + " failed: " +
             std::to_string(GetLastError()));
  }
}

void send_unicode(const std::wstring& text) {
  for (wchar_t ch : text) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = ch;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
  }
}

std::string handle_line(const std::string& line, std::vector<uint8_t>& frame,
                        int& width, int& height) {
  // Opening the input desktop for every command (each mouse move included)
  // made the host cursor blink and added latency. The capture command runs
  // continuously, so a desktop switch is still noticed within a few frames.
  // Per-thread: the capture pipe and the input pipe each own their own
  // desktop handle. A shared timestamp let the capture thread's refresh
  // suppress the input thread's, leaving input on Winlogon after unlock.
  static thread_local ULONGLONG last_desktop_refresh = 0;
  const ULONGLONG now = GetTickCount64();
  if (line != "quit" && now - last_desktop_refresh >= 250) {
    last_desktop_refresh = now;
    refresh_input_desktop();
  }
  if (line == "info") return desktop_json();
  if (line == "capture") {
    std::string err;
    if (!capture(frame, width, height, err)) {
      return R"({"ok":false,"error":")" + err + R"("})";
    }
    char header[128] = {};
    std::snprintf(header, sizeof(header), "FRAME %d %d %zu\n", width, height,
                  frame.size());
    std::string reply = header;
    reply.append(reinterpret_cast<const char*>(frame.data()), frame.size());
    return reply;
  }
  if (line.rfind("move ", 0) == 0) {
    int x = 0;
    int y = 0;
    if (std::sscanf(line.c_str(), "move %d %d", &x, &y) == 2) {
      SetCursorPos(x, y);
      return R"({"ok":true})";
    }
    return R"({"ok":false,"error":"bad move"})";
  }
  if (line.rfind("button ", 0) == 0 || line.rfind("key ", 0) == 0 ||
      line.rfind("text ", 0) == 0) {
    log_line("cmd " + line.substr(0, 80));
  }
  if (line.rfind("button ", 0) == 0) {
    int button = 0;
    int down = 1;
    std::sscanf(line.c_str(), "button %d %d", &button, &down);
    DWORD flag = MOUSEEVENTF_LEFTDOWN;
    if (!down) flag = MOUSEEVENTF_LEFTUP;
    if (button == 2) flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    if (button == 3) flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    if (SendInput(1, &input, sizeof(input)) != 1) {
      log_line("SendInput button failed: " + std::to_string(GetLastError()));
    }
    return R"({"ok":true})";
  }
  if (line.rfind("wheel ", 0) == 0) {
    int dx = 0;
    int dy = 0;
    std::sscanf(line.c_str(), "wheel %d %d", &dx, &dy);
    if (dy != 0) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_WHEEL;
      // The wire protocol says dy > 0 scrolls down, while Win32 treats a
      // positive wheel delta as scrolling up. Keep this in step with
      // backend/src/input/windows_input.cpp.
      input.mi.mouseData = static_cast<DWORD>(-dy * WHEEL_DELTA);
      SendInput(1, &input, sizeof(input));
    }
    if (dx != 0) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      input.mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA);
      SendInput(1, &input, sizeof(input));
    }
    return R"({"ok":true})";
  }
  if (line == "release-all") {
    for (WORD vk : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
      INPUT input{};
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = vk;
      input.ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(1, &input, sizeof(input));
    }
    for (DWORD flag : {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP,
                       MOUSEEVENTF_RIGHTUP}) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = flag;
      SendInput(1, &input, sizeof(input));
    }
    return R"({"ok":true})";
  }
  if (line.rfind("key ", 0) == 0) {
    int vk = 0;
    int down = 1;
    std::sscanf(line.c_str(), "key %d %d", &vk, &down);
    send_key(static_cast<WORD>(vk), down != 0);
    return R"({"ok":true})";
  }
  if (line.rfind("text ", 0) == 0) {
    const std::string text = line.substr(5);
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()), wide.data(), size);
    send_unicode(wide);
    return R"({"ok":true})";
  }
  if (line == "quit") return R"({"ok":true,"quit":true})";
  return R"({"ok":false,"error":"unknown command"})";
}

void pipe_loop(std::atomic<bool>& running) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1,
      &attributes.lpSecurityDescriptor, nullptr);
  std::vector<uint8_t> frame;
  while (running.load()) {
    // Several instances: the capture asks for a frame while the core's desktop
    // watcher asks which desktop is in front. A single instance made the two
    // evict each other, and a failed capture fell back to a source that cannot
    // see the secure desktop.
    HANDLE pipe = CreateNamedPipeW(
        kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 8,
        64 * 1024 * 1024, 64 * 1024 * 1024, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(500);
      continue;
    }
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) != 0 ||
                           GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected) {
      // One thread per connection. The capture (called a few times a second)
      // and the core's desktop watcher used to queue behind each other on a
      // single-threaded accept loop, and a queued capture timed out -- which
      // froze the secure-desktop picture while the page kept asking for
      // keyframes. Each thread opens its own desktop handle (thread_local).
      std::thread([pipe, &running] {
        std::vector<uint8_t> frame;
        std::vector<char> buffer(64 * 1024);
        DWORD read = 0;
        const BOOL read_ok = ReadFile(
            pipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read,
            nullptr);
        if (!read_ok || read == 0) {
          log_line("pipe read failed: " + std::to_string(GetLastError()) +
                   " bytes=" + std::to_string(read));
        } else {
          buffer[read] = '\0';
          const std::string command(buffer.data(), read);
          int width = 0;
          int height = 0;
          const std::string reply = handle_line(command, frame, width, height);
          DWORD written = 0;
          if (!WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()),
                         &written, nullptr)) {
            log_line("pipe write failed: " + std::to_string(GetLastError()) +
                     " bytes=" + std::to_string(reply.size()));
          }
          // DisconnectNamedPipe discards messages the client has not read yet.
          // Flush waits for the read to drain, so the reply is never lost.
          FlushFileBuffers(pipe);
          if (reply.find("\"quit\":true") != std::string::npos) {
            running.store(false);
          }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
      }).detach();
      continue;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
  if (attributes.lpSecurityDescriptor != nullptr) {
    LocalFree(attributes.lpSecurityDescriptor);
  }
}

void input_pipe_loop(std::atomic<bool>& running) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1,
      &attributes.lpSecurityDescriptor, nullptr);
  std::vector<uint8_t> frame;
  while (running.load()) {
    HANDLE pipe = CreateNamedPipeW(
        kInputPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 8, 64 * 1024,
        64 * 1024, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(500);
      continue;
    }
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) != 0 ||
                           GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected) {
      char buffer[64 * 1024] = {};
      DWORD read = 0;
      if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read > 0) {
        buffer[read] = '\0';
        int width = 0;
        int height = 0;
        const std::string reply =
            handle_line(std::string(buffer, read), frame, width, height);
        DWORD written = 0;
        WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()),
                  &written, nullptr);
        FlushFileBuffers(pipe);
        if (reply.find("\"quit\":true") != std::string::npos) {
          running.store(false);
        }
      }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
  if (attributes.lpSecurityDescriptor != nullptr) {
    LocalFree(attributes.lpSecurityDescriptor);
  }
}

}  // namespace

int wmain() {
  enable_physical_pixels();
  std::string err;
  if (!open_input_desktop(err)) {
    log_line("open_input_desktop failed: " + err);
    std::fprintf(stderr, "molink-helper: %s\n", err.c_str());
    return 1;
  }
  log_line("helper started, desktop=" + desktop_json());
  std::atomic<bool> running{true};
  std::thread input_thread(input_pipe_loop, std::ref(running));
  pipe_loop(running);
  input_thread.join();
  if (g_desktop != nullptr) CloseDesktop(g_desktop);
  return 0;
}
