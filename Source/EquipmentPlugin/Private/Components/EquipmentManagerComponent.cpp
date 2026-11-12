#include "Components/EquipmentManagerComponent.h"
#include "EquipmentPlugin.h"
#include "Components/InventoryComponent.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Data/ItemDefinition.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
#include "Data/Fragments/ItemFragment_Durability.h"
#include "Data/Fragments/ItemFragment_LightSource.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Types/ItemInstanceFragments.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "Engine/AssetManager.h"
#include "Net/UnrealNetwork.h"
#include "Engine/CollisionProfile.h"

namespace
{
	/**
	 * Equipment visuals are cosmetic: they must never collide. A new mesh component defaults to BlockAllDynamic,
	 * and the item meshes carry simple collision (pipeline policy auto_box), so an equipped sword, helmet or chest
	 * piece used to block everything while riding the owner's bones. When the owner ragdolled on death its physics
	 * bodies started inside them, the solver launched the body, and it tunnelled through the terrain (QA F1-04).
	 */
	void DisableVisualCollision(UPrimitiveComponent* Visual)
	{
		Visual->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Visual->SetGenerateOverlapEvents(false);
		Visual->SetCanEverAffectNavigation(false);
	}
}

// Static factory delegate — set by EquipmentGASIntegration module
TFunction<void(UEquipmentManagerComponent*)> UEquipmentManagerComponent::GASSetupFactory;

UEquipmentManagerComponent::UEquipmentManagerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UEquipmentManagerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The ability system this component granted to may outlive it (a player state's ASC survives
	// the pawn's death). Revoke every slot's abilities and effects so a destroyed pawn does not
	// leave its equipment stats behind on the next avatar.
	if (OnGASUnequipCallback && GetOwner() && GetOwner()->HasAuthority())
	{
		for (const FEquipmentSlot& Slot : EquipmentSlots)
		{
			if (Slot.bIsOccupied && Slot.IsInHand()) // a sheathed item granted nothing
			{
				OnGASUnequipCallback(Slot.SlotTag);
			}
		}
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FuelTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void UEquipmentManagerComponent::BeginPlay()
{
	Super::BeginPlay();

	// The runtime slots are the authority's, built from the definitions. A client keeps the array the server sent, which
	// arrives with the actor before this BeginPlay and can hold equipped items (a player joining, an actor becoming
	// relevant): rebuilding it emptied those slots on the client, and the server, seeing nothing changed, never resent
	// them (G102).
	if (HasMutationAuthority())
	{
		EquipmentSlots.Reset();
		for (const FEquipmentSlotDefinition& Def : AvailableSlots)
		{
			FEquipmentSlot Slot;
			Slot.SlotTag = Def.SlotTag;
			Slot.AttachSocket = Def.AttachSocket;
			Slot.AttachOffset = Def.AttachOffset;
			Slot.AcceptedItemTags = Def.AcceptedItemTags;
			Slot.DrawSlotTag = Def.DrawSlotTag;
			Slot.bIsOccupied = false;
			EquipmentSlots.Add(Slot);
		}
	}

	// Initialize GAS integration if the module is loaded
	if (GASSetupFactory)
	{
		GASSetupFactory(this);
	}

	// Carried lights burn fuel on the authority only; clients see it through the replicated durability.
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(FuelTimerHandle, this, &UEquipmentManagerComponent::TickFuel, FuelTimerInterval, true);
		}
	}
}

void UEquipmentManagerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UEquipmentManagerComponent, EquipmentSlots);
}

// ===========================================================================
// Replication
// ===========================================================================

void UEquipmentManagerComponent::OnRep_EquipmentSlots()
{
	// Sync visuals on clients based on replicated state
	for (FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.bIsOccupied && !Slot.AttachedVisualComponent)
		{
			ApplyVisuals(Slot.EquippedItem, Slot.SlotTag);
		}
		else if (!Slot.bIsOccupied && Slot.AttachedVisualComponent)
		{
			RemoveVisuals(Slot.SlotTag);
		}
		else if (IsVisualStale(Slot))
		{
			// A swap: the server unequipped one item and equipped another in one frame, so this client sees the slot occupied
			// before and after with another item in it. The old visual and its layer go, the new item's come (G153).
			RemoveVisuals(Slot.SlotTag);
			ApplyVisuals(Slot.EquippedItem, Slot.SlotTag);
		}
		else if (Slot.bIsOccupied && Slot.AttachedLightComponent)
		{
			// Same item, possibly less fuel: the light and the HUD follow the replicated durability (ApplyDurabilityLoss
			// marks the item changed, so a loss is sent: G136).
			RefreshSlotLight(Slot);
			NotifyCarriedLight(Slot);
		}

		// A draw or a sheathe of the same item: the visual changes bone and fit (no rebuild), the layer follows, and the
		// owner hears of it. A rebuilt visual is seated by OnMeshLoaded from the state; this records and announces it.
		if (Slot.bAppliedDrawn != Slot.bDrawn)
		{
			ApplyHandState(Slot);
		}
	}

	OnEquipmentChanged.Broadcast();
}

// ===========================================================================
// Direct Equip/Unequip
// ===========================================================================

EEquipmentResult UEquipmentManagerComponent::TryEquip(const FItemInstance& Item)
{
	FGameplayTag TargetSlot = FindTargetSlot(Item);
	if (!TargetSlot.IsValid())
	{
		return EEquipmentResult::IncompatibleSlot;
	}
	return TryEquipToSlot(Item, TargetSlot);
}

EEquipmentResult UEquipmentManagerComponent::TryEquipToSlot(const FItemInstance& Item, FGameplayTag SlotTag)
{
	// The item is whole: server code only. A client equips from an inventory by instance id (G91).
	if (!HasMutationAuthority())
	{
		UE_LOG(LogEQP, Warning, TEXT("TryEquipToSlot on a client (%s): equip from an inventory instead."), *GetNameSafe(GetOwner()));
		return EEquipmentResult::NotAuthorized;
	}

	// A carried weapon asked into its hand goes into its sheath, drawn (D77).
	bool bDraw = false;
	ResolveSheathTarget(Item, SlotTag, bDraw);

	EEquipmentResult ValidationResult = ValidateEquip(Item, SlotTag);
	if (ValidationResult != EEquipmentResult::Success)
	{
		return ValidationResult;
	}

	// No inventory to hand a replaced item to: refuse rather than drop it (G90). The same for an item it would send off
	// the place it shares (a quiver beside a shield, D97).
	if (IsSlotOccupied(SlotTag) || !GetPlaceConflicts(Item, SlotTag).IsEmpty())
	{
		return EEquipmentResult::SlotOccupied;
	}

	Internal_Equip(Item, SlotTag);
	if (bDraw && !GetDrawLeadSlotTag(SlotTag).IsValid())
	{
		// The item is equipped either way; with an item in the hand slot itself and no inventory it stays sheathed.
		const EEquipmentResult DrawResult = DrawSheath(SlotTag, nullptr);
		if (DrawResult != EEquipmentResult::Success)
		{
			UE_LOG(LogEQP, Log, TEXT("TryEquipToSlot: %s equipped in %s, not drawn (%s)."), *Item.ItemDefinitionId.ToString(),
				*SlotTag.ToString(), *UEnum::GetValueAsString(DrawResult));
		}
	}
	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::TryUnequip(FGameplayTag SlotTag, FItemInstance& OutItem)
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return EEquipmentResult::Failed;
	}

	if (!HasMutationAuthority())
	{
		ServerRPC_RequestUnequip(SlotTag);
		return EEquipmentResult::Success;
	}

	OutItem = Internal_Unequip(SlotTag);
	return EEquipmentResult::Success;
}

// ===========================================================================
// Durability
// ===========================================================================

bool UEquipmentManagerComponent::GetDurability(FGameplayTag SlotTag, float& OutCurrent, float& OutMax) const
{
	OutCurrent = 0.f;
	OutMax = 0.f;
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return false;
	}
	const UInstanceFragment_DurabilityState* State = Slot->EquippedItem.FindFragment<UInstanceFragment_DurabilityState>();
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	const UItemDefinition* Def = DB ? DB->GetDefinition(Slot->EquippedItem.ItemDefinitionId) : nullptr;
	const UItemFragment_Durability* Durability = Def ? Def->FindFragment<UItemFragment_Durability>() : nullptr;
	if (!State || !Durability)
	{
		return false;
	}
	OutCurrent = State->CurrentDurability;
	OutMax = Durability->MaxDurability;
	return true;
}

