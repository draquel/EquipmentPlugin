#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Data/Fragments/ItemFragment_Equipment.h"

#if WITH_AUTOMATION_TESTS

// ===========================================================================
// ChooseAttachSocket: an item with an AttachBoneOverride attaches to that bone (a bow in the main hand on hand_l);
// one without, or no fragment at all, to the slot's socket. An unknown slot has none.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipAttach_BoneOverride,
	"Equipment.Visuals.AttachBoneOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipAttach_BoneOverride::RunTest(const FString& Parameters)
{
	const FName HandR(TEXT("hand_r"));
	const FName HandL(TEXT("hand_l"));

	TestEqual("No fragment: the slot's socket", UEquipmentManagerComponent::ChooseAttachSocket(HandR, nullptr), HandR);

	UItemFragment_Equipment* Sword = NewObject<UItemFragment_Equipment>();
	TestEqual("No override: the slot's socket", UEquipmentManagerComponent::ChooseAttachSocket(HandR, Sword), HandR);

	UItemFragment_Equipment* Bow = NewObject<UItemFragment_Equipment>();
	Bow->AttachBoneOverride = HandL;
	TestEqual("An override: its bone", UEquipmentManagerComponent::ChooseAttachSocket(HandR, Bow), HandL);
	TestEqual("An override with no slot socket: its bone", UEquipmentManagerComponent::ChooseAttachSocket(NAME_None, Bow), HandL);

	UEquipmentManagerComponent* Manager = NewObject<UEquipmentManagerComponent>();
	Manager->AddToRoot();
	FEquipmentSlot Slot;
	Slot.SlotTag = FGameplayTag::RequestGameplayTag(TEXT("Equipment.Slot.MainHand"), false);
	Slot.AttachSocket = HandR;
	Manager->EquipmentSlots.Add(Slot);
	TestEqual("An empty slot: its own socket", Manager->GetVisualAttachSocket(Slot.SlotTag), HandR);
	TestEqual("An empty slot: no item bone", Manager->GetItemAttachBone(Slot.SlotTag), FName());
	TestEqual("An unknown slot: none", Manager->GetVisualAttachSocket(FGameplayTag()), FName());
	Manager->RemoveFromRoot();
	return true;
}

// ===========================================================================
// IsVisualStale (G153): a client's view of a hotbar swap is one replication of an occupied slot holding another item.
// The slot's visual is stale when it was built for another item id; an empty slot, a slot with no visual, a visual not yet
// bound to an item, or the same item are not.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipAttach_VisualStale,
	"Equipment.Visuals.StaleAfterSwap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEquipAttach_VisualStale::RunTest(const FString& Parameters)
{
	UStaticMeshComponent* Visual = NewObject<UStaticMeshComponent>();
	Visual->AddToRoot();

	FEquipmentSlot Slot;
	Slot.SlotTag = FGameplayTag::RequestGameplayTag(TEXT("Equipment.Slot.MainHand"), false);
	TestFalse("An empty slot is not stale", UEquipmentManagerComponent::IsVisualStale(Slot));

	Slot.bIsOccupied = true;
	Slot.EquippedItem.InstanceId = FGuid::NewGuid();
	TestFalse("An occupied slot with no visual is not stale", UEquipmentManagerComponent::IsVisualStale(Slot));

	Slot.AttachedVisualComponent = Visual;
	TestFalse("A visual not yet bound to an item is not stale", UEquipmentManagerComponent::IsVisualStale(Slot));

	Slot.AttachedVisualItemId = Slot.EquippedItem.InstanceId;
	TestFalse("The same item is not stale", UEquipmentManagerComponent::IsVisualStale(Slot));

	Slot.EquippedItem.InstanceId = FGuid::NewGuid(); // the swap: another item replicated into the occupied slot
	TestTrue("Another item in the slot: stale", UEquipmentManagerComponent::IsVisualStale(Slot));

	Slot.bIsOccupied = false;
	TestFalse("Emptied: not stale (it is removed instead)", UEquipmentManagerComponent::IsVisualStale(Slot));

	Visual->RemoveFromRoot();
	return true;
}

#endif // WITH_AUTOMATION_TESTS
