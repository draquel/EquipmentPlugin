#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Types/CGFItemTypes.h"
#include "Types/CGFCommonEnums.h"
#include "Engine/StreamableManager.h"
#include "EquipmentSystemTypes.generated.h"

class UPointLightComponent;
class UAnimInstance;

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

	/** Visual offset relative to the socket (from FEquipmentSlotDefinition::AttachOffset). */
	UPROPERTY()
	FTransform AttachOffset = FTransform::Identity;

	/** Tags this slot accepts (copied from slot definition) */
	UPROPERTY()
	FGameplayTagContainer AcceptedItemTags;

	/** Set on a sheath: the hand slot its item is drawn into (copied from FEquipmentSlotDefinition::DrawSlotTag, D77). */
	UPROPERTY(BlueprintReadOnly)
	FGameplayTag DrawSlotTag;

	/**
	 * A sheath's item is in its hand (D77): its visual rides the hand with the item's hand fit, and its abilities, effects
	 * and stance layer apply. Always false on a slot that is no sheath. Changed on the authority (TryDraw, TrySheathe).
	 */
	UPROPERTY(BlueprintReadOnly)
	bool bDrawn = false;

	/** Whether this slot's item is where it is used: an ordinary slot's always, a sheath's while drawn. */
	bool IsInHand() const { return !DrawSlotTag.IsValid() || bDrawn; }

	// -----------------------------------------------------------------------
	// Non-replicated visual state (rebuilt locally from replicated item data)
	// -----------------------------------------------------------------------

	/** Attached visual mesh component (static or skeletal) */
	UPROPERTY(NotReplicated)
	TObjectPtr<USceneComponent> AttachedVisualComponent;

	/**
	 * The item instance the attached visual was built for. A client sees a hotbar swap as one replication of an occupied
	 * slot holding another item (the server unequipped and equipped in one frame): the visual is rebuilt when this differs
	 * from EquippedItem's id (G153).
	 */
	UPROPERTY(NotReplicated)
	FGuid AttachedVisualItemId;

	/** The anim layer linked for the attached visual's item, unlinked when the visual goes (the fragment of a swapped-in item names another). */
	UPROPERTY(NotReplicated)
	TSubclassOf<UAnimInstance> AttachedAnimLayerClass;

	/**
	 * The drawn state this machine last acted on (seated the visual, linked the layer, announced OnDrawnChanged): a client
	 * compares it with the replicated bDrawn to see a draw or a sheathe of the same item.
	 */
	UPROPERTY(NotReplicated)
	bool bAppliedDrawn = false;

	/** Async mesh load handle */
	TSharedPtr<FStreamableHandle> MeshLoadHandle;

	/** Carried light (feature 7): present while a LightSource item is equipped; intensity follows the fuel. */
	UPROPERTY(NotReplicated)
	TObjectPtr<UPointLightComponent> AttachedLightComponent;

	/** Seconds of burn not yet deducted from the fuel (authority; see UItemFragment_LightSource::FuelStepSeconds). */
	float FuelAccumulatorSeconds = 0.0f;
};