float UEquipmentManagerComponent::ApplyDurabilityLoss(FGameplayTag SlotTag, float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return -1.f;
	}
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return -1.f;
	}
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	const UItemDefinition* Def = DB ? DB->GetDefinition(Slot->EquippedItem.ItemDefinitionId) : nullptr;
	const UItemFragment_Durability* Durability = Def ? Def->FindFragment<UItemFragment_Durability>() : nullptr;
	if (!Durability || !Slot->EquippedItem.FindFragment<UInstanceFragment_DurabilityState>())
	{
		return -1.f;
	}

	// Through the slot's own item: the edit marks it changed, so the slot is sent and a client's durability follows. The
	// state replication last sent shares the fragment object, so a change made only to the object was never seen (G136).
	UInstanceFragment_DurabilityState* State = Slot->EquippedItem.EditFragment<UInstanceFragment_DurabilityState>();
	State->CurrentDurability = FMath::Clamp(State->CurrentDurability - FMath::Max(Amount, 0.f), 0.f, Durability->MaxDurability);
	const float Remaining = State->CurrentDurability;
	OnDurabilityChanged.Broadcast(SlotTag, Remaining, Durability->MaxDurability);

	if (Remaining <= 0.f && Durability->bDestroyAtZero)
	{
		// Worn through: the item is gone, not returned to any inventory.
		const FItemInstance Broken = Internal_Unequip(SlotTag);
		OnItemBroken.Broadcast(SlotTag, Broken);
		return 0.f;
	}
	if (Slot->AttachedLightComponent)
	{
		RefreshSlotLight(*Slot);
		NotifyCarriedLight(*Slot);
	}
	return Remaining;
}

// ===========================================================================
// Inventory-Integrated Equip/Unequip
// ===========================================================================

EEquipmentResult UEquipmentManagerComponent::TryEquipFromInventory(const FGuid& ItemInstanceId,
	UInventoryComponent* SourceInventory, FGameplayTag SlotTag, bool bDraw)
{
	if (!HasMutationAuthority())
	{
		// The client's check (the server runs it again): only the instance id travels. The draw's own check needs the
		// equip done, so the server alone runs it (a refusal comes back as OnOperationFailed).
		FItemInstance Item;
		FGameplayTag ResolvedSlot = SlotTag;
		bool bResolvedDraw = bDraw;
		const EEquipmentResult ValidationResult = ValidateEquipFromInventory(ItemInstanceId, SourceInventory, ResolvedSlot, Item, bResolvedDraw);
		if (ValidationResult != EEquipmentResult::Success)
		{
			return ValidationResult;
		}
		ServerRPC_RequestEquipFromInventory(ItemInstanceId, SourceInventory, SlotTag, bDraw);
		return EEquipmentResult::Success; // Optimistic
	}

	return EquipFromInventory(ItemInstanceId, SourceInventory, SlotTag, bDraw);
}

EEquipmentResult UEquipmentManagerComponent::TryUnequipToInventory(FGameplayTag SlotTag,
	UInventoryComponent* TargetInventory)
{
	if (!HasMutationAuthority())
	{
		const EEquipmentResult ValidationResult = ValidateUnequipToInventory(SlotTag, TargetInventory);
		if (ValidationResult != EEquipmentResult::Success)
		{
			return ValidationResult;
		}
		ServerRPC_RequestUnequipToInventory(SlotTag, TargetInventory);
		return EEquipmentResult::Success; // Optimistic
	}

	return UnequipToInventory(SlotTag, TargetInventory);
}

EEquipmentResult UEquipmentManagerComponent::EquipFromInventory(const FGuid& ItemInstanceId,
	UInventoryComponent* SourceInventory, FGameplayTag SlotTag, bool bDraw)
{
	FItemInstance Item;
	const EEquipmentResult ValidationResult = ValidateEquipFromInventory(ItemInstanceId, SourceInventory, SlotTag, Item, bDraw);
	if (ValidationResult != EEquipmentResult::Success)
	{
		return ValidationResult;
	}

	// What shares no place with it goes to the bag first (a shield off the back for the quiver, D97); each step is
	// validated before it moves anything, so a refusal leaves the item where it was.
	for (const FGameplayTag& Conflict : GetPlaceConflicts(Item, SlotTag))
	{
		const EEquipmentResult Evicted = UnequipToInventory(Conflict, SourceInventory);
		if (Evicted != EEquipmentResult::Success)
		{
			return Evicted;
		}
	}

	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (Slot && Slot->bIsOccupied)
	{
		// A swap: the replaced item takes the inventory slot the new one leaves, validated whole before either side
		// changes, so a refusal leaves both as they were and nothing is dropped (G90).
		const EInventoryOperationResult Exchange = SourceInventory->TryReplaceItem(ItemInstanceId, Slot->EquippedItem);
		if (Exchange != EInventoryOperationResult::Success)
		{
			UE_LOG(LogEQP, Log, TEXT("EquipFromInventory: %s cannot take the inventory slot of %s (%s)."),
				*Slot->EquippedItem.ItemDefinitionId.ToString(), *Item.ItemDefinitionId.ToString(), *UEnum::GetValueAsString(Exchange));
			return Exchange == EInventoryOperationResult::ItemNotFound ? EEquipmentResult::InvalidItem : EEquipmentResult::NoInventorySpace;
		}
		// Its copy is in the inventory now; this only empties the slot (GAS, visuals, events).
		Internal_Unequip(SlotTag);
	}
	else
	{
		const EInventoryOperationResult Removed = SourceInventory->TryRemoveItem(ItemInstanceId);
		if (Removed != EInventoryOperationResult::Success)
		{
			UE_LOG(LogEQP, Error, TEXT("EquipFromInventory: removing %s failed after validation passed (%s)."),
				*Item.ItemDefinitionId.ToString(), *UEnum::GetValueAsString(Removed));
			return Removed == EInventoryOperationResult::ItemNotFound ? EEquipmentResult::InvalidItem : EEquipmentResult::Failed;
		}
	}

	Internal_Equip(Item, SlotTag);
	// A shield follows its lead (D96): it is already in the hand if the lead is, and is never drawn on its own.
	if (bDraw && IsSheathSlot(SlotTag) && !GetDrawLeadSlotTag(SlotTag).IsValid())
	{
		// The equip stands whatever the draw answers (an item in the hand slot itself the bag cannot take).
		return DrawSheath(SlotTag, SourceInventory);
	}
	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::UnequipToInventory(FGameplayTag SlotTag, UInventoryComponent* TargetInventory)
{
	const EEquipmentResult ValidationResult = ValidateUnequipToInventory(SlotTag, TargetInventory);
	if (ValidationResult != EEquipmentResult::Success)
	{
		return ValidationResult;
	}

	FItemInstance UnequippedItem = Internal_Unequip(SlotTag);

	EInventoryOperationResult AddResult = TargetInventory->TryAddItem(UnequippedItem);
	if (AddResult != EInventoryOperationResult::Success)
	{
		// Rollback: re-equip
		Internal_Equip(UnequippedItem, SlotTag);
		UE_LOG(LogEQP, Error, TEXT("EquipmentManager: Failed to add unequipped item to inventory after validation passed (%s)."),
			*UEnum::GetValueAsString(AddResult));
		return EEquipmentResult::NoInventorySpace;
	}

	return EEquipmentResult::Success;
}

// ===========================================================================
// Queries
// ===========================================================================

FItemInstance UEquipmentManagerComponent::GetEquippedItem(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (Slot && Slot->bIsOccupied)
	{
		return Slot->EquippedItem;
	}
	return FItemInstance();
}

bool UEquipmentManagerComponent::IsSlotOccupied(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	return Slot && Slot->bIsOccupied;
}

TArray<FGameplayTag> UEquipmentManagerComponent::GetOccupiedSlotTags() const
{
	TArray<FGameplayTag> Result;
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.bIsOccupied)
		{
			Result.Add(Slot.SlotTag);
		}
	}
	return Result;
}

