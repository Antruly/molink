// Native picture window for Windows: draws the core's shared-memory frame
// surface directly with GDI, with no WebView in the picture path. The page
// keeps owning the session; this window is the first step towards a fully
// native picture path (see backend/src/surface/frame_surface.h).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr uint32_t kMagic = 0x5255534d;  // "MSUR"
constexpr std::size_t kHeaderBytes = 64;

struct surface_header {
  uint32_t magic;
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t format;
  uint64_t sequence;
  uint64_t timestamp_us;
  uint32_t slot_bytes;
  uint32_t reserved[5];
};

const wchar_t* kClassName = L"moLinkSurfaceNative";

struct surface_state {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
  uint8_t* base = nullptr;
  std::size_t bytes = 0;
  uint64_t seen = 0;
  uint64_t drawn = 0;
  uint64_t reported = 0;
  ULONGLONG last_report = 0;
};

surface_state g_state;

bool map_surface(const std::wstring& path) {
  g_state.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_state.file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(g_state.file, &size)) return false;
  g_state.bytes = static_cast<std::size_t>(size.QuadPart);
  g_state.mapping = CreateFileMappingW(g_state.file, nullptr, PAGE_READONLY, 0, 0,
                                       nullptr);
  if (g_state.mapping == nullptr) return false;
  g_state.base = static_cast<uint8_t*>(
      MapViewOfFile(g_state.mapping, FILE_MAP_READ, 0, 0, 0));
  return g_state.base != nullptr;
}

void unmap_surface() {
  if (g_state.base != nullptr) UnmapViewOfFile(g_state.base);
  if (g_state.mapping != nullptr) CloseHandle(g_state.mapping);
  if (g_state.file != INVALID_HANDLE_VALUE) CloseHandle(g_state.file);
  g_state = surface_state{};
}

void paint(HWND window) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(window, &ps);
  RECT client{};
  GetClientRect(window, &client);
  const int cw = client.right - client.left;
  const int ch = client.bottom - client.top;
  if (g_state.base == nullptr) {
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    EndPaint(window, &ps);
    return;
  }
  auto* header = reinterpret_cast<surface_header*>(g_state.base);
  if (header->magic != kMagic || header->slot_bytes == 0) {
    EndPaint(window, &ps);
    return;
  }
  const uint64_t sequence =
      std::atomic_ref<uint64_t>(header->sequence).load(
          std::memory_order_acquire);
  const uint8_t* pixels =
      g_state.base + kHeaderBytes +
      static_cast<std::size_t>(sequence & 1) * header->slot_bytes;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = static_cast<LONG>(header->width);
  // Negative height: top-down rows, which is how the core writes BGRA.
  info.bmiHeader.biHeight = -static_cast<LONG>(header->height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, HALFTONE);
  StretchDIBits(dc, 0, 0, cw, ch, 0, 0, static_cast<int>(header->width),
                static_cast<int>(header->height), pixels, &info, DIB_RGB_COLORS,
                SRCCOPY);
  ++g_state.drawn;
  EndPaint(window, &ps);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      SetTimer(window, 1, 16, nullptr);
      return 0;
    case WM_TIMER: {
      if (g_state.base != nullptr) {
        auto* header = reinterpret_cast<surface_header*>(g_state.base);
        const uint64_t sequence =
            std::atomic_ref<uint64_t>(header->sequence).load(
                std::memory_order_acquire);
        if (sequence != g_state.seen) {
          g_state.seen = sequence;
          InvalidateRect(window, nullptr, FALSE);
        }
        const ULONGLONG now = GetTickCount64();
        if (g_state.last_report == 0) g_state.last_report = now;
        if (now - g_state.last_report >= 1000) {
          std::fprintf(stderr, "surface: %.1f fps %ux%u seq %llu\n",
                       static_cast<double>(g_state.drawn - g_state.reported) /
                           ((now - g_state.last_report) / 1000.0),
                       header->width, header->height,
                       static_cast<unsigned long long>(sequence));
          g_state.reported = g_state.drawn;
          g_state.last_report = now;
        }
      }
      return 0;
    }
    case WM_PAINT:
      paint(window);
      return 0;
    case WM_ERASEBKGND:
      return 1;  // paint() fills every pixel
    case WM_DESTROY:
      KillTimer(window, 1);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

/// Opens the native picture window for the core's surface at `path` and runs
/// its message loop. Returns the process exit code.
int run_surface_window(const std::wstring& path) {
  if (!map_surface(path)) {
    std::fprintf(stderr, "surface: cannot open %ls\n", path.c_str());
    return 1;
  }
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  cls.lpszClassName = kClassName;
  RegisterClassExW(&cls);
  HWND window = CreateWindowExW(
      0, kClassName, L"moLink \x2014 native picture", WS_OVERLAPPEDWINDOW |
                                                           WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance,
      nullptr);
  if (window == nullptr) {
    unmap_surface();
    return 1;
  }
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  unmap_surface();
  return 0;
}
