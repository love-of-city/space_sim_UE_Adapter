#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "BskCameraPawn.generated.h"

class UCameraComponent;

UENUM(BlueprintType)
enum class EBskCameraMode : uint8
{
    MainView,
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

    void SetMainViewTransform(const FVector& Location, const FRotator& Rotation);

    UFUNCTION(BlueprintCallable, Category="BSK Renderer|Camera")
    void SetCameraTarget(AActor* Target, EBskCameraMode Mode = EBskCameraMode::Orbit);

    UFUNCTION(BlueprintCallable, Category="BSK Renderer|Camera")
    void ClearCameraTarget();

    UFUNCTION(BlueprintCallable, Category="BSK Renderer|Camera")
    void SetOrbitDistanceMeters(double DistanceMeters);

private:
    void ToggleFreeCamera();
    void ReturnToMainView();
    void MoveForwardPressed();
    void MoveForwardReleased();
    void MoveBackwardPressed();
    void MoveBackwardReleased();
    void MoveRightPressed();
    void MoveRightReleased();
    void MoveLeftPressed();
    void MoveLeftReleased();
    void MoveUpPressed();
    void MoveUpReleased();
    void MoveDownPressed();
    void MoveDownReleased();
    void Turn(float Value);
    void LookUp(float Value);
    void ToggleCssVisuals();
    void ToggleGenericSensorVisuals();
    void ToggleTransceiverVisuals();
    void TogglePictureInPictureOne();
    void TogglePictureInPictureTwo();
    void ToggleMissionUi();
    void ToggleMissionUiVisibility();
    void TogglePictureInPicture(int32 Slot);
    void ToggleVisualKind(const FString& VisualKind);

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UCameraComponent> Camera;

    UPROPERTY()
    TObjectPtr<AActor> TargetActor;

    UPROPERTY()
    TObjectPtr<AActor> MainTargetActor;

    EBskCameraMode CameraMode = EBskCameraMode::MainView;
    EBskCameraMode MainCameraMode = EBskCameraMode::MainView;
    FVector MainViewLocation = FVector::ZeroVector;
    FRotator MainViewRotation = FRotator::ZeroRotator;
    bool bFreeCameraActive = false;
    bool bMoveForward = false;
    bool bMoveBackward = false;
    bool bMoveRight = false;
    bool bMoveLeft = false;
    bool bMoveUp = false;
    bool bMoveDown = false;
    float FreeCameraSpeedCentimetersPerSecond = 5000.0f;
    double OrbitDistanceCentimeters = 2500.0;
    double MainOrbitDistanceCentimeters = 2500.0;
    double OrbitYawDegrees = -35.0;
    double MainOrbitYawDegrees = -35.0;
    double OrbitPitchDegrees = 20.0;
    double MainOrbitPitchDegrees = 20.0;
    FVector FollowOffsetCentimeters = FVector(-1500.0, 0.0, 600.0);
    FVector MainFollowOffsetCentimeters = FVector(-1500.0, 0.0, 600.0);
};
