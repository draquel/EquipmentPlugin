#include "Components/EquipmentManagerComponent.h"
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
#include "Engine/AssetManager.h"
#include "Net/UnrealNetwork.h"

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
			if (Slot.bIsOccupied)
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

	// Create runtime slots from definitions
	EquipmentSlots.Reset();
	for (const FEquipmentSlotDefinition& Def : AvailableSlots)
	{
		FEquipmentSlot Slot;
		Slot.SlotTag = Def.SlotTag;
		Slot.AttachSocket = Def.AttachSocket;
		Slot.AcceptedItemTags = Def.AcceptedItemTags;
		Slot.bIsOccupied = false;
		EquipmentSlots.Add(Slot);
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
		else if (Slot.bIsOccupied && Slot.AttachedLightComponent)
		{
			// Same item, possibly less fuel: the light and the HUD follow the replicated durability.
			RefreshSlotLight(Slot);
			NotifyCarriedLight(Slot);
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
	EEquipmentResult ValidationResult = ValidateEquip(Item, SlotTag);
	if (ValidationResult != EEquipmentResult::Success)
	{
		return ValidationResult;
	}

	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		ServerRPC_RequestEquip(Item, SlotTag);
		return EEquipmentResult::Success; // Optimistic
	}

	// If slot is occupied, auto-unequip first
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (Slot && Slot->bIsOccupied)
	{
		Internal_Unequip(SlotTag);
	}

	Internal_Equip(Item, SlotTag);
	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::TryUnequip(FGameplayTag SlotTag, FItemInstance& OutItem)
{
	const FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return EEquipmentResult::Failed;
	}

	if (GetOwner() && !GetOwner()->HasAuthority())
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
	UInstanceFragment_DurabilityState* State = Slot->EquippedItem.FindFragment<UInstanceFragment_DurabilityState>();
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	const UItemDefinition* Def = DB ? DB->GetDefinition(Slot->EquippedItem.ItemDefinitionId) : nullptr;
	const UItemFragment_Durability* Durability = Def ? Def->FindFragment<UItemFragment_Durability>() : nullptr;
	if (!State || !Durability)
	{
		return -1.f;
	}

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
	UInventoryComponent* SourceInventory, FGameplayTag SlotTag)
{
	if (!SourceInventory)
	{
		return EEquipmentResult::Failed;
	}

	// Find item in inventory
	int32 SlotIndex = SourceInventory->FindSlotIndexByInstanceId(ItemInstanceId);
	if (SlotIndex == INDEX_NONE)
	{
		return EEquipmentResult::InvalidItem;
	}

	FItemInstance Item = SourceInventory->GetItemInSlot(SlotIndex);

	// Auto-detect slot if not specified
	if (!SlotTag.IsValid())
	{
		SlotTag = FindTargetSlot(Item);
		if (!SlotTag.IsValid())
		{
			return EEquipmentResult::IncompatibleSlot;
		}
	}

	EEquipmentResult ValidationResult = ValidateEquip(Item, SlotTag);
	if (ValidationResult != EEquipmentResult::Success)
	{
		return ValidationResult;
	}

	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		ServerRPC_RequestEquipFromInventory(ItemInstanceId, SourceInventory, SlotTag);
		return EEquipmentResult::Success;
	}

	// If slot is occupied, check that inventory can accept the old item
	FEquipmentSlot* ExistingSlot = FindSlot(SlotTag);
	if (ExistingSlot && ExistingSlot->bIsOccupied)
	{
		if (!SourceInventory->CanAcceptItem(ExistingSlot->EquippedItem))
		{
			return EEquipmentResult::NoInventorySpace;
		}

		// Unequip old item back to inventory
		FItemInstance OldItem = Internal_Unequip(SlotTag);
		EInventoryOperationResult AddResult = SourceInventory->TryAddItem(OldItem);
		if (AddResult != EInventoryOperationResult::Success)
		{
			// Rollback: re-equip old item
			Internal_Equip(OldItem, SlotTag);
			return EEquipmentResult::NoInventorySpace;
		}
	}

	// Remove item from inventory
	EInventoryOperationResult RemoveResult = SourceInventory->TryRemoveItem(ItemInstanceId);
	if (RemoveResult != EInventoryOperationResult::Success)
	{
		return EEquipmentResult::Failed;
	}

	Internal_Equip(Item, SlotTag);
	return EEquipmentResult::Success;
}

