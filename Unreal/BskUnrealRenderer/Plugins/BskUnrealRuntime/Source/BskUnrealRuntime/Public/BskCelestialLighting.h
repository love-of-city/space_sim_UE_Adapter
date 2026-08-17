#pragma once

#include "CoreMinimal.h"

namespace BskCelestialLighting
{
/** Direction travelled by light rays from an ephemeris source to a local target. */
inline FVector DirectionFromSourceToTarget(const FVector& Source, const FVector& Target = FVector::ZeroVector)
{
    return (Target - Source).GetSafeNormal();
}

/** Inverse-square illuminance with a bounded denominator for malformed frames. */
inline double IlluminanceLux(
    const double IlluminanceAtReference,
    const double ReferenceDistanceMeters,
    const double ActualDistanceMeters)
{
    if (IlluminanceAtReference <= 0.0 || ReferenceDistanceMeters <= 0.0 || ActualDistanceMeters <= 0.0)
    {
        return FMath::Max(0.0, IlluminanceAtReference);
    }
    const double Ratio = ReferenceDistanceMeters / ActualDistanceMeters;
    return FMath::Clamp(IlluminanceAtReference * Ratio * Ratio, 0.0, IlluminanceAtReference * 100.0);
}
}
