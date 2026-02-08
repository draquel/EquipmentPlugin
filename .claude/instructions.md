# EquipmentPlugin — Plugin Instructions

## Purpose

Manages equipping and unequipping items, applying stat modifiers, granting abilities via GAS, and attaching visual meshes. Works with items from ItemInventoryPlugin but should function at a basic level independently (equipping raw equipment definitions without inventory integration).

Depends on CommonGameFramework. Optional dependency on ItemInventoryPlugin.

## Module Structure

```
EquipmentPlugin/
├── Source/
│   ├── EquipmentSystem/                   ← Runtime: core equipment logic
│   │   ├── Public/
│   │   │   ├── Components/
│   │   │   │   └── EquipmentManagerComponent.h    ← Manages equipped items per actor
│   │   │   ├── Data/
│   │   │   │   └── EquipmentDefinition.h          ← Data asset or fragment defining equip behavior
│   │   │   ├── Types/
│   │   │   │   └── EquipmentSystemTypes.h         ← FEquipmentSlot, FEquipmentChangePayload
│   │   │   └── EquipmentSystem.h
│   │   └── Private/
│   │       └── (mirrors Public)
│   └── EquipmentGASIntegration/           ← Optional module: GAS ability/effect granting
│       ├── Public/
│       │   ├── EquipmentAbilityGranter.h          ← Handles granting/revoking abilities
│       │   └── EquipmentEffectApplier.h           ← Handles applying/removing gameplay effects
│       └── Private/
│           └── (mirrors Public)
└── EquipmentPlugin.uplugin
```

Two modules: `EquipmentSystem` (no GAS dependency) and `EquipmentGASIntegration` (depends on GAS). Projects not using GAS only load EquipmentSystem. The split ensures compilation doesn't fail if GAS modules aren't present.

## Key Classes

### UEquipmentManagerComponent

```cpp
UCLASS(BlueprintType, meta = (BlueprintSpawnableComponent))
class EQUIPMENTSYSTEM_API UEquipmentManagerComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    // --- Configuration ---
    // Available equipment slots on this actor
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Equipment|Config")
    TArray<FEquipmentSlotDefinition> AvailableSlots;

    // --- Operations ---
    UFUNCTION(BlueprintCallable, Category = "Equipment|Operations")
    EEquipmentResult TryEquip(const FItemInstance& Item);

    UFUNCTION(BlueprintCallable, Category = "Equipment|Operations")
    EEquipmentResult TryEquipToSlot(const FItemInstance& Item, FGameplayTag SlotTag);

    UFUNCTION(BlueprintCallable, Category = "Equipment|Operations")
    EEquipmentResult TryUnequip(FGameplayTag SlotTag);

    UFUNCTION(BlueprintCallable, Category = "Equipment|Operations")
    EEquipmentResult TrySwapEquipment(FGameplayTag SlotA, FGameplayTag SlotB);

    // --- Queries ---
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
    FItemInstance GetEquippedItem(FGameplayTag SlotTag) const;

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
    bool IsSlotOccupied(FGameplayTag SlotTag) const;

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
    TArray<FGameplayTag> GetAllOccupiedSlots() const;

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Equipment|Query")
    bool CanEquipItem(const FItemInstance& Item) const;

    // --- Events ---
    UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
    FOnItemEquipped OnItemEquipped;

    UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
    FOnItemUnequipped OnItemUnequipped;

    UPROPERTY(BlueprintAssignable, Category = "Equipment|Events")
    FOnEquipmentChanged OnEquipmentChanged;  // Catch-all

    // --- Extension Points ---
    // Called after equip — override in BP or subclass to add custom behavior
    UFUNCTION(BlueprintNativeEvent, Category = "Equipment")
    void OnPostEquip(const FItemInstance& Item, FGameplayTag SlotTag);

    UFUNCTION(BlueprintNativeEvent, Category = "Equipment")
    void OnPostUnequip(const FItemInstance& Item, FGameplayTag SlotTag);

protected:
    // Replicated equipment state
    UPROPERTY(ReplicatedUsing = OnRep_EquipmentSlots)
    TArray<FEquipmentSlot> EquipmentSlots;

    UFUNCTION()
    void OnRep_EquipmentSlots();

    // Server RPCs
    UFUNCTION(Server, Reliable)
    void ServerRPC_RequestEquip(const FItemInstance& Item, FGameplayTag SlotTag);

    UFUNCTION(Server, Reliable)
    void ServerRPC_RequestUnequip(FGameplayTag SlotTag);
};
```

