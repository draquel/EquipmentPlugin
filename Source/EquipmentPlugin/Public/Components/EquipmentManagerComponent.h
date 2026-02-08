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

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEquipmentOperationFailed, EEquipmentResult, Result);

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

protected:
	virtual void BeginPlay() override;
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
	// Visuals
	// -----------------------------------------------------------------------

	void ApplyVisuals(const FItemInstance& Item, FGameplayTag SlotTag);
	void RemoveVisuals(FGameplayTag SlotTag);
	void OnMeshLoaded(FGameplayTag SlotTag);

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
