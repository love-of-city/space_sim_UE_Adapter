#include "BskSceneController.h"

#include "BskTcpReceiver.h"
#include "BskRenderWorldSubsystem.h"
#include "BskReplaySource.h"
#include "BskCameraPawn.h"
#include "BskUnrealRuntime.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "DrawDebugHelpers.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/Texture.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/SkeletalMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

namespace
{
bool JsonVector3(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FVector3d& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(Name, Values) || Values == nullptr || Values->Num() != 3)
    {
        return false;
    }
    Out = FVector3d((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
    return true;
}

bool JsonColor(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FLinearColor& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(Name, Values) || Values == nullptr || Values->Num() < 3)
    {
        return false;
    }
    Out = FLinearColor(
        static_cast<float>((*Values)[0]->AsNumber()),
        static_cast<float>((*Values)[1]->AsNumber()),
        static_cast<float>((*Values)[2]->AsNumber()),
        Values->Num() >= 4 ? static_cast<float>((*Values)[3]->AsNumber()) : 1.0f);
    return true;
}

FString BindingTag(const FString& Name)
{
    return FString::Printf(TEXT("BSK.%s"), *Name);
}

FName SafeActorName(const FString& Value)
{
    FString Result = Value;
    Result.ReplaceInline(TEXT("/"), TEXT("_"));
    Result.ReplaceInline(TEXT("\\"), TEXT("_"));
    Result.ReplaceInline(TEXT("."), TEXT("_"));
    return FName(*FString::Printf(TEXT("%s_H%08X"), *Result, GetTypeHash(Value)));
}

const TCHAR* PrimitiveMeshPath(const FString& Shape)
{
    if (Shape.Equals(TEXT("sphere"), ESearchCase::IgnoreCase) || Shape.Equals(TEXT("ellipsoid"), ESearchCase::IgnoreCase))
    {
        return TEXT("/Engine/BasicShapes/Sphere.Sphere");
    }
    if (Shape.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase) || Shape.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
    {
        return TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
    }
    if (Shape.Equals(TEXT("cone"), ESearchCase::IgnoreCase))
    {
        return TEXT("/Engine/BasicShapes/Cone.Cone");
    }
    return TEXT("/Engine/BasicShapes/Cube.Cube");
}

void ApplyUnlitColor(UStaticMeshComponent* Component, UObject* Owner, const FLinearColor& Color)
{
    if (!Component) return;
    if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BSK/M_BskUnlitColor.M_BskUnlitColor")))
    {
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Owner);
        Material->SetVectorParameterValue(TEXT("Color"), Color);
        Component->SetMaterial(0, Material);
    }
}

void ApplyGeometryMaterial(UStaticMeshComponent* Component, UObject* Owner, const FBskGeometryDefinition& Geometry, double Ambient)
{
    if (!Component) return;
    if (Geometry.bUseAssetMaterials && Geometry.MaterialName.IsEmpty()) return;
    const bool bTranslucent = Geometry.Color.A < 0.999f;
    const TCHAR* MaterialPath = bTranslucent
        ? TEXT("/Game/BSK/M_BskPbrTranslucent.M_BskPbrTranslucent")
        : TEXT("/Game/BSK/M_BskPbrOpaque.M_BskPbrOpaque");
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, MaterialPath);
    if (!Base)
    {
        ApplyUnlitColor(Component, Owner, Geometry.Color);
        return;
    }

    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Owner);
    const double BlinnExponent = FMath::Clamp(Geometry.MaterialShininess, 0.0, 1.0) * 128.0;
    const float Roughness = static_cast<float>(FMath::Clamp(FMath::Sqrt(2.0 / (BlinnExponent + 2.0)), 0.08, 1.0));
    const float Specular = static_cast<float>(FMath::Clamp(
        Geometry.MaterialSpecular + Geometry.MaterialReflectance * (1.0 - Geometry.MaterialSpecular), 0.0, 1.0));
    Material->SetVectorParameterValue(TEXT("BaseColor"), Geometry.Color);
    Material->SetScalarParameterValue(TEXT("Roughness"), Roughness);
    Material->SetScalarParameterValue(TEXT("Specular"), Specular);
    Material->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
    Material->SetScalarParameterValue(TEXT("Emission"), static_cast<float>(FMath::Max(0.0, Geometry.MaterialEmission)));
    Material->SetScalarParameterValue(TEXT("Ambient"), static_cast<float>(FMath::Max(0.0, Ambient)));
    Material->SetScalarParameterValue(TEXT("Opacity"), Geometry.Color.A);
    Material->SetScalarParameterValue(TEXT("UseTexture"), 0.0f);
    Material->SetVectorParameterValue(TEXT("TextureRepeat"), FLinearColor(
        static_cast<float>(Geometry.MaterialTextureRepeat.X),
        static_cast<float>(Geometry.MaterialTextureRepeat.Y), 0.0f, 0.0f));
    if (!Geometry.MaterialTextureAssetPath.IsEmpty())
    {
        if (UTexture* Texture = LoadObject<UTexture>(nullptr, *Geometry.MaterialTextureAssetPath))
        {
            Material->SetTextureParameterValue(TEXT("BaseColorTexture"), Texture);
            Material->SetScalarParameterValue(TEXT("UseTexture"), 1.0f);
        }
        else
        {
            UE_LOG(LogBskUnreal, Warning, TEXT("Could not load packaged MJCF texture %s for %s"),
                *Geometry.MaterialTextureAssetPath, *Geometry.GeometryId);
        }
    }
    const int32 SlotCount = FMath::Max(1, Component->GetNumMaterials());
    for (int32 Slot = 0; Slot < SlotCount; ++Slot) Component->SetMaterial(Slot, Material);
}

bool IsSensorFrustumKind(const FString& Kind)
{
    return Kind.Equals(TEXT("css"), ESearchCase::IgnoreCase) ||
        Kind.Equals(TEXT("generic_sensor"), ESearchCase::IgnoreCase) ||
        Kind.Equals(TEXT("transceiver"), ESearchCase::IgnoreCase);
}
}

ABskSceneController::ABskSceneController()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    VisualKindVisibility.Add(TEXT("css"), false);
    VisualKindVisibility.Add(TEXT("generic_sensor"), false);
    VisualKindVisibility.Add(TEXT("transceiver"), false);
}

ABskSceneController::~ABskSceneController() = default;

void ABskSceneController::BeginPlay()
{
    Super::BeginPlay();
    LoadConfiguration();
    CreateEnvironment();
    if (!ReplayPath.IsEmpty()) Receiver = MakeUnique<FBskReplaySource>(ReplayPath, ReplayRate);
    else Receiver = MakeUnique<FBskTcpReceiver>(ListenAddress, static_cast<uint16>(ListenPort), MaxPacketBytes);
    Receiver->StartSource();
}

void ABskSceneController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (Receiver)
    {
        Receiver->StopSource();
        Receiver.Reset();
    }
    Super::EndPlay(EndPlayReason);
}

