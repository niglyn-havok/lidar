#include "DublinFlightPawn.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "CollisionQueryParams.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"
#include "UnrealClient.h"
#include "Visuals/DublinAircraftVisual.h"
#include "Weapons/DublinWeaponComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinFlight, Log, All);

namespace DublinFlight
{
struct FNativeKey
{
	FKey Key;
	EControlKey Control;
};

const TArray<FNativeKey>& GetNativeKeys()
{
	static const TArray<FNativeKey> Keys = {
		{EKeys::W, EControlKey::Forward}, {EKeys::S, EControlKey::Backward},
		{EKeys::A, EControlKey::Left}, {EKeys::D, EControlKey::Right},
		{EKeys::Q, EControlKey::YawLeft}, {EKeys::E, EControlKey::YawRight},
		{EKeys::LeftShift, EControlKey::ThrottleUpLeft}, {EKeys::RightShift, EControlKey::ThrottleUpRight},
		{EKeys::LeftControl, EControlKey::ThrottleDownLeft}, {EKeys::RightControl, EControlKey::ThrottleDownRight},
		{EKeys::SpaceBar, EControlKey::Up}, {EKeys::C, EControlKey::Down}
	};
	return Keys;
}
}

ADublinFlightPawn::ADublinFlightPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	bReplicates = false;
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	CollisionRoot = CreateDefaultSubobject<USphereComponent>(TEXT("FlightCollision"));
	SetRootComponent(CollisionRoot);
	CollisionRoot->InitSphereRadius(470.0f);
	CollisionRoot->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionRoot->SetCollisionObjectType(ECC_Pawn);
	CollisionRoot->SetCollisionResponseToAllChannels(ECR_Block);
	CollisionRoot->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	CollisionRoot->SetGenerateOverlapEvents(false);
	CollisionRoot->SetCanEverAffectNavigation(false);
	CollisionRoot->SetSimulatePhysics(false);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
		TEXT("/Game/Materials/M_FlightSurface.M_FlightSurface"));
	AircraftMaterial = Material.Object;
	if (!AircraftMaterial)
	{
		UE_LOG(LogDublinFlight, Error, TEXT("Missing /Game/Materials/M_FlightSurface; aircraft visuals disabled."));
		FlightStatus = TEXT("Aircraft material unavailable; inspect the log.");
	}
	AircraftVisualsRoot = CreateDefaultSubobject<USceneComponent>(TEXT("AircraftVisualsRoot"));
	AircraftVisualsRoot->SetupAttachment(CollisionRoot);
	AircraftAirframe = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("AircraftAirframe"));
	AircraftPropeller = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("AircraftPropeller"));
	AircraftParts = {AircraftAirframe, AircraftPropeller};
	for (UProceduralMeshComponent* Part : AircraftParts)
	{
		Part->SetupAttachment(AircraftVisualsRoot);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCollisionResponseToAllChannels(ECR_Ignore);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
		Part->SetSimulatePhysics(false);
		Part->bUseComplexAsSimpleCollision = false;
		Part->PrimaryComponentTick.bCanEverTick = false;
		Part->SetVisibility(false);
	}
	AircraftPropeller->SetRelativeLocation(FVector(DublinAircraftVisual::PropellerOffsetCm, 0.0, 0.0));

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("ChaseBoom"));
	CameraBoom->SetupAttachment(CollisionRoot);
	CameraBoom->TargetArmLength = 1750.0f;
	CameraBoom->TargetOffset = FVector(0.0, 0.0, 200.0);
	CameraBoom->SocketOffset = FVector(0.0, 0.0, 70.0);
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bEnableCameraLag = false;
	CameraBoom->bEnableCameraRotationLag = false;
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->ProbeSize = 20.0f;
	CameraBoom->AddTickPrerequisiteActor(this);

	ChaseCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ChaseCamera"));
	ChaseCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	ChaseCamera->bUsePawnControlRotation = false;
	ChaseCamera->FieldOfView = 90.0f;
	Weapons = CreateDefaultSubobject<UDublinWeaponComponent>(TEXT("Weapons"));
}

