# Equipment System

> State as of 2026-11-02 (code review CR5).

## Overview

The design of EquipmentPlugin: the equipment manager component and its slots, the equip and unequip flows with and
without an inventory, visual attachment, durability, carried light, the GAS module, replication and the equipment UI.
How the plugin fits with ItemInventoryPlugin and InteractionPlugin is in
`Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md`.

Two modules:
- `EquipmentPlugin` (runtime, log category `LogEQP`): `UEquipmentManagerComponent`, `FEquipmentSlot`
  (`Types/EquipmentSystemTypes.h`), the slot and panel widgets. No GAS dependency.
- `EquipmentGASIntegration` (runtime, log category `LogEQPGAS`): `UEquipmentAbilityGranter`, `UEquipmentEffectApplier`,
  `UEquipmentGASSettings`. It registers itself with the core module at startup; without it equipment works with no
  abilities or effects.

Both depend on ItemInventoryPlugin, privately and required: the manager reads `UItemFragment_Equipment`,
`UItemFragment_Durability` and `UItemFragment_LightSource` through `UItemDatabaseSubsystem`, equips from and unequips to
a `UInventoryComponent`, and the slot widget shows `UItemTooltipWidget` (D-CR22, FW-05).

---

## Equipment Slot Model

### Slot Configuration vs Runtime State

**`FEquipmentSlotDefinition`** (CommonGameFramework, configuration): `SlotTag`, `AcceptedItemTags`, `SlotDisplayName`,
`AttachSocket`, `AttachOffset`. Set in `UEquipmentManagerComponent::AvailableSlots` on the owner.

**`FEquipmentSlot`** (runtime state, replicated): created from the definitions in `BeginPlay` on the authority; a client
keeps the array the server sent, which arrives before its `BeginPlay` with whatever is equipped (G102: rebuilding it on
the client emptied the slots of an actor that first replicated with items equipped, and the server never resent them).
Holds `SlotTag`,
`EquippedItem`, `bIsOccupied`, copies of `AttachSocket`, `AttachOffset` and `AcceptedItemTags`, and, not replicated, the
attached visual component, its load handle, the attached light and the fuel accumulator. GAS handles are kept by the GAS
module per slot, not in the slot.

```
UEquipmentManagerComponent (on the character)
├── AvailableSlots (config):
│   ├── { SlotTag=Equipment.Slot.Head,     AcceptedItemTags={Item.Category.Armor},  AttachSocket=head_socket }
│   ├── { SlotTag=Equipment.Slot.MainHand, AcceptedItemTags={Item.Category.Weapon}, AttachSocket=hand_r_socket }
│   └── ...
└── EquipmentSlots (runtime, replicated):
    ├── { SlotTag=Head,     EquippedItem=<helmet>, bIsOccupied=true,  AttachedVisualComponent=... }
    └── { SlotTag=MainHand, EquippedItem=<empty>,  bIsOccupied=false }
```

### Slot Matching

`TryEquip(Item)` picks the slot with `FindTargetSlot(Item)` from the item's `UItemFragment_Equipment::EquipmentSlotTag`:

```
1. an empty slot with exactly that tag
2. else an empty slot whose tag is a child of it (Equipment.Slot.Accessory → Accessory1, Accessory2)
3. else the first slot with that tag or a child tag, occupied (an equip from an inventory swaps; a direct equip is
   refused with SlotOccupied)
4. else none → IncompatibleSlot
```

`TryEquipToSlot(Item, SlotTag)` skips the search.

### Validation