void ABskSceneController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    check(IsInGameThread());
    if (Receiver)
    {
        FBskSceneManifest Manifest;
        if (Receiver->ConsumeLatestManifest(Manifest)) ApplyManifest(Manifest);
        FBskRenderEvent Event;
        while (Receiver->ConsumeEvent(Event)) ApplyEvent(Event);
        FBskRenderFrame Latest;
        if (Receiver->ConsumeLatest(Latest))
        {
            const double NowSeconds = FPlatformTime::Seconds();
            FString RejectionReason;
            UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>();
            if (RenderSubsystem && !RenderSubsystem->AcceptFrame(Latest, RejectionReason))
            {
                UE_LOG(LogBskUnreal, Warning, TEXT("Ignoring BSK frame: %s"), *RejectionReason);
            }
            else if (TimeMode.Equals(TEXT("latest"), ESearchCase::IgnoreCase) || !bHasTargetFrame)
            {
                PreviousFrame = Latest;
                TargetFrame = Latest;
                bHasTargetFrame = true;
                ApplyFrame(Latest);
            }
            else
            {
                PreviousFrame = TargetFrame;
                TargetFrame = MoveTemp(Latest);
                BlendElapsedSeconds = 0.0;
                if (LastFrameArrivalSeconds > 0.0)
                {
                    BlendDurationSeconds = FMath::Clamp(NowSeconds - LastFrameArrivalSeconds, 1.0 / 240.0, InterpolationDelaySeconds);
                }
            }
            LastFrameArrivalSeconds = NowSeconds;
        }
        if (bHasTargetFrame && !TimeMode.Equals(TEXT("latest"), ESearchCase::IgnoreCase) && PreviousFrame.FrameId != TargetFrame.FrameId)
        {
            BlendElapsedSeconds += DeltaSeconds;
            const double Alpha = FMath::Clamp(BlendElapsedSeconds / FMath::Max(BlendDurationSeconds, UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
            ApplyFrame(InterpolateFrame(PreviousFrame, TargetFrame, Alpha));
        }
    }
}

FString ABskSceneController::GetReceiverStatus() const
{
    return Receiver ? Receiver->GetStatus() : TEXT("not started");
}

bool ABskSceneController::SetReplayPaused(bool bPaused)
{
    return Receiver && Receiver->SetPaused(bPaused);
}

bool ABskSceneController::SetReplayRate(double Rate)
{
    return Receiver && Receiver->SetPlaybackRate(Rate);
}

bool ABskSceneController::StepReplay()
{
    return Receiver && Receiver->StepOnce();
}

bool ABskSceneController::SeekReplayNanoseconds(int64 SimulationTimeNanoseconds)
{
    return Receiver && Receiver->SeekSimulationTime(SimulationTimeNanoseconds);
}

bool ABskSceneController::FocusObject(const FString& ObjectId, bool bFollow)
{
    AActor* Target = BoundActors.FindRef(ObjectId);
    APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    ABskCameraPawn* Pawn = Player ? Cast<ABskCameraPawn>(Player->GetPawn()) : nullptr;
    if (!Target || !Pawn) return false;
    Pawn->SetCameraTarget(Target, bFollow ? EBskCameraMode::Follow : EBskCameraMode::Orbit);
    return true;
}

void ABskSceneController::SetVisualKindVisible(const FString& VisualKind, bool bVisible)
{
    const FString NormalizedKind = VisualKind.TrimStartAndEnd().ToLower();
    if (NormalizedKind.IsEmpty()) return;
    VisualKindVisibility.Add(NormalizedKind, bVisible);
    for (const TPair<FString, FBskVisualDefinition>& Pair : ManifestVisuals)
    {
        if (Pair.Value.Kind.Equals(NormalizedKind, ESearchCase::IgnoreCase)) RefreshVisualVisibility(Pair.Key);
    }
}

bool ABskSceneController::ToggleVisualKindVisible(const FString& VisualKind)
{
    const FString NormalizedKind = VisualKind.TrimStartAndEnd().ToLower();
    const bool bNewVisibility = !IsVisualKindVisible(NormalizedKind);
    SetVisualKindVisible(NormalizedKind, bNewVisibility);
    UE_LOG(LogBskUnreal, Display, TEXT("BSK visual helpers kind=%s visible=%s"),
        *NormalizedKind, bNewVisibility ? TEXT("true") : TEXT("false"));
    return bNewVisibility;
}

bool ABskSceneController::IsVisualKindVisible(const FString& VisualKind) const
{
    const FString NormalizedKind = VisualKind.TrimStartAndEnd().ToLower();
    if (const bool* Visibility = VisualKindVisibility.Find(NormalizedKind)) return *Visibility;
    return true;
}

FVector ABskSceneController::GetConfiguredCameraPositionCentimeters() const
{
    return FVector(Converter.LocalMetersToUnrealCentimeters(CameraPositionMeters));
}

FVector ABskSceneController::GetConfiguredCameraLookAtCentimeters() const
{
    return FVector(Converter.LocalMetersToUnrealCentimeters(CameraLookAtMeters));
}

bool ABskSceneController::LoadConfiguration()
{
    FString ConfigPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("bsk_unreal_scene.json"));
    FParse::Value(FCommandLine::Get(), TEXT("BskConfig="), ConfigPath);
    if (FPaths::IsRelative(ConfigPath))
    {
        ConfigPath = FPaths::ConvertRelativePathToFull(ConfigPath);
    }

    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *ConfigPath))
    {
        UE_LOG(LogBskUnreal, Warning, TEXT("Could not read %s; using built-in defaults"), *ConfigPath);
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    {
        UE_LOG(LogBskUnreal, Error, TEXT("Invalid scene configuration: %s"), *ConfigPath);
        return false;
    }

    const TSharedPtr<FJsonObject>* Network = nullptr;
    if (Root->TryGetObjectField(TEXT("network"), Network) && Network != nullptr)
    {
        (*Network)->TryGetStringField(TEXT("listen_address"), ListenAddress);
        double Number = 0.0;
        if ((*Network)->TryGetNumberField(TEXT("port"), Number)) ListenPort = FMath::Clamp(static_cast<int32>(Number), 1, 65535);
        if ((*Network)->TryGetNumberField(TEXT("max_packet_bytes"), Number)) MaxPacketBytes = FMath::Clamp(static_cast<uint32>(Number), 1024u, BskProtocol::DefaultMaxPacketBytes);
    }

    const TSharedPtr<FJsonObject>* Replay = nullptr;
    if (Root->TryGetObjectField(TEXT("replay"), Replay) && Replay != nullptr)
    {
        (*Replay)->TryGetStringField(TEXT("path"), ReplayPath);
        (*Replay)->TryGetNumberField(TEXT("rate"), ReplayRate);
    }
    const TSharedPtr<FJsonObject>* Assets = nullptr;
    if (Root->TryGetObjectField(TEXT("assets"), Assets) && Assets != nullptr)
    {
        (*Assets)->TryGetBoolField(TEXT("allow_external_files"), bAllowExternalAssets);
    }

    double CentimetersPerMeter = 100.0;
    bool bMirrorY = true;
    const TSharedPtr<FJsonObject>* Coordinates = nullptr;
    if (Root->TryGetObjectField(TEXT("coordinates"), Coordinates) && Coordinates != nullptr)
    {
        (*Coordinates)->TryGetNumberField(TEXT("centimeters_per_meter"), CentimetersPerMeter);
        (*Coordinates)->TryGetBoolField(TEXT("mirror_local_y_for_unreal"), bMirrorY);
    }
    Converter = FBskCoordinateConverter(CentimetersPerMeter, bMirrorY);

    const TSharedPtr<FJsonObject>* Scene = nullptr;
    if (Root->TryGetObjectField(TEXT("scene"), Scene) && Scene != nullptr)
    {
        double Number = 0.0;
        if ((*Scene)->TryGetNumberField(TEXT("star_count"), Number)) StarCount = FMath::Clamp(static_cast<int32>(Number), 0, 2000);
        if ((*Scene)->TryGetNumberField(TEXT("star_radius_m"), Number)) StarRadiusMeters = FMath::Max(10.0, Number);
        if ((*Scene)->TryGetNumberField(TEXT("celestial_vault_radius_km"), Number)) CelestialVaultRadiusKilometers = FMath::Clamp(Number, 10000.0, 500000.0);
        if ((*Scene)->TryGetNumberField(TEXT("celestial_background_intensity"), Number)) CelestialBackgroundIntensity = FMath::Clamp(Number, 0.0, 8.0);
        if ((*Scene)->TryGetNumberField(TEXT("sun_intensity_lux"), Number)) SunIntensityLux = FMath::Max(0.0, Number);
        if ((*Scene)->TryGetNumberField(TEXT("fill_light_intensity_lux"), Number)) FillLightIntensityLux = FMath::Max(0.0, Number);
        if ((*Scene)->TryGetNumberField(TEXT("material_exposure_bias"), Number)) MaterialExposureBias = FMath::Clamp(Number, -8.0, 8.0);
        (*Scene)->TryGetBoolField(TEXT("use_official_celestial_assets"), bUseOfficialCelestialAssets);
        (*Scene)->TryGetBoolField(TEXT("use_earth_sky_atmosphere"), bUseEarthSkyAtmosphere);
        JsonVector3(*Scene, TEXT("camera_position_m"), CameraPositionMeters);
        JsonVector3(*Scene, TEXT("camera_look_at_m"), CameraLookAtMeters);
        FVector3d Rotation;
        if (JsonVector3(*Scene, TEXT("sun_rotation_deg"), Rotation)) SunRotation = FRotator(Rotation.X, Rotation.Y, Rotation.Z);
        (*Scene)->TryGetStringField(TEXT("time_mode"), TimeMode);
        double InterpolationDelayMs = InterpolationDelaySeconds * 1000.0;
        double MaxExtrapolationMs = MaxExtrapolationSeconds * 1000.0;
        if ((*Scene)->TryGetNumberField(TEXT("interpolation_delay_ms"), InterpolationDelayMs)) InterpolationDelaySeconds = FMath::Max(0.001, InterpolationDelayMs / 1000.0);
        if ((*Scene)->TryGetNumberField(TEXT("max_extrapolation_ms"), MaxExtrapolationMs)) MaxExtrapolationSeconds = FMath::Max(0.0, MaxExtrapolationMs / 1000.0);
    }

    const TSharedPtr<FJsonObject>* Visuals = nullptr;
    if (Root->TryGetObjectField(TEXT("visuals"), Visuals) && Visuals != nullptr)
    {
        const TSharedPtr<FJsonObject>* VisibilityByKind = nullptr;
        if ((*Visuals)->TryGetObjectField(TEXT("visibility_by_kind"), VisibilityByKind) && VisibilityByKind != nullptr)
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*VisibilityByKind)->Values)
            {
                bool bVisible = true;
                if (Entry.Value.IsValid() && Entry.Value->TryGetBool(bVisible))
                {
                    VisualKindVisibility.Add(Entry.Key.TrimStartAndEnd().ToLower(), bVisible);
                }
            }
        }
    }

    const TSharedPtr<FJsonObject>* Objects = nullptr;
    if (Root->TryGetObjectField(TEXT("objects"), Objects) && Objects != nullptr)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Objects)->Values)
        {
            const TSharedPtr<FJsonObject> Object = Entry.Value->AsObject();
            if (!Object.IsValid()) continue;
            FObjectSpec Spec;
            Object->TryGetStringField(TEXT("asset_type"), Spec.AssetType);
            Object->TryGetStringField(TEXT("asset_path"), Spec.AssetPath);
            Object->TryGetStringField(TEXT("actor_class"), Spec.ActorClass);
            Object->TryGetStringField(TEXT("placeholder_shape"), Spec.PlaceholderShape);
            JsonVector3(Object, TEXT("size_m"), Spec.SizeMeters);
            JsonVector3(Object, TEXT("scale"), Spec.Scale);
            JsonColor(Object, TEXT("color"), Spec.Color);
            ObjectSpecs.Add(Entry.Key, MoveTemp(Spec));
        }
    }

    FString AddressOverride;
    if (FParse::Value(FCommandLine::Get(), TEXT("BskListen="), AddressOverride)) ListenAddress = AddressOverride;
    int32 PortOverride = 0;
    if (FParse::Value(FCommandLine::Get(), TEXT("BskPort="), PortOverride)) ListenPort = FMath::Clamp(PortOverride, 1, 65535);
    FParse::Value(FCommandLine::Get(), TEXT("BskReplay="), ReplayPath);
    FParse::Value(FCommandLine::Get(), TEXT("BskReplayRate="), ReplayRate);
    if (!ReplayPath.IsEmpty() && FPaths::IsRelative(ReplayPath)) ReplayPath = FPaths::ConvertRelativePathToFull(ReplayPath);
    UE_LOG(LogBskUnreal, Display, TEXT("Loaded BSK scene config %s"), *ConfigPath);
    return true;
}

