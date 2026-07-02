#include "adapttree/pgm_builder.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adapttree {

std::vector<LearnedSegment> PGMBuilder::fit(
    const std::vector<KeyPos>& sorted_points) const {

    std::vector<LearnedSegment> segments;
    if (sorted_points.empty()) return segments;

    size_t start = 0;
    while (start < sorted_points.size()) {
        double lower_slope = -std::numeric_limits<double>::infinity();
        double upper_slope =  std::numeric_limits<double>::infinity();
        size_t end = start;

        uint64_t origin_key = sorted_points[start].key;
        uint32_t origin_pos = sorted_points[start].position;

        for (size_t i = start; i < sorted_points.size(); ++i) {
            double dx = static_cast<double>(sorted_points[i].key) - origin_key;
            if (dx == 0) { end = i; continue; }

            double y_lo = static_cast<double>(sorted_points[i].position) - target_error_;
            double y_hi = static_cast<double>(sorted_points[i].position) + target_error_;

            double slope_lo = (y_lo - origin_pos) / dx;
            double slope_hi = (y_hi - origin_pos) / dx;

            double new_lower = std::max(lower_slope, slope_lo);
            double new_upper = std::min(upper_slope, slope_hi);

            if (new_lower > new_upper) break;

            lower_slope = new_lower;
            upper_slope = new_upper;
            end = i;
        }

        double chosen_slope = std::isinf(lower_slope) ? 0.0
                            : (lower_slope + upper_slope) / 2.0;

        LearnedSegment seg;
        seg.slope      = chosen_slope;
        seg.intercept  = static_cast<double>(origin_pos) - chosen_slope * static_cast<double>(origin_key);
        seg.error_bound = target_error_;
        segments.push_back(seg);

        start = end + 1;
    }

    return segments;
}

void PGMBuilder::validate(const std::vector<KeyPos>& sorted_points,
                           const std::vector<LearnedSegment>& segments) const {
    if (segments.empty()) return;
    for (const auto& kp : sorted_points) {
        // Use last segment (simplified: single-segment case)
        const LearnedSegment& seg = segments.back();
        double raw = seg.slope * static_cast<double>(kp.key) + seg.intercept;
        int64_t pred = static_cast<int64_t>(raw);
        int64_t diff = pred - static_cast<int64_t>(kp.position);
        if (std::abs(diff) > static_cast<int64_t>(target_error_)) {
            throw std::logic_error("PGM error bound violated for key " +
                                   std::to_string(kp.key));
        }
    }
}

}  // namespace adapttree
