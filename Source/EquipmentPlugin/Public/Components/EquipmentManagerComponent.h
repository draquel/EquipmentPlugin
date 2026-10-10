#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "Types/CGFCommonEnums.h"
#include "Types/CGFEquipmentTypes.h"
#include "Types/CGFItemTypes.h"
#include "Types/EquipmentSystemTypes.h"
#include "EquipmentManagerComponent.generated.h"

class UInventoryComponent;
class UItemDatabaseSubsystem;
class UItemFragment_Equipment;
class UItemFragment_LightSource;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEquipmentOperationFailed, EEquipmentResult, Result);
/** An equipped item's durability changed (server on write, clients on replication). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnEquipmentDurabilityChanged, FGameplayTag, SlotTag, float, Current, float, Max);
/** An equipped item hit zero durability with DestroyAtZero and was removed. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEquippedItemBroken, FGameplayTag, SlotTag, const FItemInstance&, Item);
/**
 * The carried light changed (feature 7): a light item was equipped / unequipped / burnt out, or its fuel moved.
 * Fires on every machine. bLit = that slot's light source has fuel; FuelSeconds / MaxFuel = the item's
 * durability (both 0 when the light needs no fuel).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCarriedLightChanged, FGameplayTag, SlotTag, bool, bLit, float, FuelSeconds, float, MaxFuel);

/**
 * Manages equipment slots on a character. Handles equip/unequip flow,
 * visual attachment, inventory integration, and multiplayer replication.
 * GAS integration is handled by the optional EquipmentGASIntegration module (Phase 9).
 */