`ValidateEquip(Item, SlotTag)` returns `EEquipmentResult`: `InvalidItem` (invalid instance or no Equipment fragment),
`IncompatibleSlot` (no such slot, or `AcceptedItemTags` set and the item's tags match none of them), else `Success`.
`CanEquipItem(Item)` is the same check on the slot `FindTargetSlot` picks (`BlueprintPure`). The inventory flows wrap it
in `ValidateEquipFromInventory` (the item is in the inventory, the slot resolved) and `ValidateUnequipToInventory` (the
slot holds an item the inventory can take): the `Try*` function runs the predicate on a client before it sends the
request, and the server runs it again (code review decision D-CR16).

---

## Equip Flow

### Without an Inventory

```
TryEquip(Item) → FindTargetSlot → TryEquipToSlot(Item, SlotTag):      authority only
    client: NotAuthorized (no request carries an item: G91)
    ValidateEquip; failure → its result
    slot occupied → SlotOccupied (no inventory to take the old item: G90)
    Internal_Equip(Item, SlotTag)

TryUnequip(SlotTag, OutItem): the slot must be occupied; Internal_Unequip; the item comes back in OutItem
    (on a client: ServerRPC_RequestUnequip, and the item stays with the server)
```

**`Internal_Equip`:** set the slot's item and occupied flag, `ApplyVisuals`, `ApplyGAS` (authority only), broadcast
`OnItemEquipped` and `OnEquipmentChanged`, call `OnPostEquip`, and for a light-source item announce the carried light.

**`Internal_Unequip`:** `RemoveGAS` (authority only), `RemoveVisuals`, clear the slot, broadcast `OnItemUnequipped` and
`OnEquipmentChanged`, call `OnPostUnequip`, announce a light going out; returns the item.

### With an Inventory

```
TryEquipFromInventory(ItemInstanceId, SourceInventory, SlotTag):
    ValidateEquipFromInventory: the item in the inventory (InvalidItem when absent); SlotTag empty → FindTargetSlot;
        ValidateEquip
    client: ServerRPC_RequestEquipFromInventory(ItemInstanceId, SourceInventory, SlotTag), returns Success
    server (EquipFromInventory, also the RPC's body after it checks the inventory is the requester's):
        ValidateEquipFromInventory again
        slot occupied → SourceInventory->TryReplaceItem(ItemInstanceId, old item): the old item takes the new one's
            inventory slot in one validated step (refused → NoInventorySpace, nothing changed); Internal_Unequip
        slot empty → SourceInventory->TryRemoveItem(ItemInstanceId); a failure returns Failed with nothing changed
        Internal_Equip

TryUnequipToInventory(SlotTag, TargetInventory):
    ValidateUnequipToInventory: the slot occupied (Failed), CanAcceptItem (NoInventorySpace)
    client: ServerRPC_RequestUnequipToInventory
    server (UnequipToInventory): the predicate again; Internal_Unequip → TargetInventory->TryAddItem; on an
        unexpected failure re-equip and log an error
```

Validation comes before any change, as for cross-inventory moves, and every inventory result is read: nothing is
dropped (G90). A swap needs no free inventory slot, since the replaced item takes the slot the new one leaves; a full
bag can still swap a sword for an axe.

---

## Visual Attachment

```
ApplyVisuals(Item, SlotTag):
    1. Equipment fragment from the item definition
    2. EquipSkeletalMesh set → skeletal mesh component; else EquipMesh → static mesh component;
       neither → no visual (ability-only equipment)
    3. Async load the mesh (FStreamableManager), handle kept on the slot
    4. On load (OnMeshLoaded): create the component with collision off, attach it to the owner's body mesh
       (a character's GetMesh(), not the first skeletal mesh found) at the slot's AttachSocket,
       SetRelativeTransform(GetVisualAttachOffset(SlotTag)), link AnimLayerClass if set, broadcast OnVisualAttached

RemoveVisuals(SlotTag): cancel the load, unlink AnimLayerClass, detach and destroy the component, drop the light
```

**Collision off.** Visuals are cosmetic and are created with the `NoCollision` profile, no overlaps and no navigation
effect. Item meshes carry simple collision, and colliding visuals on the bones launched the owner's death ragdoll through
the terrain (QA F1-04).

**Attach offsets.** `GetVisualAttachOffset(SlotTag)` is the item's fit composed with the slot's correction:
`UItemFragment_Equipment::AttachOffset * FEquipmentSlot::AttachOffset`. The slot's offset (from
`FEquipmentSlotDefinition::AttachOffset`) corrects for the skeleton: mannequin head and spine bones point X up the chain
with Y forward, while item meshes are authored Z-up with +X forward, so the character sets a rotation (and a lift for the
helmet rim) per slot. The item's offset is the per-item fit on top (the leather armour scales up a little). Owners that
re-attach visuals themselves, such as a first-person socket swap, apply `GetVisualAttachOffset` again after attaching.
`SetSlotAttachOffset` changes a slot's correction at runtime and re-applies it (how the offsets were tuned in PIE).

