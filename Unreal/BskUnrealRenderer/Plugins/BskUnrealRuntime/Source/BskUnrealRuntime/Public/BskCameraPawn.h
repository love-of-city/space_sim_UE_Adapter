#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "BskCameraPawn.generated.h"

class UCameraComponent;

UENUM(BlueprintType)
enum class EBskCameraMode : uint8
{
    Free,
    Orbit,
    Follow
};

UCLASS()
class BSKUNREALRUNTIME_API ABskCameraPawn : public ASpectatorPawn
{
    GENERATED_BODY()

public:
    ABskCameraPawn();
    UCameraComponent* GetBskCameraComponent() const { return Camera; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    UFUNCTION(BlueprintCallable, Category="BSK Renderer|Camera")
    void SetCameraTarget(AActor* Target, EBskCameraMode Mode = EBskCameraMode::Orbit);

    UFUNCTION(BlueprintCallable, Category="BSK Renderer|Camera")
    void ClearCameraTarget();

    UFUNCTION(BlueprintCallable, Category="BSK Renderer|Camera")
    void SetOrbitDistanceMeters(double DistanceMeters);

private:
    void MoveForward(float Value);
    void MoveRight(float Value);
    void MoveUp(float Value);
    void Turn(float Value);
    void LookUp(float Value);
    void ToggleCssVisuals();
    void ToggleGenericSensorVisuals();
    void ToggleTransceiverVisuals();
    void ToggleVisualKind(const FString& VisualKind);

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UCameraComponent> Camera;

    UPROPERTY()
    TObjectPtr<AActor> TargetActor;

    EBskCameraMode CameraMode = EBskCameraMode::Free;
    double OrbitDistanceCentimeters = 2500.0;
    double OrbitYawDegrees = -35.0;
    double OrbitPitchDegrees = 20.0;
    FVector FollowOffsetCentimeters = FVector(-1500.0, 0.0, 600.0);
};
