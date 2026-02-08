#include "EquipmentGASIntegration.h"
#include "Components/EquipmentManagerComponent.h"
#include "EquipmentAbilityGranter.h"
#include "EquipmentEffectApplier.h"
#include "AbilitySystemComponent.h"

#define LOCTEXT_NAMESPACE "FEquipmentGASIntegrationModule"

void FEquipmentGASIntegrationModule::StartupModule()
{
	// Register the GAS setup factory so EquipmentManagerComponent can create GAS handlers
	UEquipmentManagerComponent::GASSetupFactory = [](UEquipmentManagerComponent* Manager)
	{
		UEquipmentAbilityGranter* Granter = NewObject<UEquipmentAbilityGranter>(Manager);
		UEquipmentEffectApplier* Applier = NewObject<UEquipmentEffectApplier>(Manager);

		// Store as UObject to prevent GC (core module doesn't know concrete types)
		Manager->GASAbilityGranter = Granter;
		Manager->GASEffectApplier = Applier;

		// Bind equip callback
		Manager->OnGASEquipCallback = [Granter, Applier, Manager](const FItemInstance& Item, FGameplayTag SlotTag)
		{
			UAbilitySystemComponent* ASC = Manager->GetOwner()
				? Manager->GetOwner()->FindComponentByClass<UAbilitySystemComponent>()
				: nullptr;

			if (!ASC)
			{
				return;
			}

			Granter->GrantAbilities(Item, SlotTag, ASC);
			Applier->ApplyEffects(Item, SlotTag, ASC);
		};

		// Bind unequip callback
		Manager->OnGASUnequipCallback = [Granter, Applier, Manager](FGameplayTag SlotTag)
		{
			UAbilitySystemComponent* ASC = Manager->GetOwner()
				? Manager->GetOwner()->FindComponentByClass<UAbilitySystemComponent>()
				: nullptr;

			if (!ASC)
			{
				return;
			}

			Granter->RevokeAbilities(SlotTag, ASC);
			Applier->RemoveEffects(SlotTag, ASC);
		};
	};
}

void FEquipmentGASIntegrationModule::ShutdownModule()
{
	UEquipmentManagerComponent::GASSetupFactory = nullptr;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FEquipmentGASIntegrationModule, EquipmentGASIntegration)
