#if WITH_DEV_AUTOMATION_TESTS
#include "BskLeRobotSampling.h"
#include "Misc/AutomationTest.h"
#include "BskTcpReceiver.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBskLeRobotSamplingTest, "BskUnreal.Dataset.LeRobotSampling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBskLeRobotSamplingTest::RunTest(const FString& Parameters)
{
    int64 Tick = -1;
    TestTrue(TEXT("100ms is a 10Hz sample"), BskLeRobotSample(100000000, 10.0, Tick));
    TestEqual(TEXT("global sample index"), Tick, static_cast<int64>(1));
    TestFalse(TEXT("preview/intermediate frame is not a sample"), BskLeRobotSample(33333333, 10.0, Tick));
    TestTrue(TEXT("integration rounding"), BskLeRobotSample(100000001, 10.0, Tick));
    TestFalse(TEXT("30Hz not exact on 2ms dynamics"), BskLeRobotSample(33333333, 30.0, Tick));
    TestFalse(TEXT("24Hz cannot be sampled uniformly from 30Hz"), BskLeRobotSample(1000000000, 24.0, Tick));
    TestFalse(TEXT("fractional FPS is unsupported"), BskLeRobotSample(1000000000, 9.5, Tick));
    TestFalse(TEXT("negative time"), BskLeRobotSample(-1, 10.0, Tick));
    TestTrue(TEXT("reset starts at sample zero"), BskLeRobotSample(0, 10.0, Tick));
    TestEqual(TEXT("reset index"), Tick, static_cast<int64>(0));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBskReliableFramesTest, "BskUnreal.Dataset.ReliableFrames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBskReliableFramesTest::RunTest(const FString& Parameters)
{
    FBskTcpReceiver Receiver(TEXT("127.0.0.1"), 0, 1024 * 1024, true);
    FBskSceneManifest Manifest;
    Manifest.SessionId = TEXT("session");
    Receiver.PublishManifest(MoveTemp(Manifest));
    Receiver.ConsumeLatestManifest(Manifest);
    for (int64 Index = 0; Index < 5; ++Index)
    {
        FBskRenderFrame Frame;
        Frame.FrameId = Index;
        Frame.SessionId = TEXT("session");
        Receiver.PublishLatest(MoveTemp(Frame));
    }
    for (int64 Index = 0; Index < 5; ++Index)
    {
        FBskRenderFrame Frame;
        TestTrue(TEXT("all frames retained"), Receiver.ConsumeLatest(Frame));
        TestEqual(TEXT("FIFO frame order"), Frame.FrameId, Index);
    }
    TestEqual(TEXT("no overwrites"), Receiver.GetOverwrittenFrameCount(), uint64(0));
    FBskRenderFrame Old;
    Old.SessionId = TEXT("session");
    Receiver.PublishLatest(MoveTemp(Old));
    FBskSceneManifest Reset;
    Reset.SessionId = TEXT("new-session");
    Receiver.PublishManifest(MoveTemp(Reset));
    Receiver.ConsumeLatestManifest(Manifest);
    TestFalse(TEXT("reset clears queued frames"), Receiver.ConsumeLatest(Old));
    return true;
}
#endif