void ADublinFlightPawn::InitializeAircraftMaterials()
{
	if (bAircraftVisualsReady || HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	FLinearColor ExistingColor;
	float ExistingRoughness = 0.0f;
	if (!AircraftMaterial
		|| !AircraftMaterial->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tint")), ExistingColor)
		|| !AircraftMaterial->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Roughness")), ExistingRoughness))
	{
		UE_LOG(LogDublinFlight, Error, TEXT("M_FlightSurface requires Tint and Roughness; aircraft visuals disabled."));
		FlightStatus = TEXT("Aircraft material unavailable; inspect the log.");
		return;
	}
	AircraftMaterials.Reset();
	for (int32 Index = 0; Index < DublinAircraftVisual::FinishCount; ++Index)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(AircraftMaterial, this);
		if (Instance)
		{
			const DublinAircraftVisual::FFinish Finish =
				DublinAircraftVisual::GetFinish(static_cast<DublinAircraftVisual::EFinish>(Index));
			Instance->SetVectorParameterValue(TEXT("Tint"), Finish.Tint);
			Instance->SetScalarParameterValue(TEXT("Roughness"), Finish.Roughness);
			AircraftMaterials.Add(Instance);
		}
		else
		{
			UE_LOG(LogDublinFlight, Error, TEXT("Unable to create aircraft material for part %d."), Index);
			FlightStatus = TEXT("Aircraft material unavailable; inspect the log.");
			AircraftMaterials.Reset();
			return;
		}
	}
	// Cache only pure CPU geometry. Components and materials remain owned by each pawn.
	static const DublinAircraftVisual::FGeometry Geometry = DublinAircraftVisual::BuildGeometry();
	const auto CreateSection = [this](UProceduralMeshComponent* Component, int32 Index,
		const DublinAircraftVisual::FSection& Section)
	{
		Component->CreateMeshSection(Index, Section.Vertices, Section.Triangles, Section.Normals,
			Section.UV, TArray<FColor>(), Section.Tangents, false);
		Component->SetMaterial(Index, AircraftMaterials[static_cast<int32>(Section.Finish)]);
	};
	for (int32 Index = 0; Index < Geometry.Airframe.Num(); ++Index)
	{
		CreateSection(AircraftAirframe, Index, Geometry.Airframe[Index]);
	}
	CreateSection(AircraftPropeller, 0, Geometry.Propeller);
	for (UProceduralMeshComponent* Part : AircraftParts)
	{
		Part->SetVisibility(true);
	}
	bAircraftVisualsReady = true;
}

void ADublinFlightPawn::UpdateAircraftVisuals(float DeltaSeconds)
{
	if (!bAircraftVisualsReady || bAircraftHiddenForCamera || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f)
	{
		return;
	}
	PropellerUpdateSeconds += FMath::Min(DeltaSeconds, 0.1f);
	if (PropellerUpdateSeconds < 1.0f / 30.0f)
	{
		return;
	}
	// Deliberately stylized rotation; it never drives thrust, velocity, collision, or weapon timing.
	PropellerAngleDegrees = FMath::Fmod(PropellerAngleDegrees + 1500.0 * PropellerUpdateSeconds, 360.0);
	PropellerUpdateSeconds = 0.0f;
	AircraftPropeller->SetRelativeRotation(FRotator(0.0, 0.0, PropellerAngleDegrees));
}

void ADublinFlightPawn::BeginPlay()
{
	Super::BeginPlay();
	const bool bValidSpawn = Simulation.Initialize(GetActorLocation(), GetActorQuat());
	InitialSpawnTransform = Simulation.GetSpawnTransform();
	bSpawnCaptured = true;
	InitializeAircraftMaterials();
	if (!FFileHelper::LoadFileToString(CreditsText,
		*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data"), TEXT("ATTRIBUTION.txt"))))
	{
		CreditsText = TEXT("(c) OpenStreetMap contributors; Survey Laefer et al., CC BY 4.0.\n"
			"Full credits unavailable: Content/Data/ATTRIBUTION.txt could not be loaded.");
		UE_LOG(LogDublinFlight, Warning, TEXT("Could not load Content/Data/ATTRIBUTION.txt for in-game credits."));
	}
	if (!bValidSpawn)
	{
		SetBlockedStatus(TEXT("Invalid spawn transform. Place a finite PlayerStart in clear air."));
	}
	else if (!IsPositionClear(GetActorLocation(), GetActorQuat()))
	{
		SetBlockedStatus(TEXT("Spawn obstructed. Clear a 4.7 m radius around PlayerStart, then Home."));
	}
	UpdateReadableState();
}

void ADublinFlightPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	check(PlayerInputComponent);
	// UEnhancedInputComponent inherits these native bindings; no mapping assets are needed.
	for (const DublinFlight::FNativeKey& Key : DublinFlight::GetNativeKeys())
	{
		PlayerInputComponent->BindKey(Key.Key, IE_Pressed, this, &ADublinFlightPawn::HandleKeyPressed);
		PlayerInputComponent->BindKey(Key.Key, IE_Released, this, &ADublinFlightPawn::HandleKeyReleased);
	}
	PlayerInputComponent->BindKey(EKeys::G, IE_Pressed, this, &ADublinFlightPawn::ToggleGodMode);
	PlayerInputComponent->BindKey(EKeys::Home, IE_Pressed, this, &ADublinFlightPawn::ResetFlight);
	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &ADublinFlightPawn::HandleMouseX);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &ADublinFlightPawn::HandleMouseY);
	PlayerInputComponent->BindKey(EKeys::F1, IE_Pressed, this, &ADublinFlightPawn::ToggleCredits);
	Weapons->BindInput(PlayerInputComponent);
}