TArray<FGameplayTag> UEquipmentManagerComponent::GetEmptySlotTags() const
{
	TArray<FGameplayTag> Result;
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (!Slot.bIsOccupied)
		{
			Result.Add(Slot.SlotTag);
		}
	}
	return Result;
}

bool UEquipmentManagerComponent::CanEquipItem(const FItemInstance& Item) const
{
	FGameplayTag TargetSlot = FindTargetSlot(Item);
	if (!TargetSlot.IsValid())
	{
		return false;
	}
	return ValidateEquip(Item, TargetSlot) == EEquipmentResult::Success;
}

// ===========================================================================
// Sheaths (the equipment sheathing plan, D77)
// ===========================================================================

EEquipmentResult UEquipmentManagerComponent::TryDraw(FGameplayTag SheathSlotTag, UInventoryComponent* Inventory)
{
	if (!HasMutationAuthority())
	{
		const EEquipmentResult ValidationResult = ValidateDraw(SheathSlotTag, Inventory);
		if (ValidationResult != EEquipmentResult::Success)
		{
			return ValidationResult;
		}
		ServerRPC_RequestDraw(SheathSlotTag, Inventory);
		return EEquipmentResult::Success; // Optimistic
	}
	return DrawSheath(SheathSlotTag, Inventory);
}

EEquipmentResult UEquipmentManagerComponent::TrySheathe(FGameplayTag SlotTag)
{
	const FEquipmentSlot* Target = FindSheatheTarget(SlotTag);
	// A shield goes back with its lead, not on its own (D96).
	if (!Target || !Target->bDrawn || GetDrawLeadSlotTag(Target->SlotTag).IsValid())
	{
		return EEquipmentResult::Failed;
	}
	if (!HasMutationAuthority())
	{
		ServerRPC_RequestSheathe(SlotTag);
		return EEquipmentResult::Success; // Optimistic
	}
	SetDrawnInternal(*FindSlot(Target->SlotTag), false);
	return EEquipmentResult::Success;
}

FGameplayTag UEquipmentManagerComponent::GetHeldSlotTag(FGameplayTag HandSlotTag) const
{
	if (IsSlotOccupied(HandSlotTag))
	{
		return HandSlotTag;
	}
	const FEquipmentSlot* Drawn = FindDrawnSheath(HandSlotTag);
	return Drawn ? Drawn->SlotTag : FGameplayTag();
}

FItemInstance UEquipmentManagerComponent::GetHeldItem(FGameplayTag HandSlotTag) const
{
	const FGameplayTag Held = GetHeldSlotTag(HandSlotTag);
	return Held.IsValid() ? GetEquippedItem(Held) : FItemInstance();
}

bool UEquipmentManagerComponent::IsDrawn(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	return Slot && Slot->bDrawn;
}

bool UEquipmentManagerComponent::IsSheathSlot(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	return Slot && Slot->DrawSlotTag.IsValid();
}

FGameplayTag UEquipmentManagerComponent::GetDrawLeadSlotTag(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied || !Slot->DrawSlotTag.IsValid())
	{
		return FGameplayTag();
	}
	const UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Slot->EquippedItem);
	return EquipFrag && EquipFrag->DrawWithSlotTag != Slot->DrawSlotTag ? EquipFrag->DrawWithSlotTag : FGameplayTag();
}

FGameplayTag UEquipmentManagerComponent::GetDrawHandFor(const FItemInstance& Item, FGameplayTag SheathSlotTag) const
{
	const FEquipmentSlotDefinition* Def = AvailableSlots.FindByPredicate([&SheathSlotTag](const FEquipmentSlotDefinition& D)
	{
		return D.SlotTag == SheathSlotTag;
	});
	const FEquipmentSlot* Sheath = FindSlot(SheathSlotTag);
	const FGameplayTag Default = Def ? Def->DrawSlotTag : (Sheath ? Sheath->DrawSlotTag : FGameplayTag());
	if (!Default.IsValid())
	{
		return FGameplayTag();
	}
	// The item's own hand when this owner has it as an ordinary slot: the back draws a bow into the main hand and a shield
	// into the off hand (D95).
	const UItemFragment_Equipment* EquipFrag = Item.IsValid() ? GetEquipmentFragment(Item) : nullptr;
	const FEquipmentSlot* ItemHand = EquipFrag ? FindSlot(EquipFrag->EquipmentSlotTag) : nullptr;
	return ItemHand && !ItemHand->DrawSlotTag.IsValid() ? ItemHand->SlotTag : Default;
}

FGameplayTagContainer UEquipmentManagerComponent::GetBlockedSlotTags() const
{
	FGameplayTagContainer Blocked;
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (!Slot.bIsOccupied || !Slot.IsInHand())
		{
			continue;
		}
		if (const UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Slot.EquippedItem))
		{
			Blocked.AppendTags(EquipFrag->BlockedSlotTags);
		}
	}
	return Blocked;
}

bool UEquipmentManagerComponent::IsSlotBlocked(FGameplayTag SlotTag) const
{
	return SlotTag.IsValid() && GetBlockedSlotTags().HasTagExact(SlotTag);
}

TArray<FGameplayTag> UEquipmentManagerComponent::GetPlaceConflicts(const FItemInstance& Item, FGameplayTag SlotTag) const
{
	TArray<FGameplayTag> Conflicts;
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.bIsOccupied && Slot.SlotTag != SlotTag && ItemsShareNoPlace(Item, SlotTag, Slot.EquippedItem, Slot.SlotTag))
		{
			Conflicts.Add(Slot.SlotTag);
		}
	}
	return Conflicts;
}

bool UEquipmentManagerComponent::ItemsShareNoPlace(const FItemInstance& A, FGameplayTag SlotA, const FItemInstance& B, FGameplayTag SlotB) const
{
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	// The item in OwnSlot shares OtherSlot's place, and Other is none of the items it allows there.
	auto Refuses = [this, DB](const FItemInstance& Own, const FItemInstance& Other, FGameplayTag OtherSlot)
	{
		const UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Own);
		if (!EquipFrag || !EquipFrag->SharesPlaceWithSlotTag.IsValid() || EquipFrag->SharesPlaceWithSlotTag != OtherSlot)
		{
			return false;
		}
		const UItemDefinition* OtherDef = DB ? DB->GetDefinition(Other.ItemDefinitionId) : nullptr;
		return !OtherDef || !OtherDef->ItemTags.HasAny(EquipFrag->SharesPlaceWithItemTags);
	};
	return Refuses(A, B, SlotB) || Refuses(B, A, SlotA);
}