ABskSceneController::FObjectSpec ABskSceneController::ResolveSpec(const FBskRenderObjectState& State) const
{
    if (const FBskObjectDefinition* Definition = ManifestObjects.Find(State.ObjectId))
    {
        if (const FObjectSpec* ById = ObjectSpecs.Find(Definition->ObjectId)) return *ById;
        if (const FObjectSpec* ByName = ObjectSpecs.Find(Definition->DisplayName)) return *ByName;
        if (!Definition->AssetPath.IsEmpty())
        {
            FObjectSpec Result;
            Result.AssetType = TEXT("auto");
            Result.AssetPath = Definition->AssetPath;
            return Result;
        }
    }
    if (const FObjectSpec* Configured = ObjectSpecs.Find(State.Name))
    {
        return *Configured;
    }
    FObjectSpec Result;
    if (!State.AssetPath.IsEmpty())
    {
        Result.AssetType = State.AssetPath.EndsWith(TEXT(".usd"), ESearchCase::IgnoreCase) ||
            State.AssetPath.EndsWith(TEXT(".usda"), ESearchCase::IgnoreCase) ||
            State.AssetPath.EndsWith(TEXT(".usdc"), ESearchCase::IgnoreCase) ? TEXT("usd") : TEXT("auto");
        Result.AssetPath = State.AssetPath;
    }
    const uint32 Hash = GetTypeHash(State.Name);
    Result.Color = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash & 0xff), 190, 240);
    return Result;
}

AActor* ABskSceneController::FindBoundActor(const FString& ObjectName) const
{
    TArray<AActor*> Tagged;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), FName(*BindingTag(ObjectName)), Tagged);
    if (!Tagged.IsEmpty())
    {
        return Tagged[0];
    }
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName().Equals(ObjectName, ESearchCase::IgnoreCase))
        {
            return Actor;
        }
    }
    return nullptr;
}

AActor* ABskSceneController::GetOrCreateActor(const FBskRenderObjectState& State)
{
    const FString& Key = State.ObjectId.IsEmpty() ? State.Name : State.ObjectId;
    if (TObjectPtr<AActor>* Existing = BoundActors.Find(Key))
    {
        return Existing->Get();
    }
    AActor* Actor = FindBoundActor(Key);
    if (!Actor)
    {
        if (const FBskObjectDefinition* Definition = ManifestObjects.Find(Key)) Actor = SpawnManifestObject(*Definition);
        if (!Actor) Actor = SpawnFromSpec(Key, ResolveSpec(State));
    }
    if (Actor)
    {
        Actor->Tags.AddUnique(FName(*BindingTag(Key)));
        if (!State.SemanticLabel.IsEmpty()) Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("BSK.Semantic.%s"), *State.SemanticLabel)));
        BoundActors.Add(Key, Actor);
    }
    return Actor;
}

AActor* ABskSceneController::SpawnFromSpec(const FString& ObjectName, const FObjectSpec& Spec)
{
    FActorSpawnParameters Params;
    Params.Name = MakeUniqueObjectName(GetWorld(), AActor::StaticClass(), SafeActorName(ObjectName));
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    if (!Spec.ActorClass.IsEmpty())
    {
        if (UClass* Class = FSoftClassPath(Spec.ActorClass).TryLoadClass<AActor>())
        {
            AActor* Actor = GetWorld()->SpawnActor<AActor>(Class, FTransform::Identity, Params);
            if (Actor) Actor->SetActorScale3D(FVector(Spec.Scale));
            return Actor;
        }
    }
    if (Spec.AssetType.Equals(TEXT("usd"), ESearchCase::IgnoreCase))
    {
        if (!bAllowExternalAssets && !Spec.AssetPath.StartsWith(TEXT("/Game/")))
        {
            UE_LOG(LogBskUnreal, Warning, TEXT("External asset loading is disabled for %s: %s"), *ObjectName, *Spec.AssetPath);
            return SpawnPlaceholder(ObjectName, Spec);
        }
        if (AActor* Actor = SpawnUsdStage(ObjectName, Spec)) return Actor;
    }
    if ((Spec.AssetType.Equals(TEXT("static_mesh"), ESearchCase::IgnoreCase) || Spec.AssetType.Equals(TEXT("auto"), ESearchCase::IgnoreCase)) && !Spec.AssetPath.IsEmpty())
    {
        if (UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Spec.AssetPath).TryLoad()))
        {
            AStaticMeshActor* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform::Identity, Params);
            Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
            Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            Actor->SetActorScale3D(FVector(Spec.Scale));
            return Actor;
        }
    }
    if (Spec.AssetType.Equals(TEXT("skeletal_mesh"), ESearchCase::IgnoreCase) && !Spec.AssetPath.IsEmpty())
    {
        if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(FSoftObjectPath(Spec.AssetPath).TryLoad()))
        {
            ASkeletalMeshActor* Actor = GetWorld()->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(), FTransform::Identity, Params);
            Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
            Actor->SetActorScale3D(FVector(Spec.Scale));
            return Actor;
        }
    }
    if (!Spec.AssetPath.IsEmpty())
    {
        UE_LOG(LogBskUnreal, Warning, TEXT("Could not load '%s' for %s; using placeholder"), *Spec.AssetPath, *ObjectName);
    }
    return SpawnPlaceholder(ObjectName, Spec);
}

AActor* ABskSceneController::SpawnUsdStage(const FString& ObjectName, const FObjectSpec& Spec)
{
    UClass* StageClass = FSoftClassPath(TEXT("/Script/USDStage.UsdStageActor")).TryLoadClass<AActor>();
    if (!StageClass)
    {
        UE_LOG(LogBskUnreal, Warning, TEXT("USDStage runtime module is unavailable for %s. Enable UE's USD Importer plugin or map an imported asset."), *ObjectName);
        return nullptr;
    }
    const FTransform Transform(FQuat::Identity, FVector::ZeroVector, FVector(Spec.Scale));
    AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(StageClass, Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Actor) return nullptr;
    if (FStructProperty* RootLayer = FindFProperty<FStructProperty>(StageClass, TEXT("RootLayer")))
    {
        if (FFilePath* FilePath = RootLayer->ContainerPtrToValuePtr<FFilePath>(Actor)) FilePath->FilePath = Spec.AssetPath;
    }
    UGameplayStatics::FinishSpawningActor(Actor, Transform);
    return Actor;
}

AActor* ABskSceneController::SpawnPlaceholder(const FString& ObjectName, const FObjectSpec& Spec)
{
    FActorSpawnParameters Params;
    Params.Name = MakeUniqueObjectName(GetWorld(), AStaticMeshActor::StaticClass(), SafeActorName(ObjectName));
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AStaticMeshActor* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform::Identity, Params);
    if (!Actor) return nullptr;
    const TCHAR* MeshPath = PrimitiveMeshPath(Spec.PlaceholderShape);
    UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
    Component->SetMobility(EComponentMobility::Movable);
    UStaticMesh* PlaceholderMesh = LoadObject<UStaticMesh>(nullptr, MeshPath);
    if (!PlaceholderMesh || !Component->SetStaticMesh(PlaceholderMesh))
    {
        UE_LOG(LogBskUnreal, Error, TEXT("Failed to assign placeholder mesh %s to %s"), MeshPath, *ObjectName);
    }
    Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ApplyUnlitColor(Component, Actor, Spec.Color);
    // Engine basic shapes are one meter across, so a size in meters is also
    // their dimensionless actor scale.
    Actor->SetActorScale3D(FVector(Spec.SizeMeters));
    UE_LOG(LogBskUnreal, Display, TEXT("Created visible placeholder %s shape=%s size_m=%s color=%s"),
        *ObjectName, *Spec.PlaceholderShape, *Spec.SizeMeters.ToString(), *Spec.Color.ToString());
    return Actor;
}