void ADublinFlightPawn::PawnClientRestart()
{
	Super::PawnClientRestart();
	InputState.Clear();
	PendingMouseDelta = FVector2D::ZeroVector;
	Weapons->SuppressInput();
	if (APlayerController* Player = Cast<APlayerController>(GetController()); Player && Player->IsLocalController())
	{
		Player->bShowMouseCursor = false;
		Player->SetInputMode(FInputModeGameOnly());
		Player->SetViewTarget(this);
	}
}

void ADublinFlightPawn::UnPossessed()
{
	InputState.Clear();
	PendingMouseDelta = FVector2D::ZeroVector;
	Weapons->SuppressInput();
	Super::UnPossessed();
}

void ADublinFlightPawn::HandleKeyPressed(FKey Key)
{
	for (const DublinFlight::FNativeKey& Binding : DublinFlight::GetNativeKeys())
	{
		if (Binding.Key == Key)
		{
			InputState.SetKey(Binding.Control, true);
			return;
		}
	}
}

void ADublinFlightPawn::HandleKeyReleased(FKey Key)
{
	for (const DublinFlight::FNativeKey& Binding : DublinFlight::GetNativeKeys())
	{
		if (Binding.Key == Key)
		{
			InputState.SetKey(Binding.Control, false);
			return;
		}
	}
}

void ADublinFlightPawn::HandleMouseX(float Value)
{
	PendingMouseDelta.X += Value;
}

void ADublinFlightPawn::HandleMouseY(float Value)
{
	PendingMouseDelta.Y += Value;
}

void ADublinFlightPawn::ToggleCredits()
{
	bShowCredits = !bShowCredits;
}

void ADublinFlightPawn::SuppressHeldInput()
{
	Weapons->SuppressInput();
	if (const APlayerController* Player = Cast<APlayerController>(GetController()))
	{
		for (const DublinFlight::FNativeKey& Key : DublinFlight::GetNativeKeys())
		{
			InputState.SetKey(Key.Control, Player->IsInputKeyDown(Key.Key));
		}
	}
	InputState.SuppressHeldKeys();
	PendingMouseDelta = FVector2D::ZeroVector;
}

