#pragma once

#include "CoreMinimal.h"

// Dataset sampling must align with both 500 Hz dynamics and 30 Hz render sampling, never
// wall-clock presentation/preview time. Keep in sync with server sample_tick.
inline bool BskLeRobotSample(int64 SimulationTimeNanoseconds, double RateHertz, int64& OutSampleIndex)
{
    const int32 Fps = FMath::RoundToInt(RateHertz);
    if (!FMath::IsNearlyEqual(RateHertz, static_cast<double>(Fps), 1.e-9) ||
        !(Fps == 1 || Fps == 2 || Fps == 5 || Fps == 10) ||
        SimulationTimeNanoseconds < 0)
    {
        return false;
    }
    // Avoid multiplying the full nanosecond timestamp (long-running scenes).
    const int64 WholeSeconds = SimulationTimeNanoseconds / 1000000000LL;
    const int64 Fraction = SimulationTimeNanoseconds % 1000000000LL;
    const int64 TickInSecond = (Fraction * Fps + 500000000LL) / 1000000000LL;
    OutSampleIndex = WholeSeconds * Fps + TickInSecond;
    return FMath::Abs(Fraction * Fps - TickInSecond * 1000000000LL) <= static_cast<int64>(Fps) * 1000;
}
