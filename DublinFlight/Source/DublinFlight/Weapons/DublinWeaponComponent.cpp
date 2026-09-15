#include "Weapons/DublinWeaponComponent.h"

#include "City/DublinCityWorld.h"
#include "Components/InputComponent.h"
#include "Components/SphereComponent.h"
#include "DublinFlightPawn.h"
#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"
#include "Weapons/DublinProjectile.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinWeapons, Log, All);

UDublinWeaponComponent::UDublinWeaponComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UDublinWeaponComponent::BeginPlay()
{
	Super::BeginPlay();
	AddTickPrerequisiteActor(GetOwner());
	for (TActorIterator<ADublinCityWorld> It(GetWorld()); It; ++It)
	{
		City = *It;
		break;
	}
	if (!City.IsValid()) { Fail(TEXT("No Dublin city actor; weapons unavailable.")); }
	BombYieldTonsTNT = DublinWeapons::ClampYield(BombYieldTonsTNT);
}

void UDublinWeaponComponent::BindInput(UInputComponent* Input)
{
	check(Input);
	Input->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &UDublinWeaponComponent::StartCannon);
	Input->BindKey(EKeys::LeftMouseButton, IE_Released, this, &UDublinWeaponComponent::StopCannon);
	Input->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &UDublinWeaponComponent::DropBomb);
	Input->BindKey(EKeys::B, IE_Pressed, this, &UDublinWeaponComponent::DropBomb);
	Input->BindKey(EKeys::LeftBracket, IE_Pressed, this, &UDublinWeaponComponent::DecreaseYield);
	Input->BindKey(EKeys::RightBracket, IE_Pressed, this, &UDublinWeaponComponent::IncreaseYield);
}

bool UDublinWeaponComponent::InputAllowed() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* Player = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	const UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	return Player && Player->IsLocalController() && !Player->IsMoveInputIgnored() && !Player->IsLookInputIgnored()
		&& (!Viewport || !Viewport->Viewport || (Viewport->Viewport->HasFocus() && Viewport->Viewport->HasMouseCapture()));
}

void UDublinWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	BombYieldTonsTNT = DublinWeapons::ClampYield(BombYieldTonsTNT);
	Projectiles.RemoveAll([](const TWeakObjectPtr<ADublinProjectile>& Projectile)
	{
		return !Projectile.IsValid() || Projectile->IsActorBeingDestroyed();
	});
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* Player = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!InputAllowed() || !Player || !Player->IsInputKeyDown(EKeys::LeftMouseButton))
	{
		bCannonHeld = false;
	}
	const float Rate = FMath::IsFinite(CannonRoundsPerSecond) ? FMath::Clamp(CannonRoundsPerSecond, 6.0f, 8.0f) : 7.0f;
	if (bCannonHeld && CannonCooldown.TryConsume(GetWorld()->GetTimeSeconds(), 1.0 / Rate))
	{
		Launch(EDublinImpactKind::Cannon);
	}
}

void UDublinWeaponComponent::StartCannon() { bCannonHeld = InputAllowed(); }
void UDublinWeaponComponent::StopCannon() { bCannonHeld = false; }
void UDublinWeaponComponent::SuppressInput() { StopCannon(); }
void UDublinWeaponComponent::DecreaseYield() { BombYieldTonsTNT = DublinWeapons::ClampYield(DublinWeapons::ClampYield(BombYieldTonsTNT) * 0.5f); }
void UDublinWeaponComponent::IncreaseYield() { BombYieldTonsTNT = DublinWeapons::ClampYield(DublinWeapons::ClampYield(BombYieldTonsTNT) * 2.0f); }

void UDublinWeaponComponent::DropBomb()
{
	if (!InputAllowed()) { return; }
	const float Cooldown = FMath::IsFinite(BombCooldownSeconds) ? FMath::Clamp(BombCooldownSeconds, 0.5f, 10.0f) : 1.5f;
	if (!BombCooldown.TryConsume(GetWorld()->GetTimeSeconds(), Cooldown)) { return; }
	Launch(EDublinImpactKind::Bomb);
}

bool UDublinWeaponComponent::IsCityReady() const { return City.IsValid() && City->bCityReady; }
ADublinProjectile* UDublinWeaponComponent::GetLastProjectile() const { return LastProjectile.Get(); }
ADublinCityWorld* UDublinWeaponComponent::GetCity() const { return City.Get(); }

int32 UDublinWeaponComponent::GetActiveProjectileCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ADublinProjectile>& Projectile : Projectiles)
	{
		Count += Projectile.IsValid() && !Projectile->IsActorBeingDestroyed() ? 1 : 0;
	}
	return Count;
}

