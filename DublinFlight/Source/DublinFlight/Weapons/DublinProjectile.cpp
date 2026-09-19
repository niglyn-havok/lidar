#include "Weapons/DublinProjectile.h"

#include "City/DublinCityWorld.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Weapons/DublinWeaponComponent.h"

ADublinProjectile::ADublinProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	CollisionRoot = CreateDefaultSubobject<USphereComponent>(TEXT("ProjectileCollision"));
	SetRootComponent(CollisionRoot);
	CollisionRoot->InitSphereRadius(8.0f);
	CollisionRoot->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionRoot->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionRoot->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionRoot->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionRoot->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	CollisionRoot->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	CollisionRoot->SetGenerateOverlapEvents(false);
	CollisionRoot->SetCanEverAffectNavigation(false);
	VisibleBody = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileBody"));
	VisibleBody->SetupAttachment(CollisionRoot);
	VisibleBody->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisibleBody->SetCanEverAffectNavigation(false);
	VisibleBody->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TracerMaterial(
		TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"));
	if (Sphere.Succeeded()) { VisibleBody->SetStaticMesh(Sphere.Object); }
	if (TracerMaterial.Succeeded()) { VisibleBody->SetMaterial(0, TracerMaterial.Object); }
	VisibleBody->SetRelativeScale3D(FVector(1.5, 0.12, 0.12));
	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	Movement->SetUpdatedComponent(CollisionRoot);
	Movement->InitialSpeed = 0.0f;
	Movement->MaxSpeed = 100000.0f;
	Movement->bInitialVelocityInLocalSpace = false;
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
	Movement->bSweepCollision = true;
	Movement->bForceSubStepping = true;
	Movement->MaxSimulationTimeStep = 1.0f / 120.0f;
	Movement->MaxSimulationIterations = 32;
	Movement->ProjectileGravityScale = 0.0f;
	Movement->OnProjectileStop.AddDynamic(this, &ADublinProjectile::OnProjectileStopped);
	PrimaryActorTick.AddPrerequisite(Movement, Movement->PrimaryComponentTick);
}

void ADublinProjectile::Initialize(const FDublinImpact& InImpact, const FVector& InitialVelocity,
	ADublinCityWorld* InCity, UDublinWeaponComponent* InWeapon)
{
	Impact = InImpact;
	City = InCity;
	Weapon = InWeapon;
	LaunchPosition = GetActorLocation();
	LaunchOwnerPosition = GetOwner() ? GetOwner()->GetActorLocation() : LaunchPosition;
	PreviousPosition = LaunchPosition;
	LaunchVelocity = InitialVelocity;
	bInitialized = Impact.IsValid() && !InitialVelocity.ContainsNaN() && !LaunchPosition.ContainsNaN();
	if (GetOwner()) { CollisionRoot->IgnoreActorWhenMoving(GetOwner(), true); }
	if (GetInstigator()) { CollisionRoot->IgnoreActorWhenMoving(GetInstigator(), true); }
	const bool bBomb = Impact.Kind == EDublinImpactKind::Bomb;
	CollisionRoot->SetSphereRadius(bBomb ? 20.0f : 8.0f);
	VisibleBody->SetRelativeScale3D(bBomb ? FVector(0.35, 0.35, 0.65) : FVector(1.5, 0.12, 0.12));
	Movement->ProjectileGravityScale = bBomb ? 1.0f : 0.0f;
	Movement->Velocity = bInitialized ? InitialVelocity : FVector::ZeroVector;
	SetLifeSpan(bBomb ? 20.0f : 4.0f);
}

void ADublinProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (!bInitialized) { Destroy(); return; }
	if (Impact.Kind == EDublinImpactKind::Bomb && GetOwner() && GetWorld()
		&& !LaunchOwnerPosition.Equals(LaunchPosition))
	{
		// Deferred spawning does not sweep the displacement from the aircraft to the release point.
		FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBombReleaseSweep), false, this);
		FCollisionResponseParams Response;
		CollisionRoot->InitSweepCollisionParams(Query, Response);
		Query.AddIgnoredActor(GetOwner());
		if (GetInstigator()) { Query.AddIgnoredActor(GetInstigator()); }
		FHitResult Hit;
		const bool bHit = GetWorld()->SweepSingleByChannel(Hit, LaunchOwnerPosition, LaunchPosition,
			FQuat::Identity, CollisionRoot->GetCollisionObjectType(),
			FCollisionShape::MakeSphere(CollisionRoot->GetScaledSphereRadius()), Query, Response);
		FVector WaterPosition;
		const FVector SegmentEnd = bHit ? FVector(Hit.Location) : LaunchPosition;
		if (City.IsValid() && DublinWeapons::FindWaterCrossing(LaunchOwnerPosition, SegmentEnd,
			[this](const FVector& Point, float& SurfaceZ) { return City->GetWaterSurfaceZ(Point, SurfaceZ); },
			WaterPosition))
		{
			Resolve(WaterPosition, FVector::UpVector, true, nullptr);
			return;
		}
		if (bHit)
		{
			Resolve(Hit.bStartPenetrating ? LaunchOwnerPosition : FVector(Hit.ImpactPoint),
				Hit.ImpactNormal, false, Hit.GetActor());
			return;
		}
	}
	if (City.IsValid())
	{
		float SurfaceZ = 0.0f;
		if (City->GetWaterSurfaceZ(LaunchPosition, SurfaceZ) && FMath::IsFinite(SurfaceZ) && LaunchPosition.Z <= SurfaceZ)
		{
			Resolve(FVector(LaunchPosition.X, LaunchPosition.Y, SurfaceZ), FVector::UpVector, true, nullptr);
		}
	}
}

bool ADublinProjectile::CrossedWater(const FVector& End, FVector& WaterPosition) const
{
	return City.IsValid() && DublinWeapons::FindWaterCrossing(PreviousPosition, End,
		[this](const FVector& Position, float& SurfaceZ) { return City->GetWaterSurfaceZ(Position, SurfaceZ); },
		WaterPosition);
}

void ADublinProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bInitialized || ImpactOnce.IsConsumed() || IsActorBeingDestroyed()) { return; }
	const FVector Position = GetActorLocation();
	if (Position.ContainsNaN() || Movement->Velocity.ContainsNaN() || Position.GetAbsMax() > 1.0e8)
	{
		Destroy();
		return;
	}
	FVector WaterPosition;
	if (CrossedWater(Position, WaterPosition))
	{
		Resolve(WaterPosition, FVector::UpVector, true, nullptr);
		return;
	}
	PreviousPosition = Position;
}

void ADublinProjectile::OnProjectileStopped(const FHitResult& Hit)
{
	if (!bInitialized || ImpactOnce.IsConsumed() || Hit.GetActor() == GetOwner()) { return; }
	FVector WaterPosition;
	// A solid below the nonblocking river must not win over the earlier water crossing.
	if (CrossedWater(Hit.Location, WaterPosition))
	{
		Resolve(WaterPosition, FVector::UpVector, true, nullptr);
		return;
	}
	Resolve(Hit.bStartPenetrating ? GetActorLocation() : FVector(Hit.ImpactPoint), Hit.ImpactNormal, false, Hit.GetActor());
}

void ADublinProjectile::Resolve(const FVector& Position, const FVector& Normal, bool bWater, const AActor* HitActor)
{
	if (!ImpactOnce.TryConsume(HitActor, GetOwner())) { return; }
	Impact.PositionCm = Position;
	Impact.Normal = Normal.ContainsNaN() ? FVector::UpVector : Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	Impact.bWater = bWater;
	if (Weapon.IsValid() && GetWorld() && !GetWorld()->bIsTearingDown)
	{
		Weapon->DispatchImpact(Impact);
	}
	Movement->StopMovementImmediately();
	CollisionRoot->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Destroy();
}