void ABskSceneController::ApplyManifest(const FBskSceneManifest& Manifest)
{
    check(IsInGameThread());
    ActiveSessionId = Manifest.SessionId;
    ActiveManifestRevision = Manifest.Revision;
    ConfigureManifestLighting(Manifest);
    InterpolationDelaySeconds = FMath::Max(0.001, Manifest.InterpolationDelayMilliseconds / 1000.0);
    MaxExtrapolationSeconds = FMath::Max(0.0, Manifest.MaxExtrapolationMilliseconds / 1000.0);
    ManifestObjects.Reset();
    ManifestCelestialBodies.Reset();
    ManifestVisuals.Reset();
    ManifestCameras.Reset();
    VisualBaseRotations.Reset();
    VisualBaseScales.Reset();
    VisualDynamicVisibility.Reset();
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        RenderSubsystem->AcceptManifest(Manifest);
    }
    for (const FBskObjectDefinition& Definition : Manifest.Objects)
    {
        ManifestObjects.Add(Definition.ObjectId, Definition);
        if (!BoundActors.Contains(Definition.ObjectId))
        {
            if (AActor* Actor = SpawnManifestObject(Definition))
            {
                Actor->Tags.AddUnique(FName(*BindingTag(Definition.ObjectId)));
                if (!Definition.SemanticLabel.IsEmpty()) Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("BSK.Semantic.%s"), *Definition.SemanticLabel)));
                BoundActors.Add(Definition.ObjectId, Actor);
            }
        }
    }
    for (const FBskCelestialBodyDefinition& Definition : Manifest.CelestialBodies)
    {
        ManifestCelestialBodies.Add(Definition.BodyId, Definition);
        if (!CelestialActors.Contains(Definition.BodyId))
        {
            if (AActor* Actor = SpawnCelestialBody(Definition)) CelestialActors.Add(Definition.BodyId, Actor);
        }
    }
    for (const FBskVisualDefinition& Definition : Manifest.Visuals)
    {
        ManifestVisuals.Add(Definition.VisualId, Definition);
        if (!VisualActors.Contains(Definition.VisualId))
        {
            if (AActor* Actor = SpawnVisual(Definition)) VisualActors.Add(Definition.VisualId, Actor);
        }
        if (AActor* Actor = VisualActors.FindRef(Definition.VisualId))
        {
            ApplyVisualMountTransform(Actor, Definition);
            const FQuat BaseRotation = Actor->GetRootComponent()
                ? Actor->GetRootComponent()->GetRelativeRotation().Quaternion()
                : FQuat::Identity;
            VisualBaseRotations.Add(Definition.VisualId, BaseRotation);
            VisualBaseScales.Add(Definition.VisualId, Actor->GetActorRelativeScale3D());
            VisualDynamicVisibility.Add(Definition.VisualId, true);
            RefreshVisualVisibility(Definition.VisualId);
        }
    }
    for (const FBskCameraDefinition& Definition : Manifest.Cameras)
    {
        ManifestCameras.Add(Definition.CameraId, Definition);
        if (!CameraActors.Contains(Definition.CameraId))
        {
            if (AActor* Actor = SpawnCamera(Definition))
            {
                CameraActors.Add(Definition.CameraId, Actor);
                if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
                {
                    RenderSubsystem->RegisterCameraForCapture(Definition.CameraId, Actor);
                }
            }
        }
    }
    AttachManifestChildren();
    if (APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
    {
        if (ABskCameraPawn* Pawn = Cast<ABskCameraPawn>(Player->GetPawn()))
        {
            Pawn->SetOrbitDistanceMeters(Manifest.DefaultCameraDistanceMeters);
        }
    }
    const FString CameraTarget = Manifest.DefaultCameraTarget.IsEmpty()
        ? Manifest.OriginObjectId
        : Manifest.DefaultCameraTarget;
    if (!CameraTarget.IsEmpty()) FocusObject(CameraTarget, false);
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        RenderSubsystem->NotifyManifestApplied(Manifest);
    }
    int32 GeometryCount = 0;
    for (const FBskObjectDefinition& Definition : Manifest.Objects) GeometryCount += Definition.Geometries.Num();
    UE_LOG(LogBskUnreal, Display, TEXT("Applied BSK scene manifest session=%s revision=%lld objects=%d geoms/visuals=%d/%d celestial=%d cameras=%d"),
        *Manifest.SessionId, Manifest.Revision, Manifest.Objects.Num(), GeometryCount,
        Manifest.Visuals.Num(), Manifest.CelestialBodies.Num(), Manifest.Cameras.Num());
}

void ABskSceneController::ApplyEvent(const FBskRenderEvent& Event)
{
    check(IsInGameThread());
    if (Event.EventKind == TEXT("scene_reset"))
    {
        LastFrameId = -1;
        bHasTargetFrame = false;
    }
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        RenderSubsystem->NotifyEventApplied(Event);
    }
    UE_LOG(LogBskUnreal, Display, TEXT("BSK event kind=%s sequence=%lld"), *Event.EventKind, Event.Sequence);
}

AActor* ABskSceneController::SpawnManifestObject(const FBskObjectDefinition& Definition)
{
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        const FBskRenderSpawnContext Context{GetWorld(), this, &Converter};
        if (AActor* Actor = RenderSubsystem->TrySpawnObject(Context, Definition)) return Actor;
    }

    FBskRenderObjectState State;
    State.ObjectId = Definition.ObjectId;
    State.Name = Definition.DisplayName;
    State.AssetPath = Definition.AssetPath;
    const bool bHasConfiguredSpec = ObjectSpecs.Contains(Definition.ObjectId) || ObjectSpecs.Contains(Definition.DisplayName) || !Definition.AssetPath.IsEmpty();
    if (bHasConfiguredSpec) return SpawnFromSpec(Definition.ObjectId, ResolveSpec(State));
    if (Definition.Geometries.IsEmpty()) return nullptr;

    FActorSpawnParameters Params;
    Params.Name = MakeUniqueObjectName(GetWorld(), AActor::StaticClass(), SafeActorName(Definition.ObjectId));
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
    if (!Actor) return nullptr;
    USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("BodyRoot"));
    Root->SetMobility(EComponentMobility::Movable);
    Root->RegisterComponent();
    Actor->SetRootComponent(Root);
    TMap<int32, UStaticMesh*> ResolvedMeshAssets;
    bool bHasRenderableVisualMesh = false;
    for (int32 Index = 0; Index < Definition.Geometries.Num(); ++Index)
    {
        const FBskGeometryDefinition& Geometry = Definition.Geometries[Index];
        if (!Geometry.Shape.Equals(TEXT("mesh"), ESearchCase::IgnoreCase) || Geometry.AssetPath.IsEmpty()) continue;
        if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Geometry.AssetPath))
        {
            ResolvedMeshAssets.Add(Index, Mesh);
            if (!Geometry.RenderRole.Equals(TEXT("collision"), ESearchCase::IgnoreCase)) bHasRenderableVisualMesh = true;
        }
        else
        {
            UE_LOG(LogBskUnreal, Warning, TEXT("Unable to load mesh '%s' for geometry '%s'; collision fallback will be used"),
                *Geometry.AssetPath, *Geometry.GeometryId);
        }
    }
    for (int32 Index = 0; Index < Definition.Geometries.Num(); ++Index)
    {
        const FBskGeometryDefinition& Geometry = Definition.Geometries[Index];
        const bool bCollisionGeometry = Geometry.RenderRole.Equals(TEXT("collision"), ESearchCase::IgnoreCase);
        if (bCollisionGeometry && bHasRenderableVisualMesh) continue;
        UStaticMesh* Mesh = ResolvedMeshAssets.FindRef(Index);
        if (!Mesh && Geometry.Shape.Equals(TEXT("mesh"), ESearchCase::IgnoreCase)) continue;
        if (!Mesh) Mesh = LoadObject<UStaticMesh>(nullptr, PrimitiveMeshPath(Geometry.Shape));
        if (!Mesh) continue;
        UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor, *FString::Printf(TEXT("Geometry_%d"), Index));
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetCastShadow(true);
        Component->SetStaticMesh(Mesh);
        Component->SetupAttachment(Root);
        Component->RegisterComponent();
        Component->SetRelativeLocation(FVector(Converter.LocalMetersToUnrealCentimeters(Geometry.PositionBodyMeters)));
        Component->SetRelativeRotation(FQuat(Converter.ActiveLocalWxyzToUnreal(Geometry.OrientationBodyFromGeometryWxyz)));
        Component->SetRelativeScale3D(FVector(ResolvedMeshAssets.Contains(Index) ? Geometry.Scale : Geometry.DimensionsMeters));
        ApplyGeometryMaterial(Component, Actor, Geometry, ActiveMaterialAmbient);
    }
    return Actor;
}