EEquipmentResult UEquipmentManagerComponent::ValidateDraw(FGameplayTag SheathSlotTag, const UInventoryComponent* Inventory) const
{
	const FEquipmentSlot* Sheath = FindSlot(SheathSlotTag);
	if (!Sheath || !Sheath->DrawSlotTag.IsValid())
	{
		return EEquipmentResult::IncompatibleSlot;
	}
	if (!Sheath->bIsOccupied)
	{
		return EEquipmentResult::InvalidItem;
	}
	// A shield comes into the hand with its lead and goes back with it, never on its own (D96; ReconcileHands).
	if (GetDrawLeadSlotTag(SheathSlotTag).IsValid())
	{
		return EEquipmentResult::IncompatibleSlot;
	}
	const FEquipmentSlot* Hand = FindSlot(Sheath->DrawSlotTag);
	if (!Hand)
	{
		return EEquipmentResult::IncompatibleSlot;
	}
	if (Sheath->bDrawn)
	{
		return EEquipmentResult::Success;
	}
	// A hand an item in another hand blocks takes nothing (the off hand beside a pickaxe).
	if (IsSlotBlocked(Sheath->DrawSlotTag))
	{
		return EEquipmentResult::HandBlocked;
	}
	// One item per hand: an item in the hand slot itself (one without a sheath) has to go to the bag first, and so does
	// one in a hand slot the drawn item blocks (a torch in the off hand when the bow comes out). A sheath's item drawn into
	// a blocked hand goes back to its sheath on its own (ReconcileHands).
	TArray<const FEquipmentSlot*> ToBag;
	if (Hand->bIsOccupied)
	{
		ToBag.Add(Hand);
	}
	if (const UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Sheath->EquippedItem))
	{
		for (const FGameplayTag& BlockedTag : EquipFrag->BlockedSlotTags)
		{
			const FEquipmentSlot* Blocked = FindSlot(BlockedTag);
			if (Blocked && Blocked != Hand && Blocked->bIsOccupied && !Blocked->DrawSlotTag.IsValid())
			{
				ToBag.Add(Blocked);
			}
		}
	}
	for (const FEquipmentSlot* Slot : ToBag)
	{
		if (!Inventory)
		{
			return EEquipmentResult::SlotOccupied;
		}
		if (!Inventory->CanAcceptItem(Slot->EquippedItem))
		{
			return EEquipmentResult::NoInventorySpace;
		}
	}
	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::DrawSheath(FGameplayTag SheathSlotTag, UInventoryComponent* Inventory)
{
	const EEquipmentResult ValidationResult = ValidateDraw(SheathSlotTag, Inventory);
	if (ValidationResult != EEquipmentResult::Success)
	{
		return ValidationResult;
	}
	FEquipmentSlot* Sheath = FindSlot(SheathSlotTag);
	if (Sheath->bDrawn)
	{
		return EEquipmentResult::Success;
	}
	const FGameplayTag HandTag = Sheath->DrawSlotTag;
	TArray<FGameplayTag> ToBag;
	if (IsSlotOccupied(HandTag))
	{
		ToBag.Add(HandTag);
	}
	if (const UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Sheath->EquippedItem))
	{
		for (const FGameplayTag& BlockedTag : EquipFrag->BlockedSlotTags)
		{
			if (BlockedTag != HandTag && IsSlotOccupied(BlockedTag) && !IsSheathSlot(BlockedTag))
			{
				ToBag.Add(BlockedTag);
			}
		}
	}
	for (const FGameplayTag& SlotTag : ToBag)
	{
		const EEquipmentResult Cleared = UnequipToInventory(SlotTag, Inventory);
		if (Cleared != EEquipmentResult::Success)
		{
			return Cleared;
		}
	}
	// The weapon drawn into this hand before goes back to its own sheath in the same step.
	if (FEquipmentSlot* Previous = FindDrawnSheath(HandTag))
	{
		SetDrawnInternal(*Previous, false);
	}
	SetDrawnInternal(*Sheath, true);
	return EEquipmentResult::Success;
}

const FEquipmentSlot* UEquipmentManagerComponent::FindSheatheTarget(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (Slot && Slot->DrawSlotTag.IsValid())
	{
		return Slot;
	}
	return FindDrawnSheath(SlotTag);
}

FEquipmentSlot* UEquipmentManagerComponent::FindDrawnSheath(FGameplayTag HandSlotTag)
{
	return const_cast<FEquipmentSlot*>(static_cast<const UEquipmentManagerComponent*>(this)->FindDrawnSheath(HandSlotTag));
}

const FEquipmentSlot* UEquipmentManagerComponent::FindDrawnSheath(FGameplayTag HandSlotTag) const
{
	if (!HandSlotTag.IsValid())
	{
		return nullptr;
	}
	return EquipmentSlots.FindByPredicate([&HandSlotTag](const FEquipmentSlot& S)
	{
		return S.bDrawn && S.bIsOccupied && S.DrawSlotTag == HandSlotTag;
	});
}

void UEquipmentManagerComponent::SetDrawnInternal(FEquipmentSlot& Slot, bool bDrawn)
{
	if (!Slot.DrawSlotTag.IsValid() || !Slot.bIsOccupied || Slot.bDrawn == bDrawn)
	{
		return;
	}
	Slot.bDrawn = bDrawn;
	// A sheath grants its item's abilities and effects only while drawn: a belted pickaxe does not dig (D77).
	if (bDrawn)
	{
		ApplyGAS(Slot.EquippedItem, Slot.SlotTag);
	}
	else
	{
		RemoveGAS(Slot.SlotTag);
	}
	ApplyHandState(Slot);
	OnEquipmentChanged.Broadcast();
	ReconcileHands();
}

void UEquipmentManagerComponent::ReconcileHands()
{
	if (bReconcilingHands || !HasMutationAuthority())
	{
		return;
	}
	TGuardValue<bool> Reconciling(bReconcilingHands, true);
	// What is drawn into a blocked hand goes back to its sheath (a shield when a pickaxe comes out, G175).
	const FGameplayTagContainer Blocked = GetBlockedSlotTags();
	for (FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.bDrawn && Blocked.HasTagExact(Slot.DrawSlotTag))
		{
			SetDrawnInternal(Slot, false);
		}
	}
	// A shield follows its lead (D96): in its hand while the lead's hand holds an item that leaves its hand free (a sword,
	// not a pickaxe) and nothing else is in its hand (a torch), on its sheath otherwise.
	for (FEquipmentSlot& Slot : EquipmentSlots)
	{
		const FGameplayTag Lead = GetDrawLeadSlotTag(Slot.SlotTag);
		if (!Lead.IsValid())
		{
			continue;
		}
		const FEquipmentSlot* Drawn = FindDrawnSheath(Slot.DrawSlotTag);
		const bool bWanted = GetHeldItem(Lead).IsValid() && !Blocked.HasTagExact(Slot.DrawSlotTag)
			&& !IsSlotOccupied(Slot.DrawSlotTag) && (!Drawn || Drawn == &Slot);
		if (bWanted != Slot.bDrawn)
		{
			SetDrawnInternal(Slot, bWanted);
		}
	}
}

void UEquipmentManagerComponent::ApplyHandState(FEquipmentSlot& Slot)
{
	if (Slot.AttachedVisualComponent)
	{
		if (USkeletalMeshComponent* OwnerMesh = GetOwnerMesh())
		{
			Slot.AttachedVisualComponent->AttachToComponent(OwnerMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale,
				GetVisualAttachSocket(Slot.SlotTag));
		}
		Slot.AttachedVisualComponent->SetRelativeTransform(GetVisualAttachOffset(Slot.SlotTag));
		// A pending mesh load links the layer when it lands (OnMeshLoaded).
		RefreshSlotLayer(Slot);
	}
	const bool bDrawn = Slot.bDrawn;
	if (Slot.bAppliedDrawn != bDrawn)
	{
		Slot.bAppliedDrawn = bDrawn;
		OnDrawnChanged.Broadcast(Slot.SlotTag, bDrawn);
	}
}

void UEquipmentManagerComponent::RefreshSlotLayer(FEquipmentSlot& Slot)
{
	const UItemFragment_Equipment* EquipFrag = Slot.bIsOccupied ? GetEquipmentFragment(Slot.EquippedItem) : nullptr;
	TSubclassOf<UAnimInstance> Wanted;
	if (EquipFrag && Slot.IsInHand())
	{
		Wanted = EquipFrag->AnimLayerClass;
	}
	if (Wanted == Slot.AttachedAnimLayerClass)
	{
		return;
	}
	USkeletalMeshComponent* OwnerMesh = GetOwnerMesh();
	if (OwnerMesh && Slot.AttachedAnimLayerClass)
	{
		OwnerMesh->UnlinkAnimClassLayers(Slot.AttachedAnimLayerClass);
	}
	Slot.AttachedAnimLayerClass = Wanted;
	if (OwnerMesh && Wanted)
	{
		OwnerMesh->LinkAnimClassLayers(Wanted);
	}
}

// ===========================================================================
// Extension Points
// ===========================================================================

void UEquipmentManagerComponent::OnPostEquip_Implementation(const FItemInstance& Item, FGameplayTag SlotTag)
{
}

void UEquipmentManagerComponent::OnPostUnequip_Implementation(const FItemInstance& Item, FGameplayTag SlotTag)
{
}

// ===========================================================================
// Server RPCs
// ===========================================================================

