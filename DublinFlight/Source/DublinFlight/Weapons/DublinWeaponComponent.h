#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DublinImpact.h"
#include "Weapons/DublinWeaponModel.h"
#include "DublinWeaponComponent.generated.h"

class ADublinCityWorld;
class ADublinProjectile;
class UInputComponent;

UCLASS(ClassGroup = (Dublin), meta = (BlueprintSpawnableComponent))
class DUBLINFLIGHT_API UDublinWeaponComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UDublinWeaponComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void BindInput(UInputComponent* Input);
	void SuppressInput();
	void ClearProjectiles();
	bool DispatchImpact(const FDublinImpact& Impact);
	void NotifyDeferredImpactEffectsRequested(const FGuid& Epoch, uint64 EventId);
	bool IsCityReady() const;
	int32 GetActiveProjectileCount() const;
	TArray<ADublinProjectile*> GetActiveBombs() const;
	ADublinProjectile* GetLastProjectile() const;
	ADublinCityWorld* GetCity() const;
	bool GetCannonLaunch(FTransform& OutTransform, FVector& OutVelocity) const;
	const FDublinImpact& GetLastImpact() const { return LastImpact; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Weapons", meta = (ClampMin = "0.1", ClampMax = "1000"))
	float BombYieldTonsTNT = 1.0f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons", meta = (ClampMin = "6", ClampMax = "8"))
	float CannonRoundsPerSecond = 7.0f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons", meta = (ClampMin = "0.5", ClampMax = "10"))
	float BombCooldownSeconds = 1.5f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaximumActiveProjectiles = 32;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombRadiusAtOneCm = 600.0f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombDepthAtOneCm = 250.0f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombStrengthAtOne = 2.0f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombYieldExponent = 0.25f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombMaximumRadiusCm = DublinWeapons::MaximumBombRadiusCm;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombMaximumDepthCm = 2000.0f;
	UPROPERTY(EditAnywhere, Category = "Dublin|Weapons|GameTuning")
	float BombMaximumStrength = 50.0f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 CannonShots = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 BombsDropped = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 AcceptedImpacts = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 AcceptedCannonImpacts = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 AcceptedBombImpacts = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 AcceptedWaterImpacts = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 RejectedImpacts = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	int32 EffectsRequests = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	FString CurrentWeapon = TEXT("CANNON");
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dublin|Weapons")
	FString LastFailure;
	FGuid LastDeferredEffectsEpoch;
	uint64 LastDeferredEffectsEventId = 0;

protected:
	virtual void BeginPlay() override;
private:
	TWeakObjectPtr<ADublinCityWorld> City;
	TWeakObjectPtr<ADublinProjectile> LastProjectile;
	TArray<TWeakObjectPtr<ADublinProjectile>> Projectiles;
	DublinWeapons::FCooldown CannonCooldown;
	DublinWeapons::FCooldown BombCooldown;
	FDublinImpact LastImpact;
	bool bCannonHeld = false;
	int32 NextSeed = 1;
	void StartCannon();
	void StopCannon();
	void DropBomb();
	void DecreaseYield();
	void IncreaseYield();
	bool InputAllowed() const;
	bool Launch(EDublinImpactKind Kind);
	void Fail(const FString& Message);
};
