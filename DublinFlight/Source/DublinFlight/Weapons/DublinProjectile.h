#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DublinImpact.h"
#include "Weapons/DublinWeaponModel.h"
#include "DublinProjectile.generated.h"

class ADublinCityWorld;
class UDublinWeaponComponent;
class UProjectileMovementComponent;
class USphereComponent;
class UStaticMeshComponent;

UCLASS()
class DUBLINFLIGHT_API ADublinProjectile : public AActor
{
	GENERATED_BODY()
public:
	ADublinProjectile();
	void Initialize(const FDublinImpact& InImpact, const FVector& InitialVelocity,
		ADublinCityWorld* InCity, UDublinWeaponComponent* InWeapon);
	virtual void Tick(float DeltaSeconds) override;
	FVector GetLaunchVelocity() const { return LaunchVelocity; }
	FVector GetLaunchPosition() const { return LaunchPosition; }
	FVector GetLaunchOwnerPosition() const { return LaunchOwnerPosition; }
	EDublinImpactKind GetImpactKind() const { return Impact.Kind; }
	bool HasResolvedImpact() const { return ImpactOnce.IsConsumed(); }

	UPROPERTY(VisibleAnywhere, Category = "Dublin|Projectile")
	TObjectPtr<USphereComponent> CollisionRoot;
	UPROPERTY(VisibleAnywhere, Category = "Dublin|Projectile")
	TObjectPtr<UProjectileMovementComponent> Movement;
	UPROPERTY(VisibleAnywhere, Category = "Dublin|Projectile")
	TObjectPtr<UStaticMeshComponent> VisibleBody;

protected:
	virtual void BeginPlay() override;
private:
	FDublinImpact Impact;
	DublinWeapons::FImpactOnce ImpactOnce;
	TWeakObjectPtr<ADublinCityWorld> City;
	TWeakObjectPtr<UDublinWeaponComponent> Weapon;
	FVector PreviousPosition = FVector::ZeroVector;
	FVector LaunchPosition = FVector::ZeroVector;
	FVector LaunchOwnerPosition = FVector::ZeroVector;
	FVector LaunchVelocity = FVector::ZeroVector;
	bool bInitialized = false;
	UFUNCTION()
	void OnProjectileStopped(const FHitResult& Hit);
	bool CrossedWater(const FVector& End, FVector& WaterPosition) const;
	void Resolve(const FVector& Position, const FVector& Normal, bool bWater, const AActor* HitActor);
};
