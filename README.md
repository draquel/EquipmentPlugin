# EquipmentPlugin

Equipment slot management with visual mesh attachment and Gameplay Ability System integration for Unreal Engine 5.7. Equip items, apply stats, grant abilities, and attach visible gear to characters.

## What This Plugin Does

EquipmentPlugin manages what a character has equipped — head armor, held weapons, accessories — and handles everything that follows from equipping: attaching a visible mesh to a socket, granting gameplay abilities through GAS, applying passive stat effects, and synchronizing it all across the network.

### Equipment Slots

The `UEquipmentManagerComponent` holds configurable slots defined by gameplay tags. A slot specifies what it accepts (via tag filters) and where the visual attaches (via socket name). The same component works for players, NPCs, and any actor that equips items.

Default slots: Head, Chest, Legs, Feet, Hands, MainHand, OffHand, Accessory1, Accessory2. Add custom slots by adding tag/definition entries — no code changes required.

### Visual Attachment

When an item is equipped, the plugin reads its mesh data from the item's Equipment fragment, async loads the mesh, and attaches it to the specified skeleton socket. Static meshes, skeletal meshes, and animation layer linking are all supported. Visuals sync across clients via replication — each client independently loads and attaches meshes based on replicated equipment state.

### GAS Integration

An **optional** separate module (`EquipmentGASIntegration`) that handles:

- **Ability granting** — Equipping a sword grants a "Slash" ability to the character's ASC
- **Passive effects** — Equipping armor applies an infinite-duration gameplay effect for stat bonuses (armor, health, speed)
- **One-time equip effects** — Trigger an effect once on equip (e.g., gain temporary shield)
- **Clean revocation** — All abilities and effects are tracked by handle and surgically removed on unequip

The core equipment system compiles and functions without GAS. Projects that don't use GAS simply don't load the integration module.

### Inventory Integration

Equipment works with ItemInventoryPlugin for equip-from-inventory and unequip-to-inventory flows, with full atomicity (validates space before moving). Also functions standalone for simpler setups where items are equipped directly without an inventory system.

## Requirements

- Unreal Engine 5.7
- [CommonGameFramework](../CommonGameFramework/) plugin
- GameplayAbilities plugin (enabled in .uproject)
- [ItemInventoryPlugin](../ItemInventoryPlugin/) (optional — for inventory-backed equipment)

## Installation

Clone into your project's `Plugins/` directory:

```bash
git clone <repo-url> Plugins/EquipmentPlugin
```

Ensure CommonGameFramework is also present in `Plugins/`.

Add to your module's `Build.cs`:

```csharp
PublicDependencyModuleNames.Add("EquipmentSystem");

// Only if using GAS integration:
PublicDependencyModuleNames.Add("EquipmentGASIntegration");
```

## Module Dependencies

```
EquipmentSystem (Runtime — core, no GAS dependency)
├── CommonGameFramework
├── Core, CoreUObject, Engine, NetCore
├── GameplayTags
└── ItemSystem  (optional, for inventory integration)

EquipmentGASIntegration (Runtime — optional GAS module)
├── EquipmentSystem
├── CommonGameFramework
├── GameplayAbilities, GameplayTags, GameplayTasks
└── Core, CoreUObject, Engine
```

## Plugin Structure

```
EquipmentPlugin/
├── Source/
│   ├── EquipmentSystem/               Core equipment logic
│   │   ├── Public/
│   │   │   ├── Components/            UEquipmentManagerComponent
│   │   │   ├── Types/                 FEquipmentSlot, result enums
│   │   │   └── Data/                  Equipment definitions
│   │   └── Private/
│   └── EquipmentGASIntegration/       Optional GAS module
│       ├── Public/                    Ability granter, effect applier
│       └── Private/
├── Documentation/
│   └── EQUIPMENT_SYSTEM.md            Detailed system design
└── .claude/
    └── instructions.md                Claude Code implementation instructions
```

## Quick Start

### Add Equipment to a Character

