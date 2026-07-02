#pragma once
#include <vector>
#include <cstdint>

namespace adapttree {

struct KeyPos {
    uint64_t key;
    uint32_t position;
};

struct LearnedSegment {
    double   slope     = 0.0;
    double   intercept = 0.0;
    uint16_t error_bound = 0;

    uint32_t predict(uint64_t key, uint32_t num_slots) const {
        double p = slope * static_cast<double>(key) + intercept;
        if (p < 0) return 0;
        uint32_t pred = static_cast<uint32_t>(p);
        return pred < num_slots ? pred : num_slots - 1;
    }
};

class PGMBuilder {
public:
    explicit PGMBuilder(uint16_t target_error) : target_error_(target_error) {}

    std::vector<LearnedSegment> fit(const std::vector<KeyPos>& sorted_points) const;

    void validate(const std::vector<KeyPos>& sorted_points,
                  const std::vector<LearnedSegment>& segments) const;

private:
    uint16_t target_error_;
};

}  // namespace adapttree
