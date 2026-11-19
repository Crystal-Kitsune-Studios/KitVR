#pragma once

namespace kitvr {

enum class BodyTrackerMode { Unavailable, UpperBody, FullBody };

template <typename Create>
BodyTrackerMode createBodyTrackerWithFallback(bool fullBodySupported, Create create) {
    if (fullBodySupported && create(true)) return BodyTrackerMode::FullBody;
    if (create(false)) return BodyTrackerMode::UpperBody;
    return BodyTrackerMode::Unavailable;
}

}  // namespace kitvr
