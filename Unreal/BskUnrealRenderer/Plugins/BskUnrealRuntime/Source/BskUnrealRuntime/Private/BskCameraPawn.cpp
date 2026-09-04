#include "BskCameraPawn.h"

#include "BskSceneController.h"
#include "BskRendererHUD.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "InputCoreTypes.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "Kismet/GameplayStatics.h"

ABskCameraPawn::ABskCameraPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    // Use explicit key state for Pixel Streaming free-flight.  The inherited
    // DefaultPawn axis bindings are not needed and can compete with the
    // browser-controlled robot input.
    bAddDefaultMovementBindings = false;
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
    if (CameraMode == EBskCameraMode::MainView)
    {
        SetActorLocation(MainViewLocation);
        if (Controller) Controller->SetControlRotation(MainViewRotation);
        return;
    }
    if (CameraMode == EBskCameraMode::Free)
    {
        FVector MoveDirection = FVector::ZeroVector;
        const FRotator ControlRotation = Controller ? Controller->GetControlRotation() : GetActorRotation();
        const FRotationMatrix ControlMatrix(ControlRotation);
        if (bMoveForward) MoveDirection += ControlMatrix.GetUnitAxis(EAxis::X);
        if (bMoveBackward) MoveDirection -= ControlMatrix.GetUnitAxis(EAxis::X);
        if (bMoveRight) MoveDirection += ControlMatrix.GetUnitAxis(EAxis::Y);
        if (bMoveLeft) MoveDirection -= ControlMatrix.GetUnitAxis(EAxis::Y);
        if (bMoveUp) MoveDirection += FVector::UpVector;
        if (bMoveDown) MoveDirection -= FVector::UpVector;
        if (!MoveDirection.IsNearlyZero())
        {
            const FVector Delta = MoveDirection.GetSafeNormal() * FreeCameraSpeedCentimetersPerSecond * DeltaSeconds;
            // Free-flight is a spectator camera, so do not let scene collision
            // geometry stop the camera.
            SetActorLocation(GetActorLocation() + Delta, false);
        }
        return;
    }
    if (!IsValid(TargetActor)) return;
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

void ABskCameraPawn::SetMainViewTransform(const FVector& Location, const FRotator& Rotation)
{
    MainViewLocation = Location;
    MainViewRotation = Rotation;
    MainTargetActor = nullptr;
    MainCameraMode = EBskCameraMode::MainView;
    if (!bFreeCameraActive)
    {
        TargetActor = nullptr;
        CameraMode = EBskCameraMode::MainView;
        SetActorLocation(MainViewLocation);
        if (Controller) Controller->SetControlRotation(MainViewRotation);
    }
}

void ABskCameraPawn::SetCameraTarget(AActor* Target, EBskCameraMode Mode)
{
    MainTargetActor = IsValid(Target) ? Target : nullptr;
    MainCameraMode = IsValid(Target) ? Mode : EBskCameraMode::MainView;
    if (bFreeCameraActive) return;
    TargetActor = MainTargetActor;
    CameraMode = MainCameraMode;
}

void ABskCameraPawn::ClearCameraTarget()
{
    MainTargetActor = nullptr;
    MainCameraMode = EBskCameraMode::MainView;
    if (bFreeCameraActive) return;
    TargetActor = nullptr;
    CameraMode = EBskCameraMode::MainView;
}

void ABskCameraPawn::SetOrbitDistanceMeters(double DistanceMeters)
{
    const double DistanceCentimeters = FMath::Clamp(DistanceMeters * 100.0, 100.0, 1.0e9);
    MainOrbitDistanceCentimeters = DistanceCentimeters;
    OrbitDistanceCentimeters = DistanceCentimeters;
}

void ABskCameraPawn::ToggleFreeCamera()
{
    if (bFreeCameraActive)
    {
        ReturnToMainView();
        return;
    }

    bFreeCameraActive = true;
    TargetActor = nullptr;
    CameraMode = EBskCameraMode::Free;
    UE_LOG(LogTemp, Display, TEXT("BSK camera entered free-flight mode (C toggles, Home restores main view)"));
}

void ABskCameraPawn::ReturnToMainView()
{
    bFreeCameraActive = false;
    TargetActor = MainTargetActor;
    CameraMode = MainCameraMode;
    OrbitDistanceCentimeters = MainOrbitDistanceCentimeters;
    OrbitYawDegrees = MainOrbitYawDegrees;
    OrbitPitchDegrees = MainOrbitPitchDegrees;
    FollowOffsetCentimeters = MainFollowOffsetCentimeters;

    bMoveForward = false;
    bMoveBackward = false;
    bMoveRight = false;
    bMoveLeft = false;
    bMoveUp = false;
    bMoveDown = false;
    if (GetMovementComponent()) GetMovementComponent()->StopMovementImmediately();
    if (CameraMode == EBskCameraMode::MainView)
    {
        SetActorLocation(MainViewLocation);
        if (Controller) Controller->SetControlRotation(MainViewRotation);
    }
    UE_LOG(LogTemp, Display, TEXT("BSK camera restored main view"));
}

void ABskCameraPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    // Pixel Streaming delivers key down/up events reliably, while legacy axis
    // callbacks can be lost when the browser toggles keyboard input at runtime.
    // Track the held keys and apply the translation in Tick instead.
    PlayerInputComponent->BindKey(EKeys::W, IE_Pressed, this, &ABskCameraPawn::MoveForwardPressed);
    PlayerInputComponent->BindKey(EKeys::W, IE_Released, this, &ABskCameraPawn::MoveForwardReleased);
    PlayerInputComponent->BindKey(EKeys::S, IE_Pressed, this, &ABskCameraPawn::MoveBackwardPressed);
    PlayerInputComponent->BindKey(EKeys::S, IE_Released, this, &ABskCameraPawn::MoveBackwardReleased);
    PlayerInputComponent->BindKey(EKeys::D, IE_Pressed, this, &ABskCameraPawn::MoveRightPressed);
    PlayerInputComponent->BindKey(EKeys::D, IE_Released, this, &ABskCameraPawn::MoveRightReleased);
    PlayerInputComponent->BindKey(EKeys::A, IE_Pressed, this, &ABskCameraPawn::MoveLeftPressed);
    PlayerInputComponent->BindKey(EKeys::A, IE_Released, this, &ABskCameraPawn::MoveLeftReleased);
    PlayerInputComponent->BindKey(EKeys::E, IE_Pressed, this, &ABskCameraPawn::MoveUpPressed);
    PlayerInputComponent->BindKey(EKeys::E, IE_Released, this, &ABskCameraPawn::MoveUpReleased);
    PlayerInputComponent->BindKey(EKeys::Q, IE_Pressed, this, &ABskCameraPawn::MoveDownPressed);
    PlayerInputComponent->BindKey(EKeys::Q, IE_Released, this, &ABskCameraPawn::MoveDownReleased);
    PlayerInputComponent->BindAxis(TEXT("Turn"), this, &ABskCameraPawn::Turn);
    PlayerInputComponent->BindAxis(TEXT("LookUp"), this, &ABskCameraPawn::LookUp);
    PlayerInputComponent->BindAction(TEXT("ToggleFreeCamera"), IE_Pressed, this, &ABskCameraPawn::ToggleFreeCamera);
    PlayerInputComponent->BindAction(TEXT("ReturnMainView"), IE_Pressed, this, &ABskCameraPawn::ReturnToMainView);
    PlayerInputComponent->BindAction(TEXT("ToggleCssVisuals"), IE_Pressed, this, &ABskCameraPawn::ToggleCssVisuals);
    PlayerInputComponent->BindAction(TEXT("ToggleGenericSensorVisuals"), IE_Pressed, this, &ABskCameraPawn::ToggleGenericSensorVisuals);
    PlayerInputComponent->BindAction(TEXT("ToggleTransceiverVisuals"), IE_Pressed, this, &ABskCameraPawn::ToggleTransceiverVisuals);
    PlayerInputComponent->BindAction(TEXT("TogglePictureInPictureOne"), IE_Pressed, this, &ABskCameraPawn::TogglePictureInPictureOne);
    PlayerInputComponent->BindAction(TEXT("TogglePictureInPictureTwo"), IE_Pressed, this, &ABskCameraPawn::TogglePictureInPictureTwo);
    PlayerInputComponent->BindAction(TEXT("ToggleMissionUi"), IE_Pressed, this, &ABskCameraPawn::ToggleMissionUi);
    PlayerInputComponent->BindAction(TEXT("ToggleMissionUiVisibility"), IE_Pressed, this, &ABskCameraPawn::ToggleMissionUiVisibility);
}

void ABskCameraPawn::MoveForwardPressed() { bMoveForward = true; }
void ABskCameraPawn::MoveForwardReleased() { bMoveForward = false; }
void ABskCameraPawn::MoveBackwardPressed() { bMoveBackward = true; }
void ABskCameraPawn::MoveBackwardReleased() { bMoveBackward = false; }
void ABskCameraPawn::MoveRightPressed() { bMoveRight = true; }
void ABskCameraPawn::MoveRightReleased() { bMoveRight = false; }
void ABskCameraPawn::MoveLeftPressed() { bMoveLeft = true; }
void ABskCameraPawn::MoveLeftReleased() { bMoveLeft = false; }
void ABskCameraPawn::MoveUpPressed() { bMoveUp = true; }
void ABskCameraPawn::MoveUpReleased() { bMoveUp = false; }
void ABskCameraPawn::MoveDownPressed() { bMoveDown = true; }
void ABskCameraPawn::MoveDownReleased() { bMoveDown = false; }
void ABskCameraPawn::Turn(float Value)
{
    if (CameraMode == EBskCameraMode::MainView) return;
    if (CameraMode == EBskCameraMode::Orbit) OrbitYawDegrees += Value;
    else AddControllerYawInput(Value);
}
void ABskCameraPawn::LookUp(float Value)
{
    if (CameraMode == EBskCameraMode::MainView) return;
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