AActor* ABskSceneController::SpawnCelestialBody(const FBskCelestialBodyDefinition& Definition)
{
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        const FBskRenderSpawnContext Context{GetWorld(), this, &Converter};
        if (AActor* Actor = RenderSubsystem->TrySpawnCelestialBody(Context, Definition)) return Actor;
    }

    if (bUseOfficialCelestialAssets && Definition.BodyId.Equals(TEXT("moon"), ESearchCase::IgnoreCase))
    {
        UStaticMesh* MoonPlane = LoadObject<UStaticMesh>(nullptr, TEXT("/CelestialVault/Meshes/SM_Plane_FacingX.SM_Plane_FacingX"));
        UMaterialInterface* MoonMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/CelestialVault/Materials/MI_Moon.MI_Moon"));
        if (MoonPlane && MoonMaterial)
        {
            FActorSpawnParameters Params;
            Params.Name = MakeUniqueObjectName(GetWorld(), AStaticMeshActor::StaticClass(), SafeActorName(TEXT("celestial_moon")));
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AStaticMeshActor* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform::Identity, Params);
            if (Actor)
            {
                UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
                Component->SetMobility(EComponentMobility::Movable);
                Component->SetStaticMesh(MoonPlane);
                Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                Component->SetCastShadow(false);
                UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(MoonMaterial, Actor);
                Instance->SetScalarParameterValue(TEXT("MoonAge"), 0.5f);
                Instance->SetScalarParameterValue(TEXT("Brightness"), 1.15f);
                Instance->SetScalarParameterValue(TEXT("EarthLightContribution"), 0.06f);
                Component->SetMaterial(0, Instance);
                const double DiameterMeters = 2.0 * FMath::Max(Definition.EquatorialRadiusMeters, 1.0);
                Actor->SetActorScale3D(FVector(DiameterMeters));
                Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("BSK.Celestial.%s"), *Definition.BodyId)));
                CelestialBillboardIds.Add(Definition.BodyId);
                UE_LOG(LogBskUnreal, Display, TEXT("Using Epic Celestial Vault Moon material for %s"), *Definition.BodyId);
                return Actor;
            }
        }
        UE_LOG(LogBskUnreal, Warning, TEXT("Epic Celestial Vault Moon assets unavailable; using the BSK fallback sphere"));
    }

    FObjectSpec Spec;
    Spec.PlaceholderShape = TEXT("sphere");
    const double DiameterMeters = 2.0 * FMath::Max(Definition.EquatorialRadiusMeters, 1.0);
    Spec.SizeMeters = FVector3d(DiameterMeters, DiameterMeters, DiameterMeters * Definition.PolarRadiusRatio);
    if (Definition.BodyId.Equals(TEXT("earth"), ESearchCase::IgnoreCase)) Spec.Color = FLinearColor(0.03f, 0.16f, 0.65f);
    else if (Definition.BodyId.Equals(TEXT("moon"), ESearchCase::IgnoreCase)) Spec.Color = FLinearColor(0.45f, 0.45f, 0.48f);
    else if (Definition.bLuminous) Spec.Color = FLinearColor(3.0f, 2.5f, 1.2f);
    else Spec.Color = FLinearColor(0.35f, 0.25f, 0.18f);
    AActor* Actor = SpawnPlaceholder(FString::Printf(TEXT("celestial_%s"), *Definition.BodyId), Spec);
    if (Actor) Actor->Tags.AddUnique(FName(*FString::Printf(TEXT("BSK.Celestial.%s"), *Definition.BodyId)));

    if (Actor && bUseEarthSkyAtmosphere && Definition.BodyId.Equals(TEXT("earth"), ESearchCase::IgnoreCase) && !EarthAtmosphere)
    {
        FActorSpawnParameters Params;
        Params.Name = MakeUniqueObjectName(GetWorld(), ASkyAtmosphere::StaticClass(), TEXT("BSK_EarthAtmosphere"));
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        EarthAtmosphere = GetWorld()->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FTransform::Identity, Params);
        if (EarthAtmosphere)
        {
            USkyAtmosphereComponent* Atmosphere = EarthAtmosphere->GetComponent();
            Atmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
            Atmosphere->SetBottomRadius(static_cast<float>(Definition.EquatorialRadiusMeters / 1000.0));
            Atmosphere->SetAtmosphereHeight(100.0f);
            Atmosphere->SetGroundAlbedo(FColor(32, 48, 70));
            Atmosphere->SetMultiScatteringFactor(1.0f);
            EarthAtmosphere->SetActorHiddenInGame(true);
            UE_LOG(LogBskUnreal, Display, TEXT("Created UE SkyAtmosphere for BSK Earth radius %.3f km"), Definition.EquatorialRadiusMeters / 1000.0);
        }
    }
    return Actor;
}

AActor* ABskSceneController::SpawnVisual(const FBskVisualDefinition& Definition)
{
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        const FBskRenderSpawnContext Context{GetWorld(), this, &Converter};
        if (AActor* Actor = RenderSubsystem->TrySpawnVisual(Context, Definition)) return Actor;
    }

    if (Definition.Kind.Equals(TEXT("light"), ESearchCase::IgnoreCase))
    {
        FActorSpawnParameters Params;
        Params.Name = MakeUniqueObjectName(GetWorld(), AActor::StaticClass(), SafeActorName(Definition.VisualId));
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* LightActor = nullptr;
        ULightComponent* LightComponent = nullptr;
        if (Definition.LightType.Equals(TEXT("directional"), ESearchCase::IgnoreCase))
        {
            ADirectionalLight* Light = GetWorld()->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform::Identity, Params);
            LightActor = Light;
            LightComponent = Light ? Light->GetLightComponent() : nullptr;
            if (LightComponent) LightComponent->SetIntensity(static_cast<float>(10.0 * FMath::Max(0.0, Definition.LightIntensity)));
        }
        else
        {
            ASpotLight* Light = GetWorld()->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), FTransform::Identity, Params);
            LightActor = Light;
            LightComponent = Light ? Light->GetLightComponent() : nullptr;
            if (USpotLightComponent* Spot = Light ? Cast<USpotLightComponent>(Light->GetLightComponent()) : nullptr)
            {
                Spot->SetIntensity(static_cast<float>(650.0 * FMath::Max(0.0, Definition.LightIntensity)));
                Spot->SetAttenuationRadius(static_cast<float>(FMath::Max(Definition.RangeMeters, 20.0) * Converter.GetCentimetersPerMeter()));
                Spot->SetInnerConeAngle(static_cast<float>(FMath::Clamp(Definition.LightCutoffDegrees * 0.65, 0.0, 80.0)));
                Spot->SetOuterConeAngle(static_cast<float>(FMath::Clamp(Definition.LightCutoffDegrees, 1.0, 89.0)));
            }
        }
        if (!LightActor || !LightComponent) return nullptr;
        // MJCF target-body lights are rotated as their target moves. Unreal
        // defaults spawned light components to a non-movable mobility, which
        // otherwise emits a warning on every rendered frame and stalls the
        // Game Thread with log I/O.
        LightComponent->SetMobility(EComponentMobility::Movable);
        const double Peak = FMath::Max3(Definition.LightDiffuseRgb.X, Definition.LightDiffuseRgb.Y, Definition.LightDiffuseRgb.Z);
        const FVector3d Normalized = Peak > UE_DOUBLE_SMALL_NUMBER ? Definition.LightDiffuseRgb / Peak : FVector3d::OneVector;
        LightComponent->SetLightColor(FLinearColor(static_cast<float>(Normalized.X), static_cast<float>(Normalized.Y), static_cast<float>(Normalized.Z)));
        const double SpecularPeak = FMath::Max3(Definition.LightSpecularRgb.X, Definition.LightSpecularRgb.Y, Definition.LightSpecularRgb.Z);
        LightComponent->SetSpecularScale(static_cast<float>(Peak > UE_DOUBLE_SMALL_NUMBER ? SpecularPeak / Peak : 0.0));
        LightComponent->SetCastShadows(Definition.bLightCastShadows);
        ApplyVisualMountTransform(LightActor, Definition);
        LightActor->Tags.AddUnique(FName(*FString::Printf(TEXT("BSK.Light.%s"), *Definition.VisualId)));
        return LightActor;
    }

    FObjectSpec Spec;
    Spec.PlaceholderShape = Definition.Kind == TEXT("reaction_wheel") ? TEXT("cylinder") : TEXT("cone");
    Spec.Color = Definition.Color;
    const double LengthMeters = Definition.RangeMeters > 0.0 ? Definition.RangeMeters : FMath::Max(Definition.SizeMeters, 0.1);
    if (Definition.Kind == TEXT("reaction_wheel"))
    {
        Spec.SizeMeters = FVector3d(
            FMath::Max(Definition.SizeMeters, 0.05),
            FMath::Max(Definition.SizeMeters, 0.05),
            FMath::Max(LengthMeters, 0.02));
    }
    else
    {
        const double FovRadians = Definition.FieldOfViewRadians.X > 0.0 ? Definition.FieldOfViewRadians.X : PI / 6.0;
        const double DiameterMeters = 2.0 * LengthMeters * FMath::Tan(0.5 * FovRadians);
        Spec.SizeMeters = FVector3d(FMath::Max(DiameterMeters, 0.02), FMath::Max(DiameterMeters, 0.02), LengthMeters);
    }
    AActor* Actor = SpawnPlaceholder(FString::Printf(TEXT("visual_%s"), *Definition.VisualId), Spec);
    if (!Actor) return nullptr;
    if (UStaticMeshComponent* Component = Actor->FindComponentByClass<UStaticMeshComponent>())
    {
        Component->SetCastShadow(false);
    }
    ApplyVisualMountTransform(Actor, Definition);
    return Actor;
}

void ABskSceneController::ApplyVisualMountTransform(AActor* Actor, const FBskVisualDefinition& Definition) const
{
    if (!Actor) return;
    FVector Location = FVector(Converter.LocalMetersToUnrealCentimeters(Definition.PositionBodyMeters));
    const FVector3d LocalNormal = Definition.NormalBody.GetSafeNormal();
    const FVector Direction = FVector(Converter.LocalMetersToUnrealCentimeters(LocalNormal)).GetSafeNormal();
    FQuat Rotation = Direction.IsNearlyZero() ? FQuat::Identity : FRotationMatrix::MakeFromZ(Direction).ToQuat();
    if (Definition.Kind.Equals(TEXT("light"), ESearchCase::IgnoreCase))
    {
        Rotation = Direction.IsNearlyZero() ? FQuat::Identity : Direction.Rotation().Quaternion();
    }
    if (!Direction.IsNearlyZero() && IsSensorFrustumKind(Definition.Kind))
    {
        const double LengthMeters = Definition.RangeMeters > 0.0
            ? Definition.RangeMeters
            : FMath::Max(Definition.SizeMeters, 0.1);
        // UE's basic cone is centered and points along +Z. Put its apex at the
        // sensor mount and let the volume expand along the sensor boresight.
        Location += FVector(Converter.LocalMetersToUnrealCentimeters(LocalNormal * (0.5 * LengthMeters)));
        Rotation = FRotationMatrix::MakeFromZ(-Direction).ToQuat();
    }
    Actor->SetActorRelativeLocation(Location);
    Actor->SetActorRelativeRotation(Rotation);
}

