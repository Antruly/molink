// Renders the web's lucide icons from the generated shape data
// (lucide_data.h): SVG path/shape strings -> Direct2D geometry, stroked with
// the same 2/24 width, round caps and round joins the web uses. Included from
// ui_kit.h inside namespace molink::ui, after `enum class icon`.
#pragma once

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lucide_data.h"

namespace molink::ui {

namespace lucide_detail {

inline const char* skip_sep(const char* p) {
  while (*p != '\0' &&
         (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) {
    ++p;
  }
  return p;
}

inline float parse_float(const char** p) {
  const char* s = skip_sep(*p);
  char* end = nullptr;
  const float value = std::strtof(s, &end);
  *p = (end != nullptr && end != s) ? end : s;
  return value;
}

inline bool parse_flag(const char** p, bool* out) {
  const char* s = skip_sep(*p);
  if (*s == '0' || *s == '1') {
    *out = *s == '1';
    *p = s + 1;
    return true;
  }
  return false;
}

/// SVG elliptical arc (endpoint form) appended to the sink as cubics.
inline void arc_to(ID2D1GeometrySink* sink, float x0, float y0, float rx,
                   float ry, float rotation_deg, bool large_arc, bool sweep,
                   float x, float y, float* out_x, float* out_y) {
  const double kPi = 3.14159265358979323846;
  if (rx == 0.0f || ry == 0.0f) {
    sink->AddLine(D2D1::Point2F(x, y));
    *out_x = x;
    *out_y = y;
    return;
  }
  double rxs = std::fabs(static_cast<double>(rx));
  double rys = std::fabs(static_cast<double>(ry));
  const double phi = rotation_deg * kPi / 180.0;
  const double cosphi = std::cos(phi);
  const double sinphi = std::sin(phi);
  const double dx2 = (x0 - x) / 2.0;
  const double dy2 = (y0 - y) / 2.0;
  const double x1p = cosphi * dx2 + sinphi * dy2;
  const double y1p = -sinphi * dx2 + cosphi * dy2;
  const double lambda =
      (x1p * x1p) / (rxs * rxs) + (y1p * y1p) / (rys * rys);
  if (lambda > 1.0) {
    const double s = std::sqrt(lambda);
    rxs *= s;
    rys *= s;
  }
  const double sign = (large_arc == sweep) ? -1.0 : 1.0;
  const double num = rxs * rxs * rys * rys - rxs * rxs * y1p * y1p -
                     rys * rys * x1p * x1p;
  const double den = rxs * rxs * y1p * y1p + rys * rys * x1p * x1p;
  const double coef = den == 0.0 ? 0.0 : sign * std::sqrt(std::max(0.0, num / den));
  const double cxp = coef * (rxs * y1p / rys);
  const double cyp = coef * (-rys * x1p / rxs);
  const double cx = cosphi * cxp - sinphi * cyp + (x0 + x) / 2.0;
  const double cy = sinphi * cxp + cosphi * cyp + (y0 + y) / 2.0;
  auto angle = [](double ux, double uy, double vx, double vy) {
    const double dot = ux * vx + uy * vy;
    const double len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    double a = len == 0.0 ? 0.0 : std::acos(std::max(-1.0, std::min(1.0, dot / len)));
    if (ux * vy - uy * vx < 0.0) a = -a;
    return a;
  };
  const double start = angle(1.0, 0.0, (x1p - cxp) / rxs, (y1p - cyp) / rys);
  double delta = angle((x1p - cxp) / rxs, (y1p - cyp) / rys,
                       (-x1p - cxp) / rxs, (-y1p - cyp) / rys);
  if (!sweep && delta > 0.0) delta -= 2.0 * kPi;
  if (sweep && delta < 0.0) delta += 2.0 * kPi;
  const int segments =
      std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 2.0))));
  const double step = delta / segments;
  auto point = [&](double t, double* px, double* py) {
    const double ct = std::cos(t);
    const double st = std::sin(t);
    const double ex = rxs * ct;
    const double ey = rys * st;
    *px = cosphi * ex - sinphi * ey + cx;
    *py = sinphi * ex + cosphi * ey + cy;
  };
  auto derivative = [&](double t, double* dx, double* dy) {
    const double ct = std::cos(t);
    const double st = std::sin(t);
    const double ex = -rxs * st;
    const double ey = rys * ct;
    *dx = cosphi * ex - sinphi * ey;
    *dy = sinphi * ex + cosphi * ey;
  };
  double t = start;
  double fx = x0;
  double fy = y0;
  for (int i = 0; i < segments; ++i) {
    const double t2 = t + step;
    double x2 = 0, y2 = 0;
    double d1x = 0, d1y = 0, d2x = 0, d2y = 0;
    point(t2, &x2, &y2);
    derivative(t, &d1x, &d1y);
    derivative(t2, &d2x, &d2y);
    const double alpha = (4.0 / 3.0) * std::tan(step / 4.0);
    D2D1_BEZIER_SEGMENT bezier;
    bezier.point1 = D2D1::Point2F(static_cast<float>(fx + alpha * d1x),
                                  static_cast<float>(fy + alpha * d1y));
    bezier.point2 = D2D1::Point2F(static_cast<float>(x2 - alpha * d2x),
                                  static_cast<float>(y2 - alpha * d2y));
    bezier.point3 = D2D1::Point2F(static_cast<float>(x2),
                                  static_cast<float>(y2));
    sink->AddBezier(bezier);
    fx = x2;
    fy = y2;
    t = t2;
  }
  *out_x = x;
  *out_y = y;
}