void UEquipmentManagerComponent::ServerRPC_RequestUnequip_Implementation(FGameplayTag SlotTag)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::Failed);
		return;
	}

	Internal_Unequip(SlotTag);
}

void UEquipmentManagerComponent::ServerRPC_RequestEquipFromInventory_Implementation(const FGuid& ItemInstanceId,
	UInventoryComponent* SourceInventory, FGameplayTag SlotTag, bool bDraw)
{
	// The client names an item; the server equips what it holds under that id, and only from the player's own inventory.
	if (!IsOwnersInventory(SourceInventory))
	{
		UE_LOG(LogEQP, Warning, TEXT("ServerRPC_RequestEquipFromInventory: %s named an inventory it does not own (%s)."),
			*GetNameSafe(GetOwner()), SourceInventory ? *GetNameSafe(SourceInventory->GetOwner()) : TEXT("none"));
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::NotAuthorized);
		return;
	}

	const EEquipmentResult Result = EquipFromInventory(ItemInstanceId, SourceInventory, SlotTag, bDraw);
	if (Result != EEquipmentResult::Success)
	{
		ClientRPC_EquipmentOperationFailed(Result);
	}
}

void UEquipmentManagerComponent::ServerRPC_RequestDraw_Implementation(FGameplayTag SheathSlotTag, UInventoryComponent* Inventory)
{
	if (Inventory && !IsOwnersInventory(Inventory))
	{
		UE_LOG(LogEQP, Warning, TEXT("ServerRPC_RequestDraw: %s named an inventory it does not own (%s)."),
			*GetNameSafe(GetOwner()), *GetNameSafe(Inventory->GetOwner()));
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::NotAuthorized);
		return;
	}
	const EEquipmentResult Result = DrawSheath(SheathSlotTag, Inventory);
	if (Result != EEquipmentResult::Success)
	{
		ClientRPC_EquipmentOperationFailed(Result);
	}
}

void UEquipmentManagerComponent::ServerRPC_RequestSheathe_Implementation(FGameplayTag SlotTag)
{
	const FEquipmentSlot* Target = FindSheatheTarget(SlotTag);
	if (!Target || !Target->bDrawn)
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::Failed);
		return;
	}
	SetDrawnInternal(*FindSlot(Target->SlotTag), false);
}

void UEquipmentManagerComponent::ServerRPC_RequestUnequipToInventory_Implementation(FGameplayTag SlotTag,
	UInventoryComponent* TargetInventory)
{
	if (!IsOwnersInventory(TargetInventory))
	{
		UE_LOG(LogEQP, Warning, TEXT("ServerRPC_RequestUnequipToInventory: %s named an inventory it does not own (%s)."),
			*GetNameSafe(GetOwner()), TargetInventory ? *GetNameSafe(TargetInventory->GetOwner()) : TEXT("none"));
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::NotAuthorized);
		return;
	}

	const EEquipmentResult Result = UnequipToInventory(SlotTag, TargetInventory);
	if (Result != EEquipmentResult::Success)
	{
		ClientRPC_EquipmentOperationFailed(Result);
	}
}

// ===========================================================================
// Client RPC
// ===========================================================================

void UEquipmentManagerComponent::ClientRPC_EquipmentOperationFailed_Implementation(EEquipmentResult Result)
{
	OnOperationFailed.Broadcast(Result);
}

// ===========================================================================
// Slot Finding & Validation
// ===========================================================================

FGameplayTag UEquipmentManagerComponent::FindTargetSlot(const FItemInstance& Item) const
{
	UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Item);
	if (!EquipFrag || !EquipFrag->EquipmentSlotTag.IsValid())
	{
		return FGameplayTag();
	}

	// A carried weapon goes into its sheath where this owner has one (D77); an owner without sheaths equips it as before.
	if (EquipFrag->SheathSlotTag.IsValid())
	{
		const FGameplayTag Sheath = FindSlotMatching(EquipFrag->SheathSlotTag);
		if (Sheath.IsValid())
		{
			return Sheath;
		}
	}
	return FindSlotMatching(EquipFrag->EquipmentSlotTag);
}

FGameplayTag UEquipmentManagerComponent::FindSlotMatching(FGameplayTag PreferredTag) const
{
	if (!PreferredTag.IsValid())
	{
		return FGameplayTag();
	}

	// Exact match: find empty slot with this tag
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.SlotTag == PreferredTag && !Slot.bIsOccupied)
		{
			return Slot.SlotTag;
		}
	}

	// Parent tag match: find first empty child slot
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.SlotTag.MatchesTag(PreferredTag) && !Slot.bIsOccupied)
		{
			return Slot.SlotTag;
		}
	}

	// All matching slots occupied — return first match (an equip from an inventory swaps; a direct equip is refused)
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.SlotTag == PreferredTag || Slot.SlotTag.MatchesTag(PreferredTag))
		{
			return Slot.SlotTag;
		}
	}

	return FGameplayTag();
}

void UEquipmentManagerComponent::ResolveSheathTarget(const FItemInstance& Item, FGameplayTag& InOutSlotTag, bool& bInOutDraw) const
{
	const UItemFragment_Equipment* EquipFrag = InOutSlotTag.IsValid() ? GetEquipmentFragment(Item) : nullptr;
	if (!EquipFrag || !EquipFrag->SheathSlotTag.IsValid() || IsSheathSlot(InOutSlotTag))
	{
		return;
	}
	// The item asked into a hand (the panel's Main Hand, an old save's main-hand entry) while it has a sheath for that hand.
	const FGameplayTag Sheath = FindSlotMatching(EquipFrag->SheathSlotTag);
	const FEquipmentSlot* SheathSlot = Sheath.IsValid() ? FindSlot(Sheath) : nullptr;
	if (SheathSlot && GetDrawHandFor(Item, Sheath) == InOutSlotTag)
	{
		InOutSlotTag = Sheath;
		bInOutDraw = true;
	}
}

EEquipmentResult UEquipmentManagerComponent::ValidateEquip(const FItemInstance& Item, FGameplayTag SlotTag) const
{
	if (!Item.IsValid())
	{
		return EEquipmentResult::InvalidItem;
	}

	UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Item);
	if (!EquipFrag)
	{
		return EEquipmentResult::InvalidItem;
	}

	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return EEquipmentResult::IncompatibleSlot;
	}

	// A hand slot an item in another hand blocks takes nothing (a torch beside a bow). A sheath for that hand takes its
	// item, which stays sheathed until the hand is free. A sheath is one place on the body and takes one item (D95): an
	// occupied one is a swap from the bag, a refusal without one (the callers).
	if (IsSlotBlocked(SlotTag))
	{
		return EEquipmentResult::HandBlocked;
	}

	// Check accepted item tags (if any are configured)
	if (Slot->AcceptedItemTags.Num() > 0)
	{
		UItemDatabaseSubsystem* DB = GetItemDatabase();
		if (DB)
		{
			UItemDefinition* Def = DB->GetDefinition(Item.ItemDefinitionId);
			if (Def && !Def->ItemTags.HasAny(Slot->AcceptedItemTags))
			{
				return EEquipmentResult::IncompatibleSlot;
			}
		}
	}

	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::ValidateEquipFromInventory(const FGuid& ItemInstanceId,
	const UInventoryComponent* SourceInventory, FGameplayTag& InOutSlotTag, FItemInstance& OutItem, bool& bInOutDraw) const
{
	if (!SourceInventory)
	{
		return EEquipmentResult::Failed;
	}

	const int32 SlotIndex = SourceInventory->FindSlotIndexByInstanceId(ItemInstanceId);
	if (SlotIndex == INDEX_NONE)
	{
		return EEquipmentResult::InvalidItem;
	}
	OutItem = SourceInventory->GetItemInSlot(SlotIndex);

	// Auto-detect the slot if not specified
	if (!InOutSlotTag.IsValid())
	{
		InOutSlotTag = FindTargetSlot(OutItem);
		if (!InOutSlotTag.IsValid())
		{
			return EEquipmentResult::IncompatibleSlot;
		}
	}
	else
	{
		ResolveSheathTarget(OutItem, InOutSlotTag, bInOutDraw);
	}

	const EEquipmentResult Result = ValidateEquip(OutItem, InOutSlotTag);
	if (Result != EEquipmentResult::Success)
	{
		return Result;
	}
	// The items it sends off a shared place go to the bag (D97): the bag must take them beside the swap.
	for (const FGameplayTag& Conflict : GetPlaceConflicts(OutItem, InOutSlotTag))
	{
		if (!SourceInventory->CanAcceptItem(GetEquippedItem(Conflict)))
		{
			return EEquipmentResult::NoInventorySpace;
		}
	}
	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::ValidateUnequipToInventory(FGameplayTag SlotTag,
	const UInventoryComponent* TargetInventory) const
{
	if (!TargetInventory)
	{
		return EEquipmentResult::Failed;
	}

	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return EEquipmentResult::Failed;
	}

	if (!TargetInventory->CanAcceptItem(Slot->EquippedItem))
	{
		return EEquipmentResult::NoInventorySpace;
	}

	return EEquipmentResult::Success;
}

