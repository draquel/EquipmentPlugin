#include "EquipmentGASIntegration.h"
#include "Components/EquipmentManagerComponent.h"
#include "EquipmentAbilityGranter.h"
#include "EquipmentEffectApplier.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"

#define LOCTEXT_NAMESPACE "FEquipmentGASIntegrationModule"

UAbilitySystemComponent* FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(AActor* Owner)
{
	// IAbilitySystemInterface first: the ASC may live on another actor (e.g. the player state),
	// where FindComponentByClass on the pawn finds nothing. LookForComponent keeps the old
	// behaviour as a fallback for actors that just carry an ASC component.
	return UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Owner, /*LookForComponent*/ true);
}

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
			UAbilitySystemComponent* ASC = FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(Manager->GetOwner());

			if (!ASC)
			{
				return;
			}

			Granter->GrantAbilities(Item, SlotTag, ASC);
			Applier->ApplyEffects(Item, SlotTag, ASC);
		};

		// Bind unequip callback. A null ASC is passed through: the granter/applier fall back to the
		// ASC they applied on, which matters when the owner is a pawn already unpossessed on its way
		// to destruction (the player state's ASC outlives it).
		Manager->OnGASUnequipCallback = [Granter, Applier, Manager](FGameplayTag SlotTag)
		{
			UAbilitySystemComponent* ASC = FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(Manager->GetOwner());
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
