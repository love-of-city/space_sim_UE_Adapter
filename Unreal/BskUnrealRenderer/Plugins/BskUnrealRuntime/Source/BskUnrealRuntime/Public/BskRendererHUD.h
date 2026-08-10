#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "BskRendererHUD.generated.h"

/** Lightweight runtime mission overlay with connection and simulation status. */
UCLASS()
class BSKUNREALRUNTIME_API ABskRendererHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;
};
