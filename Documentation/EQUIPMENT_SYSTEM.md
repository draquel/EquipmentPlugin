# Equipment System — Detailed Design

## Overview

This document covers the internal design of the EquipmentPlugin: the equipment manager component, the equip/unequip flow, visual attachment, and the GAS integration module. For how this plugin integrates with ItemInventoryPlugin and InteractionPlugin, see `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md`.

---

## Equipment Slot Model

### Slot Configuration vs Runtime State

There are two structs that work together:

**FEquipmentSlotDefinition** (configuration, design-time):
Defined in CommonGameFramework. Set on the EquipmentManagerComponent in the editor. Describes what slots exist and what they accept.

**FEquipmentSlot** (runtime state):
Created from definitions during `BeginPlay`. Holds the currently equipped item and cleanup handles.

```
EquipmentManagerComponent (on APlayerCharacter)
├── AvailableSlots (config):
│   ├── { SlotTag=Equipment.Slot.Head,     AcceptedTags={Item.Category.Armor}, Socket=head_socket }
│   ├── { SlotTag=Equipment.Slot.Chest,    AcceptedTags={Item.Category.Armor}, Socket=NAME_None }
│   ├── { SlotTag=Equipment.Slot.MainHand, AcceptedTags={Item.Category.Weapon}, Socket=hand_r_socket }
│   └── { SlotTag=Equipment.Slot.OffHand,  AcceptedTags={Item.Category.Weapon,Item.Category.Armor}, Socket=hand_l_socket }
│
└── EquipmentSlots (runtime, replicated):
    ├── { SlotTag=Head,     EquippedItem=ID_Helm_Iron,  bIsOccupied=true,  VisualComp=ptr, AbilityHandles=[...] }
    ├── { SlotTag=Chest,    EquippedItem=Invalid,        bIsOccupied=false }
    ├── { SlotTag=MainHand, EquippedItem=ID_Sword_Iron, bIsOccupied=true,  VisualComp=ptr, AbilityHandles=[...] }
    └── { SlotTag=OffHand,  EquippedItem=Invalid,        bIsOccupied=false }
```

### Slot Matching

When `TryEquip(Item)` is called without a specific slot:

```
1. Read item's Equipment fragment → get EquipmentSlotTag
2. Find matching EquipmentSlot by tag
3. If tag is a parent tag (e.g., Equipment.Slot.Accessory), find first empty child slot
   (Accessory1, Accessory2)
4. If no empty matching slot: return SlotOccupied
5. Validate item tags against slot's AcceptedItemTags
```

`TryEquipToSlot(Item, SlotTag)` skips step 1-3 and goes directly to the specified slot.

---

## Equip Flow (Detailed)

### Without Inventory Integration

The simplest path — equip an item directly without managing an inventory source:

```
TryEquip(Item):
    1. FindTargetSlot(Item) → SlotTag, or fail
    2. ValidateEquip(Item, SlotTag):
       a. Item.IsValid()
       b. Item has Equipment fragment
       c. Slot exists in AvailableSlots
       d. Item tags pass slot's AcceptedItemTags filter
    3. If slot is occupied:
       a. Call Internal_Unequip(SlotTag)  → fires unequip, clears visuals/abilities
    4. Internal_Equip(Item, SlotTag):
       a. Set EquipmentSlot.EquippedItem = Item
       b. Set EquipmentSlot.bIsOccupied = true
       c. ApplyVisuals(Item, SlotTag)
       d. ApplyGAS(Item, SlotTag)  — if GAS module available
       e. Fire OnItemEquipped(Item, SlotTag)
       f. Mark slot dirty for replication
```

### With Inventory Integration

When equipping from inventory and unequipping back to inventory:

```
TryEquipFromInventory(ItemInstanceId, InventoryComponent, SlotTag):
    1. Find item in inventory by InstanceId
    2. ValidateEquip(Item, SlotTag)
    3. If slot is occupied:
       a. Check InventoryComponent has space for the currently equipped item
       b. If no space → return NoInventorySpace
       c. Internal_Unequip(SlotTag) → item goes to InventoryComponent
    4. Remove item from InventoryComponent (TryRemoveItem)
    5. Internal_Equip(Item, SlotTag)

TryUnequipToInventory(SlotTag, InventoryComponent):
    1. Validate slot is occupied
    2. Validate InventoryComponent can accept the item (space, weight, tags)
    3. Internal_Unequip(SlotTag) → extracts item
    4. Add item to InventoryComponent (TryAddItem)
    5. If add fails (shouldn't after validation) → re-equip, log error
```

