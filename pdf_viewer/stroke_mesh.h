#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Geometry helper shared by on-page strokes and cached scratchpad strokes.
// The two coordinate axes may have different scales (e.g. OpenGL NDC on a
// wide window), while the radii are expressed in the corresponding axes.
namespace sioyek_stroke_mesh {

struct Point {
    float x;
    float y;
    float thickness;
};

// -1 denotes mouse input (fixed width), [0,1] denotes tablet pressure.
inline float pressure_width_factor(float pressure, float minimum_width, float gamma) {
    if (pressure < 0.0f || !std::isfinite(pressure)) return 1.0f;
    const float p = std::max(0.0f, std::min(1.0f, pressure));
    const float low = std::max(0.05f, std::min(1.0f, minimum_width));
    const float curve = std::max(0.2f, std::min(4.0f, gamma));
    return low + (1.0f - low) * std::pow(p, curve);
}

inline Point smooth_input(Point previous, Point current, float strength) {
    const float amount = std::max(0.0f, std::min(0.85f, strength));
    return {previous.x * amount + current.x * (1.0f - amount),
            previous.y * amount + current.y * (1.0f - amount),
            previous.thickness * amount + current.thickness * (1.0f - amount)};
}

struct Vec2 {
    float x;
    float y;
};

inline Vec2 direction(const Point& a, const Point& b,
                      float aspect_x, float aspect_y) {
    const float dx = (b.x - a.x) * aspect_x;
    const float dy = (b.y - a.y) * aspect_y;
    const float length = std::hypot(dx, dy);
    if (length <= 1e-5f || !std::isfinite(length)) return {0.0f, 0.0f};
    return {dx / length, dy / length};
}

// Adaptive Catmull-Rom-to-cubic Bezier subdivision. Coordinates stay in the
// document's vector space; px_x/px_y are used ONLY for the flatness test.
inline Point lerp_point(const Point& a, const Point& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            a.thickness + (b.thickness - a.thickness) * t};
}

inline float pixel_distance(const Point& a, const Point& b,
                            float px_x, float px_y) {
    return std::hypot((a.x - b.x) * px_x, (a.y - b.y) * px_y);
}

inline float control_distance(const Point& p, const Point& a, const Point& b,
                              float px_x, float px_y) {
    const float dx = (b.x - a.x) * px_x, dy = (b.y - a.y) * px_y;
    const float length = std::hypot(dx, dy);
    if (length < 1e-5f) return pixel_distance(p, a, px_x, px_y);
    return std::abs((p.x - a.x) * px_x * dy - (p.y - a.y) * px_y * dx) / length;
}

inline void append_bezier(const Point& a, const Point& b,
                          const Point& c, const Point& d,
                          float px_x, float px_y, int depth,
                          std::vector<Point>& output) {
    const float flatness = std::max(control_distance(b, a, d, px_x, px_y),
                                    control_distance(c, a, d, px_x, px_y));
    if (depth >= 6 || (flatness <= 0.55f &&
                       pixel_distance(a, d, px_x, px_y) <= 7.0f)) {
        output.push_back(d);
        return;
    }
    const auto ab = lerp_point(a, b, 0.5f);
    const auto bc = lerp_point(b, c, 0.5f);
    const auto cd = lerp_point(c, d, 0.5f);
    const auto abc = lerp_point(ab, bc, 0.5f);
    const auto bcd = lerp_point(bc, cd, 0.5f);
    const auto mid = lerp_point(abc, bcd, 0.5f);
    append_bezier(a, ab, abc, mid, px_x, px_y, depth + 1, output);
    append_bezier(mid, bcd, cd, d, px_x, px_y, depth + 1, output);
}

inline std::vector<Point> smooth_curve(const std::vector<Point>& input,
                                      float px_x, float px_y) {
    if (input.size() < 3) return input;
    const float sx = std::max(0.001f, px_x), sy = std::max(0.001f, px_y);
    std::vector<Point> out;
    out.reserve(input.size() * 3);
    out.push_back(input.front());
    for (size_t i = 0; i + 1 < input.size(); ++i) {
        const Point& p0 = input[i == 0 ? i : i - 1];
        const Point& p1 = input[i];
        const Point& p2 = input[i + 1];
        const Point& p3 = input[std::min(i + 2, input.size() - 1)];
        // Catmull-Rom tangents represented as cubic Bezier control points.
        // Endpoint tangents use the adjacent segment, preventing extrapolation.
        Point b = {p1.x + (p2.x - p0.x) / 6.0f,
                   p1.y + (p2.y - p0.y) / 6.0f, p1.thickness};
        Point c = {p2.x - (p3.x - p1.x) / 6.0f,
                   p2.y - (p3.y - p1.y) / 6.0f, p2.thickness};
        append_bezier(p1, b, c, p2, sx, sy, 0, out);
    }
    return out;
}