EEquipmentResult UEquipmentManagerComponent::TryUnequipToInventory(FGameplayTag SlotTag,
	UInventoryComponent* TargetInventory)
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

	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		ServerRPC_RequestUnequipToInventory(SlotTag, TargetInventory);
		return EEquipmentResult::Success;
	}

	FItemInstance UnequippedItem = Internal_Unequip(SlotTag);

	EInventoryOperationResult AddResult = TargetInventory->TryAddItem(UnequippedItem);
	if (AddResult != EInventoryOperationResult::Success)
	{
		// Rollback: re-equip
		Internal_Equip(UnequippedItem, SlotTag);
		UE_LOG(LogTemp, Error, TEXT("EquipmentManager: Failed to add unequipped item to inventory after validation passed."));
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

void UEquipmentManagerComponent::ServerRPC_RequestEquip_Implementation(const FItemInstance& Item,
	FGameplayTag SlotTag)
{
	EEquipmentResult Result = ValidateEquip(Item, SlotTag);
	if (Result != EEquipmentResult::Success)
	{
		ClientRPC_EquipmentOperationFailed(Result);
		return;
	}

	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (Slot && Slot->bIsOccupied)
	{
		Internal_Unequip(SlotTag);
	}

	Internal_Equip(Item, SlotTag);
}

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
	UInventoryComponent* SourceInventory, FGameplayTag SlotTag)
{
	if (!SourceInventory)
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::Failed);
		return;
	}

	int32 SlotIndex = SourceInventory->FindSlotIndexByInstanceId(ItemInstanceId);
	if (SlotIndex == INDEX_NONE)
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::InvalidItem);
		return;
	}

	FItemInstance Item = SourceInventory->GetItemInSlot(SlotIndex);

	if (!SlotTag.IsValid())
	{
		SlotTag = FindTargetSlot(Item);
	}

	EEquipmentResult Result = ValidateEquip(Item, SlotTag);
	if (Result != EEquipmentResult::Success)
	{
		ClientRPC_EquipmentOperationFailed(Result);
		return;
	}

	FEquipmentSlot* ExistingSlot = FindSlot(SlotTag);
	if (ExistingSlot && ExistingSlot->bIsOccupied)
	{
		if (!SourceInventory->CanAcceptItem(ExistingSlot->EquippedItem))
		{
			ClientRPC_EquipmentOperationFailed(EEquipmentResult::NoInventorySpace);
			return;
		}

		FItemInstance OldItem = Internal_Unequip(SlotTag);
		SourceInventory->TryAddItem(OldItem);
	}

	SourceInventory->TryRemoveItem(ItemInstanceId);
	Internal_Equip(Item, SlotTag);
}

void UEquipmentManagerComponent::ServerRPC_RequestUnequipToInventory_Implementation(FGameplayTag SlotTag,
	UInventoryComponent* TargetInventory)
{
	if (!TargetInventory)
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::Failed);
		return;
	}

	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::Failed);
		return;
	}

	if (!TargetInventory->CanAcceptItem(Slot->EquippedItem))
	{
		ClientRPC_EquipmentOperationFailed(EEquipmentResult::NoInventorySpace);
		return;
	}

	FItemInstance UnequippedItem = Internal_Unequip(SlotTag);
	TargetInventory->TryAddItem(UnequippedItem);
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

	FGameplayTag PreferredTag = EquipFrag->EquipmentSlotTag;

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

	// All matching slots occupied — return first match (will trigger swap)
	for (const FEquipmentSlot& Slot : EquipmentSlots)
	{
		if (Slot.SlotTag == PreferredTag || Slot.SlotTag.MatchesTag(PreferredTag))
		{
			return Slot.SlotTag;
		}
	}

	return FGameplayTag();
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

	Slot->EquippedItem = Item;
	Slot->bIsOccupied = true;

	ApplyVisuals(Item, SlotTag);
	ApplyGAS(Item, SlotTag);

	OnItemEquipped.Broadcast(Item, SlotTag);
	OnEquipmentChanged.Broadcast();
	OnPostEquip(Item, SlotTag);
	if (GetLightSourceFragment(Item))
	{
		NotifyCarriedLight(*Slot);
	}
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

	RemoveGAS(SlotTag);
	RemoveVisuals(SlotTag);

	Slot->EquippedItem = FItemInstance();
	Slot->bIsOccupied = false;
	Slot->FuelAccumulatorSeconds = 0.f;

	OnItemUnequipped.Broadcast(UnequippedItem, SlotTag);
	OnEquipmentChanged.Broadcast();
	OnPostUnequip(UnequippedItem, SlotTag);
	if (bWasLight)
	{
		NotifyCarriedLight(*Slot);
	}

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

	FName Socket = Slot->AttachSocket;

	if (!EquipFrag->EquipSkeletalMesh.IsNull())
	{
		USkeletalMesh* SkelMesh = EquipFrag->EquipSkeletalMesh.Get();
		if (SkelMesh)
		{
			USkeletalMeshComponent* SkelComp = NewObject<USkeletalMeshComponent>(GetOwner());
			SkelComp->SetSkeletalMesh(SkelMesh);
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
			StaticComp->AttachToComponent(OwnerMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
			StaticComp->RegisterComponent();
			Slot->AttachedVisualComponent = StaticComp;
		}
	}

	// Animation layer support
	if (EquipFrag->AnimLayerClass)
	{
		OwnerMesh->LinkAnimClassLayers(EquipFrag->AnimLayerClass);
	}

	// A light source hangs its flame off the held visual (every machine).
	RefreshSlotLight(*Slot);
	if (Slot->AttachedLightComponent)
	{
		NotifyCarriedLight(*Slot);
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

	// Unlink animation layers if applicable
	if (Slot->bIsOccupied)
	{
		UItemFragment_Equipment* EquipFrag = GetEquipmentFragment(Slot->EquippedItem);
		if (EquipFrag && EquipFrag->AnimLayerClass)
		{
			USkeletalMeshComponent* OwnerMesh = GetOwnerMesh();
			if (OwnerMesh)
			{
				OwnerMesh->UnlinkAnimClassLayers(EquipFrag->AnimLayerClass);
			}
		}
	}
}

USkeletalMeshComponent* UEquipmentManagerComponent::GetOwnerMesh() const
{
	if (!GetOwner())
	{
		return nullptr;
	}
	return GetOwner()->FindComponentByClass<USkeletalMeshComponent>();
}

// ===========================================================================
// GAS Helpers
// ===========================================================================

void UEquipmentManagerComponent::ApplyGAS(const FItemInstance& Item, FGameplayTag SlotTag)
{
	// Server-only — ASC replication handles clients
	if (GetOwner() && !GetOwner()->HasAuthority())
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
	if (GetOwner() && !GetOwner()->HasAuthority())
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
			Slot.AttachedVisualComponent ? NAME_None : Slot.AttachSocket);
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