**Animation layers.** An Equipment fragment's `AnimLayerClass` is linked on the owner mesh with `LinkAnimClassLayers`
when the visual attaches and unlinked when it is removed, so a weapon can swap animation layers without the equipment
system knowing the animations.

### Client-Side Visual Sync

`EquipmentSlots` replicate with `OnRep_EquipmentSlots`. Each client compares the replicated slots with what it has
attached: an occupied slot without a visual gets `ApplyVisuals`, an empty slot with one gets `RemoveVisuals`, an occupied
light slot refreshes its light from the replicated durability. Then `OnEquipmentChanged` fires. Only item data
replicates; every client loads meshes and builds components itself.

---

## Durability

`GetDurability(SlotTag, OutCurrent, OutMax)` reads the equipped item's `UInstanceFragment_DurabilityState` against its
definition's `UItemFragment_Durability`. `ApplyDurabilityLoss(SlotTag, Amount)` (authority) lowers it, broadcasts
`OnDurabilityChanged(SlotTag, Current, Max)`, and at zero with `bDestroyAtZero` unequips the item without returning it
anywhere and broadcasts `OnItemBroken(SlotTag, Item)`. Callers decide what wears an item (VoxelCharacterPlugin's melee
ability applies the weapon's `DegradeRate` per hit).

## Carried Light

An equipped item whose definition has a `UItemFragment_LightSource` gets a `UPointLightComponent` hung off its held
visual (`FEquipmentSlot::AttachedLightComponent`, on every machine, built in `OnMeshLoaded` / `RefreshSlotLight`). The
fuel is the item's Durability fragment: an authority timer (`TickFuel`, every 1 s) accumulates burn time per lit slot
and calls `ApplyDurabilityLoss` every `FuelStepSeconds`, so the replicated durability changes a few times a minute. At
zero the light goes dark (`LitIntensityFor`), and with `bDestroyAtZero` the item burns out through `OnItemBroken`. A
light item without a Durability fragment burns forever.

Queries: `GetCarriedLightLevel()` (lumens / 1000, 0 = dark), `GetCarriedLight(SlotTag, Fuel, MaxFuel)`, and the
authority `ConsumeCarriedLightFuel(Seconds)` (lighting a sconce). `OnCarriedLightChanged(SlotTag, bLit, FuelSeconds,
MaxFuel)` fires on every machine on equip, unequip and fuel change (the HUD binds here). The character implements
`ICGFLightBearerInterface` (CommonGameFramework) on top of these.

---

## GAS Integration

### How the Modules Connect

The core module never names a GAS type. `UEquipmentManagerComponent` declares a static
`GASSetupFactory` (`TFunction<void(UEquipmentManagerComponent*)>`); `FEquipmentGASIntegrationModule::StartupModule`
sets it and `ShutdownModule` clears it. In `BeginPlay` every manager calls the factory when it is set, which:

- creates a `UEquipmentAbilityGranter` and a `UEquipmentEffectApplier` owned by the manager (held in
  `GASAbilityGranter` / `GASEffectApplier` as `UObject`s for GC);
- binds `OnGASEquipCallback`: resolve the owner's ASC, `GrantAbilities`, `ApplyEffects`;
- binds `OnGASUnequipCallback`: `RevokeAbilities`, `RemoveEffects`.

`ApplyGAS` / `RemoveGAS` call those callbacks on the authority only; without the GAS module they are not bound and the
equipment flow is otherwise identical. The ASC is found with
`FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent(Owner)` (`UAbilitySystemGlobals`, so an
`IAbilitySystemInterface` owner whose ASC lives on the player state is found, with a component search as fallback).

### Abilities

`UEquipmentAbilityGranter::GrantAbilities(Item, SlotTag, ASC)` gives each class in the fragment's `GrantedAbilities` and
keeps the spec handles per slot (with the ASC they were granted on). `RevokeAbilities(SlotTag, ASC)` clears those
handles; a null ASC falls back to the one recorded, which matters when the owner is a pawn already unpossessed on its way
to destruction (the player state's ASC outlives it).

### Effects

`UEquipmentEffectApplier::ApplyEffects(Item, SlotTag, ASC)`:
- `PassiveEffects`: applied to self with the equipment manager as source object; handles kept per slot and removed on
  unequip.
- `StatModifiers` (`TArray<FCGFAttributeModifier>`, e.g. Defense +2, MaxHealth +10): applied through **one**
  source-defined effect class, `UEquipmentGASSettings::StatModifierEffectClass` (Project Settings > Plugins > Equipment
  GAS). That class declares an additive SetByCaller modifier per supported attribute keyed
  `SetByCaller.Stat.<AttributeName>`; `UCGFGameplayEffectStatics::ApplyStatModifierEffect` zeroes every key and fills the
  requested ones. The handle is tracked and removed like a passive effect. The game supplies the class because only it
  knows its attribute sets (VoxelCharacterPlugin ships `UVCEquipmentStatEffect`); without one the stats are skipped with
  a warning. A transient runtime effect is not used for lasting stats: active effects replicate to the owning client by
  definition reference, which a runtime object cannot satisfy.
- `OnEquipEffects`: applied once, not tracked, not removed on unequip.

`RemoveEffects(SlotTag, ASC)` removes the tracked handles, with the same ASC fallback as abilities.

**Handles, never classes.** Removal is by handle: two pieces granting the same class would both lose it if removal went
by class.

**Server only.** Abilities and effects are granted on the authority; the ASC replicates them. Clients only attach
visuals.

---

## Multiplayer

```
CLIENT                                   SERVER
──────                                   ──────
1. UI calls TryEquipFromInventory
2. ValidateEquipFromInventory passes;
   no authority
3. ServerRPC_RequestEquipFromInventory ► 4. The inventory must be the requester's (on its pawn, or
   (InstanceId, Inventory, SlotTag)         another actor of its connection: NotAuthorized); the
                                            predicate again; a refusal goes back as
                                            ClientRPC_EquipmentOperationFailed → OnOperationFailed
                                         5. The old item takes the new one's inventory slot (or the
                                            new one just leaves it), Internal_Equip (visuals, GAS)
                                         6. EquipmentSlots and the inventory replicate
7. OnRep_EquipmentSlots: visuals attach,
   OnEquipmentChanged → UI
```

The request names an item by instance id; the server equips the item it holds under that id, so a client cannot equip
an item it does not have (G91). `TryEquip` / `TryEquipToSlot` take a whole item and are server code only (a client gets
`NotAuthorized`). RPCs from one connection arrive in order, so two equips to the same slot run one after the other: the
second finds the slot occupied and swaps the first back into the inventory.

---

## UI Widgets

Code-built like ItemInventoryPlugin's widgets: the tree is made in `NativeOnInitialized()` with `WidgetTree`, no UMG
Designer assets.

### UEquipmentSlotWidget

`Public/UI/EquipmentSlotWidget.h`. One slot: the item icon (or empty) with the slot name underneath ("Main Hand"), and a
`UItemTooltipWidget` for the equipped item. `InitSlot(EquipmentManager, SlotTag)`; `RefreshSlot()` follows the
manager's changes and durability.

- `NativeOnMouseButtonDown`: left click → `OnSlotClicked(SlotTag, EquipmentManager)`, right click →
  `OnSlotRightClicked`; both `FOnEquipmentSlotClicked(FGameplayTag SlotTag, UEquipmentManagerComponent* EquipmentManager)`.
- `SetHeld(bool)`: gold `(0.8, 0.6, 0.2, 0.9)` while held by click-to-move, default `(0.08, 0.08, 0.12, 0.9)`.
- The icon starts `Collapsed`; `RefreshSlot()` makes it `SelfHitTestInvisible` while an item is equipped, so clicks reach
  the slot.

### UEquipmentPanelWidget

`Public/UI/EquipmentPanelWidget.h`. A vertical panel of every slot in the manager's `AvailableSlots`, created in
`InitPanel()`. It relays each child's click delegates on its own `OnSlotClicked` / `OnSlotRightClicked` (the same relay
pattern as `UHotbarWidget` and `UInventoryPanelWidget`); `SetSlotHeld(SlotTag, bool)` highlights the grabbed slot;
`RefreshAllSlots()`.

### Click-to-Move Integration

VoxelCharacterPlugin's `AVCPlayerController` binds the panel's delegates in `BindSlotClickDelegates()` (after
`ShowInventoryPanels()` creates the inventory and equipment panels):

| Source → Target | Operation |
|-----------------|-----------|
| Inventory slot → equipment slot | `TryEquipFromInventory(InstanceId, Inventory, SlotTag)` |
| Equipment slot → inventory slot | `TryUnequipToInventory(SlotTag, Inventory)`, then `TrySwapSlots` to the clicked slot |
| Equipment → equipment | cancelled (not supported) |

`TryUnequipToInventory` puts the item in the first slot it fits; the swap then moves it where the player clicked.

---

## Console Commands

`Equipment.Equip <DefName> <SlotTag>` (creates the item and equips it to the player's empty slot; server or standalone)
and `Equipment.UnequipAll`,
registered in `EquipmentPlugin.cpp`; `Documentation/EDITOR_CONSOLE_GUIDE.md` lists them with every project command.

## Tests

`Equipment.*`: `Queries.*` (`Private/Tests/Test_EquipmentQueries.cpp`), `Equip.Results` (the equip results),
`Equip.FromInventorySwap` (a swap from a full bag keeps both items; a refused swap changes nothing),
`Authority.ClientRequests` (a client's direct equip refused; its equip request delivered to the server as an instance
id, simulated server/client with `CGFTesting::FRPCCall`; another player's inventory refused),
`Durability.EmptySlotHasNone`, `LightSource.IntensityRule` / `NothingCarried`, and
`GAS.ResolvesAbilitySystemThroughInterface` in the GAS module. VoxelCharacterPlugin's `VoxelCharacter.EquipmentLoop.*` tests cover
the stat effect end to end. The equip flow above over a real connection (a dedicated server and a client in PIE: the
request, the server's checks, the replicated slot and `OnRep_EquipmentSlots`, a rack's sword at its first replication)
is the game module's `VoxelEngine.Replication.Framework`, run by the in-editor gate.

---

## Extension Points

**`OnPostEquip` / `OnPostUnequip`** (`BlueprintNativeEvent`, empty in C++) run after the core equip and unequip. Games
override them for modular meshes instead of socket attachment, material tints, sounds, camera changes, or a character
preview.

**Custom slot rules.** `CanEquipItem` and `ValidateEquip` are not virtual. A rule such as a level requirement goes in the
game's code before it calls `TryEquip*`, or as tags: give the slot `AcceptedItemTags` and tag the items.

**Equipment sets.** Not built in. Bind `OnEquipmentChanged`, scan the equipped items' tags, and apply a set bonus effect
through the ASC when a threshold is met.
