#include "EquipmentAbilityGranter.h"
#include "AbilitySystemComponent.h"
#include "Types/CGFItemTypes.h"
#include "Data/ItemDefinition.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
#include "Subsystems/ItemDatabaseSubsystem.h"

void UEquipmentAbilityGranter::GrantAbilities(const FItemInstance& Item, FGameplayTag SlotTag,
	UAbilitySystemComponent* ASC)
{
	if (!ASC)
	{
		return;
	}

	// Look up the equipment fragment
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UItemDatabaseSubsystem* DB = GI ? GI->GetSubsystem<UItemDatabaseSubsystem>() : nullptr;
	if (!DB)
	{
		return;
	}

	UItemDefinition* Def = DB->GetDefinition(Item.ItemDefinitionId);
	if (!Def)
	{
		return;
	}

	UItemFragment_Equipment* EquipFrag = Def->FindFragment<UItemFragment_Equipment>();
	if (!EquipFrag)
	{
		return;
	}

	TArray<FGameplayAbilitySpecHandle>& Handles = GrantedAbilityHandles.FindOrAdd(SlotTag);

	for (const TSubclassOf<UGameplayAbility>& AbilityClass : EquipFrag->GrantedAbilities)
	{
		if (!AbilityClass)
		{
			continue;
		}

		FGameplayAbilitySpec Spec(AbilityClass, 1, INDEX_NONE, GetOuter());
		FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);
		if (Handle.IsValid())
		{
			Handles.Add(Handle);
		}
	}
}

void UEquipmentAbilityGranter::RevokeAbilities(FGameplayTag SlotTag, UAbilitySystemComponent* ASC)
{
	if (!ASC)
	{
		return;
	}

	TArray<FGameplayAbilitySpecHandle>* Handles = GrantedAbilityHandles.Find(SlotTag);
	if (!Handles)
	{
		return;
	}

	for (const FGameplayAbilitySpecHandle& Handle : *Handles)
	{
		if (Handle.IsValid())
		{
			ASC->ClearAbility(Handle);
		}
	}

	GrantedAbilityHandles.Remove(SlotTag);
}
