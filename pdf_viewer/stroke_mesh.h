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
                // Keep the most recent pressure/thickness at a repeated point.
                points.back() = p;
                continue;
            }
        }
        points.push_back(p);
    }
    if (points.size() < 2) return {};

    std::vector<Vec2> directions;
    directions.reserve(points.size() - 1);
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        directions.push_back(direction(points[i], points[i + 1],
                                       aspect_x, aspect_y));
    }

    // Exactly two vertices per unique sample, in GL_TRIANGLE_STRIP order.
    // At corners use the angle bisector to avoid overlapping / folded quads.
    // The miter is capped to prevent spikes on tight bends.
    std::vector<float> vertices;
    vertices.reserve(points.size() * 4);
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec2 before = directions[i == 0 ? 0 : i - 1];
        const Vec2 after = directions[i == directions.size()
                                         ? directions.size() - 1 : i];
        Vec2 normal = {-after.y, after.x};
        float miter_scale = 1.0f;
        if (i != 0 && i + 1 < points.size()) {
            const float nx = -before.y - after.y;
            const float ny = before.x + after.x;
            const float norm = std::hypot(nx, ny);
            if (norm > 1e-4f) {
                normal = {nx / norm, ny / norm};
                const float projected = normal.x * (-after.y) +
                                        normal.y * after.x;
                if (projected > 1e-4f)
                    miter_scale = std::min(2.0f, 1.0f / projected);
            }
        }
        const float r = std::max(0.0f, points[i].thickness) * miter_scale;
        const float ox = normal.x * r * radius_scale_x;
        const float oy = normal.y * r * radius_scale_y;
        vertices.insert(vertices.end(), {
            points[i].x - ox, points[i].y - oy,
            points[i].x + ox, points[i].y + oy
        });
    }
    return vertices;
}

} // namespace sioyek_stroke_mesh