The inventory operations are atomic — validation before execution, rollback on unexpected failure. Same pattern as cross-inventory moves.

---

## Visual Attachment

### Basic Attachment Flow

```
ApplyVisuals(Item, SlotTag):
    1. Get UItemDefinition from ItemDatabaseSubsystem
    2. Find UItemFragment_Equipment on definition
    3. Determine mesh type:
       a. If EquipSkeletalMesh is set → use skeletal mesh component
       b. Else if EquipMesh is set → use static mesh component
       c. Else → no visual (ability-only equipment)
    4. Async load the mesh
    5. On load complete:
       a. Create mesh component (UStaticMeshComponent or USkeletalMeshComponent)
       b. Get AttachSocket from FEquipmentSlotDefinition
       c. AttachToComponent(OwnerMesh, AttachSocket)
       d. Store component reference in FEquipmentSlot.AttachedVisualComponent
```

```
RemoveVisuals(SlotTag):
    1. Get FEquipmentSlot for SlotTag
    2. If AttachedVisualComponent exists:
       a. DetachFromComponent
       b. DestroyComponent
       c. Clear reference
```

### Animation Layer Support

If `UItemFragment_Equipment` specifies an `AnimLayerClass`:

```
On equip:
    OwnerMesh->LinkAnimClassLayers(AnimLayerClass)

On unequip:
    OwnerMesh->UnlinkAnimClassLayers(AnimLayerClass)
```

This allows weapons to override animation layers (e.g., sword idle vs bow idle) without the equipment system knowing about specific animations.

### Client-Side Visual Sync

Visuals are applied on all clients via `OnRep_EquipmentSlots`:

```cpp
void UEquipmentManagerComponent::OnRep_EquipmentSlots()
{
    // Compare old and new state, apply/remove visuals for changes
    for (FEquipmentSlot& Slot : EquipmentSlots)
    {
        if (Slot.bIsOccupied && !Slot.AttachedVisualComponent)
        {
            // Newly equipped on server, apply visuals locally
            ApplyVisuals(Slot.EquippedItem, Slot.SlotTag);
        }
        else if (!Slot.bIsOccupied && Slot.AttachedVisualComponent)
        {
            // Unequipped on server, remove visuals locally
            RemoveVisuals(Slot.SlotTag);
        }
    }

    OnEquipmentChanged.Broadcast();
}
```

Each client independently loads meshes and creates visual components based on the replicated equipment state. The server never sends mesh component references over the network — only the item data replicates.

---

## GAS Integration

### Module Architecture

The GAS integration is a separate module (`EquipmentGASIntegration`) that the core `EquipmentSystem` module detects at runtime:

```
EquipmentManagerComponent:
    BeginPlay:
        if FModuleManager::Get().IsModuleLoaded("EquipmentGASIntegration"):
            AbilityGranter = NewObject<UEquipmentAbilityGranter>()
            bGASAvailable = true
```

When `bGASAvailable` is false, `ApplyGAS()` and `RemoveGAS()` are no-ops. The core equipment flow (slot management, visuals) works identically without GAS.

### Ability Granting

```
ApplyGAS(Item, SlotTag):
    1. Get UItemFragment_Equipment from item definition
    2. Get ASC from owning actor (FindComponentByClass<UAbilitySystemComponent>)
    3. If no ASC → warn and return

    // Grant abilities
    4. For each TSubclassOf<UGameplayAbility> in Fragment.GrantedAbilities:
       a. FGameplayAbilitySpec Spec(AbilityClass, 1, INDEX_NONE, this)
       b. Handle = ASC->GiveAbility(Spec)
       c. Store Handle in EquipmentSlot.GrantedAbilityHandles

    // Apply passive effects (permanent while equipped)
    5. For each TSubclassOf<UGameplayEffect> in Fragment.PassiveEffects:
       a. FGameplayEffectContextHandle Context = ASC->MakeEffectContext()
       b. Context.AddSourceObject(this)
       c. FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, 1, Context)
       d. Handle = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data)
       e. Store Handle in EquipmentSlot.AppliedEffectHandles

    // Apply one-time equip effects (e.g., "on equip, gain 10 shield")
    6. For each TSubclassOf<UGameplayEffect> in Fragment.OnEquipEffects:
       a. Apply same as above but do NOT store handle (one-time, not removed on unequip)
```

### Ability Revocation

