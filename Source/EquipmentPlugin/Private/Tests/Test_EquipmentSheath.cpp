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
#include "UI/EquipmentSlotWidget.h"

#if WITH_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Sheaths (the equipment sheathing plan, D77): a carried weapon lands in its sheath, is drawn into its hand and put
// back, one item per hand, its GAS only while drawn, its visual's bone and fit by the state, the client's requests. As
// the other equipment tests: a standalone game instance holds the definitions, the slots are built directly (play has
// not begun), a client is an AutonomousProxy actor whose RPCs reach the server's component through CGFTesting::FRPCCall.
// ---------------------------------------------------------------------------
namespace EquipmentSheathTestHelpers
{
	FGameplayTag MainHand() { return CGFGameplayTags::Equipment_Slot_MainHand; }
	FGameplayTag Head() { return CGFGameplayTags::Equipment_Slot_Head; }
	FGameplayTag HipLeft() { return CGFGameplayTags::Equipment_Slot_Sheath_HipLeft; }
	FGameplayTag Back() { return CGFGameplayTags::Equipment_Slot_Sheath_Back; }

	/** A registered definition used in Slot, carried in Sheath when it is set; Configure fills the rest of the fragment. */
	FPrimaryAssetId RegisterDefinition(UItemDatabaseSubsystem* Database, const TCHAR* Name, FGameplayTag Slot, FGameplayTag Sheath,
		TFunction<void(UItemFragment_Equipment&)> Configure = nullptr)
	{
		UItemDefinition* Definition = NewObject<UItemDefinition>(GetTransientPackage(), Name);
		Definition->AddToRoot();
		UItemFragment_Equipment* Fragment = NewObject<UItemFragment_Equipment>(Definition);
		Fragment->EquipmentSlotTag = Slot;
		Fragment->SheathSlotTag = Sheath;
		if (Configure)
		{
			Configure(*Fragment);
		}
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

	void AddSlot(UEquipmentManagerComponent* Comp, FGameplayTag Tag, FGameplayTag DrawInto = FGameplayTag(), FName Socket = NAME_None,
		const FTransform& Correction = FTransform::Identity)
	{
		FEquipmentSlot Slot;
		Slot.SlotTag = Tag;
		Slot.DrawSlotTag = DrawInto;
		Slot.AttachSocket = Socket;
		Slot.AttachOffset = Correction;
		Comp->EquipmentSlots.Add(Slot);
	}

	/** The character's layout: the hand, the head, a hip sheath and a back sheath drawn into the hand. */
	UEquipmentManagerComponent* MakeCarrier(AActor* Owner)
	{
		UEquipmentManagerComponent* Comp = NewObject<UEquipmentManagerComponent>(Owner);
		AddSlot(Comp, MainHand(), FGameplayTag(), TEXT("hand_r"));
		AddSlot(Comp, Head(), FGameplayTag(), TEXT("head"));
		AddSlot(Comp, HipLeft(), MainHand(), TEXT("pelvis"));
		AddSlot(Comp, Back(), MainHand(), TEXT("spine_05"));
		return Comp;
	}

	UInventoryComponent* MakeInventory(AActor* Owner, int32 Slots)
	{
		UInventoryComponent* Comp = NewObject<UInventoryComponent>(Owner);
		Comp->MaxSlots = Slots;
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

	/** Put an item into a slot as replication would. */
	FEquipmentSlot* PlaceEquipped(UEquipmentManagerComponent* Comp, const FItemInstance& Item, FGameplayTag SlotTag, bool bDrawn = false)
	{
		for (FEquipmentSlot& Slot : Comp->EquipmentSlots)
		{
			if (Slot.SlotTag == SlotTag)
			{
				Slot.EquippedItem = Item;
				Slot.bIsOccupied = true;
				Slot.bDrawn = bDrawn;
				return &Slot;
			}
		}
		return nullptr;
	}

	int32 Result(EEquipmentResult Value)
	{
		return static_cast<int32>(Value);
	}

	bool SameItem(const FItemInstance& A, const FItemInstance& B)
	{
		return A.IsValid() && A.InstanceId == B.InstanceId;
	}
}

// ===========================================================================
// Where an equip lands
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathTargetSlotTest,
	"Equipment.Sheath.TargetSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathTargetSlotTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("SheathTargetSword"), MainHand(), HipLeft());
	const FPrimaryAssetId Bow = RegisterDefinition(Database, TEXT("SheathTargetBow"), MainHand(), CGFGameplayTags::Equipment_Slot_Sheath);
	const FPrimaryAssetId Club = RegisterDefinition(Database, TEXT("SheathTargetClub"), MainHand(), FGameplayTag());

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);