/// Parses one SVG path `d` into the sink.
inline void parse_path(const char* d, ID2D1GeometrySink* sink) {
  const char* p = d;
  float cx = 0, cy = 0, sx = 0, sy = 0;
  float ctrl_x = 0, ctrl_y = 0;
  char last = 0;
  char cmd = 0;
  bool open = false;
  auto ensure = [&]() {
    if (!open) {
      sink->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_HOLLOW);
      open = true;
    }
  };
  auto finish = [&]() {
    if (open) {
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      open = false;
    }
  };
  while (true) {
    p = skip_sep(p);
    if (*p == '\0') break;
    const char ch = *p;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
      cmd = ch;
      ++p;
    } else if (cmd == 0) {
      break;
    } else if (cmd == 'M') {
      cmd = 'L';
    } else if (cmd == 'm') {
      cmd = 'l';
    }
    switch (cmd) {
      case 'M':
      case 'm': {
        const bool rel = cmd == 'm';
        const float nx = parse_float(&p);
        const float ny = parse_float(&p);
        finish();
        cx = rel ? cx + nx : nx;
        cy = rel ? cy + ny : ny;
        sx = cx;
        sy = cy;
        break;
      }
      case 'L':
      case 'l': {
        const bool rel = cmd == 'l';
        const float nx = parse_float(&p);
        const float ny = parse_float(&p);
        ensure();
        cx = rel ? cx + nx : nx;
        cy = rel ? cy + ny : ny;
        sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      }
      case 'H':
      case 'h': {
        const bool rel = cmd == 'h';
        const float nx = parse_float(&p);
        ensure();
        cx = rel ? cx + nx : nx;
        sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      }
      case 'V':
      case 'v': {
        const bool rel = cmd == 'v';
        const float ny = parse_float(&p);
        ensure();
        cy = rel ? cy + ny : ny;
        sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      }
      case 'C':
      case 'c': {
        const bool rel = cmd == 'c';
        const float x1 = parse_float(&p);
        const float y1 = parse_float(&p);
        const float x2 = parse_float(&p);
        const float y2 = parse_float(&p);
        const float x = parse_float(&p);
        const float y = parse_float(&p);
        const D2D1_POINT_2F p1 =
            rel ? D2D1::Point2F(cx + x1, cy + y1) : D2D1::Point2F(x1, y1);
        const D2D1_POINT_2F p2 =
            rel ? D2D1::Point2F(cx + x2, cy + y2) : D2D1::Point2F(x2, y2);
        const D2D1_POINT_2F p3 =
            rel ? D2D1::Point2F(cx + x, cy + y) : D2D1::Point2F(x, y);
        ensure();
        D2D1_BEZIER_SEGMENT bezier;
        bezier.point1 = p1;
        bezier.point2 = p2;
        bezier.point3 = p3;
        sink->AddBezier(bezier);
        ctrl_x = p2.x;
        ctrl_y = p2.y;
        cx = p3.x;
        cy = p3.y;
        break;
      }
      case 'S':
      case 's': {
        const bool rel = cmd == 's';
        const float x2 = parse_float(&p);
        const float y2 = parse_float(&p);
        const float x = parse_float(&p);
        const float y = parse_float(&p);
        const bool smooth = last == 'C' || last == 'c' || last == 'S' ||
                            last == 's';
        const D2D1_POINT_2F p1 =
            smooth ? D2D1::Point2F(2 * cx - ctrl_x, 2 * cy - ctrl_y)
                   : D2D1::Point2F(cx, cy);
        const D2D1_POINT_2F p2 =
            rel ? D2D1::Point2F(cx + x2, cy + y2) : D2D1::Point2F(x2, y2);
        const D2D1_POINT_2F p3 =
            rel ? D2D1::Point2F(cx + x, cy + y) : D2D1::Point2F(x, y);
        ensure();
        D2D1_BEZIER_SEGMENT bezier;
        bezier.point1 = p1;
        bezier.point2 = p2;
        bezier.point3 = p3;
        sink->AddBezier(bezier);
        ctrl_x = p2.x;
        ctrl_y = p2.y;
        cx = p3.x;
        cy = p3.y;
        break;
      }
      case 'Q':
      case 'q': {
        const bool rel = cmd == 'q';
        const float x1 = parse_float(&p);
        const float y1 = parse_float(&p);
        const float x = parse_float(&p);
        const float y = parse_float(&p);
        const D2D1_POINT_2F p1 =
            rel ? D2D1::Point2F(cx + x1, cy + y1) : D2D1::Point2F(x1, y1);
        const D2D1_POINT_2F p2 =
            rel ? D2D1::Point2F(cx + x, cy + y) : D2D1::Point2F(x, y);
        ensure();
        D2D1_QUADRATIC_BEZIER_SEGMENT curve;
        curve.point1 = p1;
        curve.point2 = p2;
        sink->AddQuadraticBezier(curve);
        ctrl_x = p1.x;
        ctrl_y = p1.y;
        cx = p2.x;
        cy = p2.y;
        break;
      }
      case 'T':
      case 't': {
        const bool rel = cmd == 't';
        const float x = parse_float(&p);
        const float y = parse_float(&p);
        const bool smooth = last == 'Q' || last == 'q' || last == 'T' ||
                            last == 't';
        const D2D1_POINT_2F p1 =
            smooth ? D2D1::Point2F(2 * cx - ctrl_x, 2 * cy - ctrl_y)
                   : D2D1::Point2F(cx, cy);
        const D2D1_POINT_2F p2 =
            rel ? D2D1::Point2F(cx + x, cy + y) : D2D1::Point2F(x, y);
        ensure();
        D2D1_QUADRATIC_BEZIER_SEGMENT curve;
        curve.point1 = p1;
        curve.point2 = p2;
        sink->AddQuadraticBezier(curve);
        ctrl_x = p1.x;
        ctrl_y = p1.y;
        cx = p2.x;
        cy = p2.y;
        break;
      }
      case 'A':
      case 'a': {
        const bool rel = cmd == 'a';
        const float rx = parse_float(&p);
        const float ry = parse_float(&p);
        const float rotation = parse_float(&p);
        bool large = false;
        bool sweep = false;
        parse_flag(&p, &large);
        parse_flag(&p, &sweep);
        const float x = parse_float(&p);
        const float y = parse_float(&p);
        const float tx = rel ? cx + x : x;
        const float ty = rel ? cy + y : y;
        ensure();
        float out_x = tx;
        float out_y = ty;
        arc_to(sink, cx, cy, rx, ry, rotation, large, sweep, tx, ty, &out_x,
               &out_y);
        cx = out_x;
        cy = out_y;
        break;
      }
      case 'Z':
      case 'z': {
        if (open) {
          sink->EndFigure(D2D1_FIGURE_END_CLOSED);
          open = false;
        }
        cx = sx;
        cy = sy;
        break;
      }
      default:
        break;
    }
    last = cmd;
  }
  finish();
}

}  // namespace lucide_detail