### FEquipmentSlotDefinition

```cpp
USTRUCT(BlueprintType)
struct FEquipmentSlotDefinition
{
    GENERATED_BODY()

    // Slot identity (Equipment.Slot.Head, etc.)
    UPROPERTY(EditAnywhere, BlueprintReadOnly)
    FGameplayTag SlotTag;

    // What items can go in this slot (matches against item tags)
    UPROPERTY(EditAnywhere, BlueprintReadOnly)
    FGameplayTagContainer AcceptedItemTags;

    // Display name for UI
    UPROPERTY(EditAnywhere, BlueprintReadOnly)
    FText SlotDisplayName;

    // Attachment socket on the skeletal mesh (for visual attachment)
    UPROPERTY(EditAnywhere, BlueprintReadOnly)
    FName AttachSocket;
};
```

### FEquipmentSlot (Runtime)

```cpp
USTRUCT(BlueprintType)
struct FEquipmentSlot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly)
    FGameplayTag SlotTag;

    UPROPERTY(BlueprintReadOnly)
    FItemInstance EquippedItem;  // Invalid FItemInstance if empty

    UPROPERTY()
    bool bIsOccupied = false;

    // Handles for cleanup
    TArray<FActiveGameplayEffectHandle> AppliedEffectHandles;
    TArray<FGameplayAbilitySpecHandle> GrantedAbilityHandles;
    TObjectPtr<USceneComponent> AttachedVisualComponent;
};
```

### Equip/Unequip Flow

**Equip:**
1. Validate: Is the slot available? Does the item have an Equipment fragment? Does the item's slot tag match the target slot? Does the item pass the slot's tag filter?
2. If a source inventory is involved: validate that unequipping the current item (if slot is occupied) can go back to inventory, and that the new item can be removed from inventory
3. Execute:
   a. If slot is occupied, unequip current item first (put back in inventory or drop)
   b. Set the item in the equipment slot
   c. Read the Equipment fragment for visual mesh data → spawn and attach to socket
   d. If GAS integration module is loaded: grant abilities, apply effects
   e. Fire OnItemEquipped delegate

**Unequip:**
1. Validate: Is the slot occupied? If returning to inventory, is there space?
2. Execute:
   a. If GAS: revoke abilities (by handle), remove effects (by handle)
   b. Destroy/detach visual mesh component
   c. Return item to inventory (or drop as world item, or destroy — configurable)
   d. Clear the equipment slot
   e. Fire OnItemUnequipped delegate

### GAS Integration (EquipmentGASIntegration Module)

This is a separate module so the core EquipmentSystem doesn't require GAS. The integration module provides:

**UEquipmentAbilityGranter** — A helper class that the EquipmentManagerComponent delegates to when GAS is available:
```cpp
UCLASS()
class EQUIPMENTGASINTEGRATION_API UEquipmentAbilityGranter : public UObject
{
    GENERATED_BODY()
public:
    // Grant abilities from equipment fragment to the ASC
    TArray<FGameplayAbilitySpecHandle> GrantAbilities(
        UAbilitySystemComponent* ASC,
        const TArray<TSubclassOf<UGameplayAbility>>& Abilities);

    // Revoke previously granted abilities
    void RevokeAbilities(
        UAbilitySystemComponent* ASC,
        const TArray<FGameplayAbilitySpecHandle>& Handles);

    // Apply passive effects
    TArray<FActiveGameplayEffectHandle> ApplyEffects(
        UAbilitySystemComponent* ASC,
        const TArray<TSubclassOf<UGameplayEffect>>& Effects,
        float Level = 1.0f);

    // Remove applied effects
    void RemoveEffects(
        UAbilitySystemComponent* ASC,
        const TArray<FActiveGameplayEffectHandle>& Handles);
};
```