void ABskSceneController::ConfigureManifestLighting(const FBskSceneManifest& Manifest)
{
    const double AmbientPeak = FMath::Max3(Manifest.HeadlightAmbientRgb.X, Manifest.HeadlightAmbientRgb.Y, Manifest.HeadlightAmbientRgb.Z);
    ActiveMaterialAmbient = Manifest.bUseSceneLighting ? 1.8 * AmbientPeak : 0.18;
    if (SunLight) SunLight->GetLightComponent()->SetIntensity(Manifest.bUseSceneLighting ? 0.0f : static_cast<float>(SunIntensityLux));
    if (FillLight) FillLight->GetLightComponent()->SetIntensity(Manifest.bUseSceneLighting ? 0.0f : static_cast<float>(FillLightIntensityLux));
    if (!Manifest.bHeadlightEnabled)
    {
        if (Headlight) Headlight->SetActorHiddenInGame(true);
        return;
    }
    if (!Headlight)
    {
        FActorSpawnParameters Params;
        Params.Name = MakeUniqueObjectName(GetWorld(), ASpotLight::StaticClass(), TEXT("BSK_MJCF_Headlight"));
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Headlight = GetWorld()->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), FTransform::Identity, Params);
        if (Headlight)
        {
            if (APlayerController* Player = GetWorld()->GetFirstPlayerController())
            {
                if (ABskCameraPawn* Pawn = Cast<ABskCameraPawn>(Player->GetPawn()))
                {
                    Headlight->AttachToComponent(Pawn->GetBskCameraComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
                }
            }
        }
    }
    if (!Headlight) return;
    Headlight->SetActorHiddenInGame(false);
    const double Peak = FMath::Max3(Manifest.HeadlightDiffuseRgb.X, Manifest.HeadlightDiffuseRgb.Y, Manifest.HeadlightDiffuseRgb.Z);
    const FVector3d Normalized = Peak > UE_DOUBLE_SMALL_NUMBER ? Manifest.HeadlightDiffuseRgb / Peak : FVector3d::OneVector;
    USpotLightComponent* Spot = CastChecked<USpotLightComponent>(Headlight->GetLightComponent());
    Spot->SetLightColor(FLinearColor(static_cast<float>(Normalized.X), static_cast<float>(Normalized.Y), static_cast<float>(Normalized.Z)));
    const double SpecularPeak = FMath::Max3(Manifest.HeadlightSpecularRgb.X, Manifest.HeadlightSpecularRgb.Y, Manifest.HeadlightSpecularRgb.Z);
    Spot->SetSpecularScale(static_cast<float>(Peak > UE_DOUBLE_SMALL_NUMBER ? SpecularPeak / Peak : 0.0));
    Spot->SetIntensity(static_cast<float>(1200.0 * Peak));
    Spot->SetAttenuationRadius(10000.0f);
    Spot->SetInnerConeAngle(50.0f);
    Spot->SetOuterConeAngle(80.0f);
    Spot->SetCastShadows(false);
    if (ExposureVolume)
    {
        ExposureVolume->Settings.AutoExposureBias = static_cast<float>(Manifest.bUseSceneLighting ? 0.25 : MaterialExposureBias);
    }
}

void ABskSceneController::RefreshVisualVisibility(const FString& VisualId)
{
    AActor* Actor = VisualActors.FindRef(VisualId);
    const FBskVisualDefinition* Definition = ManifestVisuals.Find(VisualId);
    if (!Actor || !Definition) return;
    const bool* DynamicVisibility = VisualDynamicVisibility.Find(VisualId);
    const bool bDynamicallyVisible = DynamicVisibility == nullptr || *DynamicVisibility;
    Actor->SetActorHiddenInGame(!bDynamicallyVisible || !IsVisualKindVisible(Definition->Kind));
}

AActor* ABskSceneController::SpawnCamera(const FBskCameraDefinition& Definition)
{
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        const FBskRenderSpawnContext Context{GetWorld(), this, &Converter};
        if (AActor* Actor = RenderSubsystem->TrySpawnCamera(Context, Definition)) return Actor;
    }

    FActorSpawnParameters Params;
    Params.Name = MakeUniqueObjectName(GetWorld(), ACameraActor::StaticClass(), SafeActorName(Definition.CameraId));
    ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, Params);
    if (!Camera) return nullptr;
    Camera->GetCameraComponent()->FieldOfView = FMath::RadiansToDegrees(Definition.FieldOfViewRadians);
    Camera->SetActorRelativeLocation(FVector(Converter.LocalMetersToUnrealCentimeters(Definition.PositionBodyMeters)));
    Camera->SetActorRelativeRotation(FQuat(Converter.ActiveLocalWxyzToUnreal(Definition.OrientationBodyFromCameraWxyz)));
    Camera->Tags.AddUnique(FName(*FString::Printf(TEXT("BSK.Camera.%s"), *Definition.CameraId)));
    return Camera;
}

void ABskSceneController::AttachManifestChildren()
{
    for (const TPair<FString, FBskObjectDefinition>& Pair : ManifestObjects)
    {
        AActor* Actor = BoundActors.FindRef(Pair.Key);
        AActor* Parent = Pair.Value.ParentId.IsEmpty() ? nullptr : BoundActors.FindRef(Pair.Value.ParentId);
        if (Actor && Parent && Actor->GetAttachParentActor() != Parent) Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
    }
    for (const TPair<FString, FBskVisualDefinition>& Pair : ManifestVisuals)
    {
        AActor* Actor = VisualActors.FindRef(Pair.Key);
        AActor* Parent = BoundActors.FindRef(Pair.Value.ParentId);
        if (Actor && Parent) Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform);
    }
    for (const TPair<FString, FBskCameraDefinition>& Pair : ManifestCameras)
    {
        AActor* Actor = CameraActors.FindRef(Pair.Key);
        AActor* Parent = Pair.Value.ParentId.IsEmpty() ? nullptr : BoundActors.FindRef(Pair.Value.ParentId);
        if (Actor && Parent) Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform);
    }
}

void ABskSceneController::ApplyFrame(const FBskRenderFrame& Frame)
{
    check(IsInGameThread());
    if (LastFrameId < 0)
    {
        UE_LOG(LogBskUnreal, Display, TEXT("Applying first BSK frame %lld with %d objects on the Game Thread"), Frame.FrameId, Frame.Objects.Num());
    }
    for (const FBskRenderObjectState& State : Frame.Objects)
    {
        GetOrCreateActor(State);
    }
    for (const FBskRenderObjectState& State : Frame.Objects)
    {
        const FString& Key = State.ObjectId.IsEmpty() ? State.Name : State.ObjectId;
        AActor* Actor = BoundActors.FindRef(Key);
        if (!Actor) continue;
        FString ParentId = State.Parent;
        if (const FBskObjectDefinition* Definition = ManifestObjects.Find(Key)) ParentId = Definition->ParentId;
        AActor* DesiredParent = ParentId.IsEmpty() ? nullptr : BoundActors.FindRef(ParentId);
        if (DesiredParent && Actor->GetAttachParentActor() != DesiredParent)
        {
            Actor->AttachToActor(DesiredParent, FAttachmentTransformRules::KeepWorldTransform);
        }
        else if (!DesiredParent && Actor->GetAttachParentActor())
        {
            Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        }
        const FVector Location(Converter.LocalMetersToUnrealCentimeters(State.PositionMeters));
        const FQuat Rotation(Converter.ActiveLocalWxyzToUnreal(State.OrientationWxyz));
        Actor->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
    }
    UpdateCelestialBodies(Frame);
    UpdateVisualStates(Frame);
    UpdateLightTargets();
    DrawOrbitLines(Frame);
    LastFrameId = Frame.FrameId;
    FrameAppliedEvent.Broadcast(Frame);
    if (UBskRenderWorldSubsystem* RenderSubsystem = GetWorld()->GetSubsystem<UBskRenderWorldSubsystem>())
    {
        RenderSubsystem->NotifyFrameApplied(Frame);
    }

    if (!bScreenshotRequested)
    {
        FString ScreenshotPath;
        if (FParse::Value(FCommandLine::Get(), TEXT("BskScreenshot="), ScreenshotPath) && !ScreenshotPath.IsEmpty())
        {
            if (FPaths::IsRelative(ScreenshotPath))
            {
                ScreenshotPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), ScreenshotPath);
            }
            bScreenshotRequested = true;
            FTimerHandle ScreenshotTimer;
            GetWorldTimerManager().SetTimer(
                ScreenshotTimer,
                [ScreenshotPath]()
                {
                    FScreenshotRequest::RequestScreenshot(ScreenshotPath, false, false);
                    UE_LOG(LogBskUnreal, Display, TEXT("Requested delayed BSK validation screenshot: %s"), *ScreenshotPath);
                },
                1.5f,
                false);
        }
    }
}

