#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "BskFrameSource.h"
#include "BskFrameParser.h"

class FSocket;
class FRunnableThread;

class BSKUNREALRUNTIME_API FBskTcpReceiver final : public FRunnable, public IBskFrameSource
{
public:
    FBskTcpReceiver(FString InListenAddress, uint16 InPort, uint32 InMaxPacketBytes);
    virtual ~FBskTcpReceiver() override;

    bool StartReceiver();
    void StopReceiver();
    virtual bool StartSource() override { return StartReceiver(); }
    virtual void StopSource() override { StopReceiver(); }
    virtual bool ConsumeLatest(FBskRenderFrame& OutFrame) override;
    virtual bool ConsumeLatestManifest(FBskSceneManifest& OutManifest) override;
    virtual bool ConsumeEvent(FBskRenderEvent& OutEvent) override;

    virtual FString GetStatus() const override;
    uint64 GetReceivedFrameCount() const { return ReceivedFrameCount.Load(); }
    uint64 GetOverwrittenFrameCount() const { return OverwrittenFrameCount.Load(); }

    virtual uint32 Run() override;
    virtual void Stop() override;

private:
    bool CreateListener();
    void CloseClient();
    void CloseSockets();
    void PublishLatest(FBskRenderFrame&& Frame);
    void PublishManifest(FBskSceneManifest&& Manifest);
    void PublishEvent(FBskRenderEvent&& Event);
    void SetStatus(const FString& NewStatus);

    FString ListenAddress;
    uint16 Port;
    FBskFrameParser Parser;
    FRunnableThread* Thread = nullptr;
    FSocket* ListenSocket = nullptr;
    FSocket* ClientSocket = nullptr;
    TAtomic<bool> bStopRequested{false};
    TAtomic<uint64> ReceivedFrameCount{0};
    TAtomic<uint64> OverwrittenFrameCount{0};
    mutable FCriticalSection LatestMutex;
    TSharedPtr<FBskRenderFrame, ESPMode::ThreadSafe> LatestFrame;
    TSharedPtr<FBskSceneManifest, ESPMode::ThreadSafe> LatestManifest;
    TArray<FBskRenderEvent> Events;
    mutable FCriticalSection StatusMutex;
    FString Status = TEXT("stopped");
};