bool UEquipmentManagerComponent::IsOwnersInventory(const UInventoryComponent* Inventory) const
{
	const AActor* Owner = GetOwner();
	const AActor* InventoryOwner = Inventory ? Inventory->GetOwner() : nullptr;
	if (!Owner || !InventoryOwner)
	{
		return false;
	}
	if (InventoryOwner == Owner)
	{
		return true;
	}
	// Another actor of the same player (its player state, say): the same connection.
	const UNetConnection* Connection = Owner->GetNetConnection();
	return Connection && InventoryOwner->GetNetConnection() == Connection;
}

bool UEquipmentManagerComponent::HasMutationAuthority() const
{
	const AActor* Owner = GetOwner();
	return !Owner || Owner->HasAuthority();
}

// ===========================================================================
// Internal Equip/Unequip
// ===========================================================================

void UEquipmentManagerComponent::Internal_Equip(const FItemInstance& Item, FGameplayTag SlotTag)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return;
	}

	// One item per hand: an item equipped into a hand slot sends the weapon drawn into that hand back to its sheath. The
	// hands are reconciled once the item is in (below): before that the hand looks free, and a shield would come back.
	if (FEquipmentSlot* Drawn = FindDrawnSheath(SlotTag))
	{
		TGuardValue<bool> Reconciling(bReconcilingHands, true);
		SetDrawnInternal(*Drawn, false);
	}

	Slot->EquippedItem = Item;
	Slot->bIsOccupied = true;
	if (Slot->DrawSlotTag.IsValid())
	{
		// A sheath draws into its item's hand (D95): the back holds a bow for the main hand or a shield for the off hand.
		Slot->DrawSlotTag = GetDrawHandFor(Item, SlotTag);
	}
	Slot->bDrawn = false; // a sheath's item arrives sheathed; TryDraw (or its lead, D96) takes it into the hand
	Slot->bAppliedDrawn = false;

	ApplyVisuals(Item, SlotTag);
	if (Slot->IsInHand())
	{
		ApplyGAS(Item, SlotTag);
	}

	OnItemEquipped.Broadcast(Item, SlotTag);
	OnEquipmentChanged.Broadcast();
	OnPostEquip(Item, SlotTag);
	if (GetLightSourceFragment(Item))
	{
		NotifyCarriedLight(*Slot);
	}
	ReconcileHands();
}

FItemInstance UEquipmentManagerComponent::Internal_Unequip(FGameplayTag SlotTag)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return FItemInstance();
	}

	FItemInstance UnequippedItem = Slot->EquippedItem;
	const bool bWasLight = GetLightSourceFragment(UnequippedItem) != nullptr;

	if (Slot->IsInHand())
	{
		RemoveGAS(SlotTag);
	}
	RemoveVisuals(SlotTag);

	Slot->EquippedItem = FItemInstance();
	Slot->bIsOccupied = false;
	if (Slot->DrawSlotTag.IsValid())
	{
		Slot->DrawSlotTag = GetDrawHandFor(FItemInstance(), SlotTag); // the sheath's own hand again
	}
	Slot->bDrawn = false;
	Slot->bAppliedDrawn = false;
	Slot->FuelAccumulatorSeconds = 0.f;

	OnItemUnequipped.Broadcast(UnequippedItem, SlotTag);
	OnEquipmentChanged.Broadcast();
	OnPostUnequip(UnequippedItem, SlotTag);
	if (bWasLight)
	{
		NotifyCarriedLight(*Slot);
	}
	ReconcileHands();

	return UnequippedItem;
}

// ===========================================================================
// Slot Lookup
// ===========================================================================

FEquipmentSlot* UEquipmentManagerComponent::FindSlot(FGameplayTag SlotTag)
{
	for (FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.SlotTag == SlotTag)
		{
			return &Slot;
		}
	}
	return nullptr;
}

const FEquipmentSlot* UEquipmentManagerComponent::FindSlot(FGameplayTag SlotTag) const
{
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.SlotTag == SlotTag)
		{
			return &Slot;
		}
	}
	return nullptr;
}

const FEquipmentSlotDefinition* UEquipmentManagerComponent::FindSlotDefinition(FGameplayTag SlotTag) const
{
	for (const FEquipmentSlotDefinition& Def : AvailableSlots)
	{
		if (Def.SlotTag == SlotTag)
		{
			return &Def;
		}
	}
	return nullptr;
}

// ===========================================================================
// Visuals
// ===========================================================================

void UEquipmentManagerComponent::ApplyVisuals(const FItemInstance& Item, FGameplayTag SlotTag)
{
	UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Item);
	if (!EquipFrag)
	{
		return;
	}

	// Determine which mesh to load
	TSoftObjectPtr<UObject> MeshToLoad;
	bool bIsSkeletal = false;

	if (!EquipFrag->EquipSkeletalMesh.IsNull())
	{
		MeshToLoad = TSoftObjectPtr<UObject>(EquipFrag->EquipSkeletalMesh.ToSoftObjectPath());
		bIsSkeletal = true;
	}
	else if (!EquipFrag->EquipMesh.IsNull())
	{
		MeshToLoad = TSoftObjectPtr<UObject>(EquipFrag->EquipMesh.ToSoftObjectPath());
	}
	else
	{
		return; // No visual — ability-only equipment
	}

	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return;
	}

	// Cancel any pending load
	if (Slot->MeshLoadHandle.IsValid())
	{
		Slot->MeshLoadHandle->CancelHandle();
	}

	FStreamableManager& Manager = UAssetManager::GetStreamableManager();
	Slot->MeshLoadHandle = Manager.RequestAsyncLoad(
		MeshToLoad.ToSoftObjectPath(),
		FStreamableDelegate::CreateUObject(this, &UEquipmentManagerComponent::OnMeshLoaded, SlotTag)
	);
}