	// A carried weapon lands in its sheath, sheathed: the hand stays empty.
	const FItemInstance SwordItem = Database->CreateItemInstance(Sword, 1);
	TestEqual(TEXT("The sword equips"), Result(Carrier->TryEquip(SwordItem)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The sword is in its hip sheath"), SameItem(Carrier->GetEquippedItem(HipLeft()), SwordItem));
	TestFalse(TEXT("The sword is not drawn"), Carrier->IsDrawn(HipLeft()));
	TestFalse(TEXT("The hand slot itself is empty"), Carrier->IsSlotOccupied(MainHand()));
	TestFalse(TEXT("Nothing is held in the main hand"), Carrier->GetHeldItem(MainHand()).IsValid());
	TestTrue(TEXT("The hip is a sheath"), Carrier->IsSheathSlot(HipLeft()));
	TestFalse(TEXT("The hand is no sheath"), Carrier->IsSheathSlot(MainHand()));

	// A sheath named by its parent tag takes the first empty one (the hip is full: the back).
	const FItemInstance BowItem = Database->CreateItemInstance(Bow, 1);
	TestEqual(TEXT("The bow equips"), Result(Carrier->TryEquip(BowItem)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The bow takes the empty sheath, the back"), SameItem(Carrier->GetEquippedItem(Back()), BowItem));

	// An item without a sheath goes into its hand as before.
	const FItemInstance ClubItem = Database->CreateItemInstance(Club, 1);
	TestEqual(TEXT("The club equips"), Result(Carrier->TryEquip(ClubItem)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The club is in the hand slot itself"), SameItem(Carrier->GetEquippedItem(MainHand()), ClubItem));
	TestTrue(TEXT("The hand holds the club"), Carrier->GetHeldSlotTag(MainHand()) == MainHand());

	// A second sword asked into the hand goes to its sheath, which is full: refused directly (no inventory, G90).
	TestEqual(TEXT("A second sword asked into the hand is SlotOccupied (its sheath is full)"),
		Result(Carrier->TryEquipToSlot(Database->CreateItemInstance(Sword, 1), MainHand())), Result(EEquipmentResult::SlotOccupied));

	// An owner without sheaths equips the sword into its hand, as before sheaths existed.
	UEquipmentManagerComponent* Plain = NewObject<UEquipmentManagerComponent>(Owner);
	AddSlot(Plain, MainHand(), FGameplayTag(), TEXT("hand_r"));
	const FItemInstance PlainSword = Database->CreateItemInstance(Sword, 1);
	TestEqual(TEXT("Without sheaths the sword equips"), Result(Plain->TryEquip(PlainSword)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("Without sheaths it is in the hand"), SameItem(Plain->GetHeldItem(MainHand()), PlainSword));

	// Asked into its hand on the carrier (an old save's main-hand entry), a sword goes into its sheath, drawn.
	UEquipmentManagerComponent* Fresh = MakeCarrier(Owner);
	const FItemInstance SavedSword = Database->CreateItemInstance(Sword, 1);
	TestEqual(TEXT("A sword asked into the hand equips"), Result(Fresh->TryEquipToSlot(SavedSword, MainHand())), Result(EEquipmentResult::Success));
	TestTrue(TEXT("It sits in its sheath"), SameItem(Fresh->GetEquippedItem(HipLeft()), SavedSword));
	TestTrue(TEXT("It is drawn"), Fresh->IsDrawn(HipLeft()));
	TestTrue(TEXT("The hand holds it"), SameItem(Fresh->GetHeldItem(MainHand()), SavedSword));

	Release(Database, { Sword, Bow, Club });
	return true;
}

// ===========================================================================
// Drawing and sheathing; one item per hand
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathDrawTest,
	"Equipment.Sheath.DrawAndSheathe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathDrawTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("SheathDrawSword"), MainHand(), HipLeft());
	const FPrimaryAssetId Bow = RegisterDefinition(Database, TEXT("SheathDrawBow"), MainHand(), Back());
	const FPrimaryAssetId Club = RegisterDefinition(Database, TEXT("SheathDrawClub"), MainHand(), FGameplayTag());

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);
	const FItemInstance SwordItem = Database->CreateItemInstance(Sword, 1);
	const FItemInstance BowItem = Database->CreateItemInstance(Bow, 1);
	Carrier->TryEquip(SwordItem);
	Carrier->TryEquip(BowItem);

	// The refusals first: the hand is no sheath, the head is no sheath either, an empty sheath has nothing to draw.
	TestEqual(TEXT("Drawing the hand is IncompatibleSlot"), Result(Carrier->TryDraw(MainHand())), Result(EEquipmentResult::IncompatibleSlot));
	TestEqual(TEXT("Drawing an ordinary slot is IncompatibleSlot"), Result(Carrier->TryDraw(Head())), Result(EEquipmentResult::IncompatibleSlot));
	TestEqual(TEXT("Sheathing with nothing drawn is Failed"), Result(Carrier->TrySheathe(MainHand())), Result(EEquipmentResult::Failed));

	// Draw the sword: the hand holds it, the sheath keeps it.
	TestEqual(TEXT("Drawing the sword succeeds"), Result(Carrier->TryDraw(HipLeft())), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The sword is drawn"), Carrier->IsDrawn(HipLeft()));
	TestTrue(TEXT("The hand's held slot is the hip"), Carrier->GetHeldSlotTag(MainHand()) == HipLeft());
	TestTrue(TEXT("The hand holds the sword"), SameItem(Carrier->GetHeldItem(MainHand()), SwordItem));
	TestTrue(TEXT("The sword's home is still the hip"), SameItem(Carrier->GetEquippedItem(HipLeft()), SwordItem));
	TestEqual(TEXT("Drawing it again is Success (nothing to do)"), Result(Carrier->TryDraw(HipLeft())), Result(EEquipmentResult::Success));

	// Draw the bow: the sword goes back in the same step.
	TestEqual(TEXT("Drawing the bow succeeds"), Result(Carrier->TryDraw(Back())), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The bow is drawn"), Carrier->IsDrawn(Back()));
	TestFalse(TEXT("The sword went back to its sheath"), Carrier->IsDrawn(HipLeft()));
	TestTrue(TEXT("The hand holds the bow"), SameItem(Carrier->GetHeldItem(MainHand()), BowItem));

	// Sheathe by the hand, then by the sheath's own tag.
	TestEqual(TEXT("Sheathing the hand succeeds"), Result(Carrier->TrySheathe(MainHand())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The bow is sheathed"), Carrier->IsDrawn(Back()));
	TestFalse(TEXT("The hand is empty"), Carrier->GetHeldItem(MainHand()).IsValid());
	Carrier->TryDraw(HipLeft());
	TestEqual(TEXT("Sheathing by the sheath's tag succeeds"), Result(Carrier->TrySheathe(HipLeft())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The sword is sheathed"), Carrier->IsDrawn(HipLeft()));

	// An item equipped into the hand slot itself sends the drawn weapon back to its sheath.
	Carrier->TryDraw(HipLeft());
	const FItemInstance ClubItem = Database->CreateItemInstance(Club, 1);
	TestEqual(TEXT("The club equips into the hand"), Result(Carrier->TryEquipToSlot(ClubItem, MainHand())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The sword was sheathed for it"), Carrier->IsDrawn(HipLeft()));
	TestTrue(TEXT("The hand holds the club"), Carrier->GetHeldSlotTag(MainHand()) == MainHand());

	// A draw with the hand slot itself full: no bag is SlotOccupied, a full bag NoInventorySpace, room in the bag takes it.
	TestEqual(TEXT("Drawing over the club with no bag is SlotOccupied"), Result(Carrier->TryDraw(HipLeft())), Result(EEquipmentResult::SlotOccupied));
	UInventoryComponent* FullBag = MakeInventory(Owner, 1);
	PlaceItemInSlot(FullBag, Database->CreateItemInstance(Club, 1), 0);
	TestEqual(TEXT("Drawing over the club with a full bag is NoInventorySpace"), Result(Carrier->TryDraw(HipLeft(), FullBag)),
		Result(EEquipmentResult::NoInventorySpace));
	TestTrue(TEXT("The refusals changed nothing"), SameItem(Carrier->GetEquippedItem(MainHand()), ClubItem) && !Carrier->IsDrawn(HipLeft()));
	UInventoryComponent* Bag = MakeInventory(Owner, 2);
	TestEqual(TEXT("Drawing over the club with room in the bag succeeds"), Result(Carrier->TryDraw(HipLeft(), Bag)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The sword is drawn"), Carrier->IsDrawn(HipLeft()));
	TestFalse(TEXT("The hand slot itself is empty"), Carrier->IsSlotOccupied(MainHand()));
	TestNotEqual(TEXT("The club is in the bag"), Bag->FindSlotIndexByInstanceId(ClubItem.InstanceId), static_cast<int32>(INDEX_NONE));

	// Unequipping a drawn weapon empties the hand.
	FItemInstance Out;
	TestEqual(TEXT("Unequipping the drawn sword succeeds"), Result(Carrier->TryUnequip(HipLeft(), Out)), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The emptied sheath is not drawn"), Carrier->IsDrawn(HipLeft()));
	TestFalse(TEXT("The hand is empty"), Carrier->GetHeldItem(MainHand()).IsValid());
	TestEqual(TEXT("Drawing an empty sheath is InvalidItem"), Result(Carrier->TryDraw(HipLeft())), Result(EEquipmentResult::InvalidItem));

	Release(Database, { Sword, Bow, Club });
	return true;
}

// ===========================================================================
// GAS follows the hand
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathGASTest,
	"Equipment.Sheath.GASWhileDrawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathGASTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FPrimaryAssetId Pickaxe = RegisterDefinition(Database, TEXT("SheathGASPickaxe"), MainHand(), HipLeft());
	const FPrimaryAssetId Helmet = RegisterDefinition(Database, TEXT("SheathGASHelmet"), Head(), FGameplayTag());

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);
	// The GAS module's callbacks, counted per slot (what it would grant and revoke).
	TMap<FGameplayTag, int32> Granted;
	Carrier->OnGASEquipCallback = [&Granted](const FItemInstance&, FGameplayTag SlotTag) { ++Granted.FindOrAdd(SlotTag); };
	Carrier->OnGASUnequipCallback = [&Granted](FGameplayTag SlotTag) { --Granted.FindOrAdd(SlotTag); };

	Carrier->TryEquip(Database->CreateItemInstance(Pickaxe, 1));
	TestEqual(TEXT("A sheathed pickaxe grants nothing"), Granted.FindRef(HipLeft()), 0);
	Carrier->TryDraw(HipLeft());
	TestEqual(TEXT("Drawn, it grants its abilities and effects"), Granted.FindRef(HipLeft()), 1);
	Carrier->TrySheathe(MainHand());
	TestEqual(TEXT("Sheathed again, they are revoked"), Granted.FindRef(HipLeft()), 0);
	Carrier->TryDraw(HipLeft());
	FItemInstance Out;
	Carrier->TryUnequip(HipLeft(), Out);
	TestEqual(TEXT("Unequipped while drawn, they are revoked"), Granted.FindRef(HipLeft()), 0);
	Carrier->TryEquip(Database->CreateItemInstance(Pickaxe, 1));
	Carrier->TryUnequip(HipLeft(), Out);
	TestEqual(TEXT("Unequipped while sheathed, nothing is revoked that was not granted"), Granted.FindRef(HipLeft()), 0);

	// An ordinary slot grants on the equip, as before.
	Carrier->TryEquip(Database->CreateItemInstance(Helmet, 1));
	TestEqual(TEXT("A helmet grants on the equip"), Granted.FindRef(Head()), 1);

	Carrier->OnGASEquipCallback = nullptr;
	Carrier->OnGASUnequipCallback = nullptr;
	Release(Database, { Pickaxe, Helmet });
	return true;
}

// ===========================================================================
// The visual's bone and fit by the state
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathAttachTest,
	"Equipment.Sheath.AttachByState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathAttachTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FTransform HandFit(FRotator(0.f, 0.f, 13.f), FVector(1.f, 2.f, 3.f));
	const FTransform SheathFit(FRotator(30.f, 0.f, 0.f), FVector(-8.f, 4.f, 0.f));
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("SheathAttachSword"), MainHand(), HipLeft(),
		[&](UItemFragment_Equipment& F) { F.AttachOffset = HandFit; F.SheathOffset = SheathFit; });
	const FPrimaryAssetId Bow = RegisterDefinition(Database, TEXT("SheathAttachBow"), MainHand(), Back(),
		[&](UItemFragment_Equipment& F) { F.AttachOffset = HandFit; F.AttachBoneOverride = TEXT("hand_l"); F.SheathOffset = SheathFit; F.SheathBoneOverride = TEXT("spine_04"); });

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = NewObject<UEquipmentManagerComponent>(Owner);
	const FTransform HandCorrection(FRotator(0.f, 90.f, 0.f));
	const FTransform HipCorrection(FRotator(0.f, 0.f, -90.f), FVector(0.f, 10.f, 0.f));
	AddSlot(Carrier, MainHand(), FGameplayTag(), TEXT("hand_r"), HandCorrection);
	AddSlot(Carrier, HipLeft(), MainHand(), TEXT("pelvis"), HipCorrection);
	AddSlot(Carrier, Back(), MainHand(), TEXT("spine_05"), HipCorrection);
	Carrier->TryEquip(Database->CreateItemInstance(Sword, 1));
	Carrier->TryEquip(Database->CreateItemInstance(Bow, 1));

	// Sheathed: the sheath's bone, the sheath fit then the sheath's correction.
	TestTrue(TEXT("A sheathed sword hangs on the sheath's bone"), Carrier->GetVisualAttachSocket(HipLeft()) == FName(TEXT("pelvis")));
	TestTrue(TEXT("A sheathed sword's fit is its sheath fit in the sheath's frame"),
		Carrier->GetVisualAttachOffset(HipLeft()).Equals(SheathFit * HipCorrection, 1.e-3f));
	// Drawn: the hand's bone, the hand fit then the hand's correction (as if equipped in the hand).
	TestTrue(TEXT("For the drawn state the sword rides the hand's bone"), Carrier->GetVisualAttachSocketForState(HipLeft(), true) == FName(TEXT("hand_r")));
	TestTrue(TEXT("For the drawn state its fit is the hand fit in the hand's frame"),
		Carrier->GetVisualAttachOffsetForState(HipLeft(), true).Equals(HandFit * HandCorrection, 1.e-3f));
	Carrier->TryDraw(HipLeft());
	TestTrue(TEXT("Drawn, the sword rides the hand"), Carrier->GetVisualAttachSocket(HipLeft()) == FName(TEXT("hand_r")));
	TestTrue(TEXT("Drawn, its own bone is none"), Carrier->GetItemAttachBone(HipLeft()).IsNone());

	// The item's own bones: the bow on hand_l in the hand and on spine_04 sheathed, its fits alone on them.
	TestTrue(TEXT("A sheathed bow hangs on its own sheath bone"), Carrier->GetVisualAttachSocket(Back()) == FName(TEXT("spine_04")));
	TestTrue(TEXT("On its own sheath bone the sheath's correction does not apply"), Carrier->GetVisualAttachOffset(Back()).Equals(SheathFit, 1.e-3f));
	TestTrue(TEXT("Sheathed, the bow's own bone is the sheath bone"), Carrier->GetItemAttachBone(Back()) == FName(TEXT("spine_04")));
	Carrier->TryDraw(Back());
	TestTrue(TEXT("A drawn bow rides its own hand bone"), Carrier->GetVisualAttachSocket(Back()) == FName(TEXT("hand_l")));
	TestTrue(TEXT("On its own hand bone only its hand fit applies"), Carrier->GetVisualAttachOffset(Back()).Equals(HandFit, 1.e-3f));
	TestTrue(TEXT("Drawn, the bow's own bone is its hand bone"), Carrier->GetItemAttachBone(Back()) == FName(TEXT("hand_l")));
	TestTrue(TEXT("The sword, sheathed again by the bow's draw, is back on the hip"), Carrier->GetVisualAttachSocket(HipLeft()) == FName(TEXT("pelvis")));

	Release(Database, { Sword, Bow });
	return true;
}

// ===========================================================================
// From the bag: equip and draw in one request
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathFromInventoryTest,
	"Equipment.Sheath.FromInventoryDraw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathFromInventoryTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("SheathBagSword"), MainHand(), HipLeft());
	const FPrimaryAssetId Bow = RegisterDefinition(Database, TEXT("SheathBagBow"), MainHand(), Back());

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);
	UInventoryComponent* Bag = MakeInventory(Owner, 3);
	const FItemInstance WoodenSword = Database->CreateItemInstance(Sword, 1);
	const FItemInstance IronSword = Database->CreateItemInstance(Sword, 1);
	const FItemInstance BowItem = Database->CreateItemInstance(Bow, 1);
	PlaceItemInSlot(Bag, WoodenSword, 0);
	PlaceItemInSlot(Bag, IronSword, 1);
	PlaceItemInSlot(Bag, BowItem, 2);

	// The hotbar's F: equip into the sheath and draw.
	TestEqual(TEXT("Equip-and-draw of the bow succeeds"), Result(Carrier->TryEquipFromInventory(BowItem.InstanceId, Bag, FGameplayTag(), true)),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The bow is on the back, drawn"), SameItem(Carrier->GetEquippedItem(Back()), BowItem) && Carrier->IsDrawn(Back()));
	TestFalse(TEXT("The bow left the bag"), Bag->GetItemInSlot(2).IsValid());

	// The next weapon drawn the same way: the bow goes back to its own sheath, not the bag.
	TestEqual(TEXT("Equip-and-draw of a sword succeeds"), Result(Carrier->TryEquipFromInventory(WoodenSword.InstanceId, Bag, FGameplayTag(), true)),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The sword is drawn"), SameItem(Carrier->GetHeldItem(MainHand()), WoodenSword));
	TestTrue(TEXT("The bow stays on the back, sheathed"), SameItem(Carrier->GetEquippedItem(Back()), BowItem) && !Carrier->IsDrawn(Back()));

	// A second sword asked into the hand (the panel's Main Hand): its sheath's sword swaps into the bag, the new one drawn.
	TestEqual(TEXT("A second sword into the hand succeeds"), Result(Carrier->TryEquipFromInventory(IronSword.InstanceId, Bag, MainHand())),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The iron sword is in the hip sheath, drawn"), SameItem(Carrier->GetEquippedItem(HipLeft()), IronSword) && Carrier->IsDrawn(HipLeft()));
	TestEqual(TEXT("The wooden sword took the iron one's bag slot"), Bag->FindSlotIndexByInstanceId(WoodenSword.InstanceId), 1);

	// Without the draw, an equip from the bag lands sheathed.
	FItemInstance Out;
	Carrier->TryUnequipToInventory(Back(), Bag);
	TestEqual(TEXT("The bow equips without a draw"), Result(Carrier->TryEquipFromInventory(BowItem.InstanceId, Bag, FGameplayTag())),
		Result(EEquipmentResult::Success));
	TestFalse(TEXT("It lands sheathed"), Carrier->IsDrawn(Back()));
	TestTrue(TEXT("The sword stays drawn"), Carrier->IsDrawn(HipLeft()));

	Release(Database, { Sword, Bow });
	return true;
}

// ===========================================================================
// A client's requests and its view of a draw
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathClientTest,
	"Equipment.Sheath.ClientRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathClientTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("SheathClientSword"), MainHand(), HipLeft());

	AActor* ServerPlayer = Game.SpawnOwner();
	UEquipmentManagerComponent* ServerEquipment = MakeCarrier(ServerPlayer);
	AActor* ClientPlayer = Game.SpawnRemoteOwner();
	UEquipmentManagerComponent* ClientEquipment = MakeCarrier(ClientPlayer);
	AActor* OtherPlayer = Game.SpawnOwner();
	UInventoryComponent* OtherBag = MakeInventory(OtherPlayer, 2);

	const FItemInstance SwordItem = Database->CreateItemInstance(Sword, 1);
	ServerEquipment->TryEquip(SwordItem);
	FEquipmentSlot* ClientHip = PlaceEquipped(ClientEquipment, SwordItem, HipLeft()); // replicated

	// The client's predicate: an empty sheath is refused at home, a carried one is sent (optimistic).
	TestEqual(TEXT("The client's draw of an empty sheath is InvalidItem"), Result(ClientEquipment->TryDraw(Back())), Result(EEquipmentResult::InvalidItem));
	TestEqual(TEXT("The client's draw is Success (optimistic)"), Result(ClientEquipment->TryDraw(HipLeft())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The client's copy waits for replication"), ClientEquipment->IsDrawn(HipLeft()));

	// The server receives it and draws.
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestDraw")).Set(TEXT("SheathSlotTag"), HipLeft()).Send();
	TestTrue(TEXT("On the server the sword is drawn"), ServerEquipment->IsDrawn(HipLeft()));

	// Another player's bag named in the request changes nothing.
	AddExpectedMessage(TEXT("named an inventory it does not own"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestDraw")).Set(TEXT("SheathSlotTag"), Back()).Set(TEXT("Inventory"), OtherBag).Send();

	// The replicated draw reaches the client: OnRep_EquipmentSlots acts on the state it had not seen.
	ClientHip->bDrawn = true;
	UFunction* OnRep = ClientEquipment->FindFunction(TEXT("OnRep_EquipmentSlots"));
	if (TestNotNull(TEXT("OnRep_EquipmentSlots"), OnRep))
	{
		ClientEquipment->ProcessEvent(OnRep, nullptr);
	}
	TestTrue(TEXT("The client acted on the draw"), ClientHip->bAppliedDrawn);
	TestTrue(TEXT("The client's hand holds the sword"), SameItem(ClientEquipment->GetHeldItem(MainHand()), SwordItem));

	// Sheathing: the client's request, then the server's step.
	TestEqual(TEXT("The client's sheathe is Success (optimistic)"), Result(ClientEquipment->TrySheathe(MainHand())), Result(EEquipmentResult::Success));
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestSheathe")).Set(TEXT("SlotTag"), MainHand()).Send();
	TestFalse(TEXT("On the server the sword is sheathed"), ServerEquipment->IsDrawn(HipLeft()));
	ClientHip->bDrawn = false;
	if (OnRep)
	{
		ClientEquipment->ProcessEvent(OnRep, nullptr);
	}
	TestFalse(TEXT("The client acted on the sheathe"), ClientHip->bAppliedDrawn);

	Release(Database, { Sword });
	return true;
}

// ===========================================================================
// The equipment panel's slot: a drawn sheath is framed blue, a grab stays gold over it
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathSlotColourTest,
	"Equipment.Sheath.SlotWidgetColour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathSlotColourTest::RunTest(const FString& Parameters)
{
	const FLinearColor Plain = UEquipmentSlotWidget::BackgroundColorFor(false, false);
	const FLinearColor Drawn = UEquipmentSlotWidget::BackgroundColorFor(false, true);
	const FLinearColor Grabbed = UEquipmentSlotWidget::BackgroundColorFor(true, false);
	TestFalse(TEXT("A drawn sheath stands out from a plain slot"), Drawn.Equals(Plain));
	TestFalse(TEXT("A drawn sheath is not the grab's gold"), Drawn.Equals(Grabbed));
	TestTrue(TEXT("A grab is gold whatever the slot's state"), UEquipmentSlotWidget::BackgroundColorFor(true, true).Equals(Grabbed));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
