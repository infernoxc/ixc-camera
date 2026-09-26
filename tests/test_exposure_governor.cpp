#include "camera/exposure_governor.h"
#include "ixc_test.h"

#include <cmath>

using namespace ixc::camera;
using A = ExposureGovernor::Action;
using S = ExposureGovernor::State;

namespace {
ExposureGovernor::Config Fast() {
    ExposureGovernor::Config c;
    c.observeFrames = 5;
    c.verifyFrames = 5;
    c.settleFrames = 0;
    c.refineFrames = 0;
    return c;
}
// Feeds n frames and returns the last non-None action.
A Feed(ExposureGovernor& g, int n, double intervalMs, double luma) {
    A last = A::None;
    for (int i = 0; i < n; ++i) {
        const A a = g.OnFrame(intervalMs, luma);
        if (a != A::None) last = a;
    }
    return last;
}
}  // namespace

IXC_TEST(Exposure_ForFpsFitsOneFrame) {
    IXC_CHECK_EQ(ExposureGovernor::ExposureForFps(30), -5);  // 31.25 ms <= 33.3 ms
    IXC_CHECK_EQ(ExposureGovernor::ExposureForFps(60), -6);  // 15.6 ms <= 16.7 ms
    IXC_CHECK_EQ(ExposureGovernor::ExposureForFps(15), -4);  // 62.5 ms <= 66.7 ms
    IXC_CHECK_EQ(ExposureGovernor::ExposureForFps(25), -5);  // 40 ms frame: 31.25 ms fits, 62.5 ms doesn't
}

IXC_TEST(Exposure_SmoothCameraIsLeftAlone) {
    ExposureGovernor g(Fast());
    g.Reset(true, 30);
    IXC_CHECK(Feed(g, 50, 33.4, 120) == A::None);
    IXC_CHECK(g.state() == S::Observing);
    IXC_CHECK(!g.ExposureChanged());
}

IXC_TEST(Exposure_DisabledNeverActs) {
    ExposureGovernor g(Fast());
    g.Reset(false, 30);
    IXC_CHECK(Feed(g, 50, 50.0, 100) == A::None);
    IXC_CHECK(g.state() == S::Disabled);
}

IXC_TEST(Exposure_SlowCameraGetsFixedExposureAndCompensation) {
    ExposureGovernor g(Fast());
    g.Reset(true, 30);
    IXC_CHECK(Feed(g, 5, 50.0, 80) == A::SetManualExposure);  // 20 FPS on a 30 FPS mode
    IXC_CHECK_EQ(g.RequestedExposure(), -5);
    g.OnExposureApplied(true);
    IXC_CHECK(g.ExposureChanged());
    IXC_CHECK(Feed(g, 5, 33.3, 40) == A::None);               // 30 FPS, half as bright
    IXC_CHECK(g.state() == S::Locked);
    IXC_CHECK(std::abs(g.CompensationEv() - 1.0) < 1e-9);      // +1 EV restores the brightness
}

IXC_TEST(Exposure_CompensationIsCapped) {
    ExposureGovernor g(Fast());
    g.Reset(true, 30);
    Feed(g, 5, 50.0, 200);
    g.OnExposureApplied(true);
    Feed(g, 5, 33.3, 10);  // 20x darker
    IXC_CHECK_EQ(g.CompensationEv(), 1.5);
}

IXC_TEST(Exposure_CameraThatStaysSlowIsRestored) {
    ExposureGovernor g(Fast());
    g.Reset(true, 30);
    Feed(g, 5, 50.0, 80);
    g.OnExposureApplied(true);
    IXC_CHECK(Feed(g, 5, 50.0, 60) == A::RestoreAutoExposure);
    IXC_CHECK(g.state() == S::Disabled);
    IXC_CHECK_EQ(g.CompensationEv(), 0.0);
    IXC_CHECK(Feed(g, 20, 50.0, 60) == A::None);  // gives up for the session
}

IXC_TEST(Exposure_UnsupportedControlGivesUp) {
    ExposureGovernor g(Fast());
    g.Reset(true, 30);
    Feed(g, 5, 50.0, 80);
    g.OnExposureApplied(false);
    IXC_CHECK(g.state() == S::Disabled);
    IXC_CHECK(!g.ExposureChanged());  // nothing to restore
}

