#include "EquipmentEffectApplier.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Types/CGFItemTypes.h"
#include "Data/ItemDefinition.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
#include "Subsystems/ItemDatabaseSubsystem.h"

void UEquipmentEffectApplier::ApplyEffects(const FItemInstance& Item, FGameplayTag SlotTag,
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

	TArray<FActiveGameplayEffectHandle>& Handles = AppliedEffectHandles.FindOrAdd(SlotTag);

	// Apply passive effects (tracked — removed on unequip)
	for (const TSubclassOf<UGameplayEffect>& EffectClass : EquipFrag->PassiveEffects)
	{
		if (!EffectClass)
		{
			continue;
		}

		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		Context.AddSourceObject(GetOuter());
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, 1, Context);
		if (Spec.IsValid())
		{
			FActiveGameplayEffectHandle Handle = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
			if (Handle.IsValid())
			{
				Handles.Add(Handle);
			}
		}
	}

	// Apply on-equip effects (fire-and-forget — NOT stored for removal)
	for (const TSubclassOf<UGameplayEffect>& EffectClass : EquipFrag->OnEquipEffects)
	{
		if (!EffectClass)
		{
			continue;
		}

		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		Context.AddSourceObject(GetOuter());
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, 1, Context);
		if (Spec.IsValid())
		{
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		}
	}
}

void UEquipmentEffectApplier::RemoveEffects(FGameplayTag SlotTag, UAbilitySystemComponent* ASC)
{
	if (!ASC)
	{
		return;
	}

	TArray<FActiveGameplayEffectHandle>* Handles = AppliedEffectHandles.Find(SlotTag);
	if (!Handles)
	{
		return;
	}

	for (const FActiveGameplayEffectHandle& Handle : *Handles)
	{
		if (Handle.IsValid())
		{
			ASC->RemoveActiveGameplayEffect(Handle);
		}
	}

	AppliedEffectHandles.Remove(SlotTag);
}