void UEquipmentManagerComponent::OnMeshLoaded(FGameplayTag SlotTag)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return;
	}

	UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Slot->EquippedItem);
	if (!EquipFrag)
	{
		return;
	}

	USkeletalMeshComponent* OwnerMesh = GetOwnerMesh();
	if (!OwnerMesh)
	{
		return;
	}

	// Remove old visual if any
	if (Slot->AttachedVisualComponent)
	{
		Slot->AttachedVisualComponent->DestroyComponent();
		Slot->AttachedVisualComponent = nullptr;
	}

	const FName Socket = GetVisualAttachSocket(SlotTag); // the hand's bone or the sheath's, by the slot's state

	if (!EquipFrag->EquipSkeletalMesh.IsNull())
	{
		USkeletalMesh* SkelMesh = EquipFrag->EquipSkeletalMesh.Get();
		if (SkelMesh)
		{
			USkeletalMeshComponent* SkelComp = NewObject<USkeletalMeshComponent>(GetOwner());
			SkelComp->SetSkeletalMesh(SkelMesh);
			DisableVisualCollision(SkelComp);
			SkelComp->AttachToComponent(OwnerMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
			SkelComp->RegisterComponent();
			Slot->AttachedVisualComponent = SkelComp;
		}
	}
	else if (!EquipFrag->EquipMesh.IsNull())
	{
		UStaticMesh* StaticMesh = EquipFrag->EquipMesh.Get();
		if (StaticMesh)
		{
			UStaticMeshComponent* StaticComp = NewObject<UStaticMeshComponent>(GetOwner());
			StaticComp->SetStaticMesh(StaticMesh);
			DisableVisualCollision(StaticComp);
			StaticComp->AttachToComponent(OwnerMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
			StaticComp->RegisterComponent();
			Slot->AttachedVisualComponent = StaticComp;
		}
	}

	// Snap to the socket, then sit the mesh properly: item meshes are authored Z-up / +X forward, bones point along
	// the chain (QA F2-06: a helmet lay on its side at the neck, the chest piece was turned).
	if (Slot->AttachedVisualComponent)
	{
		Slot->AttachedVisualComponent->SetRelativeTransform(GetVisualAttachOffset(SlotTag));
		Slot->AttachedVisualItemId = Slot->EquippedItem.InstanceId;
		OnVisualAttached.Broadcast(SlotTag);
	}

	// Animation layer support: the item's stance layer while it is in the hand (a sheathed weapon links none)
	RefreshSlotLayer(*Slot);

	// A light source hangs its flame off the held visual (every machine).
	RefreshSlotLight(*Slot);
	if (Slot->AttachedLightComponent)
	{
		NotifyCarriedLight(*Slot);
	}
}

FTransform UEquipmentManagerComponent::GetVisualAttachOffset(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	return Slot ? GetVisualAttachOffsetForState(SlotTag, Slot->IsInHand()) : FTransform::Identity;
}

FTransform UEquipmentManagerComponent::GetVisualAttachOffsetForState(FGameplayTag SlotTag, bool bInHand) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return FTransform::Identity;
	}
	const UItemFragment_Equipment* EquipFrag = Slot->bIsOccupied ? GetEquipmentFragment(Slot->EquippedItem) : nullptr;
	if (Slot->DrawSlotTag.IsValid() && !bInHand)
	{
		// Sheathed: the item's sheath fit, then the sheath's skeleton correction (none on a bone of the item's choosing).
		if (EquipFrag && !EquipFrag->SheathBoneOverride.IsNone())
		{
			return EquipFrag->SheathOffset;
		}
		return EquipFrag ? EquipFrag->SheathOffset * Slot->AttachOffset : Slot->AttachOffset;
	}
	// In the hand: a sheath's item takes its hand slot's correction, as if it were equipped there.
	const FEquipmentSlot* Frame = Slot->DrawSlotTag.IsValid() ? FindSlot(Slot->DrawSlotTag) : Slot;
	const FTransform SlotCorrection = Frame ? Frame->AttachOffset : FTransform::Identity;
	if (EquipFrag && !EquipFrag->AttachBoneOverride.IsNone())
	{
		// On a bone of its own choosing the slot's correction (tuned for the slot's bone) does not apply.
		return EquipFrag->AttachOffset;
	}
	// Item fit first (expressed in the corrected frame), then the slot's skeleton correction.
	return EquipFrag ? EquipFrag->AttachOffset * SlotCorrection : SlotCorrection;
}

FName UEquipmentManagerComponent::ChooseAttachSocket(FName SlotSocket, const UItemFragment_Equipment* Fragment)
{
	return Fragment && !Fragment->AttachBoneOverride.IsNone() ? Fragment->AttachBoneOverride : SlotSocket;
}

FName UEquipmentManagerComponent::GetItemAttachBone(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	const UItemFragment_Equipment* EquipFrag = Slot && Slot->bIsOccupied ? GetEquipmentFragment(Slot->EquippedItem) : nullptr;
	if (!EquipFrag)
	{
		return NAME_None;
	}
	return Slot->IsInHand() ? EquipFrag->AttachBoneOverride : EquipFrag->SheathBoneOverride;
}

FName UEquipmentManagerComponent::GetVisualAttachSocket(FGameplayTag SlotTag) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	return Slot ? GetVisualAttachSocketForState(SlotTag, Slot->IsInHand()) : NAME_None;
}

FName UEquipmentManagerComponent::GetVisualAttachSocketForState(FGameplayTag SlotTag, bool bInHand) const
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return NAME_None;
	}
	const UItemFragment_Equipment* EquipFrag = Slot->bIsOccupied ? GetEquipmentFragment(Slot->EquippedItem) : nullptr;
	if (!Slot->DrawSlotTag.IsValid())
	{
		return ChooseAttachSocket(Slot->AttachSocket, EquipFrag);
	}
	if (bInHand)
	{
		const FEquipmentSlot* Hand = FindSlot(Slot->DrawSlotTag);
		return ChooseAttachSocket(Hand ? Hand->AttachSocket : Slot->AttachSocket, EquipFrag);
	}
	return EquipFrag && !EquipFrag->SheathBoneOverride.IsNone() ? EquipFrag->SheathBoneOverride : Slot->AttachSocket;
}

void UEquipmentManagerComponent::SetSlotAttachOffset(FGameplayTag SlotTag, const FTransform& Offset)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return;
	}
	Slot->AttachOffset = Offset;
	if (Slot->AttachedVisualComponent)
	{
		Slot->AttachedVisualComponent->SetRelativeTransform(GetVisualAttachOffset(SlotTag));
	}
}

void UEquipmentManagerComponent::RemoveVisuals(FGameplayTag SlotTag)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot)
	{
		return;
	}

	// Cancel pending mesh load
	if (Slot->MeshLoadHandle.IsValid())
	{
		Slot->MeshLoadHandle->CancelHandle();
		Slot->MeshLoadHandle.Reset();
	}

	if (Slot->AttachedLightComponent)
	{
		Slot->AttachedLightComponent->DestroyComponent();
		Slot->AttachedLightComponent = nullptr;
	}
	if (Slot->AttachedVisualComponent)
	{
		Slot->AttachedVisualComponent->DestroyComponent();
		Slot->AttachedVisualComponent = nullptr;
	}
	Slot->AttachedVisualItemId.Invalidate();

	// Unlink the layer the visual's item linked (after a swap the slot already holds another item, whose fragment names
	// another layer: G153); a slot that never linked one through OnMeshLoaded falls back to the item it holds, unless the
	// item is sheathed (it links none).
	TSubclassOf<UAnimInstance> LayerClass = Slot->AttachedAnimLayerClass;
	if (!LayerClass && Slot->bIsOccupied && Slot->IsInHand())
	{
		if (const UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Slot->EquippedItem))
		{
			LayerClass = EquipFrag->AnimLayerClass;
		}
	}
	Slot->AttachedAnimLayerClass = nullptr;
	if (LayerClass)
	{
		if (USkeletalMeshComponent* OwnerMesh = GetOwnerMesh())
		{
			OwnerMesh->UnlinkAnimClassLayers(LayerClass);
		}
	}
}

bool UEquipmentManagerComponent::IsVisualStale(const FEquipmentSlot& Slot)
{
	return Slot.bIsOccupied && Slot.AttachedVisualComponent && Slot.AttachedVisualItemId.IsValid()
		&& Slot.AttachedVisualItemId != Slot.EquippedItem.InstanceId;
}

USkeletalMeshComponent* UEquipmentManagerComponent::GetOwnerMesh() const
{
	if (!GetOwner())
	{
		return nullptr;
	}
	// A character's body mesh, not just the first skeletal mesh found: owners with extra skeletal meshes
	// (first-person arms, empty in third person) otherwise got the visual and it never showed (QA F2-04).
	if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (Character->GetMesh())
		{
			return Character->GetMesh();
		}
	}
	return GetOwner()->FindComponentByClass<USkeletalMeshComponent>();
}

// ===========================================================================
// GAS Helpers
// ===========================================================================

void UEquipmentManagerComponent::ApplyGAS(const FItemInstance& Item, FGameplayTag SlotTag)
{
	// Server-only — ASC replication handles clients
	if (!HasMutationAuthority())
	{
		return;
	}

	if (OnGASEquipCallback)
	{
		OnGASEquipCallback(Item, SlotTag);
	}
}

void UEquipmentManagerComponent::RemoveGAS(FGameplayTag SlotTag)
{
	if (!HasMutationAuthority())
	{
		return;
	}

	if (OnGASUnequipCallback)
	{
		OnGASUnequipCallback(SlotTag);
	}
}

