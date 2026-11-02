# EquipmentPlugin — Plugin Instructions

> State as of 2026-11-02 (code review CR5).

## Purpose

Equipping and unequipping items into tag-identified slots, attaching their visuals, wearing their durability, carrying
light, and granting abilities and effects through GAS. Depends on CommonGameFramework and on ItemInventoryPlugin. The
ItemInventoryPlugin dependency is required, not optional: both modules link it privately, the `.uplugin` lists it, and
the manager reads item fragments and moves items to and from `UInventoryComponent` (D-CR22, FW-05). The plugin never
references InteractionPlugin.

## Reading Order

1. `Documentation/EQUIPMENT_SYSTEM.md` — slots and matching, validation, the equip flows with and without an inventory,
   visuals and attach offsets, durability, carried light, the GAS module, replication, the UI widgets.
2. `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md` — how the framework plugins fit together.
3. `Plugins/ItemInventoryPlugin/Documentation/ITEM_INVENTORY_SYSTEM.md` — `UItemFragment_Equipment`, durability and
   light-source fragments, the inventory operations equipment calls.

## Module Structure

```
EquipmentPlugin/
├── Source/
│   ├── EquipmentPlugin/                       ← runtime core, no GAS dependency (log LogEQP)
│   │   ├── Public/
│   │   │   ├── Components/EquipmentManagerComponent.h   ← slots, equip/unequip, visuals, durability, light, RPCs
│   │   │   ├── Types/EquipmentSystemTypes.h             ← FEquipmentSlot (runtime, replicated)
│   │   │   ├── UI/EquipmentSlotWidget.h, EquipmentPanelWidget.h   ← code-built slot and panel widgets
│   │   │   └── EquipmentPlugin.h
│   │   └── Private/ (mirrors Public; EquipmentPlugin.cpp holds the console commands; Tests/)
│   └── EquipmentGASIntegration/               ← GAS module (log LogEQPGAS)
│       ├── Public/
│       │   ├── EquipmentAbilityGranter.h      ← grants / revokes abilities per slot
│       │   ├── EquipmentEffectApplier.h       ← passive, stat-modifier and on-equip effects per slot
│       │   ├── EquipmentGASSettings.h         ← UEquipmentGASSettings::StatModifierEffectClass
│       │   └── EquipmentGASIntegration.h      ← the module; ResolveAbilitySystemComponent
│       └── Private/ (Tests/)
└── EquipmentPlugin.uplugin                    ← requires CommonGameFramework, ItemInventoryPlugin, GameplayAbilities
```

`FEquipmentSlotDefinition` (slot configuration) lives in CommonGameFramework.

## Rules

1. **The core module never names a GAS type.** The GAS module sets `UEquipmentManagerComponent::GASSetupFactory` in
   `StartupModule` (cleared in `ShutdownModule`); each manager calls it in `BeginPlay`, which creates the granter and
   applier and binds `OnGASEquipCallback` / `OnGASUnequipCallback`. New GAS behaviour goes in the GAS module behind
   those callbacks.
2. **GAS runs on the authority only.** `ApplyGAS` / `RemoveGAS` return on clients; the ASC replicates abilities and
   effects. Resolve the ASC with `FEquipmentGASIntegrationModule::ResolveAbilitySystemComponent` (it finds an ASC on the
   player state through `IAbilitySystemInterface`).
3. **Remove by handle, never by class.** The granter and applier keep handles per slot and the ASC they used; removal
   falls back to that ASC when the owner can no longer resolve one (a pawn being destroyed after unpossession).
4. **Lasting stats use the configured effect class.** `StatModifiers` go through
   `UEquipmentGASSettings::StatModifierEffectClass` and `UCGFGameplayEffectStatics::ApplyStatModifierEffect`; never a
   transient infinite effect (it does not replicate to the owning client).
5. **Validation (D-CR16).** `ValidateEquip` (returns `EEquipmentResult`) is the shared predicate: the `Try*` functions
   run it on the caller and the Server RPCs run it again, with the inventory checks (`CanAcceptItem`) repeated inline.
   Refusals answer with `ClientRPC_EquipmentOperationFailed` (`OnOperationFailed`).
6. **Validate before moving items, and read every result (G90).** Equip-from-inventory into an occupied slot goes
   through `UInventoryComponent::TryReplaceItem` (the old item takes the new one's slot, validated whole; refused →
   `NoInventorySpace`, nothing changed); into an empty slot it equips only after `TryRemoveItem` succeeds.
   Unequip-to-inventory checks `CanAcceptItem` first and re-equips if the add still fails. A direct `TryEquipToSlot`
   into an occupied slot is refused (`SlotOccupied`): with no inventory the old item would be lost.
7. **Clients name items, never send them (G91).** `TryEquip` / `TryEquipToSlot` take a whole item and run on the
   authority only (`NotAuthorized` on a client; there is no equip-an-item RPC). A client equips from an inventory by
   instance id, and the server accepts only an inventory of the requester (`IsOwnersInventory`: on its owner, or on
   another actor of the same connection).
8. **Visuals are cosmetic.** Every visual component is created with collision off (`NoCollision`, no overlaps, no
   navigation): colliding visuals on the bones launched the death ragdoll through the terrain (QA F1-04). Attach to the
   character's body mesh (`GetMesh()`), not the first skeletal mesh found (QA F2-04), and apply
   `GetVisualAttachOffset` whenever a visual is re-attached.
9. **Visuals and lights are rebuilt per machine** from the replicated `EquipmentSlots` in `OnRep_EquipmentSlots`; only
   item data replicates.
10. **Fuel and durability change on the authority** (`ApplyDurabilityLoss`, `TickFuel`); clients follow the replicated
   durability.

## Build.cs Dependencies

```csharp
// EquipmentPlugin
PublicDependencyModuleNames:  Core, CoreUObject, Engine, NetCore, GameplayTags, CommonGameFramework
PrivateDependencyModuleNames: ItemInventoryPlugin, InputCore, UMG, Slate, SlateCore

// EquipmentGASIntegration
PublicDependencyModuleNames:  Core, CoreUObject, Engine, GameplayTags, GameplayAbilities, CommonGameFramework,
                              DeveloperSettings
PrivateDependencyModuleNames: EquipmentPlugin, ItemInventoryPlugin
```

## Logging, Commands and Tests

- Log categories `LogEQP` (`EquipmentPlugin.h`) and `LogEQPGAS` (`EquipmentGASIntegration.h`).
- Console commands `Equipment.Equip <DefName> <SlotTag>` and `Equipment.UnequipAll`
  (`Documentation/EDITOR_CONSOLE_GUIDE.md`).
- Tests under `Equipment.*` in each module's `Private/Tests/`, on the real `Equipment.Slot.*` tags; tests that need the
  item database use `CGFTesting::FTestGameInstance` (CommonGameFramework); `Equipment.Authority.ClientRequests` plays a
  client and the server in one world with `SpawnRemoteOwner` and `CGFTesting::FRPCCall`.
