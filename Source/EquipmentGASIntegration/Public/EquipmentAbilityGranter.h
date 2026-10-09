#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GameplayTagContainer.h"
#include "GameplayAbilitySpecHandle.h"
#include "ActiveGameplayEffectHandle.h"
#include "EquipmentAbilityGranter.generated.h"

struct FItemInstance;
class UAbilitySystemComponent;

/**
 * Grants and revokes gameplay abilities from equipped items.
 * Created by the EquipmentGASIntegration module and owned by EquipmentManagerComponent.
 * All operations are server-only — ASC replication handles clients.
 */
UCLASS()
class EQUIPMENTGASINTEGRATION_API UEquipmentAbilityGranter : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Grant all abilities defined in the item's equipment fragment.
	 * Stores handles per slot for surgical revocation.
	 */
	void GrantAbilities(const FItemInstance& Item, FGameplayTag SlotTag, UAbilitySystemComponent* ASC);

	/**
	 * Revoke all abilities previously granted for this slot.
	 * @param ASC Ability system to revoke from; null = the one the slot was granted on (needed when the
	 *            owner can no longer resolve its ASC, e.g. a pawn being destroyed after unpossession).
	 */
	void RevokeAbilities(FGameplayTag SlotTag, UAbilitySystemComponent* ASC);

private:
	/** Ability handles per slot for clean removal */
	TMap<FGameplayTag, TArray<FGameplayAbilitySpecHandle>> GrantedAbilityHandles;

	/** The ASC each slot's abilities were granted on. */
	TMap<FGameplayTag, TWeakObjectPtr<UAbilitySystemComponent>> GrantedOnASC;
};
