// f4-math/table.hpp
//
// 1-D and 2-D lookup tables with bilinear interpolation and cached
// breakpoint indices for speed.
//
// Direct ports of OnedInterp() and TwodInterp() from FreeFalcon's
// simlib/math.cpp, with three behavioural improvements over the originals:
//
//   1. Boundary policy is explicit (Clamp / Error / Extrapolate) rather
//      than always clamping. FF always clamps, but callers sometimes need
//      to detect out-of-range inputs (e.g. validation) or extrapolate
//      (e.g. as-thinly-sampled aero tables at the edges).
//
//   2. The cached last-index hint is preserved (this is the perf-critical
//      optimization for 240 Hz aero lookups where Mach/alpha advance
//      smoothly frame-to-frame). The cache uses std::atomic with relaxed
//      ordering so concurrent reads on a shared table (e.g. multiple AI
//      aircraft sharing an AircraftConfig's aero tables) are race-free.
//      Relaxed atomics are ~1ns on x86, preserving the cache's perf benefit
//      while making the const operator() thread-safe by the C++ memory model.
//
//   3. The original FF TwoDimensionTable::Lookup (falclib/lookuptable.cpp)
//      has a bug: the fraction computation uses `breakPoint[t]` for both
//      the low and high bracket, so fraction is always 0 and the lookup
//      silently returns the lower-left corner. This implementation does
//      the correct bilinear interpolation that FF intended.
//
// Template parameters: any arithmetic type (double, float, int) or any
// type that supports the comparison + subtraction + multiplication operators
// (e.g. f4-units Quantity<Angle, Degrees>). This is what makes the table
// compose cleanly with the type system in the host library.

#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <vector>

namespace f4::math {

// ============================================================================
// BoundaryMode — what to do when the query point is outside the breakpoint
// range. Each table carries its own mode.
// ============================================================================
enum class BoundaryMode : unsigned char {
    Clamp,        // Return the value at the nearest breakpoint (FF default).
    Error,        // Throw std::out_of_range. Use for validation / detection.
    Extrapolate,  // Continue the linear/bilinear slope beyond the edges.
};

// ============================================================================
// Concept: the element types must support the arithmetic we use for
// interpolation. We don't require f4-units here; raw doubles satisfy this.
// ============================================================================
template<typename T>
concept Interpolable = requires(T a, T b, double t) {
    { a + (b - a) * t } -> std::convertible_to<T>;
    { a < b } -> std::convertible_to<bool>;
};

// ============================================================================
// Table1D — 1-D linear interpolation.
//
// Layout: two parallel vectors x_[i] and y_[i]. The x_ axis MUST be sorted
// ascending; this is checked at construction time. Lookups use a cached
// last index (mutable atomic, mutated through const operator()) for fast
// smooth-frame-to-frame access. The atomic is race-free under concurrent
// reads; the cache is only a hint, so a racy write simply causes the next
// call to do a full scan (still correct).
// ============================================================================
template<typename X, typename Y>
    requires Interpolable<X> && Interpolable<Y>
class Table1D {
    std::vector<X> x_;
    std::vector<Y> y_;
    BoundaryMode mode_ = BoundaryMode::Clamp;
    mutable std::atomic<std::size_t> last_{0};

public:
    Table1D() = default;

    Table1D(std::vector<X> x, std::vector<Y> y, BoundaryMode mode = BoundaryMode::Clamp)
        : x_(std::move(x)), y_(std::move(y)), mode_(mode) {
        if (x_.size() != y_.size()) {
            throw std::invalid_argument("Table1D: x and y must have equal size");
        }
        if (x_.size() < 2) {
            throw std::invalid_argument("Table1D: need at least 2 breakpoints");
        }
        if (!std::is_sorted(x_.begin(), x_.end())) {
            throw std::invalid_argument("Table1D: x breakpoints must be sorted ascending");
        }
    }

    // Note: std::atomic is non-copyable, so we define explicit copy/move
    // that load/store the cache via relaxed atomics. The cache value itself
    // is just a hint, so any value (including stale) is acceptable.
    Table1D(const Table1D& other)
        : x_(other.x_), y_(other.y_), mode_(other.mode_),
          last_(other.last_.load(std::memory_order_relaxed)) {}