FBskRenderFrame ABskSceneController::InterpolateFrame(const FBskRenderFrame& From, const FBskRenderFrame& To, double Alpha) const
{
    FBskRenderFrame Result = To;
    Result.SimulationTimeNanoseconds = FMath::RoundToInt64(FMath::Lerp(
        static_cast<double>(From.SimulationTimeNanoseconds), static_cast<double>(To.SimulationTimeNanoseconds), Alpha));
    Result.OriginInertialMeters = FMath::Lerp(From.OriginInertialMeters, To.OriginInertialMeters, Alpha);
    TMap<FString, const FBskRenderObjectState*> FromObjects;
    for (const FBskRenderObjectState& State : From.Objects)
    {
        FromObjects.Add(State.ObjectId.IsEmpty() ? State.Name : State.ObjectId, &State);
    }
    for (FBskRenderObjectState& State : Result.Objects)
    {
        const FString& Key = State.ObjectId.IsEmpty() ? State.Name : State.ObjectId;
        if (const FBskRenderObjectState* const* Prior = FromObjects.Find(Key))
        {
            State.PositionMeters = FMath::Lerp((*Prior)->PositionMeters, State.PositionMeters, Alpha);
            State.OrientationWxyz = FQuat4d::Slerp((*Prior)->OrientationWxyz, State.OrientationWxyz, Alpha).GetNormalized();
            if ((*Prior)->bHasVelocity && State.bHasVelocity)
            {
                State.VelocityMetersPerSecond = FMath::Lerp((*Prior)->VelocityMetersPerSecond, State.VelocityMetersPerSecond, Alpha);
            }
        }
    }
    TMap<FString, const FBskCelestialBodyState*> FromCelestial;
    for (const FBskCelestialBodyState& State : From.CelestialBodies) FromCelestial.Add(State.BodyId, &State);
    for (FBskCelestialBodyState& State : Result.CelestialBodies)
    {
        if (const FBskCelestialBodyState* const* Prior = FromCelestial.Find(State.BodyId))
        {
            State.PositionMeters = FMath::Lerp((*Prior)->PositionMeters, State.PositionMeters, Alpha);
            State.OrientationWxyz = FQuat4d::Slerp((*Prior)->OrientationWxyz, State.OrientationWxyz, Alpha).GetNormalized();
        }
    }
    TMap<FString, const FBskVisualState*> FromVisuals;
    for (const FBskVisualState& State : From.VisualStates) FromVisuals.Add(State.VisualId, &State);
    for (FBskVisualState& State : Result.VisualStates)
    {
        const FBskVisualState* const* Prior = FromVisuals.Find(State.VisualId);
        if (!Prior) continue;
        State.Value = FMath::Lerp((*Prior)->Value, State.Value, Alpha);
        for (TPair<FString, FBskVisualState::FChannelValue>& Pair : State.Channels)
        {
            const FBskVisualState::FChannelValue* PriorChannel = (*Prior)->Channels.Find(Pair.Key);
            if (PriorChannel && PriorChannel->Type == FBskVisualState::FChannelValue::EType::Number &&
                Pair.Value.Type == FBskVisualState::FChannelValue::EType::Number)
            {
                Pair.Value.Number = FMath::Lerp(PriorChannel->Number, Pair.Value.Number, Alpha);
            }
        }
    }
    return Result;
}

void ABskSceneController::UpdateCelestialBodies(const FBskRenderFrame& Frame)
{
    for (const FBskCelestialBodyState& State : Frame.CelestialBodies)
    {
        AActor* Actor = CelestialActors.FindRef(State.BodyId);
        if (!Actor) continue;
        const FVector Location(Converter.LocalMetersToUnrealCentimeters(State.PositionMeters));
        FQuat Rotation(Converter.ActiveLocalWxyzToUnreal(State.OrientationWxyz));
        if (CelestialBillboardIds.Contains(State.BodyId))
        {
            FVector ViewLocation = FVector::ZeroVector;
            FRotator ViewRotation;
            if (APlayerController* Player = GetWorld()->GetFirstPlayerController()) Player->GetPlayerViewPoint(ViewLocation, ViewRotation);
            const FVector DirectionToViewer = (ViewLocation - Location).GetSafeNormal();
            if (!DirectionToViewer.IsNearlyZero()) Rotation = DirectionToViewer.Rotation().Quaternion();
        }
        Actor->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
        if (EarthAtmosphere && State.BodyId.Equals(TEXT("earth"), ESearchCase::IgnoreCase))
        {
            EarthAtmosphere->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
            EarthAtmosphere->SetActorHiddenInGame(false);
        }
        if (const FBskCelestialBodyDefinition* Definition = ManifestCelestialBodies.Find(State.BodyId); Definition && Definition->bLuminous && SunLight)
        {
            const FVector DirectionToOrigin = (-Location).GetSafeNormal();
            if (!DirectionToOrigin.IsNearlyZero()) SunLight->SetActorRotation(DirectionToOrigin.Rotation());
        }
    }
}

void ABskSceneController::UpdateVisualStates(const FBskRenderFrame& Frame)
{
    for (const FBskVisualState& State : Frame.VisualStates)
    {
        if (AActor* Actor = VisualActors.FindRef(State.VisualId))
        {
            const FBskVisualDefinition* Definition = ManifestVisuals.Find(State.VisualId);
            if (!Definition) continue;
            auto NumberChannel = [&State](const TCHAR* Name, double Fallback)
            {
                if (const FBskVisualState::FChannelValue* Channel = State.Channels.Find(Name))
                {
                    if (Channel->Type == FBskVisualState::FChannelValue::EType::Number) return Channel->Number;
                    if (Channel->Type == FBskVisualState::FChannelValue::EType::Boolean) return Channel->Boolean ? 1.0 : 0.0;
                }
                return Fallback;
            };
            auto BoolChannel = [&State](const TCHAR* Name, bool Fallback)
            {
                if (const FBskVisualState::FChannelValue* Channel = State.Channels.Find(Name))
                {
                    if (Channel->Type == FBskVisualState::FChannelValue::EType::Boolean) return Channel->Boolean;
                    if (Channel->Type == FBskVisualState::FChannelValue::EType::Number) return !FMath::IsNearlyZero(Channel->Number);
                }
                return Fallback;
            };

            bool bVisible = State.bVisible;
            if (Definition->Kind == TEXT("reaction_wheel"))
            {
                const double Angle = NumberChannel(TEXT("angle_rad"), 0.0);
                const FQuat Base = VisualBaseRotations.FindRef(State.VisualId);
                Actor->SetActorRelativeRotation(Base * FQuat(FVector::UpVector, Angle));
                bVisible = BoolChannel(TEXT("enabled"), bVisible);
            }
            else if (Definition->Kind == TEXT("thruster"))
            {
                const double Throttle = FMath::Clamp(NumberChannel(TEXT("throttle"), State.Value), 0.0, 1.0);
                const FVector BaseScale = VisualBaseScales.FindRef(State.VisualId);
                Actor->SetActorRelativeScale3D(FVector(BaseScale.X, BaseScale.Y, BaseScale.Z * FMath::Max(Throttle, 0.02)));
                bVisible = BoolChannel(TEXT("enabled"), bVisible) && Throttle > 1.0e-6;
            }
            else if (Definition->Kind == TEXT("css"))
            {
                const double Signal = FMath::Clamp(NumberChannel(TEXT("normalized_signal"), State.Value), 0.0, 1.0);
                const FVector BaseScale = VisualBaseScales.FindRef(State.VisualId);
                Actor->SetActorRelativeScale3D(BaseScale * (0.85 + 0.15 * Signal));
                bVisible = BoolChannel(TEXT("enabled"), bVisible);
            }
            else if (Definition->Kind == TEXT("generic_sensor") || Definition->Kind == TEXT("light"))
            {
                bVisible = BoolChannel(TEXT("enabled"), bVisible);
            }
            VisualDynamicVisibility.Add(State.VisualId, bVisible);
            RefreshVisualVisibility(State.VisualId);

            const double Intensity = Definition->Kind == TEXT("thruster")
                ? FMath::Clamp(NumberChannel(TEXT("throttle"), State.Value), 0.0, 1.0)
                : 1.0;
            TArray<UStaticMeshComponent*> Components;
            Actor->GetComponents<UStaticMeshComponent>(Components);
            for (UStaticMeshComponent* Component : Components)
            {
                if (UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0)))
                {
                    Material->SetVectorParameterValue(TEXT("Color"), Definition->Color * static_cast<float>(0.35 + 0.65 * Intensity));
                }
            }
        }
    }
}

void ABskSceneController::UpdateLightTargets()
{
    for (const TPair<FString, FBskVisualDefinition>& Pair : ManifestVisuals)
    {
        const FBskVisualDefinition& Definition = Pair.Value;
        if (!Definition.Kind.Equals(TEXT("light"), ESearchCase::IgnoreCase) || Definition.LightTargetId.IsEmpty()) continue;
        AActor* LightActor = VisualActors.FindRef(Pair.Key);
        AActor* TargetActor = BoundActors.FindRef(Definition.LightTargetId);
        if (!LightActor || !TargetActor) continue;
        const FVector Direction = (TargetActor->GetActorLocation() - LightActor->GetActorLocation()).GetSafeNormal();
        if (!Direction.IsNearlyZero()) LightActor->SetActorRotation(Direction.Rotation());
    }
}