IXC_TEST(Exposure_SettleFramesAreIgnoredAndBrightnessIsRefined) {
    ExposureGovernor::Config c = Fast();
    c.settleFrames = 3;
    c.refineFrames = 5;
    c.refineRounds = 2;
    ExposureGovernor g(c);
    g.Reset(true, 30);
    Feed(g, 3 + 5, 50.0, 80);                 // 3 start-up frames ignored, then observed
    g.OnExposureApplied(true);
    Feed(g, 3, 60.0, 80);                     // transition frames: slow and still bright, ignored
    IXC_CHECK(Feed(g, 5, 33.3, 40) == A::None);
    IXC_CHECK(g.state() == S::Refining);
    IXC_CHECK(std::abs(g.CompensationEv() - 1.0) < 1e-9);
    Feed(g, 5, 33.3, 20);                     // camera gain kept dropping
    IXC_CHECK_EQ(g.CompensationEv(), 1.5);
    Feed(g, 5, 33.3, 40);
    IXC_CHECK(g.state() == S::Locked);
    IXC_CHECK(std::abs(g.CompensationEv() - 1.0) < 1e-9);
}

IXC_TEST(Exposure_RememberedDecisionAppliesImmediately) {
    ExposureGovernor g(Fast());
    g.Reset(true, 30);
    Feed(g, 5, 50.0, 80);
    g.OnExposureApplied(true);
    Feed(g, 5, 33.3, 40);
    const auto hint = g.CurrentHint();
    IXC_CHECK(hint.valid);

    ExposureGovernor next(Fast());
    next.Reset(true, 30, hint);
    IXC_CHECK(next.OnFrame(0, 40) == A::SetManualExposure);  // first frame, no observation delay
    next.OnExposureApplied(true);
    IXC_CHECK(std::abs(next.CompensationEv() - 1.0) < 1e-9);
    IXC_CHECK(Feed(next, 5, 33.3, 40) == A::None);
    IXC_CHECK(next.state() == S::Locked);

    ExposureGovernor other(Fast());
    other.Reset(true, 60, hint);                               // different frame rate: not reused
    IXC_CHECK(other.state() == S::Observing);
}

IXC_TEST(Exposure_StaleRememberedDecisionFallsBackToObserving) {
    ExposureGovernor::Hint hint;
    hint.valid = true;
    hint.fps = 30;
    hint.exposure = -5;
    hint.compensationEv = 1.0;
    ExposureGovernor g(Fast());
    g.Reset(true, 30, hint);
    IXC_CHECK(g.OnFrame(0, 40) == A::SetManualExposure);
    g.OnExposureApplied(true);
    IXC_CHECK(Feed(g, 5, 66.0, 10) == A::RestoreAutoExposure);  // too dark now: camera stays slow
    IXC_CHECK(g.state() == S::Observing);
    IXC_CHECK_EQ(g.CompensationEv(), 0.0);
    IXC_CHECK(!g.ExposureChanged());
}

IXC_TEST(Exposure_WatchdogReassertsWhenAnotherAppRestoresAuto) {
    ExposureGovernor::Config c = Fast();
    c.watchFrames = 5;
    ExposureGovernor g(c);
    g.Reset(true, 30);
    Feed(g, 5, 50.0, 80);
    g.OnExposureApplied(true);
    Feed(g, 5, 33.3, 40);
    IXC_CHECK(g.state() == S::Locked);
    IXC_CHECK(Feed(g, 20, 33.3, 40) == A::None);                // steady: nothing to do
    IXC_CHECK(Feed(g, 5, 50.0, 80) == A::SetManualExposure);    // auto exposure came back
    IXC_CHECK_EQ(g.Reasserts(), 1);
    g.OnExposureApplied(true);
    IXC_CHECK(g.state() == S::Locked);
    IXC_CHECK(std::abs(g.CompensationEv() - 1.0) < 1e-9);       // brightness decision unchanged
    IXC_CHECK(g.ExposureChanged());
}
