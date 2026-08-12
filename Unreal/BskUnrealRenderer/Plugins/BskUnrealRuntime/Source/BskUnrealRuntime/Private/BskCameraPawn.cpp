#include "BskCameraPawn.h"

#include "BskSceneController.h"
#include "BskRendererHUD.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "Kismet/GameplayStatics.h"

ABskCameraPawn::ABskCameraPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetRootComponent());
    Camera->bUsePawnControlRotation = true;
    Camera->FieldOfView = 60.0f;
    bUseControllerRotationYaw = true;
    bUseControllerRotationPitch = true;
    if (UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent()))
    {
        Movement->MaxSpeed = 5000.0f;
        Movement->Acceleration = 12000.0f;
        Movement->Deceleration = 12000.0f;
    }
}

void ABskCameraPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!IsValid(TargetActor) || CameraMode == EBskCameraMode::Free) return;
    const FVector TargetLocation = TargetActor->GetActorLocation();
    FVector CameraLocation;
    if (CameraMode == EBskCameraMode::Orbit)
    {
        const FRotator OrbitRotation(OrbitPitchDegrees, OrbitYawDegrees, 0.0);
        CameraLocation = TargetLocation - OrbitRotation.Vector() * OrbitDistanceCentimeters;
    }
    else
    {
        CameraLocation = TargetActor->GetActorTransform().TransformPosition(FollowOffsetCentimeters);
    }
    SetActorLocation(CameraLocation);
    if (Controller) Controller->SetControlRotation((TargetLocation - CameraLocation).Rotation());
}

void ABskCameraPawn::SetCameraTarget(AActor* Target, EBskCameraMode Mode)
{
    TargetActor = Target;
    CameraMode = IsValid(Target) ? Mode : EBskCameraMode::Free;
}

void ABskCameraPawn::ClearCameraTarget()
{
    TargetActor = nullptr;
    CameraMode = EBskCameraMode::Free;
}

void ABskCameraPawn::SetOrbitDistanceMeters(double DistanceMeters)
{
    OrbitDistanceCentimeters = FMath::Clamp(DistanceMeters * 100.0, 100.0, 1.0e9);
}

void ABskCameraPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    PlayerInputComponent->BindAxis(TEXT("MoveForward"), this, &ABskCameraPawn::MoveForward);
    PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &ABskCameraPawn::MoveRight);
    PlayerInputComponent->BindAxis(TEXT("MoveUp"), this, &ABskCameraPawn::MoveUp);
    PlayerInputComponent->BindAxis(TEXT("Turn"), this, &ABskCameraPawn::Turn);
    PlayerInputComponent->BindAxis(TEXT("LookUp"), this, &ABskCameraPawn::LookUp);
    PlayerInputComponent->BindAction(TEXT("ToggleCssVisuals"), IE_Pressed, this, &ABskCameraPawn::ToggleCssVisuals);
    PlayerInputComponent->BindAction(TEXT("ToggleGenericSensorVisuals"), IE_Pressed, this, &ABskCameraPawn::ToggleGenericSensorVisuals);
    PlayerInputComponent->BindAction(TEXT("ToggleTransceiverVisuals"), IE_Pressed, this, &ABskCameraPawn::ToggleTransceiverVisuals);
    PlayerInputComponent->BindAction(TEXT("TogglePictureInPictureOne"), IE_Pressed, this, &ABskCameraPawn::TogglePictureInPictureOne);
    PlayerInputComponent->BindAction(TEXT("TogglePictureInPictureTwo"), IE_Pressed, this, &ABskCameraPawn::TogglePictureInPictureTwo);
    PlayerInputComponent->BindAction(TEXT("ToggleMissionUi"), IE_Pressed, this, &ABskCameraPawn::ToggleMissionUi);
    PlayerInputComponent->BindAction(TEXT("ToggleMissionUiVisibility"), IE_Pressed, this, &ABskCameraPawn::ToggleMissionUiVisibility);
}

void ABskCameraPawn::MoveForward(float Value)
{
    if (CameraMode == EBskCameraMode::Orbit) OrbitDistanceCentimeters = FMath::Clamp(OrbitDistanceCentimeters - Value * 50.0, 100.0, 1.0e9);
    else AddMovementInput(GetActorForwardVector(), Value);
}
void ABskCameraPawn::MoveRight(float Value) { AddMovementInput(GetActorRightVector(), Value); }
void ABskCameraPawn::MoveUp(float Value) { AddMovementInput(FVector::UpVector, Value); }
void ABskCameraPawn::Turn(float Value)
{
    if (CameraMode == EBskCameraMode::Orbit) OrbitYawDegrees += Value;
    else AddControllerYawInput(Value);
}
void ABskCameraPawn::LookUp(float Value)
{
    if (CameraMode == EBskCameraMode::Orbit) OrbitPitchDegrees = FMath::Clamp(OrbitPitchDegrees + Value, -89.0, 89.0);
    else AddControllerPitchInput(Value);
}

void ABskCameraPawn::ToggleVisualKind(const FString& VisualKind)
{
    TArray<AActor*> Controllers;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABskSceneController::StaticClass(), Controllers);
    if (!Controllers.IsEmpty())
    {
        if (ABskSceneController* Scene = Cast<ABskSceneController>(Controllers[0])) Scene->ToggleVisualKindVisible(VisualKind);
    }
}

void ABskCameraPawn::ToggleCssVisuals() { ToggleVisualKind(TEXT("css")); }
void ABskCameraPawn::ToggleGenericSensorVisuals() { ToggleVisualKind(TEXT("generic_sensor")); }
void ABskCameraPawn::ToggleTransceiverVisuals() { ToggleVisualKind(TEXT("transceiver")); }

void ABskCameraPawn::TogglePictureInPicture(int32 Slot)
{
    TArray<AActor*> Controllers;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABskSceneController::StaticClass(), Controllers);
    if (!Controllers.IsEmpty())
    {
        if (ABskSceneController* Scene = Cast<ABskSceneController>(Controllers[0])) Scene->TogglePictureInPictureSlot(Slot);
    }
}

void ABskCameraPawn::TogglePictureInPictureOne() { TogglePictureInPicture(1); }
void ABskCameraPawn::TogglePictureInPictureTwo() { TogglePictureInPicture(2); }

void ABskCameraPawn::ToggleMissionUi()
{
    if (APlayerController* Player = Cast<APlayerController>(Controller))
    {
        if (ABskRendererHUD* Hud = Cast<ABskRendererHUD>(Player->GetHUD())) Hud->ToggleInteractiveMissionUi();
    }
}

void ABskCameraPawn::ToggleMissionUiVisibility()
{
    if (APlayerController* Player = Cast<APlayerController>(Controller))
    {
        if (ABskRendererHUD* Hud = Cast<ABskRendererHUD>(Player->GetHUD())) Hud->ToggleMissionUiVisibility();
    }
}
