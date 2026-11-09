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
/** A slot's visual finished loading and was attached (every machine; meshes load asynchronously after the equip). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEquipmentVisualAttached, FGameplayTag, SlotTag);
/** A sheath's item was drawn into its hand or put back (every machine; on clients from the replicated slots). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEquipmentDrawnChanged, FGameplayTag, SlotTag, bool, bDrawn);

/**
 * Manages equipment slots on a character. Handles equip/unequip flow,
 * visual attachment, inventory integration, and multiplayer replication.
 * GAS integration is handled by the optional EquipmentGASIntegration module (Phase 9).
 * Sheaths (slots with a DrawSlotTag) carry weapons on the body and draw them into a hand (EQUIPMENT_SYSTEM.md, "Sheaths").
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

	/**
	 * Equip an item to its preferred slot (reads EquipmentSlotTag from the fragment). Authority only, like
	 * TryEquipToSlot.
	 * @param Item The item, held by the server code calling
	 * @return As TryEquipToSlot; IncompatibleSlot when the item names no slot this component has
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryEquip(const FItemInstance& Item);

	/**
	 * Equip an item that is in no inventory to a specific slot. Authority only: the item is whole, and a client never
	 * hands the server an item (G91); clients equip from an inventory (TryEquipFromInventory). With no inventory to
	 * return a replaced item to, an occupied slot is refused rather than its item dropped (G90).
	 * @param Item    The item, held by the server code calling
	 * @param SlotTag The slot
	 * @return Success; NotAuthorized on a client; SlotOccupied; ValidateEquip's failure otherwise (nothing changes)
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryEquipToSlot(const FItemInstance& Item, FGameplayTag SlotTag);

	/** Unequip the item in a slot (returns the item) */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryUnequip(FGameplayTag SlotTag, FItemInstance& OutItem);

	// -----------------------------------------------------------------------
	// Inventory-Integrated Equip/Unequip
	// -----------------------------------------------------------------------

	/**
	 * Equip from an inventory: the item leaves the inventory and goes into the slot. When the slot is occupied, the item
	 * there takes the inventory slot the new one leaves (one validated exchange, UInventoryComponent::TryReplaceItem),
	 * or the equip is refused: nothing is lost (G90). A client sends the instance id; the server equips the item it holds
	 * under that id, from an inventory of the same player (G91).
	 * An item with a sheath on this owner goes into the sheath (its preferred slot); asked into the sheath's hand, it goes
	 * into the sheath drawn.
	 * @param ItemInstanceId  The item in SourceInventory
	 * @param SourceInventory The inventory holding it
	 * @param SlotTag         The slot (invalid = the item's preferred slot)
	 * @param bDraw           Draw the item into its hand once it is in its sheath (the previously drawn one is sheathed)
	 * @return Success (optimistic on a client); InvalidItem (not in the inventory), IncompatibleSlot, NoInventorySpace
	 *         (the replaced item cannot take the slot), Failed; with bDraw, the draw's refusal after the equip (TryDraw)
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryEquipFromInventory(const FGuid& ItemInstanceId,
		UInventoryComponent* SourceInventory, FGameplayTag SlotTag, bool bDraw = false);

	/**
	 * Unequip to an inventory: the item leaves the slot and is added to the inventory, or stays equipped.
	 * @param SlotTag         The slot
	 * @param TargetInventory The inventory (on a client's request, one of the same player's)
	 * @return Success (optimistic on a client); Failed (empty slot); NoInventorySpace
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	EEquipmentResult TryUnequipToInventory(FGameplayTag SlotTag, UInventoryComponent* TargetInventory);

	// -----------------------------------------------------------------------
	// Sheaths (the equipment sheathing plan, D77)
	// -----------------------------------------------------------------------

	/**
	 * Draw a sheath's item into its hand: the item drawn into that hand before goes back to its own sheath, and an item in
	 * the hand slot itself goes to Inventory. Its abilities, effects and stance layer apply from here. A client sends the
	 * request; the server runs the same predicate.
	 * @param SheathSlotTag The sheath
	 * @param Inventory     Where an item in the hand slot itself goes (one of the same player's; may be null)
	 * @return Success (also when already drawn; optimistic on a client); IncompatibleSlot (no sheath, or no such hand);
	 *         InvalidItem (an empty sheath); SlotOccupied (the hand slot holds an item and no inventory was given);
	 *         NoInventorySpace (the inventory cannot take it)
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment|Sheath")
	EEquipmentResult TryDraw(FGameplayTag SheathSlotTag, UInventoryComponent* Inventory = nullptr);

	/**
	 * Put the item drawn into a hand back in its sheath (its abilities and effects revoked).
	 * @param SlotTag The hand (Equipment.Slot.MainHand) or the sheath itself
	 * @return Success (optimistic on a client); Failed when nothing drawn is there
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment|Sheath")
	EEquipmentResult TrySheathe(FGameplayTag SlotTag);

	/**
	 * The slot whose item is in a hand: the hand slot when it holds an item, else the sheath drawn into it. What every
	 * caller asking "what is in the main hand" reads.
	 * @param HandSlotTag The hand
	 * @return The slot; invalid when the hand is empty
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Sheath")
	FGameplayTag GetHeldSlotTag(FGameplayTag HandSlotTag) const;

	/**
	 * The item in a hand (GetHeldSlotTag's slot's item).
	 * @param HandSlotTag The hand
	 * @return The item; invalid when the hand is empty
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Sheath")
	FItemInstance GetHeldItem(FGameplayTag HandSlotTag) const;

	/**
	 * @param SlotTag A slot
	 * @return True when the slot is a sheath whose item is drawn
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Sheath")
	bool IsDrawn(FGameplayTag SlotTag) const;

	/**
	 * @param SlotTag A slot
	 * @return True when the slot is a sheath (its definition names a hand)
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Sheath")
	bool IsSheathSlot(FGameplayTag SlotTag) const;

	/** A sheath's item was drawn or sheathed (every machine). */
	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnEquipmentDrawnChanged OnDrawnChanged;

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

	/**
	 * A slot's visual was attached to the owner's mesh (after the async mesh load). Owners that re-attach visuals
	 * per view mode (first-person arms) re-apply their socket choice here.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
	FOnEquipmentVisualAttached OnVisualAttached;

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

	/**
	 * Where a slot's visual sits relative to its socket: the item's fit composed with the slot's skeleton correction.
	 * Owners that re-attach visuals themselves (view-mode socket swaps) apply this after attaching.
	 * @param SlotTag The slot
	 * @return The relative transform (identity for an unknown or empty slot)
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Visuals")
	FTransform GetVisualAttachOffset(FGameplayTag SlotTag) const;

	/**
	 * GetVisualAttachOffset for a given state of a sheath's item: in the hand (the item's hand fit on the hand slot's
	 * bone) or sheathed (its SheathOffset on the sheath's). Owners that animate the hand-off seat the visual by their
	 * montage, not by the state (VoxelCharacterPlugin). For a slot that is no sheath the state is ignored.
	 * @param SlotTag The slot
	 * @param bInHand The state to answer for
	 * @return The relative transform
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Visuals")
	FTransform GetVisualAttachOffsetForState(FGameplayTag SlotTag, bool bInHand) const;

	/**
	 * The bone or socket a slot's visual attaches to: the equipped item's AttachBoneOverride, else the slot's own.
	 * Owners that re-attach visuals themselves start from this.
	 * @param SlotTag The slot
	 * @return The bone or socket; None for an unknown slot
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Visuals")
	FName GetVisualAttachSocket(FGameplayTag SlotTag) const;

	/**
	 * GetVisualAttachSocket for a given state of a sheath's item: the hand's bone (or the item's AttachBoneOverride) in the
	 * hand, the sheath's (or its SheathBoneOverride) sheathed. For a slot that is no sheath the state is ignored.
	 * @param SlotTag The slot
	 * @param bInHand The state to answer for
	 * @return The bone or socket; None for an unknown slot
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Visuals")
	FName GetVisualAttachSocketForState(FGameplayTag SlotTag, bool bInHand) const;

	/**
	 * The bone the equipped item asks for instead of its slot's (its Equipment fragment's AttachBoneOverride; a sheathed
	 * item's SheathBoneOverride).
	 * @param SlotTag The slot
	 * @return The bone; None when the item uses the slot's, or the slot is empty or unknown
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Visuals")
	FName GetItemAttachBone(FGameplayTag SlotTag) const;

	/**
	 * The attach rule both use: an item's override when its fragment has one, else the slot's socket.
	 * @param SlotSocket The slot's socket
	 * @param Fragment   The equipped item's Equipment fragment (may be null)
	 * @return The bone or socket to attach to
	 */
	UFUNCTION(BlueprintPure, Category = "Equipment|Visuals")
	static FName ChooseAttachSocket(FName SlotSocket, const UItemFragment_Equipment* Fragment);

	/**
	 * Whether a slot's attached visual was built for another item than the one it holds: a client's view of a hotbar swap
	 * (occupied to occupied in one replication, G153). Such a visual is rebuilt by OnRep_EquipmentSlots.
	 * @param Slot The slot as replicated
	 * @return True when the slot is occupied, has a visual, and the visual's item id is not the slot's item's
	 */
	static bool IsVisualStale(const FEquipmentSlot& Slot);

	/**
	 * Change a slot's skeleton correction at runtime and re-apply it to the equipped visual (tuning, or a body swap).
	 * @param SlotTag The slot
	 * @param Offset  New transform relative to the socket
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment|Visuals")
	void SetSlotAttachOffset(FGameplayTag SlotTag, const FTransform& Offset);

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
	void ServerRPC_RequestUnequip(FGameplayTag SlotTag);

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestEquipFromInventory(const FGuid& ItemInstanceId,
		UInventoryComponent* SourceInventory, FGameplayTag SlotTag, bool bDraw);

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestDraw(FGameplayTag SheathSlotTag, UInventoryComponent* Inventory);

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestSheathe(FGameplayTag SlotTag);

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

	/** Find the target slot for an item: its sheath when this owner has one (SheathSlotTag), else its EquipmentSlotTag's */
	FGameplayTag FindTargetSlot(const FItemInstance& Item) const;

	/** A slot with this tag or a child of it: an empty one first, else the first occupied (invalid when none). */
	FGameplayTag FindSlotMatching(FGameplayTag Tag) const;

	/**
	 * An item with a sheath on this owner, asked into that sheath's hand, goes into the sheath drawn: retarget InOutSlotTag
	 * and set bInOutDraw. Other requests are left as they are.
	 */
	void ResolveSheathTarget(const FItemInstance& Item, FGameplayTag& InOutSlotTag, bool& bInOutDraw) const;

	/** Validate that an item can go into a specific slot */
	EEquipmentResult ValidateEquip(const FItemInstance& Item, FGameplayTag SlotTag) const;

	/**
	 * Equip-from-inventory's predicate, run by the caller and again by the server: the item is in the inventory and fits
	 * the slot (InOutSlotTag resolved from the item when invalid).
	 */
	EEquipmentResult ValidateEquipFromInventory(const FGuid& ItemInstanceId, const UInventoryComponent* SourceInventory,
		FGameplayTag& InOutSlotTag, FItemInstance& OutItem, bool& bInOutDraw) const;

	/** The draw's predicate, run by the caller and again by the server (TryDraw's results). */
	EEquipmentResult ValidateDraw(FGameplayTag SheathSlotTag, const UInventoryComponent* Inventory) const;

	/** The authoritative draw (TryDraw on the authority, and its RPC). */
	EEquipmentResult DrawSheath(FGameplayTag SheathSlotTag, UInventoryComponent* Inventory);

	/** The sheath a TrySheathe names: the slot itself when it is a sheath, else the sheath drawn into that hand. */
	const FEquipmentSlot* FindSheatheTarget(FGameplayTag SlotTag) const;

	/** The sheath drawn into a hand, if any. */
	FEquipmentSlot* FindDrawnSheath(FGameplayTag HandSlotTag);
	const FEquipmentSlot* FindDrawnSheath(FGameplayTag HandSlotTag) const;

	/** Authority: draw or sheathe a sheath's item (GAS, the visual's seat, the layer, the events). */
	void SetDrawnInternal(FEquipmentSlot& Slot, bool bDrawn);

	/**
	 * Every machine: seat the slot's visual for its state, link or unlink its stance layer, and announce a change of the
	 * drawn state this machine had not acted on yet (OnDrawnChanged).
	 */
	void ApplyHandState(FEquipmentSlot& Slot);

	/** Link the stance layer of the slot's item while it is in the hand, unlink the one linked otherwise. */
	void RefreshSlotLayer(FEquipmentSlot& Slot);

	/** Unequip-to-inventory's predicate: the slot holds an item and the inventory can take it. */
	EEquipmentResult ValidateUnequipToInventory(FGameplayTag SlotTag, const UInventoryComponent* TargetInventory) const;

	/** The authoritative equip from an inventory (TryEquipFromInventory on the authority, and its RPC). */
	EEquipmentResult EquipFromInventory(const FGuid& ItemInstanceId, UInventoryComponent* SourceInventory, FGameplayTag SlotTag, bool bDraw);

	/** The authoritative unequip to an inventory (TryUnequipToInventory on the authority, and its RPC). */
	EEquipmentResult UnequipToInventory(FGameplayTag SlotTag, UInventoryComponent* TargetInventory);

	/** A client may name this inventory: it is on this component's owner, or on another actor of the same connection. */
	bool IsOwnersInventory(const UInventoryComponent* Inventory) const;

	/** True where equipment state may change: the owner has authority, or there is no owner (a bare component). */
	bool HasMutationAuthority() const;

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
