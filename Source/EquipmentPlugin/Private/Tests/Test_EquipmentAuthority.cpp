#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
#include "Data/ItemDefinition.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Tags/CGFGameplayTags.h"
#include "Testing/CGFTestGameInstance.h"
#include "Types/CGFEquipmentTypes.h"
#include "Types/CGFItemTypes.h"

#if WITH_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Equipping from an inventory into an occupied slot keeps both items (G90), and the server equips only items it holds
// (G91). A standalone game instance holds the definitions; the manager and the inventory sit on actors in its world. A
// client is an actor with role AutonomousProxy: its Try* calls take their client branch, and its Server RPCs are delivered
// to the authority's components with CGFTesting::FRPCCall, as the net driver would on the server.
// ---------------------------------------------------------------------------
namespace EquipmentAuthorityTestHelpers
{
	/** A registered definition, equippable into Slot, with a weight and tags. */
	FPrimaryAssetId RegisterDefinition(UItemDatabaseSubsystem* Database, const TCHAR* Name, FGameplayTag Slot,
		float Weight = 0.0f, const FGameplayTagContainer& Tags = FGameplayTagContainer())
	{
		UItemDefinition* Definition = NewObject<UItemDefinition>(GetTransientPackage(), Name);
		Definition->AddToRoot();
		Definition->Weight = Weight;
		Definition->ItemTags = Tags;
		UItemFragment_Equipment* Fragment = NewObject<UItemFragment_Equipment>(Definition);
		Fragment->EquipmentSlotTag = Slot;
		Definition->Fragments.Add(Fragment);
		Database->RegisterDefinition(Definition->GetPrimaryAssetId(), Definition);
		return Definition->GetPrimaryAssetId();
	}

	void Release(UItemDatabaseSubsystem* Database, std::initializer_list<FPrimaryAssetId> Ids)
	{
		for (const FPrimaryAssetId& Id : Ids)
		{
			if (UItemDefinition* Definition = Database->GetDefinition(Id))
			{
				Definition->RemoveFromRoot();
			}
		}
	}

	/** A manager on the owner with the given slots (BeginPlay copies AvailableSlots; play has not begun). */
	UEquipmentManagerComponent* MakeEquipment(AActor* Owner, std::initializer_list<FGameplayTag> Slots)
	{
		UEquipmentManagerComponent* Comp = NewObject<UEquipmentManagerComponent>(Owner);
		for (const FGameplayTag& Tag : Slots)
		{
			FEquipmentSlot Slot;
			Slot.SlotTag = Tag;
			Slot.bIsOccupied = false;
			Comp->EquipmentSlots.Add(Slot);
		}
		return Comp;
	}

	/** An inventory on the owner with pre-allocated slots. */
	UInventoryComponent* MakeInventory(AActor* Owner, int32 Slots, float MaxWeight = 0.0f)
	{
		UInventoryComponent* Comp = NewObject<UInventoryComponent>(Owner);
		Comp->MaxSlots = Slots;
		Comp->MaxWeight = MaxWeight;
		Comp->InventorySlots.Items.SetNum(Slots);
		for (int32 i = 0; i < Slots; ++i)
		{
			Comp->InventorySlots.Items[i].SlotIndex = i;
			Comp->InventorySlots.Items[i].bIsOccupied = false;
		}
		Comp->InventorySlots.OwningComponent = Comp;
		return Comp;
	}

	void PlaceItemInSlot(UInventoryComponent* Comp, const FItemInstance& Item, int32 Slot)
	{
		FInventorySlot& S = Comp->InventorySlots.Items[Slot];
		S.Item = Item;
		S.bIsOccupied = true;
	}

	/** Put an item into a slot as replication would (a client's copy, or a test's starting state). */
	void PlaceEquipped(UEquipmentManagerComponent* Comp, const FItemInstance& Item, FGameplayTag SlotTag)
	{
		for (FEquipmentSlot& Slot : Comp->EquipmentSlots)
		{
			if (Slot.SlotTag == SlotTag)
			{
				Slot.EquippedItem = Item;
				Slot.bIsOccupied = true;
			}
		}
	}

	int32 Result(EEquipmentResult Value)
	{
		return static_cast<int32>(Value);
	}
}

