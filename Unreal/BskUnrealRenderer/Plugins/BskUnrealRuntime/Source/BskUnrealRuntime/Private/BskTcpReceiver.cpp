#include "BskTcpReceiver.h"

#include "BskUnrealRuntime.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

FBskTcpReceiver::FBskTcpReceiver(FString InListenAddress, uint16 InPort, uint32 InMaxPacketBytes)
    : ListenAddress(MoveTemp(InListenAddress))
    , Port(InPort)
    , Parser(InMaxPacketBytes)
{
}

FBskTcpReceiver::~FBskTcpReceiver()
{
    StopReceiver();
}

bool FBskTcpReceiver::StartReceiver()
{
    if (Thread != nullptr)
    {
        return true;
    }
    bStopRequested.Store(false);
    Thread = FRunnableThread::Create(this, TEXT("BskTcpReceiver"), 0, TPri_BelowNormal);
    if (Thread == nullptr)
    {
        SetStatus(TEXT("failed to create receiver thread"));
        return false;
    }
    return true;
}

void FBskTcpReceiver::StopReceiver()
{
    Stop();
    if (Thread != nullptr)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }
}

void FBskTcpReceiver::Stop()
{
    bStopRequested.Store(true);
}

FString FBskTcpReceiver::GetStatus() const
{
    FScopeLock Lock(&StatusMutex);
    return Status;
}

void FBskTcpReceiver::SetStatus(const FString& NewStatus)
{
    FScopeLock Lock(&StatusMutex);
    Status = NewStatus;
}

bool FBskTcpReceiver::ConsumeLatest(FBskRenderFrame& OutFrame)
{
    FScopeLock Lock(&LatestMutex);
    if (!LatestFrame.IsValid())
    {
        return false;
    }
    OutFrame = MoveTemp(*LatestFrame);
    LatestFrame.Reset();
    return true;
}

bool FBskTcpReceiver::ConsumeLatestManifest(FBskSceneManifest& OutManifest)
{
    FScopeLock Lock(&LatestMutex);
    if (!LatestManifest.IsValid()) return false;
    OutManifest = MoveTemp(*LatestManifest);
    LatestManifest.Reset();
    return true;
}

bool FBskTcpReceiver::ConsumeEvent(FBskRenderEvent& OutEvent)
{
    FScopeLock Lock(&LatestMutex);
    if (Events.IsEmpty()) return false;
    OutEvent = MoveTemp(Events[0]);
    Events.RemoveAt(0, 1, EAllowShrinking::No);
    return true;
}

void FBskTcpReceiver::PublishLatest(FBskRenderFrame&& Frame)
{
    FScopeLock Lock(&LatestMutex);
    if (LatestFrame.IsValid())
    {
        ++OverwrittenFrameCount;
    }
    LatestFrame = MakeShared<FBskRenderFrame, ESPMode::ThreadSafe>(MoveTemp(Frame));
    ++ReceivedFrameCount;
}

void FBskTcpReceiver::PublishManifest(FBskSceneManifest&& Manifest)
{
    FScopeLock Lock(&LatestMutex);
    LatestManifest = MakeShared<FBskSceneManifest, ESPMode::ThreadSafe>(MoveTemp(Manifest));
}

void FBskTcpReceiver::PublishEvent(FBskRenderEvent&& Event)
{
    FScopeLock Lock(&LatestMutex);
    constexpr int32 MaxPendingEvents = 64;
    if (Events.Num() >= MaxPendingEvents) Events.RemoveAt(0, 1, EAllowShrinking::No);
    Events.Add(MoveTemp(Event));
}

bool FBskTcpReceiver::CreateListener()
{
    FIPv4Address Address;
    if (!FIPv4Address::Parse(ListenAddress, Address))
    {
        SetStatus(FString::Printf(TEXT("invalid listen address: %s"), *ListenAddress));
        return false;
    }
    ListenSocket = FTcpSocketBuilder(TEXT("BskTcpListener"))
        .AsReusable()
        .AsNonBlocking()
        .BoundToEndpoint(FIPv4Endpoint(Address, Port))
        .Listening(1);
    if (ListenSocket == nullptr)
    {
        SetStatus(FString::Printf(TEXT("failed to listen on %s:%u"), *ListenAddress, Port));
        return false;
    }
    SetStatus(FString::Printf(TEXT("listening on %s:%u"), *ListenAddress, Port));
    UE_LOG(LogBskUnreal, Display, TEXT("BSK receiver listening on %s:%u"), *ListenAddress, Port);
    return true;
}

