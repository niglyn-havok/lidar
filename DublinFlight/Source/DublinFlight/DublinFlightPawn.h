#pragma once

#include "CoreMinimal.h"
#include "DublinFlightSimulation.h"
#include "GameFramework/Pawn.h"
#include "InputCoreTypes.h"
#include "DublinFlightPawn.generated.h"

class UCameraComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class USceneComponent;
class USphereComponent;
class USpringArmComponent;
class UDublinWeaponComponent;

namespace DublinFlight
{
class DUBLINFLIGHT_API FCameraOrbit
{
public:
	static constexpr double OrbitMouseDegreesPerUnit = 0.36;

	bool ApplyMouseDelta(const FVector2D& MouseDelta, const FRotator& AircraftRotation);
	void Reset();
	FRotator GetRotation(const FRotator& AircraftRotation) const;

private:
	FVector2D OffsetDegrees = FVector2D::ZeroVector;
};
}

UCLASS(Blueprintable)
class DUBLINFLIGHT_API ADublinFlightPawn : public APawn
{
	GENERATED_BODY()

public:
	ADublinFlightPawn();
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void UnPossessed() override;
	virtual FVector GetVelocity() const override;
	virtual FRotator GetViewRotation() const override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

	UFUNCTION(BlueprintCallable, Category = "DublinFlight")
	void ToggleGodMode();

	UFUNCTION(BlueprintCallable, Category = "DublinFlight")
	void ResetFlight();

	UFUNCTION(BlueprintPure, Category = "DublinFlight")
	bool IsGodMode() const { return bGodMode; }

	UFUNCTION(BlueprintPure, Category = "DublinFlight")
	float GetSpeedMetersPerSecond() const { return SpeedMetersPerSecond; }

	UFUNCTION(BlueprintPure, Category = "DublinFlight")
	float GetAltitudeMeters() const { return AltitudeMeters; }

	UFUNCTION(BlueprintPure, Category = "DublinFlight")
	float GetHeadingDegrees() const { return HeadingDegrees; }

	UFUNCTION(BlueprintPure, Category = "DublinFlight")
	FTransform GetFlightTransform() const { return FlightTransform; }

	UFUNCTION(BlueprintPure, Category = "DublinFlight")
	FTransform GetInitialSpawnTransform() const { return InitialSpawnTransform; }

	const DublinFlight::FFlightState& GetFlightState() const { return Simulation.GetState(); }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<USphereComponent> CollisionRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<UCameraComponent> ChaseCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<UDublinWeaponComponent> Weapons;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|Credits")
	bool bShowCredits = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|Credits")
	FString CreditsText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<USceneComponent> AircraftVisualsRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<UProceduralMeshComponent> AircraftAirframe;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TObjectPtr<UProceduralMeshComponent> AircraftPropeller;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DublinFlight|Components")
	TArray<TObjectPtr<UProceduralMeshComponent>> AircraftParts;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	bool bGodMode = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	bool bSpawnCaptured = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	bool bResetBlocked = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State", meta = (Units = "m/s"))
	float SpeedMetersPerSecond = 40.0f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State", meta = (Units = "m/s"))
	float CruiseSpeedMetersPerSecond = 40.0f;

	// World Z, not height above terrain.
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State", meta = (Units = "m"))
	float AltitudeMeters = 0.0f;

	// Dublin map: +X east, +Y south; compass heading is (yaw + 90) modulo 360.
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	float HeadingDegrees = 90.0f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	FTransform FlightTransform = FTransform::Identity;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	FTransform InitialSpawnTransform = FTransform::Identity;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "DublinFlight|State")
	FString FlightStatus;

protected:
	virtual void BeginPlay() override;

private:
	DublinFlight::FFlightSimulation Simulation;
	DublinFlight::FInputState InputState;
	DublinFlight::FCameraOrbit CameraOrbit;
	FVector2D PendingMouseDelta = FVector2D::ZeroVector;
	double PendingSpeedSteps = 0.0;
	uint64 MouseInputSuppressedFrame = MAX_uint64;
	bool bAircraftHiddenForCamera = false;
	bool bAircraftVisualsReady = false;
	float PropellerUpdateSeconds = 0.0f;
	double PropellerAngleDegrees = 0.0;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> AircraftMaterial;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AircraftMaterials;

	void HandleKeyPressed(FKey Key);
	void HandleKeyReleased(FKey Key);
	void HandleMouseX(float Value);
	void HandleMouseY(float Value);
	void HandleMouseWheel(float Value);
	void ToggleCredits();
	void SuppressHeldInput();
	DublinFlight::FControlInput ReadControlInput();
	void UpdateReadableState();
	bool IsPositionClear(const FVector& Position, const FQuat& Rotation) const;
	void SetBlockedStatus(const FString& Message);
	void InitializeAircraftMaterials();
	void UpdateAircraftVisuals(float DeltaSeconds);
};
