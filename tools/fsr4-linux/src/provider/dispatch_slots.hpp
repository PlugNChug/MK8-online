#pragma once

#include <array>
#include <cstdint>

namespace fsr4_412 {

using SubmissionValues = std::array<uint64_t, 3>;

inline int availableSlot(const SubmissionValues& submissions, uint64_t completed) {
    for (unsigned i = 0; i < submissions.size(); ++i)
        if (submissions[i] <= completed) return static_cast<int>(i);
    return -1;
}

inline bool allSlotsComplete(const SubmissionValues& submissions, uint64_t completed) {
    for (uint64_t value : submissions)
        if (value > completed) return false;
    return true;
}

} // namespace fsr4_412
