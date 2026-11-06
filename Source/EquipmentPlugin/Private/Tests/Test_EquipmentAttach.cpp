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

#endif // WITH_AUTOMATION_TESTS
