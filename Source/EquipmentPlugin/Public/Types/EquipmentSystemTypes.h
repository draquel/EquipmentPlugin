#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Types/CGFItemTypes.h"
#include "Types/CGFCommonEnums.h"
#include "Engine/StreamableManager.h"
#include "EquipmentSystemTypes.generated.h"

/**
 * Runtime equipment slot — holds the currently equipped item and visual state.
 * Created from FEquipmentSlotDefinition during BeginPlay.
 */
USTRUCT(BlueprintType)
struct EQUIPMENTPLUGIN_API FEquipmentSlot
{
	GENERATED_BODY()

	/** Tag identifying this slot (matches FEquipmentSlotDefinition::SlotTag) */
	UPROPERTY(BlueprintReadOnly)
	FGameplayTag SlotTag;

	/** The item currently equipped in this slot */
	UPROPERTY(BlueprintReadOnly)
	FItemInstance EquippedItem;

	/** Whether an item is currently equipped */
	UPROPERTY(BlueprintReadOnly)
	bool bIsOccupied = false;

	/** Socket name for visual attachment (copied from slot definition) */
	UPROPERTY()
	FName AttachSocket;

	/** Tags this slot accepts (copied from slot definition) */
	UPROPERTY()
	FGameplayTagContainer AcceptedItemTags;

	// -----------------------------------------------------------------------
	// Non-replicated visual state (rebuilt locally from replicated item data)
	// -----------------------------------------------------------------------

	/** Attached visual mesh component (static or skeletal) */
	UPROPERTY(NotReplicated)
	TObjectPtr<USceneComponent> AttachedVisualComponent;

	/** Async mesh load handle */
	TSharedPtr<FStreamableHandle> MeshLoadHandle;
};