// Build a *single* triangle strip, including convex round corner sectors.
// Unlike overlapping circles at every point, this does not repeatedly blend
// semi-transparent paint into the same pixels along a join.
inline std::vector<float> make_strip(const std::vector<Point>& input,
                                     float aspect_x, float aspect_y,
                                     float radius_scale_x,
                                     float radius_scale_y) {
    std::vector<Point> points;
    points.reserve(input.size());
    for (const auto& p : input) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.thickness)) continue;
        if (!points.empty()) {
            const auto step = direction(points.back(), p, aspect_x, aspect_y);
            if (step.x == 0.0f && step.y == 0.0f) {
                points.back() = p;
                continue;
            }
        }
        points.push_back(p);
    }
    if (points.size() < 2) return {};
    std::vector<Vec2> directions;
    directions.reserve(points.size() - 1);
    for (size_t i = 0; i + 1 < points.size(); ++i)
        directions.push_back(direction(points[i], points[i + 1],
                                       aspect_x, aspect_y));

    std::vector<float> vertices;
    vertices.reserve(points.size() * 8);
    auto append_vertex_pair = [&](const Point& p, Vec2 left, Vec2 right) {
        const float radius = std::max(0.0f, p.thickness);
        vertices.insert(vertices.end(), {
            p.x + radius_scale_x * radius * left.x,
            p.y + radius_scale_y * radius * left.y,
            p.x + radius_scale_x * radius * right.x,
            p.y + radius_scale_y * radius * right.y
        });
    };
    // Round start cap: a strip fan folded into paired vertices, with no
    // overlapping primitive that would darken a translucent stroke.
    const Vec2 first = directions.front();
    const Vec2 first_normal = {-first.y, first.x};
    constexpr int cap_sections = 9;
    for (int j = cap_sections; j > 0; --j) {
        const float angle = 1.57079632679f * static_cast<float>(j) / cap_sections;
        const float c = std::cos(angle), st = std::sin(angle);
        append_vertex_pair(points.front(), {first_normal.x * c - first.x * st,
                              first_normal.y * c - first.y * st},
                             {-first_normal.x * c - first.x * st,
                              -first_normal.y * c - first.y * st});
    }
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec2 prev = directions[i == 0 ? 0 : i - 1];
        const Vec2 next = directions[i == directions.size() ?
                                      directions.size() - 1 : i];
        const Vec2 n0 = {-prev.y, prev.x};
        const Vec2 n1 = {-next.y, next.x};
        if (i == 0 || i + 1 == points.size()) {
            append_vertex_pair(points[i], n0, {-n0.x, -n0.y});
            continue;
        }
        const float cross = prev.x * next.y - prev.y * next.x;
        const float dot = std::max(-1.0f, std::min(1.0f,
                           prev.x * next.x + prev.y * next.y));
        const float turn = std::atan2(cross, dot);
        if (std::abs(turn) < 0.015f) {
            append_vertex_pair(points[i], n1, {-n1.x, -n1.y});
            continue;
        }
        // Inside edge uses a bounded intersection; outside edge sweeps a
        // circular arc. Segment strips then meet exactly at the arc ends.
        const bool outer_left = turn < 0.0f;
        Vec2 inner = {n0.x + n1.x, n0.y + n1.y};
        const float inner_length = std::hypot(inner.x, inner.y);
        if (inner_length > 1e-4f) {
            inner.x /= inner_length;
            inner.y /= inner_length;
        } else inner = n0;
        const float projection = std::abs(inner.x * n0.x + inner.y * n0.y);
        const float scale = std::min(2.0f, 1.0f / std::max(0.5f, projection));
        inner.x *= scale;
        inner.y *= scale;
        const Vec2 outer_begin = outer_left ? n0 : Vec2{-n0.x, -n0.y};
        const Vec2 inner_side = outer_left ? Vec2{-inner.x, -inner.y} : inner;
        const int sections = std::max(1, std::min(12,
                                 static_cast<int>(std::ceil(std::abs(turn) / 0.24f))));
        for (int j = 0; j <= sections; ++j) {
            const float angle = turn * static_cast<float>(j) / sections;
            const float c = std::cos(angle), s = std::sin(angle);
            Vec2 outer = {outer_begin.x * c - outer_begin.y * s,
                          outer_begin.x * s + outer_begin.y * c};
            if (outer_left) append_vertex_pair(points[i], outer, inner_side);
            else append_vertex_pair(points[i], inner_side, outer);
        }
    }
    // Round end cap continues the same strip; alpha is composited only once.
    const Vec2 last = directions.back();
    const Vec2 last_normal = {-last.y, last.x};
    for (int j = 1; j <= cap_sections; ++j) {
        const float angle = 1.57079632679f * static_cast<float>(j) / cap_sections;
        const float c = std::cos(angle), st = std::sin(angle);
        append_vertex_pair(points.back(), {last_normal.x * c + last.x * st,
                             last_normal.y * c + last.y * st},
                            {-last_normal.x * c + last.x * st,
                             -last_normal.y * c + last.y * st});
    }
    return vertices;
}

} // namespace sioyek_stroke_mesh