```cpp
UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
TObjectPtr<UEquipmentManagerComponent> EquipmentManager;

// In constructor
EquipmentManager = CreateDefaultSubobject<UEquipmentManagerComponent>(TEXT("Equipment"));

// Configure slots (or set in editor)
FEquipmentSlotDefinition MainHand;
MainHand.SlotTag = FGameplayTag::RequestGameplayTag("Equipment.Slot.MainHand");
MainHand.AcceptedItemTags.AddTag(FGameplayTag::RequestGameplayTag("Item.Category.Weapon"));
MainHand.AttachSocket = "hand_r_socket";
MainHand.SlotDisplayName = LOCTEXT("MainHand", "Main Hand");

EquipmentManager->AvailableSlots.Add(MainHand);
```

### Equip an Item

```cpp
// Direct equip (finds matching slot automatically)
EEquipmentResult Result = EquipmentManager->TryEquip(SwordInstance);

// Equip to specific slot
Result = EquipmentManager->TryEquipToSlot(ShieldInstance, OffHandSlotTag);

// Equip from inventory (removes from inventory, validates space for unequip)
Result = EquipmentManager->TryEquipFromInventory(ItemGuid, PlayerInventory, MainHandSlotTag);

// Unequip back to inventory
Result = EquipmentManager->TryUnequipToInventory(MainHandSlotTag, PlayerInventory);
```

### Query Equipment State

```cpp
FItemInstance Weapon = EquipmentManager->GetEquippedItem(MainHandSlotTag);
bool HasHelm = EquipmentManager->IsSlotOccupied(HeadSlotTag);
TArray<FGameplayTag> Equipped = EquipmentManager->GetAllOccupiedSlots();
```

### React to Equipment Changes

```cpp
EquipmentManager->OnItemEquipped.AddDynamic(this, &AMyCharacter::HandleItemEquipped);
EquipmentManager->OnItemUnequipped.AddDynamic(this, &AMyCharacter::HandleItemUnequipped);

void AMyCharacter::HandleItemEquipped(const FItemInstance& Item, FGameplayTag SlotTag)
{
    // Update UI, play sound, etc.
}
```

### Define an Equippable Item

On the item definition asset, add a `UItemFragment_Equipment`:

```
Fragment_Equipment:
  EquipmentSlotTag: Equipment.Slot.MainHand
  EquipMesh: SM_Sword_Iron
  AnimLayerClass: ABP_SwordAnimLayer
  GrantedAbilities: [GA_Slash, GA_Block]
  PassiveEffects: [GE_SwordDamageBonus]
```

### Custom Equipment Behavior

Override `OnPostEquip` / `OnPostUnequip` in Blueprint or C++ for game-specific logic:

```cpp
void AMyCharacter::OnPostEquip_Implementation(const FItemInstance& Item, FGameplayTag SlotTag)
{
    // Modular character: swap mesh section instead of socket attachment
    // Material changes: tint armor to player's team color
    // Camera: adjust FOV for two-handed weapons
}
```

## Multiplayer

Server-authoritative. Equipment mutations happen via server RPCs. Equipment slot state replicates to all clients. Each client independently loads and attaches visual meshes from the replicated state. GAS ability/effect granting happens only on the server — the ASC handles replicating ability state to clients.

## Blueprint Support

All operations, queries, and events are Blueprint-exposed. Equipment slots are configured as an editable array on the component. `OnPostEquip` and `OnPostUnequip` are `BlueprintNativeEvent` functions for easy customization. `CanEquipItem` is overridable for game-specific validation (level requirements, class restrictions, etc.).

## Related Plugins

| Plugin | Integration |
|--------|------------|
| [CommonGameFramework](../CommonGameFramework/) | IEquippable, FEquipmentSlotDefinition, shared tags (required) |
| [ItemInventoryPlugin](../ItemInventoryPlugin/) | Equipment fragments on item definitions, inventory-backed equip/unequip |
| [InteractionPlugin](../InteractionPlugin/) | "Equip" interaction type on world items |

## Documentation

- [EQUIPMENT_SYSTEM.md](Documentation/EQUIPMENT_SYSTEM.md) — Equip/unequip flow, visual attachment, GAS integration architecture
- [ARCHITECTURE.md](../CommonGameFramework/Documentation/ARCHITECTURE.md) — System-wide architecture and integration patterns

## License

[Your license here]
