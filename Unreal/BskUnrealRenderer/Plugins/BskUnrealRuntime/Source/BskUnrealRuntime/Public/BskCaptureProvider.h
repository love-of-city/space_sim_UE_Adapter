#pragma once

#include "CoreMinimal.h"

class AActor;

enum class EBskCaptureChannel : uint8
{
    Rgb,
    Depth,
    SemanticSegmentation
};

struct BSKUNREALRUNTIME_API FBskCaptureRequest
{
    FString CameraId;
    TArray<EBskCaptureChannel> Channels;
    FIntPoint Resolution = FIntPoint(1920, 1080);
    int64 SimulationTimeNanoseconds = 0;
    int64 SourceWallTimeNanoseconds = 0;
    int64 FrameId = -1;
    FString OutputDirectory;
    bool bWriteToDisk = true;
    bool bSendToNetwork = false;
};

/** Runtime capture extension point. Implementations are invoked only on the Game Thread. */
class BSKUNREALRUNTIME_API IBskCaptureProvider
{
public:
    virtual ~IBskCaptureProvider() = default;
    virtual bool RegisterCamera(const FString& CameraId, TWeakObjectPtr<AActor> CameraActor) = 0;
    virtual bool RequestCapture(const FBskCaptureRequest& Request, FString& OutError) = 0;
    virtual void Shutdown() = 0;
};