void ABskSceneController::DrawOrbitLines(const FBskRenderFrame& Frame) const
{
    const FBskCelestialBodyState* CentralState = nullptr;
    const FBskCelestialBodyDefinition* CentralDefinition = nullptr;
    for (const FBskCelestialBodyState& State : Frame.CelestialBodies)
    {
        const FBskCelestialBodyDefinition* Definition = ManifestCelestialBodies.Find(State.BodyId);
        if (Definition && (State.BodyId.Equals(TEXT("earth"), ESearchCase::IgnoreCase) ||
            Definition->DisplayName.Equals(TEXT("earth"), ESearchCase::IgnoreCase)))
        {
            CentralState = &State;
            CentralDefinition = Definition;
            break;
        }
    }
    if (!CentralState || !CentralDefinition || CentralDefinition->MuMetersCubedPerSecondSquared <= 0.0) return;
    constexpr int32 SegmentCount = 128;
    for (const FBskRenderObjectState& State : Frame.Objects)
    {
        const FString& Key = State.ObjectId.IsEmpty() ? State.Name : State.ObjectId;
        const FBskObjectDefinition* Definition = ManifestObjects.Find(Key);
        if (!State.bHasVelocity || (Definition && !Definition->ParentId.IsEmpty())) continue;
        const FVector3d R = State.PositionMeters - CentralState->PositionMeters;
        const FVector3d V = State.VelocityMetersPerSecond - CentralState->VelocityMetersPerSecond;
        const double Radius = R.Length();
        const FVector3d H = FVector3d::CrossProduct(R, V);
        const double HSquared = H.SquaredLength();
        if (Radius <= 1.0 || HSquared <= UE_DOUBLE_SMALL_NUMBER) continue;
        const double Mu = CentralDefinition->MuMetersCubedPerSecondSquared;
        const FVector3d EccentricityVector = FVector3d::CrossProduct(V, H) / Mu - R / Radius;
        const double Eccentricity = EccentricityVector.Length();
        const FVector3d P = Eccentricity > 1.0e-8 ? EccentricityVector / Eccentricity : R / Radius;
        const FVector3d Q = FVector3d::CrossProduct(H.GetSafeNormal(), P).GetSafeNormal();
        const double SemiLatusRectum = HSquared / Mu;
        FVector Previous;
        bool bHavePrevious = false;
        const uint32 Hash = GetTypeHash(Key);
        const FColor Color = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash & 0xff), 180, 255).ToFColor(true);
        for (int32 Index = 0; Index <= SegmentCount; ++Index)
        {
            const double Anomaly = 2.0 * PI * static_cast<double>(Index) / static_cast<double>(SegmentCount);
            const double Denominator = 1.0 + Eccentricity * FMath::Cos(Anomaly);
            if (Denominator <= 1.0e-6)
            {
                bHavePrevious = false;
                continue;
            }
            const double OrbitRadius = SemiLatusRectum / Denominator;
            const FVector3d LocalMeters = CentralState->PositionMeters + OrbitRadius * (P * FMath::Cos(Anomaly) + Q * FMath::Sin(Anomaly));
            const FVector Current(Converter.LocalMetersToUnrealCentimeters(LocalMeters));
            if (bHavePrevious) DrawDebugLine(GetWorld(), Previous, Current, Color, false, 0.0f, 0, 1.0f);
            Previous = Current;
            bHavePrevious = true;
        }
    }
}

void ABskSceneController::CreateEnvironment()
{
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (ADirectionalLight* Sun = GetWorld()->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FVector::ZeroVector, SunRotation, Params))
    {
        SunLight = Sun;
        Sun->GetLightComponent()->SetIntensity(static_cast<float>(SunIntensityLux));
        Sun->GetLightComponent()->SetCastShadows(true);
        if (UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
        {
            Directional->SetAtmosphereSunLight(true);
            Directional->SetAtmosphereSunLightIndex(0);
            Directional->bPerPixelAtmosphereTransmittance = true;
        }
    }

    // MuJoCo's default viewer uses a camera/headlight contribution in addition
    // to its key light.  This low-intensity, shadowless fill preserves the same
    // useful shape readability without pretending to be another physical sun.
    if (FillLightIntensityLux > 0.0)
    {
        const FRotator FillRotation(-SunRotation.Pitch * 0.5, SunRotation.Yaw + 165.0, 0.0);
        if (ADirectionalLight* Fill = GetWorld()->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FVector::ZeroVector, FillRotation, Params))
        {
            FillLight = Fill;
            Fill->GetLightComponent()->SetIntensity(static_cast<float>(FillLightIntensityLux));
            Fill->GetLightComponent()->SetLightColor(FLinearColor(0.55f, 0.68f, 1.0f));
            Fill->GetLightComponent()->SetCastShadows(false);
        }
    }

    // A fixed exposure makes protocol material colours reproducible between a
    // black space view, a bright planet view, screenshots, and live rendering.
    if (APostProcessVolume* Volume = GetWorld()->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params))
    {
        ExposureVolume = Volume;
        Volume->bUnbound = true;
        Volume->Priority = 1000.0f;
        Volume->Settings.bOverride_AutoExposureMethod = true;
        Volume->Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        Volume->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
        Volume->Settings.AutoExposureApplyPhysicalCameraExposure = false;
        Volume->Settings.bOverride_AutoExposureBias = true;
        Volume->Settings.AutoExposureBias = static_cast<float>(MaterialExposureBias);
    }

    if (bUseOfficialCelestialAssets)
    {
        UStaticMesh* VaultMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/CelestialVault/Meshes/SM_CelestialVault.SM_CelestialVault"));
        UMaterialInterface* VaultMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/CelestialVault/Materials/MI_CelestialVault.MI_CelestialVault"));
        if (VaultMesh && VaultMaterial)
        {
            DeepSkyComponent = NewObject<UStaticMeshComponent>(this, TEXT("EpicCelestialVaultBackground"));
            DeepSkyComponent->SetupAttachment(GetRootComponent());
            DeepSkyComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            DeepSkyComponent->SetCastShadow(false);
            DeepSkyComponent->SetAffectDynamicIndirectLighting(false);
            DeepSkyComponent->SetCanEverAffectNavigation(false);
            DeepSkyComponent->SetStaticMesh(VaultMesh);
            UMaterialInstanceDynamic* VaultInstance = UMaterialInstanceDynamic::Create(VaultMaterial, this);
            VaultInstance->SetScalarParameterValue(TEXT("Global Intensity"), static_cast<float>(CelestialBackgroundIntensity));
            VaultInstance->SetScalarParameterValue(TEXT("Background Intensity"), 1.0f);
            VaultInstance->SetScalarParameterValue(TEXT("Constellations Intensity"), 0.0f);
            VaultInstance->SetScalarParameterValue(TEXT("Grid Intensity"), 0.0f);
            DeepSkyComponent->SetMaterial(0, VaultInstance);
            DeepSkyComponent->SetRelativeScale3D(FVector(CelestialVaultRadiusKilometers * 1000.0));
            DeepSkyComponent->RegisterComponent();
            UE_LOG(LogBskUnreal, Display, TEXT("Using Epic Celestial Vault Milky Way background at %.0f km"), CelestialVaultRadiusKilometers);
        }
        else
        {
            UE_LOG(LogBskUnreal, Warning, TEXT("Epic Celestial Vault background assets unavailable; keeping black background"));
        }
    }

    UInstancedStaticMeshComponent* Stars = NewObject<UInstancedStaticMeshComponent>(this, TEXT("StarField"));
    Stars->SetupAttachment(GetRootComponent());
    Stars->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Stars->SetCastShadow(false);
    UStaticMesh* OfficialStarMesh = bUseOfficialCelestialAssets
        ? LoadObject<UStaticMesh>(nullptr, TEXT("/CelestialVault/Meshes/SM_Plane_FacingX.SM_Plane_FacingX"))
        : nullptr;
    UMaterialInterface* OfficialStarMaterial = bUseOfficialCelestialAssets
        ? LoadObject<UMaterialInterface>(nullptr, TEXT("/CelestialVault/Materials/MI_Stars.MI_Stars"))
        : nullptr;
    const bool bUsingOfficialStars = OfficialStarMesh && OfficialStarMaterial;
    Stars->SetStaticMesh(bUsingOfficialStars ? OfficialStarMesh : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    if (bUsingOfficialStars)
    {
        Stars->SetNumCustomDataFloats(4);
        UMaterialInstanceDynamic* StarInstance = UMaterialInstanceDynamic::Create(OfficialStarMaterial, this);
        StarInstance->SetScalarParameterValue(TEXT("MagnitudeOffset"), -2.0f);
        StarInstance->SetScalarParameterValue(TEXT("Global Size Factor"), 1.25f);
        Stars->SetMaterial(0, StarInstance);
    }
    else if (UMaterialInterface* StarMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BSK/M_BskUnlitColor.M_BskUnlitColor")))
    {
        UMaterialInstanceDynamic* StarInstance = UMaterialInstanceDynamic::Create(StarMaterial, this);
        StarInstance->SetVectorParameterValue(TEXT("Color"), FLinearColor(2.5f, 2.5f, 2.5f, 1.0f));
        Stars->SetMaterial(0, StarInstance);
    }
    Stars->RegisterComponent();
    FRandomStream Random(0x42534b);
    const double RadiusCm = bUsingOfficialStars
        ? CelestialVaultRadiusKilometers * 100000.0 * 0.99
        : StarRadiusMeters * Converter.GetCentimetersPerMeter();
    for (int32 Index = 0; Index < StarCount; ++Index)
    {
        FVector Direction(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f));
        if (!Direction.Normalize()) { --Index; continue; }
        if (bUsingOfficialStars)
        {
            const int32 InstanceIndex = Stars->AddInstance(FTransform(FQuat::Identity, Direction * RadiusCm));
            const float Warmth = Random.FRandRange(0.0f, 1.0f);
            const FLinearColor StarColor = FLinearColor::LerpUsingHSV(FLinearColor(0.62f, 0.75f, 1.0f), FLinearColor(1.0f, 0.78f, 0.55f), Warmth);
            TArray<float> CustomData{StarColor.R, StarColor.G, StarColor.B, static_cast<float>(Random.FRandRange(0.0f, 5.5f))};
            Stars->SetCustomData(InstanceIndex, CustomData);
        }
        else
        {
            const float Scale = Random.FRandRange(0.4f, 1.0f);
            Stars->AddInstance(FTransform(FQuat::Identity, Direction * RadiusCm, FVector(Scale)));
        }
    }
    UE_LOG(LogBskUnreal, Display, TEXT("Created %d %s stars"), StarCount, bUsingOfficialStars ? TEXT("Epic Celestial Vault") : TEXT("fallback"));
}