struct lucide_cached {
  bool ready = false;
  std::vector<ID2D1Geometry*> shapes;
};

inline std::vector<lucide_cached>& lucide_cache() {
  static std::vector<lucide_cached> entries(
      sizeof(kLucideShapes) / sizeof(kLucideShapes[0]));
  return entries;
}

inline void build_lucide_shapes(ID2D1Factory* factory, const char* data,
                                std::vector<ID2D1Geometry*>* out) {
  std::string text(data);
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('|', pos);
    if (end == std::string::npos) end = text.size();
    const std::string part = text.substr(pos, end - pos);
    pos = end + 1;
    if (part.empty()) break;
    const size_t colon = part.find(':');
    if (colon == std::string::npos) continue;
    const std::string tag = part.substr(0, colon);
    const std::string attrs = part.substr(colon + 1);
    auto attr = [&attrs](const char* key, float fallback) -> float {
      const std::string needle = std::string(key) + "=";
      size_t at = 0;
      while ((at = attrs.find(needle, at)) != std::string::npos) {
        // require a boundary before the key
        if (at == 0 || attrs[at - 1] == ':') {
          return std::strtof(attrs.c_str() + at + needle.size(), nullptr);
        }
        at += needle.size();
      }
      return fallback;
    };
    auto attr_text = [&attrs](const char* key) -> std::string {
      const std::string needle = std::string(key) + "=";
      size_t at = 0;
      while ((at = attrs.find(needle, at)) != std::string::npos) {
        if (at == 0 || attrs[at - 1] == ':') {
          size_t start = at + needle.size();
          size_t stop = attrs.find(':', start);
          if (stop == std::string::npos) stop = attrs.size();
          return attrs.substr(start, stop - start);
        }
        at += needle.size();
      }
      return std::string();
    };
    if (tag == "path") {
      const std::string d = attr_text("d");
      ID2D1PathGeometry* geometry = nullptr;
      if (d.empty() ||
          FAILED(factory->CreatePathGeometry(&geometry)) ||
          geometry == nullptr) {
        continue;
      }
      ID2D1GeometrySink* sink = nullptr;
      if (SUCCEEDED(geometry->Open(&sink)) && sink != nullptr) {
        lucide_detail::parse_path(d.c_str(), sink);
        sink->Close();
        sink->Release();
        out->push_back(geometry);
      } else {
        geometry->Release();
      }
    } else if (tag == "rect") {
      const float x = attr("x", 0);
      const float y = attr("y", 0);
      const float w = attr("width", 0);
      const float h = attr("height", 0);
      const float rx = attr("rx", 0);
      const float ry = attr("ry", rx);
      if (w <= 0 || h <= 0) continue;
      ID2D1RoundedRectangleGeometry* geometry = nullptr;
      D2D1_ROUNDED_RECT rect =
          D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry);
      if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(rect, &geometry)) &&
          geometry != nullptr) {
        out->push_back(geometry);
      }
    } else if (tag == "circle") {
      const float cx = attr("cx", 0);
      const float cy = attr("cy", 0);
      const float r = attr("r", 0);
      ID2D1EllipseGeometry* geometry = nullptr;
      if (r > 0 && SUCCEEDED(factory->CreateEllipseGeometry(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r),
                        &geometry)) &&
          geometry != nullptr) {
        out->push_back(geometry);
      }
    } else if (tag == "line") {
      ID2D1PathGeometry* geometry = nullptr;
      if (FAILED(factory->CreatePathGeometry(&geometry)) ||
          geometry == nullptr) {
        continue;
      }
      ID2D1GeometrySink* sink = nullptr;
      if (SUCCEEDED(geometry->Open(&sink)) && sink != nullptr) {
        sink->BeginFigure(D2D1::Point2F(attr("x1", 0), attr("y1", 0)),
                          D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(attr("x2", 0), attr("y2", 0)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        out->push_back(geometry);
      } else {
        geometry->Release();
      }
    } else if (tag == "polyline" || tag == "polygon") {
      const std::string points = attr_text("points");
      if (points.empty()) continue;
      ID2D1PathGeometry* geometry = nullptr;
      if (FAILED(factory->CreatePathGeometry(&geometry)) ||
          geometry == nullptr) {
        continue;
      }
      ID2D1GeometrySink* sink = nullptr;
      if (SUCCEEDED(geometry->Open(&sink)) && sink != nullptr) {
        const char* p = points.c_str();
        bool started = false;
        while (true) {
          p = lucide_detail::skip_sep(p);
          if (*p == '\0') break;
          const float x = lucide_detail::parse_float(&p);
          const float y = lucide_detail::parse_float(&p);
          if (!started) {
            sink->BeginFigure(D2D1::Point2F(x, y),
                              D2D1_FIGURE_BEGIN_HOLLOW);
            started = true;
          } else {
            sink->AddLine(D2D1::Point2F(x, y));
          }
        }
        if (started) {
          sink->EndFigure(tag == "polygon" ? D2D1_FIGURE_END_CLOSED
                                           : D2D1_FIGURE_END_OPEN);
        }
        sink->Close();
        sink->Release();
        out->push_back(geometry);
      } else {
        geometry->Release();
      }
    }
  }
}

