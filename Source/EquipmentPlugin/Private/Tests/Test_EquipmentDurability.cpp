// Copyright Daniel Raquel. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Data/Fragments/ItemFragment_Durability.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
#include "Data/ItemDefinition.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Tags/CGFGameplayTags.h"
#include "Testing/CGFTestGameInstance.h"
#include "Types/CGFItemTypes.h"
#include "Types/ItemInstanceFragments.h"
#include "GameplayTagsManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Durability (feature 5b): the guard paths that need no item database, and the
// wear on an item the database made (G136). Destroy-at-zero runs in PIE.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipDurability_LossMarksTheItem,
	"Equipment.Durability.LossMarksTheItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEquipDurability_LossMarksTheItem::RunTest(const FString& Parameters)
{
	// EquipmentSlots replicates by comparing each slot's item with the state it last sent (FItemInstance::Identical), and
	// that state shares the item's fragment objects: a loss must change the slot's item as Identical sees it, or the
	// worn value never reaches a client (G136).
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FGameplayTag MainHand = CGFGameplayTags::Equipment_Slot_MainHand;
	UItemDefinition* Definition = NewObject<UItemDefinition>(GetTransientPackage(), TEXT("DurabilityTestSword"));
	Definition->AddToRoot();
	UItemFragment_Equipment* Equip = NewObject<UItemFragment_Equipment>(Definition);
	Equip->EquipmentSlotTag = MainHand;
	Definition->Fragments.Add(Equip);
	UItemFragment_Durability* Durability = NewObject<UItemFragment_Durability>(Definition);
	Durability->MaxDurability = 50.f;
	Definition->Fragments.Add(Durability);
	UInstanceFragment_DurabilityState* Full = NewObject<UInstanceFragment_DurabilityState>(Definition);
	Full->CurrentDurability = 50.f;
	Definition->DefaultInstanceFragments.Add(Full);
	Database->RegisterDefinition(Definition->GetPrimaryAssetId(), Definition);

	UEquipmentManagerComponent* Equipment = NewObject<UEquipmentManagerComponent>(Game.SpawnOwner());
	FEquipmentSlot Slot;
	Slot.SlotTag = MainHand;
	Equipment->EquipmentSlots.Add(Slot);
	const FItemInstance Sword = Database->CreateItemInstance(Definition->GetPrimaryAssetId(), 1);
	TestEqual(TEXT("The sword equips"), static_cast<int32>(Equipment->TryEquip(Sword)), static_cast<int32>(EEquipmentResult::Success));

	const FItemInstance Before = Equipment->GetEquippedItem(MainHand);
	TestEqual(TEXT("The loss leaves the rest"), Equipment->ApplyDurabilityLoss(MainHand, 8.f), 42.f, 0.001f);
	const FItemInstance After = Equipment->GetEquippedItem(MainHand);
	float Current = 0.f, Max = 0.f;
	TestTrue(TEXT("The worn sword reports its durability"), Equipment->GetDurability(MainHand, Current, Max));
	TestEqual(TEXT("Current is the rest"), Current, 42.f, 0.001f);
	TestTrue(TEXT("The worn sword is the same instance"), Before == After);
	TestFalse(TEXT("The worn sword is not identical to its state before the loss"),
		FItemInstance::StaticStruct()->CompareScriptStruct(&Before, &After, PPF_None));

	Definition->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
