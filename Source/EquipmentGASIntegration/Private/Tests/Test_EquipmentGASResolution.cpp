#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "EquipmentGASIntegration.h"
#include "EquipmentGASTestActors.h"
#include "AbilitySystemComponent.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Regression: equipment used FindComponentByClass<UAbilitySystemComponent>() on the
// pawn, which is null when the ASC lives on the player state. The resolver must
// honour IAbilitySystemInterface.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEquipmentGAS_ResolvesThroughInterface,
	"Equipment.GAS.ResolvesAbilitySystemThroughInterface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FEquipmentGAS_ResolvesThroughInterface::RunTest(const FString& Parameters)
{
	AEquipmentGASTestHostActor* Host = NewObject<AEquipmentGASTestHostActor>(GetTransientPackage());
	AEquipmentGASTestAvatarActor* Avatar = NewObject<AEquipmentGASTestAvatarActor>(GetTransientPackage());
	AActor* Plain = NewObject<AActor>(GetTransientPackage());
	Host->AddToRoot();
	Avatar->AddToRoot();
	Plain->AddToRoot();

	Avatar->Host = Host;

	TestNull(TEXT("Avatar carries no ASC component itself"), Avatar->FindComponentByClass<UAbilitySystemComponent>());
	TestEqual(TEXT("Resolves the host's ASC through the interface"),
		FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(Avatar), Host->AbilitySystemComponent.Get());
	TestEqual(TEXT("Host resolves its own ASC"),
		FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(Host), Host->AbilitySystemComponent.Get());
	TestNull(TEXT("Plain actor resolves nothing"), FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(Plain));
	TestNull(TEXT("Null is safe"), FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(nullptr));

	Host->RemoveFromRoot();
	Avatar->RemoveFromRoot();
	Plain->RemoveFromRoot();
	return true;
}

#endif // WITH_AUTOMATION_TESTS
