#pragma once

#include <cstdint>
#include <optional>

namespace kitvr {

class TrackingSequence {
public:
    bool accept(uint16_t sequence) {
        if (last_) {
            const uint16_t delta = static_cast<uint16_t>(sequence - *last_);
            if (delta == 0 || delta >= 0x8000) return false;
        }
        last_ = sequence;
        return true;
    }

    void reset() { last_.reset(); }

private:
    std::optional<uint16_t> last_;
};

}  // namespace kitvr
