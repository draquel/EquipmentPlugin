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
// back, one item per hand, its GAS only while drawn, its visual's bone and fit by the state, the client's requests; one
// item per place on the body and the shield beside a one-handed weapon (the sheath loadout plan, D95, D96). As
// the other equipment tests: a standalone game instance holds the definitions, the slots are built directly (play has
// not begun), a client is an AutonomousProxy actor whose RPCs reach the server's component through CGFTesting::FRPCCall.
// ---------------------------------------------------------------------------
namespace EquipmentSheathTestHelpers
{
	FGameplayTag MainHand() { return CGFGameplayTags::Equipment_Slot_MainHand; }
	FGameplayTag Head() { return CGFGameplayTags::Equipment_Slot_Head; }
	FGameplayTag HipLeft() { return CGFGameplayTags::Equipment_Slot_Sheath_HipLeft; }
	FGameplayTag Back() { return CGFGameplayTags::Equipment_Slot_Sheath_Back; }
	FGameplayTag OffHand() { return CGFGameplayTags::Equipment_Slot_OffHand; }

	/** A shield's fragment: it follows the main hand (D96). */
	void MakeShield(UItemFragment_Equipment& Fragment)
	{
		Fragment.DrawWithSlotTag = MainHand();
	}

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

	/** A slot as BeginPlay would build it from its definition, which the owner keeps (a sheath's own hand). */
	void AddSlot(UEquipmentManagerComponent* Comp, FGameplayTag Tag, FGameplayTag DrawInto = FGameplayTag(), FName Socket = NAME_None,
		const FTransform& Correction = FTransform::Identity)
	{
		FEquipmentSlotDefinition Def;
		Def.SlotTag = Tag;
		Def.DrawSlotTag = DrawInto;
		Def.AttachSocket = Socket;
		Def.AttachOffset = Correction;
		Comp->AvailableSlots.Add(Def);
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

	/** The hand a slot draws into now (its replicated DrawSlotTag). */
	FGameplayTag DrawHandOf(const UEquipmentManagerComponent* Comp, FGameplayTag SlotTag)
	{
		const FEquipmentSlot* Slot = Comp->EquipmentSlots.FindByPredicate([&SlotTag](const FEquipmentSlot& S) { return S.SlotTag == SlotTag; });
		return Slot ? Slot->DrawSlotTag : FGameplayTag();
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

// ===========================================================================
// One item per place on the body (the sheath loadout plan, D95): the back holds a bow or a shield
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathOnePerPlaceTest,
	"Equipment.Sheath.OnePerPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathOnePerPlaceTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FPrimaryAssetId Bow = RegisterDefinition(Database, TEXT("OnePerPlaceBow"), MainHand(), Back(),
		[](UItemFragment_Equipment& Fragment) { Fragment.BlockedSlotTags.AddTag(OffHand()); });
	const FPrimaryAssetId Shield = RegisterDefinition(Database, TEXT("OnePerPlaceShield"), OffHand(), Back(), MakeShield);

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);
	AddSlot(Carrier, OffHand(), FGameplayTag(), TEXT("hand_l"));

