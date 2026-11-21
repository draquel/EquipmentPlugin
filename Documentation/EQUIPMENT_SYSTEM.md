# Equipment System

> State as of 2026-11-11 (blocked hands, G175; sheaths, the equipment sheathing plan S1, 2026-11-08; before it code review CR5, 2026-11-02).

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
`AttachSocket`, `AttachOffset`, `DrawSlotTag` (set on a sheath: "Sheaths" below). Set in
`UEquipmentManagerComponent::AvailableSlots` on the owner.

**`FEquipmentSlot`** (runtime state, replicated): created from the definitions in `BeginPlay` on the authority; a client
keeps the array the server sent, which arrives before its `BeginPlay` with whatever is equipped (G102: rebuilding it on
the client emptied the slots of an actor that first replicated with items equipped, and the server never resent them).
Holds `SlotTag`,
`EquippedItem`, `bIsOccupied`, copies of `AttachSocket`, `AttachOffset`, `AcceptedItemTags` and `DrawSlotTag`, a sheath's
`bDrawn`, and, not replicated, the attached visual component, its load handle, the attached light, the fuel accumulator
and the drawn state this machine last acted on (`bAppliedDrawn`). GAS handles are kept by the GAS
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

`TryEquip(Item)` picks the slot with `FindTargetSlot(Item)` from the item's `UItemFragment_Equipment::SheathSlotTag` when
the owner has such a sheath, else from its `EquipmentSlotTag` ("Sheaths" below), each matched the same way:

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
       (a character's GetMesh(), not the first skeletal mesh found) at GetVisualAttachSocket(SlotTag): the item's
       AttachBoneOverride, else the slot's AttachSocket,
       SetRelativeTransform(GetVisualAttachOffset(SlotTag)), remember the item's id (AttachedVisualItemId) and the
       layer (AttachedAnimLayerClass) on the slot, link AnimLayerClass if set, broadcast OnVisualAttached

RemoveVisuals(SlotTag): cancel the load, unlink the layer the visual's item linked (else the held item's), detach and
destroy the component, drop the light
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

