#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
#include "Data/ItemDefinition.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Tags/CGFGameplayTags.h"
#include "Testing/CGFTestGameInstance.h"
#include "Types/CGFEquipmentTypes.h"
#include "Types/CGFItemTypes.h"

#if WITH_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Equip and unequip with their results (code review CR2, D-CR23 / FW-34), on a component standing on an actor in a
// standalone game instance's world so the equipment fragment resolves through the item database. The query tests build the
// slots directly (BeginPlay copies AvailableSlots into EquipmentSlots; play has not begun here), as this one does.
// ---------------------------------------------------------------------------
namespace EquipmentEquipTestHelpers
{
	/** A registered definition, equippable into Slot when it has one (an empty tag: no equipment fragment). */
	FPrimaryAssetId RegisterDefinition(UItemDatabaseSubsystem* Database, const TCHAR* Name, FGameplayTag Slot)
	{
		UItemDefinition* Definition = NewObject<UItemDefinition>(GetTransientPackage(), Name);
		Definition->AddToRoot();
		if (Slot.IsValid())
		{
			UItemFragment_Equipment* Fragment = NewObject<UItemFragment_Equipment>(Definition);
			Fragment->EquipmentSlotTag = Slot;
			Definition->Fragments.Add(Fragment);
		}
		Database->RegisterDefinition(Definition->GetPrimaryAssetId(), Definition);
		return Definition->GetPrimaryAssetId();
	}

	void AddSlot(UEquipmentManagerComponent* Comp, FGameplayTag SlotTag)
	{
		FEquipmentSlot Slot;
		Slot.SlotTag = SlotTag;
		Slot.bIsOccupied = false;
		Comp->EquipmentSlots.Add(Slot);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentEquipResultsTest,
	"Equipment.Equip.Results",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentEquipResultsTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentEquipTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FGameplayTag MainHand = CGFGameplayTags::Equipment_Slot_MainHand;
	const FGameplayTag Head = CGFGameplayTags::Equipment_Slot_Head;
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("EquipTestSword"), MainHand);
	const FPrimaryAssetId Axe = RegisterDefinition(Database, TEXT("EquipTestAxe"), MainHand);
	const FPrimaryAssetId Pebble = RegisterDefinition(Database, TEXT("EquipTestPebble"), FGameplayTag());

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Equipment = NewObject<UEquipmentManagerComponent>(Owner);
	AddSlot(Equipment, MainHand); // one hand, no head slot

	// Equip into the matching slot.
	const FItemInstance SwordItem = Database->CreateItemInstance(Sword, 1);
	TestEqual(TEXT("A main-hand item equips"), static_cast<int32>(Equipment->TryEquip(SwordItem)), static_cast<int32>(EEquipmentResult::Success));
	TestTrue(TEXT("The main hand is occupied"), Equipment->IsSlotOccupied(MainHand));
	TestTrue(TEXT("The main hand holds the sword"), Equipment->GetEquippedItem(MainHand).InstanceId == SwordItem.InstanceId);

	// An item with no equipment fragment: TryEquip finds it no target slot (IncompatibleSlot, before validation); named a slot,
	// the validator refuses it as InvalidItem. Then a slot the component does not have.
	TestEqual(TEXT("An item with no equipment fragment has no target slot (IncompatibleSlot)"), static_cast<int32>(Equipment->TryEquip(Database->CreateItemInstance(Pebble, 1))),
		static_cast<int32>(EEquipmentResult::IncompatibleSlot));
	TestEqual(TEXT("An item with no equipment fragment named a slot is InvalidItem"), static_cast<int32>(Equipment->TryEquipToSlot(Database->CreateItemInstance(Pebble, 1), MainHand)),
		static_cast<int32>(EEquipmentResult::InvalidItem));
	TestEqual(TEXT("A slot the component lacks is IncompatibleSlot"), static_cast<int32>(Equipment->TryEquipToSlot(Database->CreateItemInstance(Axe, 1), Head)),
		static_cast<int32>(EEquipmentResult::IncompatibleSlot));
	TestTrue(TEXT("The sword is still equipped"), Equipment->GetEquippedItem(MainHand).InstanceId == SwordItem.InstanceId);

	// An occupied slot swaps: the axe replaces the sword and the result is Success (SlotOccupied is never returned).
	const FItemInstance AxeItem = Database->CreateItemInstance(Axe, 1);
	TestEqual(TEXT("Equipping into the occupied hand succeeds (a swap)"), static_cast<int32>(Equipment->TryEquip(AxeItem)), static_cast<int32>(EEquipmentResult::Success));
	TestTrue(TEXT("The main hand holds the axe"), Equipment->GetEquippedItem(MainHand).InstanceId == AxeItem.InstanceId);

	// Unequip gives the item back; an empty slot fails.
	FItemInstance Out;
	TestEqual(TEXT("Unequipping the main hand succeeds"), static_cast<int32>(Equipment->TryUnequip(MainHand, Out)), static_cast<int32>(EEquipmentResult::Success));
	TestTrue(TEXT("The unequipped item is the axe"), Out.InstanceId == AxeItem.InstanceId);
	TestFalse(TEXT("The main hand is empty"), Equipment->IsSlotOccupied(MainHand));
	TestEqual(TEXT("Unequipping an empty slot is Failed"), static_cast<int32>(Equipment->TryUnequip(MainHand, Out)), static_cast<int32>(EEquipmentResult::Failed));
	TestEqual(TEXT("Unequipping a slot the component lacks is Failed"), static_cast<int32>(Equipment->TryUnequip(Head, Out)), static_cast<int32>(EEquipmentResult::Failed));

	for (const FPrimaryAssetId& Id : { Sword, Axe, Pebble })
	{
		if (UItemDefinition* Definition = Database->GetDefinition(Id))
		{
			Definition->RemoveFromRoot();
		}
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
