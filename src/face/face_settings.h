#pragma once

// Profile → face engine settings (shared by the IXC Camera source and the app preview).

#include "face/face_engine.h"
#include "profiles/profile.h"

namespace ixc::face {

// CPU budget for detection, as a fraction of ONE core. Lower tiers leave more room for the
// video on weak PCs; the cadence then lowers the rate or the input size, or turns tracking off.
inline double BudgetForTier(PerformanceTier t) {
    switch (t) {
        case PerformanceTier::UltraLow: return 0.06;
        case PerformanceTier::Low: return 0.08;
        case PerformanceTier::High: return 0.15;
        case PerformanceTier::Balanced:
        case PerformanceTier::Auto: return 0.10;
    }
    return 0.10;
}

inline EngineConfig EngineConfigFor(const Profile& p) {
    EngineConfig c;
    c.maxFaces = p.faceTracking.maxFaces;
    c.fixedIntervalFrames = p.faceTracking.detectionIntervalFrames;
    c.cpuBudget = BudgetForTier(p.tier);
    return c;
}

inline bool SameEngineConfig(const EngineConfig& a, const EngineConfig& b) {
    return a.maxFaces == b.maxFaces && a.fixedIntervalFrames == b.fixedIntervalFrames && a.cpuBudget == b.cpuBudget &&
           a.minConfidence == b.minConfidence && a.forcePortable == b.forcePortable;
}

}  // namespace ixc::face