**How the modules connect**: EquipmentManagerComponent checks at runtime if the GAS integration module is loaded. If yes, it creates a UEquipmentAbilityGranter and delegates ability/effect management to it. If no, it skips those steps. The check uses `FModuleManager::Get().IsModuleLoaded("EquipmentGASIntegration")`.

**What the Equipment fragment on items provides for GAS:**
```cpp
// This lives in ItemInventoryPlugin as a definition fragment
UCLASS()
class UItemFragment_Equipment : public UItemDefinitionFragment
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category = "Equipment")
    FGameplayTag EquipmentSlotTag;

    UPROPERTY(EditAnywhere, Category = "Equipment|Visuals")
    TSoftObjectPtr<UStaticMesh> EquipMesh;

    UPROPERTY(EditAnywhere, Category = "Equipment|Visuals")
    TSoftObjectPtr<USkeletalMesh> EquipSkeletalMesh;

    UPROPERTY(EditAnywhere, Category = "Equipment|Visuals")
    TSubclassOf<UAnimInstance> AnimLayerClass;

    UPROPERTY(EditAnywhere, Category = "Equipment|GAS")
    TArray<TSubclassOf<UGameplayAbility>> GrantedAbilities;

    UPROPERTY(EditAnywhere, Category = "Equipment|GAS")
    TArray<TSubclassOf<UGameplayEffect>> PassiveEffects;

    UPROPERTY(EditAnywhere, Category = "Equipment|GAS")
    TArray<TSubclassOf<UGameplayEffect>> OnEquipEffects;  // Applied once on equip
};
```

### Visual Attachment System

The EquipmentManagerComponent handles basic visual attachment:

1. Read mesh reference from equipment fragment (StaticMesh or SkeletalMesh)
2. Async load the mesh
3. Create a UStaticMeshComponent or USkeletalMeshComponent
4. Attach to the specified socket on the owning actor's mesh
5. Store the component reference in FEquipmentSlot for cleanup on unequip

For more complex visual systems (modular character with mesh merging, material parameter changes, etc.), the `OnPostEquip` BlueprintNativeEvent is the extension point. Games override it to implement their specific visual logic.

## Multiplayer

Same pattern as InventoryComponent: server-authoritative with RPCs.

Equipment state replicates via the EquipmentSlots array. Visual attachment happens on all clients via OnRep — when the replicated slot data changes, each client spawns/destroys the visual components locally.

GAS ability/effect granting happens only on the server. The ASC replication handles pushing ability state to clients.

## Build.cs Dependencies

```csharp
// EquipmentSystem (Runtime)
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core", "CoreUObject", "Engine", "NetCore",
    "GameplayTags",
    "CommonGameFramework",
});

// Optional: only if item integration is desired
PrivateDependencyModuleNames.AddRange(new string[]
{
    "ItemSystem",  // For FItemInstance, equipment fragment
});

// EquipmentGASIntegration
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core", "CoreUObject", "Engine",
    "GameplayAbilities", "GameplayTags", "GameplayTasks",
    "CommonGameFramework",
    "EquipmentSystem",
});
```

## Implementation Phase

**Phase 8: Equipment Core**
1. FEquipmentSlotDefinition and FEquipmentSlot types
2. UEquipmentManagerComponent — slots, equip/unequip, validation
3. Replication — replicated slots, RPCs, OnRep
4. Visual attachment (basic static/skeletal mesh)
5. OnPostEquip/OnPostUnequip extension points
6. Integration with InventoryComponent (equip from inventory, unequip to inventory)
7. Test without GAS first

**Phase 9: GAS Integration**
1. UEquipmentAbilityGranter
2. UEquipmentEffectApplier (if separated from granter)
3. Wire into EquipmentManagerComponent's equip/unequip flow
4. Test: equip weapon → ability granted → unequip → ability revoked
5. Test: equip armor → passive stat effect applied → unequip → effect removed
6. Test with multiplayer: verify abilities replicate correctly via ASC