DublinFlight::FControlInput ADublinFlightPawn::ReadControlInput()
{
	const APlayerController* Player = Cast<APlayerController>(GetController());
	UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	if (!Player || !Player->IsLocalController() || Player->IsMoveInputIgnored() || Player->IsLookInputIgnored()
		|| (Viewport && Viewport->Viewport
			&& (!Viewport->Viewport->HasFocus() || !Viewport->Viewport->HasMouseCapture())))
	{
		InputState.Clear();
		PendingMouseDelta = FVector2D::ZeroVector;
		return DublinFlight::FControlInput();
	}
	// Reconcile dropped releases (focus changes, input flushes) without inventing presses.
	for (const DublinFlight::FNativeKey& Key : DublinFlight::GetNativeKeys())
	{
		if (InputState.IsHeld(Key.Control) && !Player->IsInputKeyDown(Key.Key))
		{
			InputState.SetKey(Key.Control, false);
		}
	}
	const DublinFlight::FControlInput Input = InputState.BuildInput(Simulation.GetState().Mode, PendingMouseDelta);
	PendingMouseDelta = FVector2D::ZeroVector;
	return Input;
}

void ADublinFlightPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bSpawnCaptured)
	{
		return;
	}
	const DublinFlight::FAdvanceResult Result = Simulation.Advance(ReadControlInput(), DeltaSeconds,
		[this](const DublinFlight::FFlightState& Proposed, FVector& ActualPosition)
		{
			FHitResult Hit;
			CollisionRoot->MoveComponent(Proposed.PositionCm - GetActorLocation(), Proposed.Orientation,
				true, &Hit, MOVECOMP_NoFlags, ETeleportType::None);
			ActualPosition = GetActorLocation();
			return !Hit.bBlockingHit && !Hit.bStartPenetrating;
		});
	if (Result.bRejectedDeltaTime || Result.bClampedDeltaTime || Result.bSanitizedInput)
	{
		UE_LOG(LogDublinFlight, Warning, TEXT("Flight input guarded: rejected dt=%d, capped dt=%d, sanitized input=%d."),
			Result.bRejectedDeltaTime, Result.bClampedDeltaTime, Result.bSanitizedInput);
	}
	if (Result.bRejectedMovement)
	{
		UE_LOG(LogDublinFlight, Error, TEXT("Non-finite/out-of-range flight movement rejected; attempting safe reset."));
		ResetFlight();
	}
	else if (Result.bMovementBlocked && Simulation.GetState().Mode == DublinFlight::EFlightMode::Flight)
	{
		UE_LOG(LogDublinFlight, Display, TEXT("Flight collision; resetting to the captured spawn."));
		ResetFlight();
	}
	UpdateReadableState();
	UpdateAircraftVisuals(DeltaSeconds);
}

bool ADublinFlightPawn::IsPositionClear(const FVector& Position, const FQuat& Rotation) const
{
	if (!GetWorld() || !DublinFlight::FFlightSimulation::IsFinitePosition(Position)
		|| !DublinFlight::FFlightSimulation::IsFiniteOrientation(Rotation))
	{
		return false;
	}
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinFlightSpawnClearance), false, this);
	const FCollisionResponseParams Response(CollisionRoot->GetCollisionResponseToChannels());
	return !GetWorld()->OverlapBlockingTestByChannel(Position, Rotation, CollisionRoot->GetCollisionObjectType(),
		FCollisionShape::MakeSphere(CollisionRoot->GetScaledSphereRadius() + 2.0f), Query, Response);
}

void ADublinFlightPawn::SetBlockedStatus(const FString& Message)
{
	bResetBlocked = true;
	FlightStatus = Message;
	Simulation.SetGodMode(true);
	SuppressHeldInput();
	UE_LOG(LogDublinFlight, Warning, TEXT("%s"), *Message);
}