```
RemoveGAS(SlotTag):
    1. Get FEquipmentSlot
    2. Get ASC from owning actor

    // Revoke abilities
    3. For each Handle in EquipmentSlot.GrantedAbilityHandles:
       a. ASC->ClearAbility(Handle)
    4. Clear GrantedAbilityHandles array

    // Remove passive effects
    5. For each Handle in EquipmentSlot.AppliedEffectHandles:
       a. ASC->RemoveActiveGameplayEffect(Handle)
    6. Clear AppliedEffectHandles array
```

**Critical:** Handles MUST be stored and used for removal. Never try to remove abilities/effects by class — if multiple equipment pieces grant the same ability class, removing by class would remove all of them. Handle-based removal is surgical.

### GAS Execution Context

GAS operations happen **only on the server** (or standalone). The ASC's built-in replication handles pushing ability and effect state to clients. The equipment system should never call `GiveAbility` or `ApplyGameplayEffectSpec` on a client — these are server-authoritative operations.

```
Server: TryEquip → Internal_Equip → ApplyGAS (grants abilities, applies effects)
        ASC replicates ability/effect state to clients automatically

Client: OnRep_EquipmentSlots → ApplyVisuals only (no GAS calls)
```

### Stat Modification via Effects

Equipment stats (armor, damage bonus, speed) are applied as `UGameplayEffect` instances with infinite duration. Example:

```
GE_IronHelm_Passive (GameplayEffect asset):
    Duration: Infinite
    Modifiers:
        - Attribute: Health.Armor, Operation: Add, Magnitude: 15
        - Attribute: Health.MaxHealth, Operation: Add, Magnitude: 25
```

When the Iron Helm is equipped, this effect is applied to the ASC. When unequipped, it's removed via the stored handle. The attribute values update automatically through GAS's modifier system.

---

## Multiplayer Flow

### Equip Sequence (Multiplayer)

```
CLIENT                                   SERVER
──────                                   ──────
1. Player drags sword to MainHand slot
   (or presses equip button)

2. UI calls EquipmentManager              
   ->TryEquip(SwordInstance)              

3. TryEquip checks HasAuthority()         
   → false (client)                       

4. Sends ServerRPC_RequestEquip ────────► 5. Server receives RPC
   (SwordInstance, SlotTag)                  
                                          6. Validates:
                                             • Item is valid
                                             • Slot exists and accepts this item
                                             • If from inventory: item exists in inventory
                                             • If slot occupied: space for old item
                                          
                                          7. Executes:
                                             • Removes from inventory (if applicable)
                                             • Sets equipment slot
                                             • Grants abilities via GAS
                                             • Applies effects via GAS
                                          
                                          8. Equipment slot marked dirty for replication
                                          
9. OnRep_EquipmentSlots fires ◄───────── Replication push
   
10. Client applies visuals                
    (mesh load, socket attach)            
    
11. UI updates from OnEquipmentChanged    
    delegate                              
```

### Conflict Resolution

If two RPCs arrive simultaneously (e.g., player tries to equip two items to the same slot):
- Server processes RPCs sequentially (UE's RPC ordering guarantee per connection)
- First equip succeeds
- Second equip finds slot occupied, triggers swap-or-reject logic
- Results replicate back in order

---

## Extension Points

### OnPostEquip / OnPostUnequip

`BlueprintNativeEvent` functions called after the core equip/unequip logic completes. C++ default implementation is empty. Games override these for:

- **Modular character systems:** Swap mesh sections instead of socket attachment
- **Material parameter changes:** Tint character mesh based on equipped armor
- **Sound effects:** Play equip/unequip audio
- **Camera adjustments:** Zoom or offset camera for large weapons
- **UI state changes:** Update character preview, stat display

### Custom Slot Validation

Override `CanEquipItem` (BlueprintNativeEvent) for game-specific rules:

```
// Example: Level requirement
bool MyEquipmentManager::CanEquipItem_Implementation(const FItemInstance& Item) const
{
    UItemDefinition* Def = GetDefinition(Item);
    int32 RequiredLevel = Def->FindFragment<UMyLevelFragment>()->RequiredLevel;
    return PlayerLevel >= RequiredLevel;
}
```

### Equipment Sets

Not built into the plugin, but the extension pattern for "wearing 3 pieces of the same set grants a bonus":

```
Game-layer code binds to OnEquipmentChanged.
On change: scan all equipped items for set tags.
Count matching tags.
If threshold met: apply set bonus effect via ASC.
```

The plugin provides the events and query functions; set logic is game-specific.
