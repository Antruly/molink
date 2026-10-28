// Native picture window: draws the core's shared-memory frame surface without
// a WebView in the path. The page keeps owning the session; this window is the
// first step towards a fully native picture path (see frame_surface.h).

#include <gtk/gtk.h>

#include <cairo.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

struct surface_window {
  uint8_t* mapping = nullptr;
  std::size_t bytes = 0;
  uint64_t seen = 0;
  GtkWidget* area = nullptr;
  guint64 drawn = 0;
  guint64 reported = 0;
  gint64 last_report_us = 0;
};

surface_window g_window;

gboolean on_tick(gpointer) {
  auto* header = reinterpret_cast<surface_header*>(g_window.mapping);
  const uint64_t sequence =
      __atomic_load_n(&header->sequence, __ATOMIC_ACQUIRE);
  if (sequence != g_window.seen) {
    g_window.seen = sequence;
    gtk_widget_queue_draw(g_window.area);
  }
  const gint64 now = g_get_monotonic_time();
  if (g_window.last_report_us == 0) g_window.last_report_us = now;
  if (now - g_window.last_report_us >= 1000000) {
    const double seconds = (now - g_window.last_report_us) / 1e6;
    const guint64 frames = g_window.drawn - g_window.reported;
    std::fprintf(stderr, "surface: %.1f fps %ux%u seq %llu\n",
                 static_cast<double>(frames) / seconds, header->width,
                 header->height,
                 static_cast<unsigned long long>(sequence));
    g_window.reported = g_window.drawn;
    g_window.last_report_us = now;
  }
  return G_SOURCE_CONTINUE;
}

gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer) {
  auto* header = reinterpret_cast<surface_header*>(g_window.mapping);
  const uint64_t sequence =
      __atomic_load_n(&header->sequence, __ATOMIC_ACQUIRE);
  if (sequence == 0 || header->width == 0 || header->height == 0) {
    return FALSE;
  }
  // The writer fills the slot named by the sequence parity, so this slot is
  // not the one being written right now.
  const std::size_t offset =
      kHeaderBytes + static_cast<std::size_t>(sequence & 1u) * header->slot_bytes;
  if (offset + static_cast<std::size_t>(header->stride) * header->height >
      g_window.bytes) {
    return FALSE;
  }
  uint8_t* slot = g_window.mapping + offset;
  // CAIRO_FORMAT_RGB24 is BGRX on little-endian: our BGRA rows, alpha ignored.
  cairo_surface_t* image = cairo_image_surface_create_for_data(
      slot, CAIRO_FORMAT_RGB24, static_cast<int>(header->width),
      static_cast<int>(header->height), static_cast<int>(header->stride));
  if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(image);
    return FALSE;
  }
  const int area_width = gtk_widget_get_allocated_width(widget);
  const int area_height = gtk_widget_get_allocated_height(widget);
  const double scale = std::min(
      static_cast<double>(area_width) / header->width,
      static_cast<double>(area_height) / header->height);
  if (scale <= 0) {
    cairo_surface_destroy(image);
    return FALSE;
  }
  const double offset_x = (area_width - header->width * scale) / 2.0;
  const double offset_y = (area_height - header->height * scale) / 2.0;
  cairo_save(cr);
  cairo_translate(cr, offset_x, offset_y);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, image, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_surface_destroy(image);
  g_window.drawn += 1;
  return TRUE;
}

}  // namespace

/// Opens a native window that mirrors the core's frame surface. Returns the
/// process exit code; the caller has already run gtk_init.
int run_surface_window(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    std::fprintf(stderr, "moLink surface: cannot open %s\n", path.c_str());
    return 1;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
    std::fprintf(stderr, "moLink surface: cannot stat %s\n", path.c_str());
    ::close(fd);
    return 1;
  }
  g_window.bytes = static_cast<std::size_t>(info.st_size);
  void* mapping =
      ::mmap(nullptr, g_window.bytes, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapping == MAP_FAILED) {
    std::fprintf(stderr, "moLink surface: mmap failed\n");
    return 1;
  }
  g_window.mapping = static_cast<uint8_t*>(mapping);
  auto* header = reinterpret_cast<surface_header*>(g_window.mapping);
  if (header->magic != kMagic) {
    std::fprintf(stderr, "moLink surface: %s is not a frame surface\n",
                 path.c_str());
    return 1;
  }

  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "moLink surface");
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 720);
  GtkWidget* area = gtk_drawing_area_new();
  gtk_widget_set_double_buffered(area, TRUE);
  gtk_container_add(GTK_CONTAINER(window), area);
  g_window.area = area;
  g_signal_connect(area, "draw", G_CALLBACK(on_draw), nullptr);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_widget_show_all(window);
  g_timeout_add(16, on_tick, nullptr);
  gtk_main();
  return 0;
}