void ADublinFlightPawn::ToggleGodMode()
{
	if (!bSpawnCaptured)
	{
		UE_LOG(LogDublinFlight, Warning, TEXT("ToggleGodMode requires BeginPlay to capture the spawn."));
		return;
	}
	if (Simulation.GetState().Mode == DublinFlight::EFlightMode::God
		&& !IsPositionClear(GetActorLocation(), GetActorQuat()))
	{
		SetBlockedStatus(TEXT("Cannot resume inside collision. Move into clear air, then G."));
		UpdateReadableState();
		return;
	}
	Simulation.ToggleGodMode();
	if (bResetBlocked && Simulation.GetState().Mode == DublinFlight::EFlightMode::Flight)
	{
		bResetBlocked = false;
		FlightStatus.Empty();
	}
	SuppressHeldInput();
	UpdateReadableState();
}

void ADublinFlightPawn::ResetFlight()
{
	if (!bSpawnCaptured)
	{
		UE_LOG(LogDublinFlight, Warning, TEXT("ResetFlight requires BeginPlay to capture the spawn."));
		return;
	}
	const FTransform Spawn = Simulation.GetSpawnTransform();
	if (!IsPositionClear(Spawn.GetLocation(), Spawn.GetRotation()))
	{
		SetBlockedStatus(TEXT("Reset blocked: original spawn is occupied. Staying in GOD; clear it, then Home."));
		UpdateReadableState();
		return;
	}
	if (!SetActorLocationAndRotation(Spawn.GetLocation(), Spawn.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics))
	{
		SetBlockedStatus(TEXT("Reset could not move the collision root. Inspect the log."));
		UpdateReadableState();
		return;
	}
	Simulation.Reset();
	bResetBlocked = false;
	FlightStatus.Empty();
	SuppressHeldInput();
	UpdateReadableState();
}

void ADublinFlightPawn::UpdateReadableState()
{
	if (!bAircraftVisualsReady && FlightStatus.IsEmpty())
	{
		FlightStatus = TEXT("Aircraft material unavailable; inspect the log.");
	}
	const DublinFlight::FFlightState& State = Simulation.GetState();
	bGodMode = State.Mode == DublinFlight::EFlightMode::God;
	SpeedMetersPerSecond = static_cast<float>(State.VelocityCmPerSecond.Size() / 100.0);
	CruiseSpeedMetersPerSecond = static_cast<float>(State.FlightSpeedCmPerSecond / 100.0);
	AltitudeMeters = static_cast<float>(State.PositionCm.Z / 100.0);
	const FRotator Rotation = State.Orientation.Rotator();
	HeadingDegrees = static_cast<float>(DublinFlight::FFlightSimulation::HeadingFromYaw(Rotation.Yaw));
	FlightTransform = FTransform(State.Orientation, State.PositionCm, GetActorScale3D());
	CollisionRoot->ComponentVelocity = State.VelocityCmPerSecond;
	CameraBoom->SetWorldRotation(FRotator(FMath::Clamp(Rotation.Pitch - 18.0, -85.0, 85.0), Rotation.Yaw, 0.0));
}

FVector ADublinFlightPawn::GetVelocity() const
{
	return Simulation.GetState().VelocityCmPerSecond;
}

FRotator ADublinFlightPawn::GetViewRotation() const
{
	return GetActorRotation();
}

void ADublinFlightPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	Super::CalcCamera(DeltaTime, OutResult);
	const bool bHideAircraft = FVector::DistSquared(OutResult.Location, GetActorLocation()) < FMath::Square(650.0);
	if (bHideAircraft != bAircraftHiddenForCamera)
	{
		bAircraftHiddenForCamera = bHideAircraft;
		for (UProceduralMeshComponent* Part : AircraftParts)
		{
			Part->SetOwnerNoSee(bHideAircraft);
		}
	}
}