/// Draws the lucide icon for `kind` centered on (cx, cy), sized so the web's
/// 24-unit box maps to `box` pixels. Returns false when this icon has no
/// lucide data (callers fall back to the hand-drawn stroke set).
inline bool draw_lucide_icon(ID2D1RenderTarget* target, int kind, float cx,
                             float cy, const D2D1_COLOR_F& color,
                             float box = 15.0f) {
  const int count = static_cast<int>(sizeof(kLucideShapes) /
                                     sizeof(kLucideShapes[0]));
  if (target == nullptr || kind <= 0 || kind >= count) return false;
  const char* data = kLucideShapes[kind];
  if (data == nullptr) return false;
  std::vector<lucide_cached>& cache = lucide_cache();
  if (static_cast<size_t>(kind) >= cache.size()) return false;
  lucide_cached& entry = cache[static_cast<size_t>(kind)];
  ID2D1Factory* factory = nullptr;
  target->GetFactory(&factory);
  if (factory == nullptr) return false;
  if (!entry.ready) {
    build_lucide_shapes(factory, data, &entry.shapes);
    entry.ready = true;
  }
  if (entry.shapes.empty()) return false;
  static ID2D1StrokeStyle* stroke = nullptr;
  if (stroke == nullptr) {
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
    props.startCap = D2D1_CAP_STYLE_ROUND;
    props.endCap = D2D1_CAP_STYLE_ROUND;
    props.dashCap = D2D1_CAP_STYLE_ROUND;
    props.lineJoin = D2D1_LINE_JOIN_ROUND;
    if (FAILED(factory->CreateStrokeStyle(props, nullptr, 0, &stroke))) {
      stroke = nullptr;
    }
  }
  const float scale = box / 24.0f;
  D2D1_MATRIX_3X2_F previous;
  target->GetTransform(&previous);
  const D2D1_MATRIX_3X2_F local =
      D2D1::Matrix3x2F::Scale(scale, scale) *
      D2D1::Matrix3x2F::Translation(cx - 12.0f * scale, cy - 12.0f * scale);
  target->SetTransform(local * previous);
  ID2D1SolidColorBrush* brush = nullptr;
  if (SUCCEEDED(target->CreateSolidColorBrush(color, &brush)) &&
      brush != nullptr) {
    for (ID2D1Geometry* shape : entry.shapes) {
      target->DrawGeometry(shape, brush, 2.0f, stroke);
    }
    brush->Release();
  }
  target->SetTransform(previous);
  return true;
}

}  // namespace molink::ui
