#include "BskRendererHUD.h"

#include "BskRenderWorldSubsystem.h"
#include "BskSceneController.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"

void ABskRendererHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas || !GetWorld()) return;
    TArray<AActor*> Controllers;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABskSceneController::StaticClass(), Controllers);
    const ABskSceneController* Scene = Controllers.IsEmpty() ? nullptr : Cast<ABskSceneController>(Controllers[0]);
    const UBskRenderWorldSubsystem* Subsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>();
    const FString Status = Scene ? Scene->GetReceiverStatus() : TEXT("renderer not ready");
    const double SimulationSeconds = Subsystem ? static_cast<double>(Subsystem->GetSimulationTimeNanoseconds()) * 1.0e-9 : 0.0;
    const TCHAR* CssStatus = Scene && Scene->IsVisualKindVisible(TEXT("css")) ? TEXT("ON") : TEXT("OFF");
    const TCHAR* SensorStatus = Scene && Scene->IsVisualKindVisible(TEXT("generic_sensor")) ? TEXT("ON") : TEXT("OFF");
    const TCHAR* TransceiverStatus = Scene && Scene->IsVisualKindVisible(TEXT("transceiver")) ? TEXT("ON") : TEXT("OFF");
    const FString Text = FString::Printf(
        TEXT("BSK Unreal Renderer\n%s\nSim: %.3f s   Frame: %lld\nW/S A/D Q/E move | Mouse look\n1 CSS:%s | 2 Sensor:%s | 3 Comms:%s"),
        *Status,
        SimulationSeconds,
        Scene ? Scene->GetLastFrameId() : -1,
        CssStatus,
        SensorStatus,
        TransceiverStatus);
    Canvas->SetDrawColor(FColor(178, 230, 255));
    Canvas->DrawText(GEngine->GetSmallFont(), Text, 24.0f, 24.0f, 1.0f, 1.0f, FFontRenderInfo());
}
