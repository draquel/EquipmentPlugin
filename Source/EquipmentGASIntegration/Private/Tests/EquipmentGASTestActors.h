#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "EquipmentGASTestActors.generated.h"

/** Test-only: an actor that carries an ability system component. */
UCLASS()
class AEquipmentGASTestHostActor : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AEquipmentGASTestHostActor()
	{
		AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	}

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystemComponent; }

	UPROPERTY()
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
};

/**
 * Test-only: an actor whose ability system lives on another actor (the player-state pattern).
 * Has no ASC component of its own, so FindComponentByClass finds nothing.
 */
UCLASS()
class AEquipmentGASTestAvatarActor : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override
	{
		return Host ? Host->GetAbilitySystemComponent() : nullptr;
	}

	UPROPERTY()
	TObjectPtr<AEquipmentGASTestHostActor> Host;
};