UCLASS(BlueprintType, ClassGroup = "Equipment", meta = (BlueprintSpawnableComponent))
class EQUIPMENTPLUGIN_API UEquipmentManagerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEquipmentManagerComponent();

	// -----------------------------------------------------------------------
	// Configuration
	// -----------------------------------------------------------------------

	/** Slot definitions — configure in editor to define available equipment slots */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Equipment|Config")
	TArray<FEquipmentSlotDefinition> AvailableSlots;

	// -----------------------------------------------------------------------
	// State
	// -----------------------------------------------------------------------

	/** Runtime equipment slots (replicated) */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_EquipmentSlots, Category = "Equipment|State")
	TArray<FEquipmentSlot> EquipmentSlots;

	// -----------------------------------------------------------------------
	// Direct Equip/Unequip (no inventory)
	// -----------------------------------------------------------------------

	/** Equip an item to its preferred slot (reads EquipmentSlotTag from fragment) */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryEquip(const FItemInstance& Item);

	/** Equip an item to a specific slot */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryEquipToSlot(const FItemInstance& Item, FGameplayTag SlotTag);

	/** Unequip the item in a slot (returns the item) */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryUnequip(FGameplayTag SlotTag, FItemInstance& OutItem);

	// -----------------------------------------------------------------------
	// Inventory-Integrated Equip/Unequip
	// -----------------------------------------------------------------------

	/** Equip from inventory — removes item from inventory, equips it */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryEquipFromInventory(const FGuid& ItemInstanceId,
		UInventoryComponent* SourceInventory, FGameplayTag SlotTag);

	/** Unequip to inventory — unequips item, adds it to inventory */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryUnequipToInventory(FGameplayTag SlotTag, UInventoryComponent* TargetInventory);

	// -----------------------------------------------------------------------
	// Queries
	// -----------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
	FItemInstance GetEquippedItem(FGameplayTag SlotTag) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
	bool IsSlotOccupied(FGameplayTag SlotTag) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
	TArray<FGameplayTag> GetOccupiedSlotTags() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
	TArray<FGameplayTag> GetEmptySlotTags() const;

	/** Check if an item can be equipped (validation only, no side effects) */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
	bool CanEquipItem(const FItemInstance& Item) const;

	// -----------------------------------------------------------------------
	// GAS Integration (set by EquipmentGASIntegration module)
	// -----------------------------------------------------------------------

	/** Static factory delegate — set by EquipmentGASIntegration module's StartupModule() */
	static TFunction<void(UEquipmentManagerComponent*)> GASSetupFactory;

	/** GAS handler objects — stored to prevent GC, created by GASSetupFactory */
	UPROPERTY()
	TObjectPtr<UObject> GASAbilityGranter;

	UPROPERTY()
	TObjectPtr<UObject> GASEffectApplier;

	/** GAS operation callbacks — bound by the GAS module, called by Internal_Equip/Unequip */
	TFunction<void(const FItemInstance&, FGameplayTag)> OnGASEquipCallback;
	TFunction<void(FGameplayTag)> OnGASUnequipCallback;

	// -----------------------------------------------------------------------
	// Extension Points
	// -----------------------------------------------------------------------

	/** Called after an item is equipped. Override for game-specific logic. */
	UFUNCTION(BlueprintNativeEvent, Category = "Equipment")
	void OnPostEquip(const FItemInstance& Item, FGameplayTag SlotTag);

	/** Called after an item is unequipped. Override for game-specific logic. */
	UFUNCTION(BlueprintNativeEvent, Category = "Equipment")
	void OnPostUnequip(const FItemInstance& Item, FGameplayTag SlotTag);

	// -----------------------------------------------------------------------
	// Events
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnItemEquipped OnItemEquipped;

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnItemUnequipped OnItemUnequipped;

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnEquipmentChanged OnEquipmentChanged;

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnEquipmentOperationFailed OnOperationFailed;

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnEquipmentDurabilityChanged OnDurabilityChanged;

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnEquippedItemBroken OnItemBroken;

	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnCarriedLightChanged OnCarriedLightChanged;

	// -----------------------------------------------------------------------
	// Durability (feature 5: weapons wear down as they are used)
	// -----------------------------------------------------------------------

	/**
	 * Authority: reduce the equipped item's DurabilityState by Amount (clamped at 0). At zero with
	 * the definition's bDestroyAtZero the item is removed (OnItemBroken); otherwise it stays
	 * equipped at zero and consumers (the melee ability) read it as worn out.
	 * @param SlotTag The slot whose item wears.
	 * @param Amount  Durability points lost (the definition's DegradeRate per use, typically).
	 * @return Remaining durability, or -1 when the slot is empty / the item has no durability.
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment|Durability")
	float ApplyDurabilityLoss(FGameplayTag SlotTag, float Amount);

	/**
	 * Current / max durability of an equipped item.
	 * @return False when the slot is empty or the item has no durability.
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Durability")
	bool GetDurability(FGameplayTag SlotTag, float& OutCurrent, float& OutMax) const;

	// -----------------------------------------------------------------------
	// Carried light (feature 7: items with a LightSource fragment light up while equipped)
	// -----------------------------------------------------------------------

	/**
	 * Strength of the strongest lit light source equipped right now (lumens / 1000; 0 = none or out
	 * of fuel). Every machine: derived from the replicated slots.
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Light")
	float GetCarriedLightLevel() const;

	/**
	 * The slot carrying the strongest lit light source, with its fuel.
	 * @param OutSlotTag   The slot (invalid when nothing lit is carried).
	 * @param OutFuel      Remaining fuel seconds (0 when the light needs no fuel).
	 * @param OutMaxFuel   Fuel capacity (0 when the light needs no fuel).
	 * @return True when a lit light source is equipped.
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Light")
	bool GetCarriedLight(FGameplayTag& OutSlotTag, float& OutFuel, float& OutMaxFuel) const;

	/**
	 * Authority: burn Seconds of fuel from the carried light (lighting a sconce from a torch).
	 * @return False when nothing lit is carried, the light needs no fuel, or not on authority.
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment|Light")
	bool ConsumeCarriedLightFuel(float Seconds);

	/** Pure: the light intensity an equipped light source shows for its fuel state (0 when burnt out). */
	static float LitIntensityFor(const UItemFragment_LightSource& Light, bool bHasFuelGauge, float FuelRemaining);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// -----------------------------------------------------------------------
	// Replication
	// -----------------------------------------------------------------------

	UFUNCTION()
	void OnRep_EquipmentSlots();

	// -----------------------------------------------------------------------
	// Server RPCs
	// -----------------------------------------------------------------------

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestEquip(const FItemInstance& Item, FGameplayTag SlotTag);

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestUnequip(FGameplayTag SlotTag);

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestEquipFromInventory(const FGuid& ItemInstanceId,
		UInventoryComponent* SourceInventory, FGameplayTag SlotTag);

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestUnequipToInventory(FGameplayTag SlotTag, UInventoryComponent* TargetInventory);

	// -----------------------------------------------------------------------
	// Client RPC
	// -----------------------------------------------------------------------

	UFUNCTION(Client, Reliable)
	void ClientRPC_EquipmentOperationFailed(EEquipmentResult Result);

	// -----------------------------------------------------------------------
	// Internal
	// -----------------------------------------------------------------------

	/** Find the target slot for an item based on its EquipmentSlotTag */
	FGameplayTag FindTargetSlot(const FItemInstance& Item) const;

	/** Validate that an item can go into a specific slot */
	EEquipmentResult ValidateEquip(const FItemInstance& Item, FGameplayTag SlotTag) const;

	/** Core equip logic (after validation) */
	void Internal_Equip(const FItemInstance& Item, FGameplayTag SlotTag);

	/** Core unequip logic */
	FItemInstance Internal_Unequip(FGameplayTag SlotTag);

	/** Find runtime slot by tag */
	FEquipmentSlot* FindSlot(FGameplayTag SlotTag);
	const FEquipmentSlot* FindSlot(FGameplayTag SlotTag) const;

	/** Find slot definition by tag */
	const FEquipmentSlotDefinition* FindSlotDefinition(FGameplayTag SlotTag) const;

	// -----------------------------------------------------------------------
	// GAS Helpers
	// -----------------------------------------------------------------------

	/** Apply GAS abilities/effects for an equipped item (server-only, no-op if GAS not available) */
	void ApplyGAS(const FItemInstance& Item, FGameplayTag SlotTag);

	/** Remove GAS abilities/effects for a slot (server-only, no-op if GAS not available) */
	void RemoveGAS(FGameplayTag SlotTag);

	// -----------------------------------------------------------------------
	// Visuals
	// -----------------------------------------------------------------------

	void ApplyVisuals(const FItemInstance& Item, FGameplayTag SlotTag);
	void RemoveVisuals(FGameplayTag SlotTag);
	void OnMeshLoaded(FGameplayTag SlotTag);

	// -----------------------------------------------------------------------
	// Carried light
	// -----------------------------------------------------------------------

	/** Create / update / remove the slot's point light for its item and fuel (every machine). */
	void RefreshSlotLight(FEquipmentSlot& Slot);

	/** Broadcast OnCarriedLightChanged for a slot from its current state. */
	void NotifyCarriedLight(const FEquipmentSlot& Slot);

	/** Authority timer: accumulate burn time per lit slot and deduct fuel every FuelStepSeconds. */
	void TickFuel();

	UItemFragment_LightSource* GetLightSourceFragment(const FItemInstance& Item) const;

	FTimerHandle FuelTimerHandle;
	static constexpr float FuelTimerInterval = 1.0f;

	/** Get the owner's skeletal mesh for socket attachment */
	USkeletalMeshComponent* GetOwnerMesh() const;

	// -----------------------------------------------------------------------
	// Helpers
	// -----------------------------------------------------------------------

	UItemDatabaseSubsystem* GetItemDatabase() const;
	UItemFragment_Equipment* GetEquipmentFragment(const FItemInstance& Item) const;

	UPROPERTY()
	mutable TObjectPtr<UItemDatabaseSubsystem> CachedItemDatabase;
};