    Table1D& operator=(const Table1D& other) {
        if (this != &other) {
            x_ = other.x_;
            y_ = other.y_;
            mode_ = other.mode_;
            last_.store(other.last_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
        }
        return *this;
    }

    Table1D(Table1D&& other) noexcept
        : x_(std::move(other.x_)), y_(std::move(other.y_)), mode_(other.mode_),
          last_(other.last_.load(std::memory_order_relaxed)) {}

    Table1D& operator=(Table1D&& other) noexcept {
        if (this != &other) {
            x_ = std::move(other.x_);
            y_ = std::move(other.y_);
            mode_ = other.mode_;
            last_.store(other.last_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
        }
        return *this;
    }

    // Lookup y(x) via linear interpolation. Uses cached last index for speed.
    Y operator()(X query) const {
        const std::size_t n = x_.size();

        // --- Below lower bound (strictly less than) ---
        if (query < x_[0]) {
            if (mode_ == BoundaryMode::Error)
                throw std::out_of_range("Table1D: query below lower bound");
            if (mode_ == BoundaryMode::Clamp)
                return y_[0];
            // Extrapolate: fall through with index 0
            return extrapolate(query, 0);
        }
        // --- At or above upper bound ---
        if (query >= x_[n - 1]) {
            if (mode_ == BoundaryMode::Error && query > x_[n - 1])
                throw std::out_of_range("Table1D: query above upper bound");
            if (mode_ == BoundaryMode::Clamp)
                return y_[n - 1];
            if (query == x_[n - 1])
                return y_[n - 1];  // exact boundary, no extrapolation needed
            // Extrapolate
            return extrapolate(query, n - 2);
        }

        // --- Interior: find bracket using cached hint ---
        std::size_t i = last_.load(std::memory_order_relaxed);
        if (i >= n - 1) i = 0;

        if (query < x_[i]) {
            // Scan backward
            while (i > 0 && query < x_[i]) --i;
        } else {
            // Scan forward
            while (i < n - 2 && query >= x_[i + 1]) ++i;
        }
        last_.store(i, std::memory_order_relaxed);

        const double t = static_cast<double>(query - x_[i]) /
                         static_cast<double>(x_[i + 1] - x_[i]);
        return static_cast<Y>(y_[i] + (y_[i + 1] - y_[i]) * t);
    }

    [[nodiscard]] std::size_t size() const noexcept { return x_.size(); }
    [[nodiscard]] const std::vector<X>& breakpoints() const noexcept { return x_; }
    [[nodiscard]] const std::vector<Y>& values() const noexcept { return y_; }
    [[nodiscard]] BoundaryMode boundary_mode() const noexcept { return mode_; }

private:
    Y extrapolate(X query, std::size_t edge_idx) const {
        // edge_idx is the bracket whose slope we extend.
        const double t = static_cast<double>(query - x_[edge_idx]) /
                         static_cast<double>(x_[edge_idx + 1] - x_[edge_idx]);
        return static_cast<Y>(y_[edge_idx] + (y_[edge_idx + 1] - y_[edge_idx]) * t);
    }
};

// ============================================================================
// Table2D — 2-D bilinear interpolation.
//
// Layout: row-major flat array data_[row * num_cols + col], matching
// FreeFalcon's `data[x * axis[0].breakPointCount + y]` convention.
// This is cache-friendly for the typical "outer axis changes slowly"
// access pattern (Mach × alpha aero tables).
//
// Both axes must be sorted ascending.
// ============================================================================
template<typename RowX, typename ColX, typename Z>
    requires Interpolable<RowX> && Interpolable<ColX> && Interpolable<Z>
class Table2D {
    std::vector<RowX> rows_;       // outer (slow) axis, e.g. Mach
    std::vector<ColX> cols_;       // inner (fast) axis, e.g. alpha
    std::vector<Z>    data_;       // data_[row * num_cols + col]
    BoundaryMode mode_ = BoundaryMode::Clamp;
    // Atomic relaxed — see Table1D comment. Race-free under concurrent reads.
    mutable std::atomic<std::size_t> last_row_{0};
    mutable std::atomic<std::size_t> last_col_{0};

public:
    Table2D() = default;

    Table2D(std::vector<RowX> rows, std::vector<ColX> cols, std::vector<std::vector<Z>> data,
            BoundaryMode mode = BoundaryMode::Clamp)
        : rows_(std::move(rows)), cols_(std::move(cols)), mode_(mode) {
        if (rows_.empty() || cols_.empty()) {
            throw std::invalid_argument("Table2D: empty breakpoints");
        }
        if (data.size() != rows_.size()) {
            throw std::invalid_argument("Table2D: row count mismatch");
        }
        // Flatten to row-major
        data_.reserve(rows_.size() * cols_.size());
        for (const auto& r : data) {
            if (r.size() != cols_.size()) {
                throw std::invalid_argument("Table2D: column count mismatch");
            }
            data_.insert(data_.end(), r.begin(), r.end());
        }
        validate_axes();
    }

    // Convenience constructor for the common case where the data is already
    // a flat row-major vector (e.g. loaded directly from a .dat file or JSON).
    // Uses FlatDataTag to disambiguate from the nested-vector constructor
    // (otherwise single-row tables are ambiguous).
    struct FlatDataTag {};
    Table2D(std::vector<RowX> rows, std::vector<ColX> cols, std::vector<Z> flat_data,
            FlatDataTag /*tag*/, BoundaryMode mode = BoundaryMode::Clamp)
        : rows_(std::move(rows)), cols_(std::move(cols)), data_(std::move(flat_data)), mode_(mode) {
        if (rows_.empty() || cols_.empty()) {
            throw std::invalid_argument("Table2D: empty breakpoints");
        }
        if (data_.size() != rows_.size() * cols_.size()) {
            throw std::invalid_argument("Table2D: flat data size mismatch");
        }
        validate_axes();
    }

    // Explicit copy/move — std::atomic is non-copyable.
    Table2D(const Table2D& other)
        : rows_(other.rows_), cols_(other.cols_), data_(other.data_), mode_(other.mode_),
          last_row_(other.last_row_.load(std::memory_order_relaxed)),
          last_col_(other.last_col_.load(std::memory_order_relaxed)) {}

    Table2D& operator=(const Table2D& other) {
        if (this != &other) {
            rows_ = other.rows_;
            cols_ = other.cols_;
            data_ = other.data_;
            mode_ = other.mode_;
            last_row_.store(other.last_row_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
            last_col_.store(other.last_col_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        }
        return *this;
    }

    Table2D(Table2D&& other) noexcept
        : rows_(std::move(other.rows_)), cols_(std::move(other.cols_)),
          data_(std::move(other.data_)), mode_(other.mode_),
          last_row_(other.last_row_.load(std::memory_order_relaxed)),
          last_col_(other.last_col_.load(std::memory_order_relaxed)) {}

    Table2D& operator=(Table2D&& other) noexcept {
        if (this != &other) {
            rows_ = std::move(other.rows_);
            cols_ = std::move(other.cols_);
            data_ = std::move(other.data_);
            mode_ = other.mode_;
            last_row_.store(other.last_row_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
            last_col_.store(other.last_col_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        }
        return *this;
    }

    // Lookup z(row_query, col_query) via bilinear interpolation.
    Z operator()(RowX row_query, ColX col_query) const {
        const std::size_t nr = rows_.size();
        const std::size_t nc = cols_.size();

        if (nr == 1 && nc == 1) return data_[0];

        // --- Resolve the row bracket [r0, r1] and fractional tr ---
        std::size_t r0, r1;
        double tr;
        std::size_t row_hint = last_row_.load(std::memory_order_relaxed);
        [[maybe_unused]] bool row_clamped =
            resolve_axis(rows_, row_query, row_hint, r0, r1, tr, "row");
        last_row_.store(row_hint, std::memory_order_relaxed);

        // --- Resolve the col bracket [c0, c1] and fractional tc ---
        std::size_t c0, c1;
        double tc;
        std::size_t col_hint = last_col_.load(std::memory_order_relaxed);
        [[maybe_unused]] bool col_clamped =
            resolve_axis(cols_, col_query, col_hint, c0, c1, tc, "col");
        last_col_.store(col_hint, std::memory_order_relaxed);

        // If both axes were clamped AND we're in Error mode, the resolver
        // already threw. If one axis is clamped in Clamp mode, we still want
        // the corner value; bilinear naturally degenerates correctly when
        // tr or tc is 0.
        if (mode_ == BoundaryMode::Extrapolate) {
            // Allow tr/tc outside [0,1] — they were already set by resolve_axis
            // using extrapolate semantics.
        } else {
            // Clamp mode: ensure tr/tc are in [0,1]
            if (tr < 0.0) tr = 0.0;
            if (tr > 1.0) tr = 1.0;
            if (tc < 0.0) tc = 0.0;
            if (tc > 1.0) tc = 1.0;
        }

        // Four corner values
        const Z v00 = data_[r0 * nc + c0];
        const Z v10 = (r1 < nr) ? data_[r1 * nc + c0] : v00;
        const Z v01 = (c1 < nc) ? data_[r0 * nc + c1] : v00;
        const Z v11 = (r1 < nr && c1 < nc) ? data_[r1 * nc + c1] : v00;

        const Z a = v00 + (v10 - v00) * tr;
        const Z b = v01 + (v11 - v01) * tr;
        return a + (b - a) * tc;
    }

    [[nodiscard]] std::size_t num_rows() const noexcept { return rows_.size(); }
    [[nodiscard]] std::size_t num_cols() const noexcept { return cols_.size(); }
    [[nodiscard]] const std::vector<RowX>& row_values() const noexcept { return rows_; }
    [[nodiscard]] const std::vector<ColX>& col_values() const noexcept { return cols_; }
    [[nodiscard]] const std::vector<Z>& flat_data() const noexcept { return data_; }
    [[nodiscard]] BoundaryMode boundary_mode() const noexcept { return mode_; }

private:
    void validate_axes() const {
        if (!std::is_sorted(rows_.begin(), rows_.end())) {
            throw std::invalid_argument("Table2D: row axis must be sorted ascending");
        }
        if (!std::is_sorted(cols_.begin(), cols_.end())) {
            throw std::invalid_argument("Table2D: col axis must be sorted ascending");
        }
    }

    // Find the bracket [lo, lo+1] for query in axis, using cached hint.
    // Returns:
    //   lo, hi(=lo+1 unless at the very top), fractional position t.
    //   clamped=true if query was outside [axis.front(), axis.back()].
    // In Extrapolate mode, t may be <0 or >1.
    // In Error mode, throws std::out_of_range.
    template<typename T>
    bool resolve_axis(const std::vector<T>& axis, T query, std::size_t& hint,
                      std::size_t& lo, std::size_t& hi, double& t,
                      const char* name) const {
        const std::size_t n = axis.size();
        if (n == 1) {
            lo = 0; hi = 0; t = 0.0;
            if (query != axis[0] && mode_ == BoundaryMode::Error) {
                throw std::out_of_range(std::string("Table2D: ") + name + " axis has single point");
            }
            return false;
        }

        if (query <= axis[0]) {
            lo = 0; hi = 1;
            t = static_cast<double>(query - axis[0]) / static_cast<double>(axis[1] - axis[0]);
            if (mode_ == BoundaryMode::Error && query < axis[0]) {
                throw std::out_of_range(std::string("Table2D: ") + name + " query below range");
            }
            return query < axis[0];
        }
        if (query >= axis[n - 1]) {
            lo = n - 2; hi = n - 1;
            t = static_cast<double>(query - axis[lo]) / static_cast<double>(axis[hi] - axis[lo]);
            if (mode_ == BoundaryMode::Error && query > axis[n - 1]) {
                throw std::out_of_range(std::string("Table2D: ") + name + " query above range");
            }
            return query > axis[n - 1];
        }

        // Interior: search from cached hint.
        std::size_t i = hint;
        if (i >= n - 1) i = 0;
        if (query < axis[i]) {
            while (i > 0 && query < axis[i]) --i;
        } else {
            while (i < n - 2 && query >= axis[i + 1]) ++i;
        }
        hint = i;
        lo = i; hi = i + 1;
        t = static_cast<double>(query - axis[lo]) / static_cast<double>(axis[hi] - axis[lo]);
        return false;
    }
};

} // namespace f4::math
