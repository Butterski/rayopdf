// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/gfx/path.cpp at 9fbbfaf (Apache-2.0, same author).
#include "raster/path.h"

#include <cmath>

namespace RayoPdf::Raster {

bool Path::Room(size_t points) {
    if (points_.size() + points <= kMaxPathPoints) return true;
    overflowed_ = true;
    return false;
}

void Path::MoveTo(float x, float y) {
    if (!Room(1)) return;
    verbs_.push_back(Verb::MoveTo);
    points_.push_back({x, y});
}

void Path::LineTo(float x, float y) {
    if (!Room(1)) return;
    verbs_.push_back(Verb::LineTo);
    points_.push_back({x, y});
}

void Path::QuadTo(float cx, float cy, float x, float y) {
    if (!Room(2)) return;
    verbs_.push_back(Verb::QuadTo);
    points_.push_back({cx, cy});
    points_.push_back({x, y});
}

void Path::CubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!Room(3)) return;
    verbs_.push_back(Verb::CubicTo);
    points_.push_back({c1x, c1y});
    points_.push_back({c2x, c2y});
    points_.push_back({x, y});
}

void Path::Close() {
    if (overflowed_) return;
    verbs_.push_back(Verb::Close);
}

void Path::AddRect(float x, float y, float w, float h) {
    MoveTo(x, y);
    LineTo(x + w, y);
    LineTo(x + w, y + h);
    LineTo(x, y + h);
    Close();
}

void Path::Append(const Path& other, const Matrix& m) {
    if (!Room(other.points_.size())) return;
    verbs_.insert(verbs_.end(), other.verbs_.begin(), other.verbs_.end());
    for (const Point& p : other.points_) {
        double x, y;
        m.Apply(p.x, p.y, x, y);
        points_.push_back({static_cast<float>(x), static_cast<float>(y)});
    }
}

void Path::Clear() {
    verbs_.clear();
    points_.clear();
    overflowed_ = false;
}

bool Path::Bounds(float& x0, float& y0, float& x1, float& y1) const {
    if (points_.empty()) return false;
    x0 = x1 = points_[0].x;
    y0 = y1 = points_[0].y;
    for (const Point& p : points_) {
        x0 = p.x < x0 ? p.x : x0;
        y0 = p.y < y0 ? p.y : y0;
        x1 = p.x > x1 ? p.x : x1;
        y1 = p.y > y1 ? p.y : y1;
    }
    return true;
}

namespace {

struct Pt {
    double x;
    double y;
};

inline Pt Transform(const Matrix& m, Point p) {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}

inline double Length(double x, double y) {
    return std::sqrt(x * x + y * y);
}

// ceil(q) clamped to [1, 256]; NaN gives 1.
inline int Pieces(double q) {
    if (!(q > 1)) return 1;
    if (q >= 256) return 256;
    const int whole = static_cast<int>(q);
    return whole < q ? whole + 1 : whole;
}

struct Cull {
    double x0, y0, x1, y1;
    // True when the n points all lie beyond one edge.
    bool Outside(const Pt* p, int n) const {
        bool left = true, right = true, above = true, below = true;
        for (int i = 0; i < n; ++i) {
            left = left && p[i].x < x0;
            right = right && p[i].x > x1;
            above = above && p[i].y < y0;
            below = below && p[i].y > y1;
        }
        return left || right || above || below;
    }
};

class Walker {
public:
    Walker(LineSink& sink, double tolerance) : sink_(sink), tol_(tolerance > 1e-6 ? tolerance : 1e-6) {}

    void Begin(Pt p) { first_ = last_ = p; }
    void To(Pt p) {
        sink_.Line(last_.x, last_.y, p.x, p.y);
        last_ = p;
    }
    // Ends a subpath: back to its start, with the closing segment when `close`.
    void End(bool close = true) {
        if (!close) {
            last_ = first_;
            return;
        }
        if (last_.x != first_.x || last_.y != first_.y) sink_.Line(last_.x, last_.y, first_.x, first_.y);
        last_ = first_;
    }
    void Quad(Pt p0, Pt p1, Pt p2) {
        const double dd = Length(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
        const int n = Pieces(std::sqrt(dd / (4 * tol_)));
        for (int i = 1; i < n; ++i) {
            const double t = static_cast<double>(i) / n, u = 1 - t;
            const double w0 = u * u, w1 = 2 * u * t, w2 = t * t;
            To({w0 * p0.x + w1 * p1.x + w2 * p2.x, w0 * p0.y + w1 * p1.y + w2 * p2.y});
        }
        To(p2);
    }
    void Cubic(Pt p0, Pt p1, Pt p2, Pt p3) {
        const double d1 = Length(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
        const double d2 = Length(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y);
        const int n = Pieces(std::sqrt(0.75 * (d1 > d2 ? d1 : d2) / tol_));
        for (int i = 1; i < n; ++i) {
            const double t = static_cast<double>(i) / n, u = 1 - t;
            const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
            To({w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x, w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y});
        }
        To(p3);
    }
    Pt Last() const { return last_; }

private:
    LineSink& sink_;
    double tol_;
    Pt first_{0, 0}, last_{0, 0};
};

} // namespace

void Flatten(const Path& path, const Matrix& m, float tolerance, double x0, double y0, double x1, double y1,
             LineSink& sink, bool closeOpen) {
    const Cull cull{x0, y0, x1, y1};
    const std::vector<Point>& points = path.Points();
    Walker walker(sink, tolerance);
    size_t pi = 0;
    bool open = false;
    Pt start = Transform(m, {0, 0});
    walker.Begin(start);
    const auto ensureOpen = [&] {
        if (!open) {
            walker.Begin(walker.Last());
            open = true;
        }
    };
    for (const Verb verb : path.Verbs()) {
        switch (verb) {
        case Verb::MoveTo:
            if (pi + 1 > points.size()) return;
            if (open) walker.End(closeOpen);
            start = Transform(m, points[pi++]);
            walker.Begin(start);
            open = true;
            break;
        case Verb::LineTo:
            if (pi + 1 > points.size()) return;
            ensureOpen();
            walker.To(Transform(m, points[pi++]));
            break;
        case Verb::QuadTo: {
            if (pi + 2 > points.size()) return;
            ensureOpen();
            const Pt p[3] = {walker.Last(), Transform(m, points[pi]), Transform(m, points[pi + 1])};
            pi += 2;
            if (cull.Outside(p, 3))
                walker.To(p[2]);
            else
                walker.Quad(p[0], p[1], p[2]);
            break;
        }
        case Verb::CubicTo: {
            if (pi + 3 > points.size()) return;
            ensureOpen();
            const Pt p[4] = {walker.Last(), Transform(m, points[pi]), Transform(m, points[pi + 1]),
                             Transform(m, points[pi + 2])};
            pi += 3;
            if (cull.Outside(p, 4))
                walker.To(p[3]);
            else
                walker.Cubic(p[0], p[1], p[2], p[3]);
            break;
        }
        case Verb::Close:
            if (open) {
                walker.End();
                open = false;
            }
            break;
        }
    }
    if (open) walker.End(closeOpen);
}

} // namespace RayoPdf::Raster
