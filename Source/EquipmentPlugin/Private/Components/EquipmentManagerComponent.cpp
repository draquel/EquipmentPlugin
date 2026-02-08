#include "Components/EquipmentManagerComponent.h"
#include "Components/InventoryComponent.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Data/ItemDefinition.h"
#include "Data/Fragments/ItemFragment_Equipment.h"
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

	if (!GetOwner()->HasAuthority())
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

	if (!GetOwner()->HasAuthority())
	{
		ServerRPC_RequestUnequip(SlotTag);
		return EEquipmentResult::Success;
	}

	OutItem = Internal_Unequip(SlotTag);
	return EEquipmentResult::Success;
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

	if (!GetOwner()->HasAuthority())
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

	if (!GetOwner()->HasAuthority())
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
}

FItemInstance UEquipmentManagerComponent::Internal_Unequip(FGameplayTag SlotTag)
{
	FEquipmentSlot* Slot = FindSlot(SlotTag);
	if (!Slot || !Slot->bIsOccupied)
	{
		return FItemInstance();
	}

	FItemInstance UnequippedItem = Slot->EquippedItem;

	RemoveGAS(SlotTag);
	RemoveVisuals(SlotTag);

	Slot->EquippedItem = FItemInstance();
	Slot->bIsOccupied = false;

	OnItemUnequipped.Broadcast(UnequippedItem, SlotTag);
	OnEquipmentChanged.Broadcast();
	OnPostUnequip(UnequippedItem, SlotTag);

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
	if (!GetOwner()->HasAuthority())
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
	if (!GetOwner()->HasAuthority())
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
