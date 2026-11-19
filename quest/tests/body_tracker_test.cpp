#include "body_tracker.h"

#include <vector>

int main() {
    std::vector<bool> attempts;
    auto full = [&](bool requestedFull) {
        attempts.push_back(requestedFull);
        return true;
    };
    if (kitvr::createBodyTrackerWithFallback(true, full) != kitvr::BodyTrackerMode::FullBody ||
        attempts != std::vector<bool>{true}) return 1;

    attempts.clear();
    auto upperOnly = [&](bool requestedFull) {
        attempts.push_back(requestedFull);
        return !requestedFull;
    };
    if (kitvr::createBodyTrackerWithFallback(true, upperOnly) != kitvr::BodyTrackerMode::UpperBody ||
        attempts != std::vector<bool>({true, false})) return 2;
    attempts.clear();
    if (kitvr::createBodyTrackerWithFallback(false, upperOnly) != kitvr::BodyTrackerMode::UpperBody ||
        attempts != std::vector<bool>{false}) return 3;

    attempts.clear();
    auto unavailable = [&](bool requestedFull) {
        attempts.push_back(requestedFull);
        return false;
    };
    if (kitvr::createBodyTrackerWithFallback(true, unavailable) != kitvr::BodyTrackerMode::Unavailable ||
        attempts != std::vector<bool>({true, false})) return 4;
}
