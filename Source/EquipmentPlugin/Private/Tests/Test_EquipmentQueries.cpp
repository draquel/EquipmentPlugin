#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Types/CGFItemTypes.h"
#include "Types/CGFEquipmentTypes.h"
#include "GameplayTagsManager.h"

#if WITH_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Helper: create an equipment manager with pre-allocated slots (no world needed)
// ---------------------------------------------------------------------------
namespace EquipmentTestHelpers
{
	// Ensure gameplay tags are available for tests
	FGameplayTag RequestTestTag(const FName& TagName)
	{
		return FGameplayTag::RequestGameplayTag(TagName, false);
	}

	UEquipmentManagerComponent* CreateTestEquipment(const TArray<FName>& SlotTagNames)
	{
		UEquipmentManagerComponent* Comp = NewObject<UEquipmentManagerComponent>();

		// Directly populate the runtime EquipmentSlots array (bypasses BeginPlay)
		for (const FName& TagName : SlotTagNames)
		{
			FEquipmentSlot Slot;
			Slot.SlotTag = RequestTestTag(TagName);
			Slot.bIsOccupied = false;
			Comp->EquipmentSlots.Add(Slot);
		}
		return Comp;
	}

	FItemInstance CreateTestItem(const FString& DefName = TEXT("TestItem"))
	{
		FItemInstance Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.ItemDefinitionId = FPrimaryAssetId(TEXT("ItemDefinition"), FName(*DefName));
		Item.StackCount = 1;
		return Item;
	}

	void PlaceItemInSlot(UEquipmentManagerComponent* Comp, const FItemInstance& Item, int32 SlotIndex)
	{
		if (SlotIndex >= 0 && SlotIndex < Comp->EquipmentSlots.Num())
		{
			Comp->EquipmentSlots[SlotIndex].EquippedItem = Item;
			Comp->EquipmentSlots[SlotIndex].bIsOccupied = true;
		}
	}
}

// ===========================================================================
// IsSlotOccupied
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_IsSlotOccupied_Empty,
	"Equipment.Queries.IsSlotOccupied.EmptySlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_IsSlotOccupied_Empty::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({TEXT("Equipment.MainHand")});
	FGameplayTag MainHand = EquipmentTestHelpers::RequestTestTag(TEXT("Equipment.MainHand"));

	TestFalse("Empty slot not occupied", Comp->IsSlotOccupied(MainHand));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_IsSlotOccupied_Filled,
	"Equipment.Queries.IsSlotOccupied.FilledSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_IsSlotOccupied_Filled::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({TEXT("Equipment.MainHand")});
	FGameplayTag MainHand = EquipmentTestHelpers::RequestTestTag(TEXT("Equipment.MainHand"));

	EquipmentTestHelpers::PlaceItemInSlot(Comp, EquipmentTestHelpers::CreateTestItem(TEXT("Sword")), 0);

	TestTrue("Filled slot is occupied", Comp->IsSlotOccupied(MainHand));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_IsSlotOccupied_InvalidTag,
	"Equipment.Queries.IsSlotOccupied.InvalidTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_IsSlotOccupied_InvalidTag::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({TEXT("Equipment.MainHand")});
	FGameplayTag Nonexistent = EquipmentTestHelpers::RequestTestTag(TEXT("Equipment.Nonexistent"));

	TestFalse("Nonexistent tag returns false", Comp->IsSlotOccupied(Nonexistent));
	return true;
}

// ===========================================================================
// GetEquippedItem
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_GetEquippedItem_Occupied,
	"Equipment.Queries.GetEquippedItem.Occupied",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_GetEquippedItem_Occupied::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({TEXT("Equipment.MainHand")});
	FGameplayTag MainHand = EquipmentTestHelpers::RequestTestTag(TEXT("Equipment.MainHand"));

	FItemInstance Sword = EquipmentTestHelpers::CreateTestItem(TEXT("Sword"));
	EquipmentTestHelpers::PlaceItemInSlot(Comp, Sword, 0);

	FItemInstance Result = Comp->GetEquippedItem(MainHand);
	TestTrue("Item is valid", Result.IsValid());
	TestEqual("Instance ID matches", Result.InstanceId, Sword.InstanceId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_GetEquippedItem_Empty,
	"Equipment.Queries.GetEquippedItem.Empty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_GetEquippedItem_Empty::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({TEXT("Equipment.MainHand")});
	FGameplayTag MainHand = EquipmentTestHelpers::RequestTestTag(TEXT("Equipment.MainHand"));

	FItemInstance Result = Comp->GetEquippedItem(MainHand);
	TestFalse("Empty slot returns invalid item", Result.IsValid());
	return true;
}

// ===========================================================================
// GetOccupiedSlotTags / GetEmptySlotTags
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_GetOccupiedTags,
	"Equipment.Queries.GetOccupiedSlotTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_GetOccupiedTags::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({
		TEXT("Equipment.MainHand"), TEXT("Equipment.OffHand"), TEXT("Equipment.Head")
	});

	// All empty initially
	TestEqual("No occupied slots", Comp->GetOccupiedSlotTags().Num(), 0);

	// Fill main hand and head
	EquipmentTestHelpers::PlaceItemInSlot(Comp, EquipmentTestHelpers::CreateTestItem(TEXT("Sword")), 0);
	EquipmentTestHelpers::PlaceItemInSlot(Comp, EquipmentTestHelpers::CreateTestItem(TEXT("Helmet")), 2);

	TArray<FGameplayTag> Occupied = Comp->GetOccupiedSlotTags();
	TestEqual("2 occupied slots", Occupied.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_GetEmptyTags,
	"Equipment.Queries.GetEmptySlotTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_GetEmptyTags::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({
		TEXT("Equipment.MainHand"), TEXT("Equipment.OffHand"), TEXT("Equipment.Head")
	});

	// All empty
	TestEqual("3 empty slots", Comp->GetEmptySlotTags().Num(), 3);

	// Fill one
	EquipmentTestHelpers::PlaceItemInSlot(Comp, EquipmentTestHelpers::CreateTestItem(), 1);

	TestEqual("2 empty slots after filling one", Comp->GetEmptySlotTags().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_AllFilled,
	"Equipment.Queries.AllSlotsFilled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_AllFilled::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({
		TEXT("Equipment.MainHand"), TEXT("Equipment.OffHand")
	});

	EquipmentTestHelpers::PlaceItemInSlot(Comp, EquipmentTestHelpers::CreateTestItem(TEXT("A")), 0);
	EquipmentTestHelpers::PlaceItemInSlot(Comp, EquipmentTestHelpers::CreateTestItem(TEXT("B")), 1);

	TestEqual("All occupied", Comp->GetOccupiedSlotTags().Num(), 2);
	TestEqual("None empty", Comp->GetEmptySlotTags().Num(), 0);
	return true;
}

// ===========================================================================
// No Slots
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipQuery_NoSlots,
	"Equipment.Queries.NoSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipQuery_NoSlots::RunTest(const FString& Parameters)
{
	auto* Comp = EquipmentTestHelpers::CreateTestEquipment({});

	TestEqual("No occupied", Comp->GetOccupiedSlotTags().Num(), 0);
	TestEqual("No empty", Comp->GetEmptySlotTags().Num(), 0);
	TestFalse("Invalid tag not occupied", Comp->IsSlotOccupied(FGameplayTag()));
	TestFalse("GetEquippedItem returns invalid", Comp->GetEquippedItem(FGameplayTag()).IsValid());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
