#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GameplayTagContainer.h"
#include "ActiveGameplayEffectHandle.h"
#include "EquipmentEffectApplier.generated.h"

struct FItemInstance;
class UAbilitySystemComponent;

/**
 * Applies and removes gameplay effects from equipped items.
 * Handles both persistent passive effects (removed on unequip) and
 * one-time on-equip effects (not tracked for removal).
 * Created by the EquipmentGASIntegration module and owned by EquipmentManagerComponent.
 */
UCLASS()
class EQUIPMENTGASINTEGRATION_API UEquipmentEffectApplier : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Apply passive effects (tracked for removal) and on-equip effects (fire-and-forget).
	 */
	void ApplyEffects(const FItemInstance& Item, FGameplayTag SlotTag, UAbilitySystemComponent* ASC);

	/**
	 * Remove all passive effects that were applied for this slot.
	 * @param ASC Ability system to remove from; null = the one the slot was applied on (needed when the
	 *            owner can no longer resolve its ASC, e.g. a pawn being destroyed after unpossession).
	 */
	void RemoveEffects(FGameplayTag SlotTag, UAbilitySystemComponent* ASC);

private:
	/** Active passive effect handles per slot for clean removal */
	TMap<FGameplayTag, TArray<FActiveGameplayEffectHandle>> AppliedEffectHandles;

	/** The ASC each slot's effects were applied on. */
	TMap<FGameplayTag, TWeakObjectPtr<UAbilitySystemComponent>> AppliedOnASC;
};