void FBskTcpReceiver::CloseClient()
{
    if (ClientSocket != nullptr)
    {
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        ClientSocket = nullptr;
    }
    Parser.Reset();
}

void FBskTcpReceiver::CloseSockets()
{
    CloseClient();
    if (ListenSocket != nullptr)
    {
        ListenSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
    }
}

uint32 FBskTcpReceiver::Run()
{
    if (!CreateListener())
    {
        return 1;
    }

    TArray<uint8> ReadBuffer;
    ReadBuffer.SetNumUninitialized(64 * 1024);
    while (!bStopRequested.Load())
    {
        if (ClientSocket == nullptr)
        {
            bool bPending = false;
            if (ListenSocket->HasPendingConnection(bPending) && bPending)
            {
                ClientSocket = ListenSocket->Accept(TEXT("BskTcpClient"));
                if (ClientSocket != nullptr)
                {
                    ClientSocket->SetNonBlocking(true);
                    ClientSocket->SetNoDelay(true);
                    Parser.Reset();
                    SetStatus(TEXT("client connected"));
                    UE_LOG(LogBskUnreal, Display, TEXT("BSK sender connected"));
                }
            }
            FPlatformProcess::SleepNoStats(0.005f);
            continue;
        }

        uint32 PendingBytes = 0;
        bool bReadAny = false;
        while (ClientSocket->HasPendingData(PendingBytes) && PendingBytes > 0 && !bStopRequested.Load())
        {
            const int32 Requested = FMath::Min<int32>(ReadBuffer.Num(), static_cast<int32>(PendingBytes));
            int32 BytesRead = 0;
            if (!ClientSocket->Recv(ReadBuffer.GetData(), Requested, BytesRead) || BytesRead <= 0)
            {
                break;
            }
            bReadAny = true;
            TArray<FBskRenderMessage> Messages;
            FString Error;
            if (!Parser.AppendMessages(ReadBuffer.GetData(), BytesRead, Messages, Error))
            {
                UE_LOG(LogBskUnreal, Warning, TEXT("Discarding invalid BSK connection: %s"), *Error);
                SetStatus(FString::Printf(TEXT("protocol error: %s"), *Error));
                CloseClient();
                break;
            }
            for (FBskRenderMessage& Message : Messages)
            {
                switch (Message.Type)
                {
                case EBskRenderMessageType::Hello:
                    UE_LOG(LogBskUnreal, Display, TEXT("BSK render session hello: %s"), *Message.SessionId);
                    break;
                case EBskRenderMessageType::SceneManifest:
                    PublishManifest(MoveTemp(Message.Manifest));
                    break;
                case EBskRenderMessageType::Frame:
                    PublishLatest(MoveTemp(Message.Frame));
                    break;
                case EBskRenderMessageType::Event:
                    PublishEvent(MoveTemp(Message.Event));
                    break;
                default:
                    break;
                }
            }
        }

        // A graceful TCP close becomes readable with zero payload. On Windows,
        // GetConnectionState() alone can continue to report Connected until a
        // recv observes that FIN, which would prevent accepting a reconnect.
        if (ClientSocket != nullptr && !bReadAny &&
            ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
        {
            uint8 Probe = 0;
            int32 ProbeBytes = 0;
            if (!ClientSocket->Recv(&Probe, 1, ProbeBytes, ESocketReceiveFlags::Peek) || ProbeBytes == 0)
            {
                UE_LOG(LogBskUnreal, Display, TEXT("BSK sender disconnected; retaining last rendered frame"));
                CloseClient();
                SetStatus(FString::Printf(TEXT("listening on %s:%u"), *ListenAddress, Port));
            }
        }

        if (ClientSocket != nullptr && ClientSocket->GetConnectionState() != SCS_Connected)
        {
            UE_LOG(LogBskUnreal, Display, TEXT("BSK sender disconnected; retaining last rendered frame"));
            CloseClient();
            SetStatus(FString::Printf(TEXT("listening on %s:%u"), *ListenAddress, Port));
        }
        if (!bReadAny)
        {
            FPlatformProcess::SleepNoStats(0.002f);
        }
    }
    CloseSockets();
    SetStatus(TEXT("stopped"));
    return 0;
}