**An item's own bone.** An Equipment fragment's `AttachBoneOverride` attaches the item to another bone than its slot's
(a bow equipped in the main hand rides `hand_l`): `ChooseAttachSocket` is the rule, `GetVisualAttachSocket` the bone a
slot's visual uses, `GetItemAttachBone` the item's choice (None when it takes the slot's), and a torch's flame follows the
same bone when there is no visual. On its own bone the slot's correction (tuned for the slot's bone) is not applied:
`GetVisualAttachOffset` is the item's `AttachOffset` alone. Owners that re-attach visuals keep the item's bone in every
view (VoxelCharacterPlugin's `UpdateEquipmentAttachments`, which also composes the owner's per-body correction for the
bone after the fit: an item is fitted once to the Mannequin's hand, and a body shaped differently corrects every item on
that bone in its appearance asset; the character refinement plan's D71). Test: `Equipment.Visuals.AttachBoneOverride`.

**Animation layers.** An Equipment fragment's `AnimLayerClass` is linked on the owner mesh with `LinkAnimClassLayers`
when the visual attaches and unlinked when it is removed, so a weapon can swap animation layers without the equipment
system knowing the animations.

### Client-Side Visual Sync

`EquipmentSlots` replicate with `OnRep_EquipmentSlots`. Each client compares the replicated slots with what it has
attached: an occupied slot without a visual gets `ApplyVisuals`, an empty slot with one gets `RemoveVisuals`, an occupied
slot whose visual was built for another item (`IsVisualStale`: the slot's `AttachedVisualItemId` is not its item's id)
gets both, since a hotbar swap is an unequip and an equip in one server frame and reaches the client as one replication
of an occupied slot holding another item (G153, 2026-11-08; before it the old mesh, its fit and its layer stayed under the
new action set until the slot emptied), and an occupied
light slot refreshes its light from the replicated durability (sent when it changes: "Durability" below, G136). Then
`OnEquipmentChanged` fires. Only item data replicates; every client loads meshes and builds components itself. Test:
`Equipment.Visuals.StaleAfterSwap`.

---

## Sheaths

Weapons carried on the body and drawn into the hand when used (the equipment sheathing plan, 2026-11-08, D77-D82;
`EQUIPMENT_SHEATHING_PLAN.md`).

**The model (D77).** A sheath is a slot whose definition names a hand, `DrawSlotTag` (Equipment.Slot.MainHand for the
character's `Equipment.Slot.Sheath.HipLeft`, `.Back`, `.Belt`). The item names its sheath and its fit there in its
Equipment fragment (ItemInventoryPlugin): `SheathSlotTag` (a sheath or a parent of several), `SheathOffset`,
`SheathBoneOverride`; `EquipmentSlotTag`, `AttachOffset` and `AttachBoneOverride` stay the hand it is used in and its fit
there. The item lives in the sheath, always: drawn is the sheath slot's replicated `bDrawn`, so an item has one home and
nothing is reserved or moved by a draw. `FEquipmentSlot::IsInHand()` is true for an ordinary slot's item and a drawn
sheath's.

- **Where an equip lands.** `FindTargetSlot` picks the item's sheath where the owner has one, else its
  `EquipmentSlotTag` slot, so an owner without sheaths (the NPCs, another game) equips as before. An equip of a sheathable
  item asked into its sheath's hand (the panel's Main Hand, an old save's main-hand entry) goes into the sheath, drawn
  (`ResolveSheathTarget`, in `TryEquipToSlot` and `ValidateEquipFromInventory`). A sheath's item arrives sheathed.
- **Draw and sheathe.** `TryDraw(SheathSlotTag, Inventory)` and `TrySheathe(SlotTag)` (the hand, or the sheath itself), each a request from a
  client (`ServerRPC_RequestDraw`, `ServerRPC_RequestSheathe`) with one predicate on both sides, `ValidateDraw`
  (D-CR16): `IncompatibleSlot` (no sheath, no such hand), `InvalidItem` (an empty sheath), `SlotOccupied` (the hand slot
  itself holds an item and no inventory was given), `NoInventorySpace`. **One item per hand**: a draw sends the sheath
  drawn into that hand back first and an item in the hand slot itself to the inventory; an item equipped into the hand
  slot sheathes the drawn one. `TryEquipFromInventory(..., bDraw)` equips into the sheath and draws in one request (the
  hotbar's F; the client's predicate checks the equip, the server the draw too). The manager flips the state when asked;
  when (the montage's hand-off) is the owner's business.
- **GAS follows the hand.** A sheath grants its item's abilities and effects while drawn and revokes them on the sheathe
  (`SetDrawnInternal`); an ordinary slot grants on the equip as before. A belted pickaxe does not dig.
- **Visuals.** `GetVisualAttachSocket` / `GetVisualAttachOffset` answer by the state: sheathed, the sheath's bone (or the
  item's `SheathBoneOverride`) and `SheathOffset * sheath correction` (`SheathOffset` alone on its own bone); drawn, the
  hand slot's bone (or `AttachBoneOverride`) and `AttachOffset * hand correction`, as if equipped in the hand.
  `GetVisualAttachSocketForState` / `GetVisualAttachOffsetForState` answer for a given state, for owners that seat the
  visual by their montage during the hand-off. The stance layer (`AnimLayerClass`) is linked while the item is in the
  hand only (`RefreshSlotLayer`). A draw or a sheathe re-seats the same component (`ApplyHandState`): no mesh reload.
- **Queries.** `GetHeldSlotTag(Hand)` (the hand slot when it holds an item, else the sheath drawn into it),
  `GetHeldItem(Hand)`: what every caller asking "what is in the main hand" reads. `IsDrawn`, `IsSheathSlot`.
- **Replication.** `bDrawn` replicates in the slot. `OnRep_EquipmentSlots` compares it with `bAppliedDrawn` and, on a
  draw or a sheathe of the same item, re-seats the visual, links or unlinks the layer and fires `OnDrawnChanged(SlotTag,
  bDrawn)` (every machine; the authority from `SetDrawnInternal`), then `OnEquipmentChanged` as always.
- Console: `Equipment.Draw <SheathSlotTag>`, `Equipment.Sheathe [SlotTag]` (no animation). Tests `Equipment.Sheath.*`.

**One item per place on the body (the sheath loadout plan, D95, 2026-11-11).** A sheath is one place on the body and,
like every slot, holds one item: you can only equip what can be sheathed together. The character's back
(`Equipment.Slot.Sheath.Back`) holds one of a bow, a two-handed weapon or a shield. The rule is the slot's occupancy,
checked by the predicates both sides run (`ValidateEquipFromInventory` → `ValidateEquip`, D-CR16): an equip from the bag
into an occupied sheath is a swap (the old item takes the new one's bag slot, so a full bag still swaps: the bow equipped
sends the shield to the bag), and a direct equip with no inventory is refused (`SlotOccupied`, nothing dropped). **A
sheath draws into its item's hand**: the item's `EquipmentSlotTag` when the owner has that hand as an ordinary slot, else
the sheath definition's `DrawSlotTag` (`GetDrawHandFor`); the authority sets the slot's `DrawSlotTag` on the equip (the
back holding a shield draws into the off hand) and puts the definition's back when it empties, and it replicates with the
slot, so every reader of the drawn hand (`GetHeldItem`, the visual's bone and fit, the character's cycle) follows the
item. A shield asked into the off hand (the panel, an old save) goes onto the back (`ResolveSheathTarget`).

**The shield beside a one-handed weapon (D96).** An item whose fragment names `DrawWithSlotTag` follows that hand
(`GetDrawLeadSlotTag`: the shield follows `Equipment.Slot.MainHand`): after every equip, unequip, draw and sheathe the
authority reconciles the hands (`ReconcileHands`), and the shield is in its hand while the lead's hand holds an item that
does not block its hand (a sword, not a pickaxe) and nothing else is in its hand (no torch), on its sheath otherwise. So
the shield comes onto the arm when the sword is drawn and onto the back when it is sheathed or leaves; unarmed, it is on
the back. Its state is its lead's: a draw of it is refused by `ValidateDraw` (`IncompatibleSlot`) and a sheathe by
`TrySheathe` (`Failed`), on both sides; an equip asking for its draw leaves that to the reconcile. Its abilities and
effects (its Defense) follow the hand as any sheath's. The swap is instant, at the sword's draw or sheathe: there is no
montage for the shield. Being equipped is not coupled: a shield stays on the back when the sword goes to the bag.

**A shared place (D97).** Two slots can share a place on the body: an item's Equipment fragment names the slot whose
place it shares (`SharesPlaceWithSlotTag`) and the items that can be there with it (`SharesPlaceWithItemTags`, matched
against the other item's definition tags). The character's quiver (`Equipment.Slot.Quiver`, on the back, its opening over
the right shoulder blade) shares the back's sheath with a bow (`Item.Type.Bow`) only. `GetPlaceConflicts(Item, Slot)`
names the slots whose items an equip would send off (either item's rule counts, the slot's own item is a swap, not a
conflict). An equip from the bag sends them to the bag first, each step validated before it moves anything; the
predicate both sides run (`ValidateEquipFromInventory`) refuses with `NoInventorySpace` when the bag cannot take them, and
an equip with no bag is refused (`SlotOccupied`). So a shield or a two-hander onto the back sends the quiver to the bag,
and the quiver sends a shield or a two-hander there; an empty back keeps the quiver. Test `Equipment.Sheath.SharedPlace`.

**Blocked hands (G175).** An item can empty other hands while it is in its own: its Equipment fragment's
`BlockedSlotTags` (the bow and the pickaxes name `Equipment.Slot.OffHand`: they need both hands). `GetBlockedSlotTags()`
gathers them from every item in a hand, `IsSlotBlocked(Slot)` asks for one. A blocked hand takes nothing: a draw into it
and an equip into a blocked hand slot itself are refused with `HandBlocked` (the predicates `ValidateDraw` and
`ValidateEquip`, on both sides), a draw of a blocking item sends what the blocked hand slot itself holds (a torch) to
the bag with the hand's own item, and the reconcile sheathes what is drawn into a blocked hand (the shield, when the
pickaxe comes out). Tests `Equipment.Sheath.OnePerPlace`, `Equipment.Sheath.DrawsWithLead`.

---

## Durability

`GetDurability(SlotTag, OutCurrent, OutMax)` reads the equipped item's `UInstanceFragment_DurabilityState` against its
definition's `UItemFragment_Durability`. `ApplyDurabilityLoss(SlotTag, Amount)` (authority) lowers it, broadcasts
`OnDurabilityChanged(SlotTag, Current, Max)`, and at zero with `bDestroyAtZero` unequips the item without returning it
anywhere and broadcasts `OnItemBroken(SlotTag, Item)`. Callers decide what wears an item (VoxelCharacterPlugin's melee
ability applies the weapon's `DegradeRate` per hit).

The loss changes the slot's own item through `FItemInstance::EditFragment`, which bumps the item's fragment revision:
`EquipmentSlots` is compared with its last sent state through `FItemInstance::Identical`, and that state shares the
item's fragment objects, so before G136 a worn item (and a carried light's fuel) never reached a client, whose
`GetDurability` kept the value the item was equipped with. Now the slot is sent, the client's copy gets the new value,
and `OnRep_EquipmentSlots` refreshes a light and fires `OnEquipmentChanged`, which the slot widget follows.
`OnDurabilityChanged` and `OnItemBroken` fire on the authority only.

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
second finds the slot occupied and swaps the first back into the inventory. A draw and a sheathe go the same way
(`ServerRPC_RequestDraw`, `ServerRPC_RequestSheathe`; the inventory a draw names must be the requester's) and reach the
other machines as the slot's replicated `bDrawn` ("Sheaths").

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
- `SetHeld(bool)`: gold `(0.8, 0.6, 0.2, 0.9)` while held by click-to-move, blue `(0.2, 0.42, 0.7, 0.9)` while the slot is a
  sheath whose weapon is drawn, default `(0.08, 0.08, 0.12, 0.9)` (`BackgroundColorFor`).
- A hand slot shows what is in the hand: its own item, or, dimmed, the weapon drawn from a sheath (`GetHeldSlotTag`); its
  tooltip is that weapon's. The sheaths appear as slots of their own (from `AvailableSlots`, no code).
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

`Equipment.Equip <DefName> <SlotTag>` (creates the item and equips it to the player's empty slot, a carried weapon asked
into its hand going into its sheath drawn; server or standalone), `Equipment.UnequipAll`, `Equipment.Draw <SheathSlotTag>`
and `Equipment.Sheathe [SlotTag]`,
registered in `EquipmentPlugin.cpp`; `Documentation/EDITOR_CONSOLE_GUIDE.md` lists them with every project command.

## Tests

`Equipment.*`: `Queries.*` (`Private/Tests/Test_EquipmentQueries.cpp`), `Equip.Results` (the equip results),
`Equip.FromInventorySwap` (a swap from a full bag keeps both items; a refused swap changes nothing),
`Authority.ClientRequests` (a client's direct equip refused; its equip request delivered to the server as an instance
id, simulated server/client with `CGFTesting::FRPCCall`; another player's inventory refused),
`Sheath.TargetSlot` (a carried weapon lands in its sheath, an owner without sheaths as before, a hand request into
the sheath drawn), `Sheath.DrawAndSheathe` (the results, one item per hand, the hand slot's item to the bag),
`Sheath.GASWhileDrawn`, `Sheath.AttachByState` (the bones and fits sheathed and drawn, the item's own bones),
`Sheath.FromInventoryDraw` (equip and draw in one request, the old weapon kept in its sheath), `Sheath.ClientRequests`
(the client's predicate and requests, `OnRep_EquipmentSlots` acting on a draw and a sheathe), `Sheath.OnePerPlace` (the
back holds a bow or a shield: a direct equip refused, a swap from a full bag, the draw hand by the item, the client's
swap request), `Sheath.DrawsWithLead` (the shield drawn and sheathed with the sword, on the back unarmed, beside a pickaxe
and behind a torch, its own draw and sheathe refused on both sides, its GAS with the hand), `Sheath.SharedPlace` (the
quiver beside the bow, sent to the bag by a shield and sending it, a direct equip and a full bag refused at home and on
the server), `Sheath.SlotWidgetColour`,
`Durability.EmptySlotHasNone`, `Durability.LossMarksTheItem` (the worn item is no longer `Identical` to its state
before, G136), `LightSource.IntensityRule` / `NothingCarried`, and
`GAS.ResolvesAbilitySystemThroughInterface` in the GAS module. VoxelCharacterPlugin's `VoxelCharacter.EquipmentLoop.*` tests cover
the stat effect end to end. The equip flow above over a real connection (a dedicated server and a client in PIE: the
request, the server's checks, the replicated slot and `OnRep_EquipmentSlots`, a rack's sword at its first replication,
the equipped sword's durability worn on the server reaching the client's `GetDurability`, a dagger carried in a sheath
drawn and sheathed at the client's request reaching the client in that state) is the game module's
`VoxelEngine.Replication.Framework`, run by the in-editor gate.

---

## Extension Points

**`OnPostEquip` / `OnPostUnequip`** (`BlueprintNativeEvent`, empty in C++) run after the core equip and unequip. Games
override them for modular meshes instead of socket attachment, material tints, sounds, camera changes, or a character
preview.

**Custom slot rules.** `CanEquipItem` and `ValidateEquip` are not virtual. A rule such as a level requirement goes in the
game's code before it calls `TryEquip*`, or as tags: give the slot `AcceptedItemTags` and tag the items.

**Equipment sets.** Not built in. Bind `OnEquipmentChanged`, scan the equipped items' tags, and apply a set bonus effect
through the ASC when a threshold is met.