// ===========================================================================
// G90: a swap through the inventory loses nothing
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentFromInventorySwapTest,
	"Equipment.Equip.FromInventorySwap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentFromInventorySwapTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentAuthorityTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FGameplayTag MainHand = CGFGameplayTags::Equipment_Slot_MainHand;
	FGameplayTagContainer QuestTags;
	QuestTags.AddTag(CGFGameplayTags::Item_Category_Quest);
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("SwapTestSword"), MainHand, 3.0f);
	const FPrimaryAssetId Axe = RegisterDefinition(Database, TEXT("SwapTestAxe"), MainHand, 2.0f);
	const FPrimaryAssetId Hammer = RegisterDefinition(Database, TEXT("SwapTestHammer"), MainHand, 30.0f);
	const FPrimaryAssetId Relic = RegisterDefinition(Database, TEXT("SwapTestRelic"), MainHand, 0.0f, QuestTags);

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Equipment = MakeEquipment(Owner, { MainHand });
	UInventoryComponent* Bag = MakeInventory(Owner, 2, 40.0f);

	// A full bag: the sword in hand, the axe and a filler in the bag. The swap needs no free slot: the sword takes the
	// axe's slot.
	const FItemInstance SwordItem = Database->CreateItemInstance(Sword, 1);
	const FItemInstance AxeItem = Database->CreateItemInstance(Axe, 1);
	const FItemInstance Filler = Database->CreateItemInstance(Axe, 1);
	PlaceEquipped(Equipment, SwordItem, MainHand);
	PlaceItemInSlot(Bag, Filler, 0);
	PlaceItemInSlot(Bag, AxeItem, 1);
	TestEqual(TEXT("Equipping the axe over the sword from a full bag succeeds"), Result(Equipment->TryEquipFromInventory(AxeItem.InstanceId, Bag, MainHand)),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The hand holds the axe"), Equipment->GetEquippedItem(MainHand).InstanceId == AxeItem.InstanceId);
	TestTrue(TEXT("The sword took the axe's slot"), Bag->GetItemInSlot(1).InstanceId == SwordItem.InstanceId);
	TestTrue(TEXT("The filler is untouched"), Bag->GetItemInSlot(0).InstanceId == Filler.InstanceId);

	// The old item's weight counts against what the new one frees: a 30 hammer for the 3 sword fits 2 + 30 of 40.
	const FItemInstance HammerItem = Database->CreateItemInstance(Hammer, 1);
	PlaceEquipped(Equipment, HammerItem, MainHand); // set directly: the hand holds the hammer, the bag the filler and the sword
	TestEqual(TEXT("A swap whose old item fits the bag succeeds"), Result(Equipment->TryEquipFromInventory(SwordItem.InstanceId, Bag, MainHand)),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The hammer took the sword's slot"), Bag->GetItemInSlot(1).InstanceId == HammerItem.InstanceId);

	// The equipped item cannot take the slot: refused, both sides as they were.
	const FItemInstance RelicItem = Database->CreateItemInstance(Relic, 1);
	Bag->DeniedItemTags.AddTag(CGFGameplayTags::Item_Category_Quest);
	PlaceEquipped(Equipment, RelicItem, MainHand); // set directly: the hand holds a quest relic
	TestEqual(TEXT("An old item the bag refuses (a denied tag) is NoInventorySpace"), Result(Equipment->TryEquipFromInventory(HammerItem.InstanceId, Bag, MainHand)),
		Result(EEquipmentResult::NoInventorySpace));
	TestTrue(TEXT("The relic is still equipped"), Equipment->GetEquippedItem(MainHand).InstanceId == RelicItem.InstanceId);
	TestTrue(TEXT("The hammer is still in the bag"), Bag->GetItemInSlot(1).InstanceId == HammerItem.InstanceId);

	// An empty slot: the item just leaves the bag. An id the bag does not hold is InvalidItem.
	FItemInstance Unequipped;
	TestEqual(TEXT("Unequipping the relic succeeds"), Result(Equipment->TryUnequip(MainHand, Unequipped)), Result(EEquipmentResult::Success));
	TestEqual(TEXT("Equipping into the empty hand succeeds"), Result(Equipment->TryEquipFromInventory(HammerItem.InstanceId, Bag, FGameplayTag())),
		Result(EEquipmentResult::Success));
	TestFalse(TEXT("Slot 1 is empty"), Bag->GetItemInSlot(1).IsValid());
	TestEqual(TEXT("An id the bag does not hold is InvalidItem"), Result(Equipment->TryEquipFromInventory(FGuid::NewGuid(), Bag, MainHand)),
		Result(EEquipmentResult::InvalidItem));

	Release(Database, { Sword, Axe, Hammer, Relic });
	return true;
}

// ===========================================================================
// G91: simulated server/client: a client names an item the server holds
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentAuthorityClientRequestsTest,
	"Equipment.Authority.ClientRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentAuthorityClientRequestsTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentAuthorityTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FGameplayTag MainHand = CGFGameplayTags::Equipment_Slot_MainHand;
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("ClientEquipTestSword"), MainHand);
	const FPrimaryAssetId Legendary = RegisterDefinition(Database, TEXT("ClientEquipTestLegendary"), MainHand);

	// The server's player and the same player as its client sees itself; another player on the server.
	AActor* ServerPlayer = Game.SpawnOwner();
	UEquipmentManagerComponent* ServerEquipment = MakeEquipment(ServerPlayer, { MainHand });
	UInventoryComponent* ServerBag = MakeInventory(ServerPlayer, 4);
	AActor* ClientPlayer = Game.SpawnRemoteOwner();
	UEquipmentManagerComponent* ClientEquipment = MakeEquipment(ClientPlayer, { MainHand });
	UInventoryComponent* ClientBag = MakeInventory(ClientPlayer, 4);
	AActor* OtherPlayer = Game.SpawnOwner();
	UInventoryComponent* OtherBag = MakeInventory(OtherPlayer, 4);

	const FItemInstance SwordItem = Database->CreateItemInstance(Sword, 1);
	PlaceItemInSlot(ServerBag, SwordItem, 0);
	PlaceItemInSlot(ClientBag, SwordItem, 0); // replicated

	// A forged item cannot be equipped from a client: the direct equip is server-only and no request carries an item.
	AddExpectedMessage(TEXT("equip from an inventory instead"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	const FItemInstance Forged = Database->CreateItemInstance(Legendary, 1);
	TestEqual(TEXT("A client's TryEquip is NotAuthorized"), Result(ClientEquipment->TryEquip(Forged)), Result(EEquipmentResult::NotAuthorized));
	TestEqual(TEXT("A client's TryEquipToSlot is NotAuthorized"), Result(ClientEquipment->TryEquipToSlot(Forged, MainHand)), Result(EEquipmentResult::NotAuthorized));
	TestFalse(TEXT("Nothing is equipped on the client"), ClientEquipment->IsSlotOccupied(MainHand));
	TestNull(TEXT("UEquipmentManagerComponent has no ServerRPC_RequestEquip"), ServerEquipment->FindFunction(TEXT("ServerRPC_RequestEquip")));

	// The client's equip from its bag: checked with the shared predicate, sent as an instance id, optimistic.
	TestEqual(TEXT("The client's equip from its bag is Success (optimistic)"), Result(ClientEquipment->TryEquipFromInventory(SwordItem.InstanceId, ClientBag, MainHand)),
		Result(EEquipmentResult::Success));
	TestFalse(TEXT("The client's copy waits for replication"), ClientEquipment->IsSlotOccupied(MainHand));
	TestEqual(TEXT("The client's check refuses an id its bag does not hold"), Result(ClientEquipment->TryEquipFromInventory(Forged.InstanceId, ClientBag, MainHand)),
		Result(EEquipmentResult::InvalidItem));

	// The server receives the id and equips the item it holds under it.
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestEquipFromInventory"))
		.Set(TEXT("ItemInstanceId"), SwordItem.InstanceId).Set(TEXT("SourceInventory"), ServerBag).Set(TEXT("SlotTag"), MainHand).Send();
	TestTrue(TEXT("On the server the sword is equipped"), ServerEquipment->GetEquippedItem(MainHand).InstanceId == SwordItem.InstanceId);
	TestFalse(TEXT("On the server the sword left the bag"), ServerBag->GetItemInSlot(0).IsValid());

	// An id the server does not hold under it changes nothing.
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestEquipFromInventory"))
		.Set(TEXT("ItemInstanceId"), Forged.InstanceId).Set(TEXT("SourceInventory"), ServerBag).Set(TEXT("SlotTag"), MainHand).Send();
	TestTrue(TEXT("A forged id leaves the sword equipped"), ServerEquipment->GetEquippedItem(MainHand).InstanceId == SwordItem.InstanceId);

	// Another player's inventory is not the requester's to name: neither to equip from nor to unequip into.
	AddExpectedMessage(TEXT("named an inventory it does not own"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	const FItemInstance OthersItem = Database->CreateItemInstance(Sword, 1);
	PlaceItemInSlot(OtherBag, OthersItem, 0);
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestEquipFromInventory"))
		.Set(TEXT("ItemInstanceId"), OthersItem.InstanceId).Set(TEXT("SourceInventory"), OtherBag).Set(TEXT("SlotTag"), MainHand).Send();
	TestTrue(TEXT("Another player's item stays in their bag"), OtherBag->GetItemInSlot(0).InstanceId == OthersItem.InstanceId);
	TestTrue(TEXT("The requester keeps its sword equipped"), ServerEquipment->GetEquippedItem(MainHand).InstanceId == SwordItem.InstanceId);
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestUnequipToInventory"))
		.Set(TEXT("SlotTag"), MainHand).Set(TEXT("TargetInventory"), OtherBag).Send();
	TestTrue(TEXT("The sword is not unequipped into another player's bag"), ServerEquipment->IsSlotOccupied(MainHand));

	// Unequip into its own bag works.
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestUnequipToInventory"))
		.Set(TEXT("SlotTag"), MainHand).Set(TEXT("TargetInventory"), ServerBag).Send();
	TestFalse(TEXT("The hand is empty"), ServerEquipment->IsSlotOccupied(MainHand));
	TestNotEqual(TEXT("The sword is back in the bag"), ServerBag->FindSlotIndexByInstanceId(SwordItem.InstanceId), static_cast<int32>(INDEX_NONE));

	Release(Database, { Sword, Legendary });
	return true;
}

#endif // WITH_AUTOMATION_TESTS