bool UDublinWeaponComponent::Launch(EDublinImpactKind Kind)
{
	ADublinFlightPawn* Plane = Cast<ADublinFlightPawn>(GetOwner());
	if (!Plane || !IsCityReady()) { Fail(TEXT("City not READY; projectile launch refused.")); return false; }
	if (GetActiveProjectileCount() >= FMath::Clamp(MaximumActiveProjectiles, 1, 64))
	{
		Fail(TEXT("Projectile budget full; wait for impacts or expiry."));
		return false;
	}
	const FVector Aim = (Plane->IsGodMode() ? Plane->GetViewRotation() : Plane->GetActorRotation()).Vector();
	const bool bBomb = Kind == EDublinImpactKind::Bomb;
	const FVector Position = Plane->GetActorLocation()
		+ (bBomb ? FVector(0.0, 0.0, -540.0) : Aim * 650.0);
	const FVector Velocity = Plane->GetVelocity() + (bBomb ? FVector::ZeroVector : Aim * 35000.0);
	if (Position.ContainsNaN() || Velocity.ContainsNaN()) { Fail(TEXT("Nonfinite weapon launch rejected.")); return false; }
	DublinWeapons::FBombCurve Curve;
	Curve.RadiusAtOneCm = BombRadiusAtOneCm;
	Curve.DepthAtOneCm = BombDepthAtOneCm;
	Curve.StrengthAtOne = BombStrengthAtOne;
	Curve.Exponent = BombYieldExponent;
	Curve.MaxRadiusCm = BombMaximumRadiusCm;
	Curve.MaxDepthCm = BombMaximumDepthCm;
	Curve.MaxStrength = BombMaximumStrength;
	FDublinImpact Payload = DublinWeapons::MakeImpact(Kind, BombYieldTonsTNT, Curve);
	Payload.Seed = NextSeed;
	NextSeed = NextSeed == MAX_int32 ? 1 : NextSeed + 1;
	const FTransform Transform(Aim.Rotation(), Position);
	ADublinProjectile* Projectile = GetWorld()->SpawnActorDeferred<ADublinProjectile>(ADublinProjectile::StaticClass(),
		Transform, Plane, Plane, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Projectile) { Fail(TEXT("Projectile spawn failed.")); return false; }
	Projectile->Initialize(Payload, Velocity, City.Get(), this);
	for (const TWeakObjectPtr<ADublinProjectile>& Existing : Projectiles)
	{
		if (Existing.IsValid())
		{
			Projectile->CollisionRoot->IgnoreActorWhenMoving(Existing.Get(), true);
			Existing->CollisionRoot->IgnoreActorWhenMoving(Projectile, true);
		}
	}
	Projectiles.Add(Projectile);
	LastProjectile = Projectile;
	Projectile->FinishSpawning(Transform);
	if (bBomb) { ++BombsDropped; CurrentWeapon = TEXT("BOMB"); }
	else { ++CannonShots; CurrentWeapon = TEXT("CANNON"); }
	return true;
}

bool UDublinWeaponComponent::DispatchImpact(const FDublinImpact& Impact)
{
	LastImpact = Impact;
	if (!Impact.IsValid() || Impact.Normal.IsNearlyZero() || !IsCityReady())
	{
		++RejectedImpacts;
		Fail(TEXT("Impact rejected: invalid payload or city not READY."));
		return false;
	}
	if (!City->ApplyImpact(Impact))
	{
		++RejectedImpacts;
		Fail(TEXT("City rejected impact: destruction/fracture data not ready or target unsupported. No impact FX emitted."));
		return false;
	}
	++AcceptedImpacts;
	AcceptedCannonImpacts += Impact.Kind == EDublinImpactKind::Cannon ? 1 : 0;
	AcceptedBombImpacts += Impact.Kind == EDublinImpactKind::Bomb ? 1 : 0;
	AcceptedWaterImpacts += Impact.bWater ? 1 : 0;
	if (UDublinImpactEffectsSubsystem* Effects = GetWorld()->GetSubsystem<UDublinImpactEffectsSubsystem>())
	{
		Effects->EmitImpact(Impact);
		++EffectsRequests;
	}
	else
	{
		Fail(TEXT("Impact accepted by city, but impact effects subsystem unavailable."));
	}
	return true;
}

void UDublinWeaponComponent::Fail(const FString& Message)
{
	if (LastFailure != Message) { UE_LOG(LogDublinWeapons, Warning, TEXT("%s"), *Message); }
	LastFailure = Message;
}

void UDublinWeaponComponent::ClearProjectiles()
{
	for (const TWeakObjectPtr<ADublinProjectile>& Projectile : Projectiles)
	{
		if (Projectile.IsValid()) { Projectile->Destroy(); }
	}
	Projectiles.Empty();
	LastProjectile.Reset();
}

void UDublinWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SuppressInput();
	ClearProjectiles();
	Super::EndPlay(EndPlayReason);
}
