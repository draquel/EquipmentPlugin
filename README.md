# EquipmentPlugin

Equipment for Unreal Engine 5.8: `UEquipmentManagerComponent` holds tag-identified slots (head, chest, legs, feet, hands,
main hand, off hand, two accessories, or any you configure), equips items directly or from an inventory, attaches their
meshes to sockets with animation layers, wears their durability and burns a carried light's fuel, all
server-authoritative with visuals rebuilt on every client. The `EquipmentGASIntegration` module grants the items'
abilities and applies their passive, stat-modifier and on-equip effects through GAS; without it equipment works with no
abilities or effects. Includes code-built slot and panel widgets. Requires CommonGameFramework
(`Plugins/CommonGameFramework/`), ItemInventoryPlugin (`Plugins/ItemInventoryPlugin/`) and the GameplayAbilities plugin.

- [EQUIPMENT_SYSTEM.md](Documentation/EQUIPMENT_SYSTEM.md) — the system design
- [.claude/instructions.md](.claude/instructions.md) — rules for working in this plugin
- `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md` — how the framework plugins fit together