	// The shield goes onto the back and draws into the off hand; with no weapon in the main hand it stays on the back.
	const FItemInstance ShieldItem = Database->CreateItemInstance(Shield, 1);
	TestEqual(TEXT("The shield equips"), Result(Carrier->TryEquip(ShieldItem)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The shield is on the back"), SameItem(Carrier->GetEquippedItem(Back()), ShieldItem));
	TestTrue(TEXT("The back draws the shield into the off hand"), DrawHandOf(Carrier, Back()) == OffHand());
	TestFalse(TEXT("With nothing in the main hand the shield stays on the back"), Carrier->IsDrawn(Back()));

	// The back holds one item: with no bag to take the shield, the bow is refused and nothing is dropped.
	const FItemInstance BowItem = Database->CreateItemInstance(Bow, 1);
	TestEqual(TEXT("The bow beside the shield is refused"), Result(Carrier->TryEquip(BowItem)), Result(EEquipmentResult::SlotOccupied));
	TestTrue(TEXT("The shield stays on the back"), SameItem(Carrier->GetEquippedItem(Back()), ShieldItem));

	// From a bag the equip swaps, even a full one: the shield takes the bow's bag slot.
	UInventoryComponent* Bag = MakeInventory(Owner, 1);
	PlaceItemInSlot(Bag, BowItem, 0);
	TestEqual(TEXT("The bow equips from the full bag"), Result(Carrier->TryEquipFromInventory(BowItem.InstanceId, Bag, FGameplayTag())),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The bow is on the back"), SameItem(Carrier->GetEquippedItem(Back()), BowItem));
	TestTrue(TEXT("The back draws the bow into the main hand"), DrawHandOf(Carrier, Back()) == MainHand());
	TestTrue(TEXT("The shield is in the bag"), SameItem(Bag->GetItemInSlot(0), ShieldItem));

	// A shield asked into the off hand (the panel, an old save) goes onto the back the same way, the bow to the bag.
	TestEqual(TEXT("The shield equips from the bag into the off hand"), Result(Carrier->TryEquipFromInventory(ShieldItem.InstanceId, Bag, OffHand(), true)),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The shield is on the back again"), SameItem(Carrier->GetEquippedItem(Back()), ShieldItem));
	TestFalse(TEXT("The off hand slot itself stays empty"), Carrier->IsSlotOccupied(OffHand()));
	TestFalse(TEXT("The asked draw waits for a weapon in the main hand"), Carrier->IsDrawn(Back()));
	TestTrue(TEXT("The bow is in the bag"), SameItem(Bag->GetItemInSlot(0), BowItem));

	// Emptied, the back draws into its own hand again.
	FItemInstance Out;
	TestEqual(TEXT("The shield unequips"), Result(Carrier->TryUnequip(Back(), Out)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The empty back draws into the main hand"), DrawHandOf(Carrier, Back()) == MainHand());

	// The client predicts through the same predicate: the swap is sent, the server makes it.
	AActor* ServerPlayer = Game.SpawnOwner();
	UEquipmentManagerComponent* ServerEquipment = MakeCarrier(ServerPlayer);
	AddSlot(ServerEquipment, OffHand(), FGameplayTag(), TEXT("hand_l"));
	UInventoryComponent* ServerBag = MakeInventory(ServerPlayer, 1);
	AActor* ClientPlayer = Game.SpawnRemoteOwner();
	UEquipmentManagerComponent* ClientEquipment = MakeCarrier(ClientPlayer);
	AddSlot(ClientEquipment, OffHand(), FGameplayTag(), TEXT("hand_l"));
	UInventoryComponent* ClientBag = MakeInventory(ClientPlayer, 1);

	const FItemInstance ServerShield = Database->CreateItemInstance(Shield, 1);
	const FItemInstance ServerBow = Database->CreateItemInstance(Bow, 1);
	ServerEquipment->TryEquip(ServerShield);
	PlaceItemInSlot(ServerBag, ServerBow, 0);
	PlaceEquipped(ClientEquipment, ServerShield, Back())->DrawSlotTag = OffHand(); // replicated
	PlaceItemInSlot(ClientBag, ServerBow, 0);

	TestEqual(TEXT("The client's equip of the bow is Success (optimistic: a swap)"),
		Result(ClientEquipment->TryEquipFromInventory(ServerBow.InstanceId, ClientBag, FGameplayTag())), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The client's copy waits for replication"), SameItem(ClientEquipment->GetEquippedItem(Back()), ServerShield));
	CGFTesting::FRPCCall(ServerEquipment, TEXT("ServerRPC_RequestEquipFromInventory"))
		.Set(TEXT("ItemInstanceId"), ServerBow.InstanceId).Set(TEXT("SourceInventory"), ServerBag).Send();
	TestTrue(TEXT("On the server the bow is on the back"), SameItem(ServerEquipment->GetEquippedItem(Back()), ServerBow));
	TestTrue(TEXT("On the server the shield is in the bag"), SameItem(ServerBag->GetItemInSlot(0), ServerShield));

	Release(Database, { Bow, Shield });
	return true;
}

// ===========================================================================
// The shield follows the one-handed weapon (D96): drawn with it, sheathed with it
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathDrawsWithLeadTest,
	"Equipment.Sheath.DrawsWithLead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathDrawsWithLeadTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FGameplayTag Belt = CGFGameplayTags::Equipment_Slot_Sheath_Belt;
	const FPrimaryAssetId Sword = RegisterDefinition(Database, TEXT("DrawsWithLeadSword"), MainHand(), HipLeft());
	const FPrimaryAssetId Pickaxe = RegisterDefinition(Database, TEXT("DrawsWithLeadPickaxe"), MainHand(), Belt,
		[](UItemFragment_Equipment& Fragment) { Fragment.BlockedSlotTags.AddTag(OffHand()); });
	const FPrimaryAssetId Shield = RegisterDefinition(Database, TEXT("DrawsWithLeadShield"), OffHand(), Back(), MakeShield);
	const FPrimaryAssetId Torch = RegisterDefinition(Database, TEXT("DrawsWithLeadTorch"), OffHand(), FGameplayTag());

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);
	AddSlot(Carrier, OffHand(), FGameplayTag(), TEXT("hand_l"));
	AddSlot(Carrier, Belt, MainHand(), TEXT("pelvis"));
	UInventoryComponent* Bag = MakeInventory(Owner, 4);
	// The GAS module's callbacks, counted per slot: the shield's Defense follows the hand.
	TMap<FGameplayTag, int32> Granted;
	Carrier->OnGASEquipCallback = [&Granted](const FItemInstance&, FGameplayTag SlotTag) { ++Granted.FindOrAdd(SlotTag); };
	Carrier->OnGASUnequipCallback = [&Granted](FGameplayTag SlotTag) { --Granted.FindOrAdd(SlotTag); };

	const FItemInstance ShieldItem = Database->CreateItemInstance(Shield, 1);
	Carrier->TryEquip(Database->CreateItemInstance(Sword, 1));
	Carrier->TryEquip(Database->CreateItemInstance(Pickaxe, 1));
	Carrier->TryEquip(ShieldItem);
	TestTrue(TEXT("The shield follows the main hand"), Carrier->GetDrawLeadSlotTag(Back()) == MainHand());
	TestFalse(TEXT("The sword follows nothing"), Carrier->GetDrawLeadSlotTag(HipLeft()).IsValid());
	TestFalse(TEXT("Unarmed, the shield is on the back"), Carrier->IsDrawn(Back()));
	TestEqual(TEXT("On the back it grants nothing"), Granted.FindRef(Back()), 0);

	// Its state is its lead's: neither drawn nor sheathed on its own, at home or on the server.
	TestEqual(TEXT("The shield is not drawn on its own"), Result(Carrier->TryDraw(Back(), Bag)), Result(EEquipmentResult::IncompatibleSlot));
	TestFalse(TEXT("The refused draw changed nothing"), Carrier->IsDrawn(Back()));

	// The sword drawn: the shield comes onto the arm; sheathed, both go back.
	TestEqual(TEXT("The sword draws"), Result(Carrier->TryDraw(HipLeft(), Bag)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("With the sword the shield is drawn"), Carrier->IsDrawn(Back()));
	TestTrue(TEXT("The off hand holds the shield"), SameItem(Carrier->GetHeldItem(OffHand()), ShieldItem));
	TestEqual(TEXT("In the hand it grants its Defense"), Granted.FindRef(Back()), 1);
	TestEqual(TEXT("The shield is not sheathed on its own"), Result(Carrier->TrySheathe(OffHand())), Result(EEquipmentResult::Failed));
	TestTrue(TEXT("The refused sheathe changed nothing"), Carrier->IsDrawn(Back()));
	TestEqual(TEXT("The sword sheathes"), Result(Carrier->TrySheathe(MainHand())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("With the sword the shield goes onto the back"), Carrier->IsDrawn(Back()));
	TestEqual(TEXT("Back on the back its Defense is revoked"), Granted.FindRef(Back()), 0);

	// A two-handed tool needs the off hand: the shield stays on the back while the pickaxe is out.
	Carrier->TryDraw(HipLeft(), Bag);
	TestEqual(TEXT("The pickaxe draws"), Result(Carrier->TryDraw(Belt, Bag)), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The pickaxe sent the sword to the hip"), Carrier->IsDrawn(HipLeft()));
	TestFalse(TEXT("With the pickaxe the shield is on the back"), Carrier->IsDrawn(Back()));
	TestTrue(TEXT("The pickaxe blocks the off hand"), Carrier->IsSlotBlocked(OffHand()));
	TestEqual(TEXT("A torch cannot go into the off hand beside the pickaxe"),
		Result(Carrier->TryEquipToSlot(Database->CreateItemInstance(Torch, 1), OffHand())), Result(EEquipmentResult::HandBlocked));
	TestEqual(TEXT("Back to the sword"), Result(Carrier->TryDraw(HipLeft(), Bag)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("With the sword again the shield is drawn"), Carrier->IsDrawn(Back()));

	// A torch in the off hand slot itself wins over the shield; the shield comes back when the torch goes.
	const FItemInstance TorchItem = Database->CreateItemInstance(Torch, 1);
	TestEqual(TEXT("The torch equips into the off hand"), Result(Carrier->TryEquipToSlot(TorchItem, OffHand())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("With the torch the shield is on the back"), Carrier->IsDrawn(Back()));
	TestTrue(TEXT("The off hand holds the torch"), SameItem(Carrier->GetHeldItem(OffHand()), TorchItem));
	TestEqual(TEXT("The torch goes to the bag"), Result(Carrier->TryUnequipToInventory(OffHand(), Bag)), Result(EEquipmentResult::Success));
	TestTrue(TEXT("Without the torch the shield is drawn again"), Carrier->IsDrawn(Back()));

	// The sword leaving the hand (unequipped while drawn) takes the shield off the arm; the shield stays on the back.
	FItemInstance Out;
	TestEqual(TEXT("The drawn sword unequips"), Result(Carrier->TryUnequip(HipLeft(), Out)), Result(EEquipmentResult::Success));
	TestFalse(TEXT("Without the sword the shield is on the back"), Carrier->IsDrawn(Back()));
	TestTrue(TEXT("The shield is still equipped"), SameItem(Carrier->GetEquippedItem(Back()), ShieldItem));
	TestEqual(TEXT("Its Defense went with the hand"), Granted.FindRef(Back()), 0);

	// An equip of the shield while the sword is drawn puts it on the arm at once.
	Carrier->TryUnequip(Back(), Out);
	Carrier->TryEquip(Database->CreateItemInstance(Sword, 1));
	Carrier->TryDraw(HipLeft(), Bag);
	Carrier->TryEquip(ShieldItem);
	TestTrue(TEXT("A shield equipped beside the drawn sword is drawn"), Carrier->IsDrawn(Back()));

	// The client's predicate refuses the shield's own draw and sheathe too (no request is sent).
	AActor* ClientPlayer = Game.SpawnRemoteOwner();
	UEquipmentManagerComponent* ClientEquipment = MakeCarrier(ClientPlayer);
	AddSlot(ClientEquipment, OffHand(), FGameplayTag(), TEXT("hand_l"));
	PlaceEquipped(ClientEquipment, ShieldItem, Back())->DrawSlotTag = OffHand(); // replicated
	TestEqual(TEXT("The client's draw of the shield is refused at home"), Result(ClientEquipment->TryDraw(Back())), Result(EEquipmentResult::IncompatibleSlot));
	PlaceEquipped(ClientEquipment, ShieldItem, Back(), true);
	TestEqual(TEXT("The client's sheathe of the shield is refused at home"), Result(ClientEquipment->TrySheathe(OffHand())), Result(EEquipmentResult::Failed));

	Carrier->OnGASEquipCallback = nullptr;
	Carrier->OnGASUnequipCallback = nullptr;
	Release(Database, { Sword, Pickaxe, Shield, Torch });
	return true;
}

// ===========================================================================
// A shared place (D97): the quiver shares the back with a bow only
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentSheathSharedPlaceTest,
	"Equipment.Sheath.SharedPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipmentSheathSharedPlaceTest::RunTest(const FString& Parameters)
{
	using namespace EquipmentSheathTestHelpers;
	CGFTesting::FTestGameInstance Game;
	UItemDatabaseSubsystem* Database = Game.GetSubsystem<UItemDatabaseSubsystem>();
	if (!TestNotNull(TEXT("The item database"), Database))
	{
		return true;
	}
	const FGameplayTag QuiverSlot = CGFGameplayTags::Equipment_Slot_Quiver;
	const FPrimaryAssetId Bow = RegisterDefinition(Database, TEXT("SharedPlaceBow"), MainHand(), Back(), [](UItemFragment_Equipment& Fragment)
	{
		CastChecked<UItemDefinition>(Fragment.GetOuter())->ItemTags.AddTag(CGFGameplayTags::Item_Type_Bow);
		Fragment.BlockedSlotTags.AddTag(OffHand());
	});
	const FPrimaryAssetId Shield = RegisterDefinition(Database, TEXT("SharedPlaceShield"), OffHand(), Back(), MakeShield);
	const FPrimaryAssetId Quiver = RegisterDefinition(Database, TEXT("SharedPlaceQuiver"), QuiverSlot, FGameplayTag(), [](UItemFragment_Equipment& Fragment)
	{
		Fragment.SharesPlaceWithSlotTag = CGFGameplayTags::Equipment_Slot_Sheath_Back;
		Fragment.SharesPlaceWithItemTags.AddTag(CGFGameplayTags::Item_Type_Bow);
	});

	AActor* Owner = Game.SpawnOwner();
	UEquipmentManagerComponent* Carrier = MakeCarrier(Owner);
	AddSlot(Carrier, OffHand(), FGameplayTag(), TEXT("hand_l"));
	AddSlot(Carrier, QuiverSlot, FGameplayTag(), TEXT("spine_05"));
	UInventoryComponent* Bag = MakeInventory(Owner, 4);
	const FItemInstance BowItem = Database->CreateItemInstance(Bow, 1);
	const FItemInstance ShieldItem = Database->CreateItemInstance(Shield, 1);
	const FItemInstance QuiverItem = Database->CreateItemInstance(Quiver, 1);
	PlaceItemInSlot(Bag, BowItem, 0);
	PlaceItemInSlot(Bag, ShieldItem, 1);
	PlaceItemInSlot(Bag, QuiverItem, 2);

	// The bow and the quiver share the back.
	TestEqual(TEXT("The bow equips"), Result(Carrier->TryEquipFromInventory(BowItem.InstanceId, Bag, FGameplayTag())), Result(EEquipmentResult::Success));
	TestEqual(TEXT("The quiver equips beside the bow"), Result(Carrier->TryEquipFromInventory(QuiverItem.InstanceId, Bag, FGameplayTag())),
		Result(EEquipmentResult::Success));
	TestTrue(TEXT("The quiver is worn"), SameItem(Carrier->GetEquippedItem(QuiverSlot), QuiverItem));
	TestTrue(TEXT("Nothing conflicts with the bow"), Carrier->GetPlaceConflicts(BowItem, Back()).IsEmpty());

	// A shield onto the back: the bow swaps to the bag and the quiver goes there too.
	TestEqual(TEXT("A shield beside the quiver is named a conflict"), Carrier->GetPlaceConflicts(ShieldItem, Back()).Num(), 1);
	TestEqual(TEXT("The shield equips"), Result(Carrier->TryEquipFromInventory(ShieldItem.InstanceId, Bag, FGameplayTag())), Result(EEquipmentResult::Success));
	TestTrue(TEXT("The shield is on the back"), SameItem(Carrier->GetEquippedItem(Back()), ShieldItem));
	TestFalse(TEXT("The quiver left"), Carrier->IsSlotOccupied(QuiverSlot));
	TestTrue(TEXT("The quiver is in the bag"), Bag->FindSlotIndexByInstanceId(QuiverItem.InstanceId) != INDEX_NONE);
	TestTrue(TEXT("The bow is in the bag"), Bag->FindSlotIndexByInstanceId(BowItem.InstanceId) != INDEX_NONE);

	// The quiver with the shield on the back: the shield goes to the bag; the back stays empty.
	TestEqual(TEXT("The quiver equips again"), Result(Carrier->TryEquipFromInventory(QuiverItem.InstanceId, Bag, FGameplayTag())), Result(EEquipmentResult::Success));
	TestFalse(TEXT("The shield left the back"), Carrier->IsSlotOccupied(Back()));
	TestTrue(TEXT("The shield is in the bag"), Bag->FindSlotIndexByInstanceId(ShieldItem.InstanceId) != INDEX_NONE);
	TestTrue(TEXT("The quiver is worn on an empty back"), Carrier->IsSlotOccupied(QuiverSlot));

	// With no bag to take the quiver a shield is refused, nothing moved.
	TestEqual(TEXT("A direct shield beside the quiver is refused"), Result(Carrier->TryEquip(Database->CreateItemInstance(Shield, 1))),
		Result(EEquipmentResult::SlotOccupied));
	TestTrue(TEXT("The quiver stays"), Carrier->IsSlotOccupied(QuiverSlot));

	// A full bag cannot take the quiver beside the swap: refused before anything moves, at home and on the server.
	UInventoryComponent* FullBag = MakeInventory(Owner, 1);
	const FItemInstance OtherShield = Database->CreateItemInstance(Shield, 1);
	PlaceItemInSlot(FullBag, OtherShield, 0);
	TestEqual(TEXT("A shield from a full bag beside the quiver is refused"),
		Result(Carrier->TryEquipFromInventory(OtherShield.InstanceId, FullBag, FGameplayTag())), Result(EEquipmentResult::NoInventorySpace));
	TestTrue(TEXT("The quiver stays worn"), SameItem(Carrier->GetEquippedItem(QuiverSlot), QuiverItem));
	TestTrue(TEXT("The shield stays in its bag"), SameItem(FullBag->GetItemInSlot(0), OtherShield));
	AActor* ClientPlayer = Game.SpawnRemoteOwner();
	UEquipmentManagerComponent* ClientEquipment = MakeCarrier(ClientPlayer);
	AddSlot(ClientEquipment, OffHand(), FGameplayTag(), TEXT("hand_l"));
	AddSlot(ClientEquipment, QuiverSlot, FGameplayTag(), TEXT("spine_05"));
	PlaceEquipped(ClientEquipment, QuiverItem, QuiverSlot); // replicated
	UInventoryComponent* ClientBag = MakeInventory(ClientPlayer, 1);
	PlaceItemInSlot(ClientBag, OtherShield, 0);
	TestEqual(TEXT("The client's check refuses it too"), Result(ClientEquipment->TryEquipFromInventory(OtherShield.InstanceId, ClientBag, FGameplayTag())),
		Result(EEquipmentResult::NoInventorySpace));

	Release(Database, { Bow, Shield, Quiver });
	return true;
}

#endif // WITH_AUTOMATION_TESTS
