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
#include <sddl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

HDESK g_desktop = nullptr;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-secure";

std::string utf8(const wchar_t* text) {
  if (text == nullptr) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

bool open_input_desktop(std::string& err) {
  g_desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS |
                                             DESKTOP_WRITEOBJECTS |
                                             DESKTOP_SWITCHDESKTOP);
  if (g_desktop == nullptr) {
    err = "OpenInputDesktop failed";
    return false;
  }
  if (!SetThreadDesktop(g_desktop)) {
    err = "SetThreadDesktop failed";
    return false;
  }
  return true;
}

std::string desktop_json() {
  wchar_t name[256] = {};
  DWORD needed = 0;
  GetUserObjectInformationW(g_desktop, UOI_NAME, name, sizeof(name), &needed);
  char buffer[512] = {};
  std::snprintf(buffer, sizeof(buffer),
                R"({"ok":true,"desktop":"%s","width":%d,"height":%d})",
                utf8(name).c_str(), GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN));
  return buffer;
}

bool capture(std::vector<uint8_t>& out, int& width, int& height,
             std::string& err) {
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
  const bool blitted = BitBlt(memory, 0, 0, width, height, screen, 0, 0,
                              SRCCOPY | CAPTUREBLT) != 0;
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
    err = "BitBlt/GetDIBits failed";
    return false;
  }
  return true;
}

void send_key(WORD vk, bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  if (!down) input.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &input, sizeof(input));
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
    SendInput(1, &input, sizeof(input));
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
    HANDLE pipe = CreateNamedPipeW(
        kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 8 * 1024 * 1024,
        8 * 1024 * 1024, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(500);
      continue;
    }
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) != 0 ||
                           GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected) {
      std::vector<char> buffer(64 * 1024);
      DWORD read = 0;
      if (ReadFile(pipe, buffer.data(),
                   static_cast<DWORD>(buffer.size() - 1), &read, nullptr) &&
          read > 0) {
        buffer[read] = '\0';
        int width = 0;
        int height = 0;
        const std::string reply =
            handle_line(std::string(buffer.data()), frame, width, height);
        DWORD written = 0;
        WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()),
                  &written, nullptr);
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
  std::string err;
  if (!open_input_desktop(err)) {
    std::fprintf(stderr, "molink-helper: %s\n", err.c_str());
    return 1;
  }
  std::atomic<bool> running{true};
  pipe_loop(running);
  if (g_desktop != nullptr) CloseDesktop(g_desktop);
  return 0;
}
