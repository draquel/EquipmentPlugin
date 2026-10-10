// Copyright Daniel Raquel. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Types/CGFItemTypes.h"
#include "GameplayTagsManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Durability (feature 5b): the guard paths that need no item database. The
// wear path itself (definition lookup, destroy-at-zero) runs in PIE.
// ---------------------------------------------------------------------------

namespace EquipmentDurabilityTestHelpers
{
	UEquipmentManagerComponent* CreateEquipment(const FName& SlotTagName)
	{
		UEquipmentManagerComponent* Comp = NewObject<UEquipmentManagerComponent>();
		Comp->AddToRoot();
		FEquipmentSlot Slot;
		Slot.SlotTag = FGameplayTag::RequestGameplayTag(SlotTagName, false);
		Slot.bIsOccupied = false;
		Comp->EquipmentSlots.Add(Slot);
		return Comp;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipDurability_EmptySlot,
	"Equipment.Durability.EmptySlotHasNone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEquipDurability_EmptySlot::RunTest(const FString& Parameters)
{
	UEquipmentManagerComponent* Comp = EquipmentDurabilityTestHelpers::CreateEquipment(TEXT("Equipment.Slot.MainHand"));
	const FGameplayTag MainHand = FGameplayTag::RequestGameplayTag(TEXT("Equipment.Slot.MainHand"), false);

	float Current = -1.f, Max = -1.f;
	TestFalse(TEXT("Empty slot reports no durability"), Comp->GetDurability(MainHand, Current, Max));
	TestEqual(TEXT("Current cleared"), Current, 0.f, 0.001f);
	TestEqual(TEXT("Loss on an empty slot returns -1"), Comp->ApplyDurabilityLoss(MainHand, 5.f), -1.f, 0.001f);
	TestEqual(TEXT("Loss on an unknown slot returns -1"),
		Comp->ApplyDurabilityLoss(FGameplayTag::RequestGameplayTag(TEXT("Equipment.Slot.OffHand"), false), 5.f), -1.f, 0.001f);

	// An item with no DurabilityState fragment never wears.
	FItemInstance Item;
	Item.InstanceId = FGuid::NewGuid();
	Item.ItemDefinitionId = FPrimaryAssetId(TEXT("ItemDefinition"), TEXT("NoDurability"));
	Item.StackCount = 1;
	Comp->EquipmentSlots[0].EquippedItem = Item;
	Comp->EquipmentSlots[0].bIsOccupied = true;
	TestFalse(TEXT("Item without durability state reports none"), Comp->GetDurability(MainHand, Current, Max));

	Comp->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
