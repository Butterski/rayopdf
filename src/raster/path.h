// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/gfx/path.h and geometry.h at 9fbbfaf (Apache-2.0, same author).
// Paths of lines and Bézier curves (glyph outlines, Type 3 glyph procedures, vector-drawn text)
// and their flattening into line segments, for the rasterizer of raster.h. A path holds at most
// kMaxPathPoints points; what comes beyond is dropped and Overflowed() says so (paths come from
// untrusted fonts and content streams).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RayoPdf::Raster {

struct Point {
    float x = 0;
    float y = 0;
};

// An affine transform in PDF's order: x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    // This transform, then `then`.
    Matrix Then(const Matrix& then) const {
        return {a * then.a + b * then.c,         a * then.b + b * then.d,
                c * then.a + d * then.c,         c * then.b + d * then.d,
                e * then.a + f * then.c + then.e, e * then.b + f * then.d + then.f};
    }
    void Apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
};

enum class Verb : uint8_t {
    MoveTo,   // 1 point
    LineTo,   // 1 point
    QuadTo,   // 2 points: control, end
    CubicTo,  // 3 points: control 1, control 2, end
    Close,    // 0 points
};

constexpr size_t kMaxPathPoints = size_t(1) << 20;

class Path {
public:
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void QuadTo(float cx, float cy, float x, float y);
    void CubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void Close();
    // A closed rectangle.
    void AddRect(float x, float y, float w, float h);
    // `other` transformed by `m`, appended.
    void Append(const Path& other, const Matrix& m);

    void Clear();
    bool Empty() const { return verbs_.empty(); }
    bool Overflowed() const { return overflowed_; }
    const std::vector<Verb>& Verbs() const { return verbs_; }
    const std::vector<Point>& Points() const { return points_; }
    // The bounds of every point, control points included; false for a path without points.
    bool Bounds(float& x0, float& y0, float& x1, float& y1) const;

private:
    bool Room(size_t points);

    std::vector<Verb> verbs_;
    std::vector<Point> points_;
    bool overflowed_ = false;
};

// Receives device-space line segments.
class LineSink {
public:
    virtual void Line(double x0, double y0, double x1, double y1) = 0;

protected:
    ~LineSink() = default;
};

// The default flattening tolerance, in device pixels.
constexpr float kFlattenTolerance = 0.2f;
// Emits `path` transformed by `m` as line segments, closing every subpath (a fill's implicit
// close) unless `closeOpen` is false (a stroke: only subpaths ended by Close are closed). A
// quadratic is cut into n = ceil(sqrt(|p0 - 2p1 + p2| / (4 tol))) pieces, a cubic into
// n = ceil(sqrt(0.75 * max(|p0 - 2p1 + p2|, |p1 - 2p2 + p3|) / tol)), n clamped to [1, 256], the
// control points measured after the transform. A curve whose control points all lie beyond one
// edge of the device rectangle [x0, x1] x [y0, y1] is emitted as its chord, which covers that
// rectangle the same.
void Flatten(const Path& path, const Matrix& m, float tolerance, double x0, double y0, double x1, double y1,
             LineSink& sink, bool closeOpen = true);

} // namespace RayoPdf::Raster
