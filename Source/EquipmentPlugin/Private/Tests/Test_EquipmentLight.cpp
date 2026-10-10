// Copyright Daniel Raquel. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Components/EquipmentManagerComponent.h"
#include "Data/Fragments/ItemFragment_LightSource.h"
#include "Types/CGFItemTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Carried light (feature 7): the fuel -> intensity rule and the guard paths that need no
// item database. The burn timer, the point light and burn-out run in PIE.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipLight_IntensityRule,
	"Equipment.LightSource.IntensityRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEquipLight_IntensityRule::RunTest(const FString& Parameters)
{
	UItemFragment_LightSource* Light = NewObject<UItemFragment_LightSource>();
	Light->Intensity = 1200.f;
	TestEqual(TEXT("Fuel left -> full intensity"), UEquipmentManagerComponent::LitIntensityFor(*Light, true, 42.f), 1200.f, 0.001f);
	TestEqual(TEXT("Fuel gone -> dark"), UEquipmentManagerComponent::LitIntensityFor(*Light, true, 0.f), 0.f, 0.001f);
	TestEqual(TEXT("No gauge -> burns forever"), UEquipmentManagerComponent::LitIntensityFor(*Light, false, 0.f), 1200.f, 0.001f);
	TestEqual(TEXT("Light level = lumens / 1000"), Light->LightLevel(), 1.2f, 0.001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipLight_NothingCarried,
	"Equipment.LightSource.NothingCarried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEquipLight_NothingCarried::RunTest(const FString& Parameters)
{
	UEquipmentManagerComponent* Comp = NewObject<UEquipmentManagerComponent>();
	Comp->AddToRoot();
	FEquipmentSlot Slot;
	Slot.SlotTag = FGameplayTag::RequestGameplayTag(TEXT("Equipment.Slot.OffHand"), false);
	Comp->EquipmentSlots.Add(Slot);

	TestEqual(TEXT("Empty slots carry no light"), Comp->GetCarriedLightLevel(), 0.f, 0.001f);
	FGameplayTag SlotTag;
	float Fuel = 1.f, MaxFuel = 1.f;
	TestFalse(TEXT("No lit slot reported"), Comp->GetCarriedLight(SlotTag, Fuel, MaxFuel));
	TestEqual(TEXT("Fuel cleared"), Fuel, 0.f, 0.001f);
	TestFalse(TEXT("Nothing to burn"), Comp->ConsumeCarriedLightFuel(30.f));

	// An item the database does not know (no fragment) is not a light either.
	FItemInstance Item;
	Item.InstanceId = FGuid::NewGuid();
	Item.ItemDefinitionId = FPrimaryAssetId(TEXT("ItemDefinition"), TEXT("Unknown"));
	Item.StackCount = 1;
	Comp->EquipmentSlots[0].EquippedItem = Item;
	Comp->EquipmentSlots[0].bIsOccupied = true;
	TestEqual(TEXT("Unknown item carries no light"), Comp->GetCarriedLightLevel(), 0.f, 0.001f);

	Comp->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