// ===========================================================================
// Helpers
// ===========================================================================

// ===========================================================================
// Carried light (feature 7)
// ===========================================================================

float UEquipmentManagerComponent::LitIntensityFor(const UItemFragment_LightSource& Light, bool bHasFuelGauge, float FuelRemaining)
{
	// A light without a fuel gauge burns forever; one with a gauge goes dark at zero.
	return (!bHasFuelGauge || FuelRemaining > 0.f) ? Light.Intensity : 0.f;
}

UItemFragment_LightSource* UEquipmentManagerComponent::GetLightSourceFragment(const FItemInstance& Item) const
{
	if (!Item.IsValid())
	{
		return nullptr;
	}
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	const UItemDefinition* Def = DB ? DB->GetDefinition(Item.ItemDefinitionId) : nullptr;
	return Def ? Def->FindFragment<UItemFragment_LightSource>() : nullptr;
}

void UEquipmentManagerComponent::RefreshSlotLight(FEquipmentSlot& Slot)
{
	UItemFragment_LightSource* Light = Slot.bIsOccupied ? GetLightSourceFragment(Slot.EquippedItem) : nullptr;
	if (!Light)
	{
		if (Slot.AttachedLightComponent)
		{
			Slot.AttachedLightComponent->DestroyComponent();
			Slot.AttachedLightComponent = nullptr;
		}
		return;
	}

	USceneComponent* Parent = Slot.AttachedVisualComponent ? Slot.AttachedVisualComponent.Get() : static_cast<USceneComponent*>(GetOwnerMesh());
	if (!Parent)
	{
		return;
	}
	if (!Slot.AttachedLightComponent)
	{
		UPointLightComponent* Flame = NewObject<UPointLightComponent>(GetOwner());
		Flame->SetMobility(EComponentMobility::Movable);
		Flame->bUseInverseSquaredFalloff = false;
		Flame->SetCastShadows(Light->bCastShadows);
		Flame->SetLightColor(Light->Color);
		Flame->SetAttenuationRadius(Light->AttenuationRadius);
		Flame->SetIntensityUnits(ELightUnits::Lumens);
		// Hang off the held visual when there is one, else the hand socket itself.
		Flame->AttachToComponent(Parent, FAttachmentTransformRules::SnapToTargetNotIncludingScale,
			Slot.AttachedVisualComponent ? NAME_None : GetVisualAttachSocket(Slot.SlotTag));
		Flame->SetRelativeLocation(Light->LightOffset);
		Flame->RegisterComponent();
		Slot.AttachedLightComponent = Flame;
	}

	float Fuel = 0.f, MaxFuel = 0.f;
	const bool bHasGauge = GetDurability(Slot.SlotTag, Fuel, MaxFuel);
	const float Intensity = LitIntensityFor(*Light, bHasGauge, Fuel);
	Slot.AttachedLightComponent->SetIntensity(Intensity);
	Slot.AttachedLightComponent->SetVisibility(Intensity > 0.f);
}

void UEquipmentManagerComponent::NotifyCarriedLight(const FEquipmentSlot& Slot)
{
	float Fuel = 0.f, MaxFuel = 0.f;
	const UItemFragment_LightSource* Light = Slot.bIsOccupied ? GetLightSourceFragment(Slot.EquippedItem) : nullptr;
	bool bLit = false;
	if (Light)
	{
		const bool bHasGauge = GetDurability(Slot.SlotTag, Fuel, MaxFuel);
		bLit = LitIntensityFor(*Light, bHasGauge, Fuel) > 0.f;
	}
	OnCarriedLightChanged.Broadcast(Slot.SlotTag, bLit, Fuel, MaxFuel);
}

bool UEquipmentManagerComponent::GetCarriedLight(FGameplayTag& OutSlotTag, float& OutFuel, float& OutMaxFuel) const
{
	OutSlotTag = FGameplayTag();
	OutFuel = 0.f;
	OutMaxFuel = 0.f;
	float Best = 0.f;
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		const UItemFragment_LightSource* Light = Slot.bIsOccupied ? GetLightSourceFragment(Slot.EquippedItem) : nullptr;
		if (!Light)
		{
			continue;
		}
		float Fuel = 0.f, MaxFuel = 0.f;
		const bool bHasGauge = GetDurability(Slot.SlotTag, Fuel, MaxFuel);
		const float Intensity = LitIntensityFor(*Light, bHasGauge, Fuel);
		if (Intensity > Best)
		{
			Best = Intensity;
			OutSlotTag = Slot.SlotTag;
			OutFuel = Fuel;
			OutMaxFuel = MaxFuel;
		}
	}
	return Best > 0.f;
}

float UEquipmentManagerComponent::GetCarriedLightLevel() const
{
	float Best = 0.f;
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		const UItemFragment_LightSource* Light = Slot.bIsOccupied ? GetLightSourceFragment(Slot.EquippedItem) : nullptr;
		if (!Light)
		{
			continue;
		}
		float Fuel = 0.f, MaxFuel = 0.f;
		const bool bHasGauge = GetDurability(Slot.SlotTag, Fuel, MaxFuel);
		Best = FMath::Max(Best, LitIntensityFor(*Light, bHasGauge, Fuel) / 1000.f);
	}
	return Best;
}

bool UEquipmentManagerComponent::ConsumeCarriedLightFuel(float Seconds)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || Seconds <= 0.f)
	{
		return false;
	}
	FGameplayTag SlotTag;
	float Fuel = 0.f, MaxFuel = 0.f;
	if (!GetCarriedLight(SlotTag, Fuel, MaxFuel) || MaxFuel <= 0.f)
	{
		return false; // nothing lit, or a light that needs no fuel
	}
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	const UItemFragment_LightSource* Light = Slot ? GetLightSourceFragment(Slot->EquippedItem) : nullptr;
	const float Burn = Light ? Light->FuelBurnPerSecond : 1.f;
	ApplyDurabilityLoss(SlotTag, Seconds * Burn);
	return true;
}

void UEquipmentManagerComponent::TickFuel()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	// Collect first: ApplyDurabilityLoss may unequip (burn-out) and touch the slot array.
	TArray<TPair<FGameplayTag, float>> Deductions;
	for (FEquipmentSlot& Slot : EquipmentSlots)
	{
		const UItemFragment_LightSource* Light = Slot.bIsOccupied ? GetLightSourceFragment(Slot.EquippedItem) : nullptr;
		if (!Light || Light->FuelBurnPerSecond <= 0.f)
		{
			continue;
		}
		float Fuel = 0.f, MaxFuel = 0.f;
		if (!GetDurability(Slot.SlotTag, Fuel, MaxFuel) || Fuel <= 0.f)
		{
			continue; // no gauge (burns forever) or already out
		}
		Slot.FuelAccumulatorSeconds += FuelTimerInterval;
		if (Slot.FuelAccumulatorSeconds + KINDA_SMALL_NUMBER >= Light->FuelStepSeconds)
		{
			Deductions.Emplace(Slot.SlotTag, Slot.FuelAccumulatorSeconds * Light->FuelBurnPerSecond);
			Slot.FuelAccumulatorSeconds = 0.f;
		}
	}
	for (const TPair<FGameplayTag, float>& Deduction : Deductions)
	{
		ApplyDurabilityLoss(Deduction.Key, Deduction.Value);
	}
}

UItemDatabaseSubsystem* UEquipmentManagerComponent::GetItemDatabase() const
{
	if (!CachedItemDatabase)
	{
		UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
		if (GI)
		{
			CachedItemDatabase = GI->GetSubsystem<UItemDatabaseSubsystem>();
		}
	}
	return CachedItemDatabase;
}

UItemFragment_Equipment* UEquipmentManagerComponent::GetEquipmentFragment(const FItemInstance& Item) const
{
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	if (!DB)
	{
		return nullptr;
	}

	UItemDefinition* Def = DB->GetDefinition(Item.ItemDefinitionId);
	if (!Def)
	{
		return nullptr;
	}

	return Def->FindFragment<UItemFragment_Equipment>();
}
