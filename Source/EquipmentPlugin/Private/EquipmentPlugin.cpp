#include "EquipmentPlugin.h"

#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameplayTagsManager.h"

#include "Components/EquipmentManagerComponent.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Data/ItemDefinition.h"
#include "Types/CGFItemTypes.h"
#include "Types/CGFEquipmentTypes.h"

#define LOCTEXT_NAMESPACE "FEquipmentPluginModule"

namespace
{
	UEquipmentManagerComponent* FindPlayerEquipment(UWorld* World)
	{
		if (!World) return nullptr;
		APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0);
		if (!Pawn) return nullptr;
		return Pawn->FindComponentByClass<UEquipmentManagerComponent>();
	}
}

void FEquipmentPluginModule::StartupModule()
{
	// Equipment.Equip <DefName> <SlotTag>
	ConsoleCommands.Add(MakeUnique<FAutoConsoleCommandWithWorldAndArgs>(
		TEXT("Equipment.Equip"),
		TEXT("Equip an item to a slot. Usage: Equipment.Equip <DefName> <SlotTag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (Args.Num() < 2)
				{
					UE_LOG(LogTemp, Warning, TEXT("Equipment.Equip: Usage: Equipment.Equip <DefName> <SlotTag>"));
					return;
				}

				UEquipmentManagerComponent* Equipment = FindPlayerEquipment(World);
				if (!Equipment)
				{
					UE_LOG(LogTemp, Warning, TEXT("Equipment.Equip: No player equipment manager found."));
					return;
				}

				UGameInstance* GI = World->GetGameInstance();
				if (!GI) return;
				UItemDatabaseSubsystem* DB = GI->GetSubsystem<UItemDatabaseSubsystem>();
				if (!DB)
				{
					UE_LOG(LogTemp, Warning, TEXT("Equipment.Equip: ItemDatabaseSubsystem not available."));
					return;
				}

				const FString& DefName = Args[0];
				const FString& SlotTagStr = Args[1];

				FPrimaryAssetId AssetId(FPrimaryAssetType("ItemDefinition"), FName(*DefName));
				FItemInstance Item = DB->CreateItemInstance(AssetId, 1);
				if (!Item.IsValid())
				{
					UE_LOG(LogTemp, Warning, TEXT("Equipment.Equip: Failed to create item '%s'. Definition not found."), *DefName);
					return;
				}

				FGameplayTag SlotTag = FGameplayTag::RequestGameplayTag(FName(*SlotTagStr), false);
				if (!SlotTag.IsValid())
				{
					UE_LOG(LogTemp, Warning, TEXT("Equipment.Equip: Invalid slot tag '%s'."), *SlotTagStr);
					return;
				}

				EEquipmentResult Result = Equipment->TryEquipToSlot(Item, SlotTag);
				UE_LOG(LogTemp, Log, TEXT("Equipment.Equip: '%s' -> slot '%s' = %s"),
					*DefName, *SlotTagStr,
					Result == EEquipmentResult::Success ? TEXT("Success") : TEXT("Failed"));
			})
	));

	// Equipment.UnequipAll
	ConsoleCommands.Add(MakeUnique<FAutoConsoleCommandWithWorldAndArgs>(
		TEXT("Equipment.UnequipAll"),
		TEXT("Unequip all items from the player's equipment slots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
			{
				UEquipmentManagerComponent* Equipment = FindPlayerEquipment(World);
				if (!Equipment)
				{
					UE_LOG(LogTemp, Warning, TEXT("Equipment.UnequipAll: No player equipment manager found."));
					return;
				}

				TArray<FGameplayTag> OccupiedSlots = Equipment->GetOccupiedSlotTags();
				int32 Unequipped = 0;
				for (const FGameplayTag& SlotTag : OccupiedSlots)
				{
					FItemInstance OutItem;
					EEquipmentResult Result = Equipment->TryUnequip(SlotTag, OutItem);
					if (Result == EEquipmentResult::Success)
					{
						Unequipped++;
					}
				}

				UE_LOG(LogTemp, Log, TEXT("Equipment.UnequipAll: Unequipped %d item(s)."), Unequipped);
			})
	));
}

void FEquipmentPluginModule::ShutdownModule()
{
	ConsoleCommands.Empty();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FEquipmentPluginModule, EquipmentPlugin)
