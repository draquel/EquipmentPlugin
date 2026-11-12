# Sheath Loadout Plan: you can only equip what can be sheathed together

> **Frozen 2026-11-11.** This plan is history: agreed the same day as drafted ("Yes, build it": D95-D97 each as
> recommended), built in one pass (section 8). Built: the shield in the back's one sheath, a sheath drawing into its
> item's hand, the shield following the drawn one-hander, G175's `bDrawWhenFree` and `Sheath.ShieldBack` retired, the
> `Equipment.Slot.Quiver` tag reserved. As it is now: `EQUIPMENT_SYSTEM.md` ("Sheaths" › One item per place, The shield
> beside a one-handed weapon, Blocked hands), `CHARACTER_SYSTEM.md` ("GAS" › the sword and the shield, Carried weapons),
> `COMMON_TYPES.md`, `ITEM_INVENTORY_SYSTEM.md`. Left open: the quiver's place (section 5, corrected after the build);
> settled by the user the same day, the back with the opening over the right shoulder blade (D97 in `FINDINGS.md`).
> Owner as planned: EquipmentPlugin; the fragment in ItemInventoryPlugin, the tags in CommonGameFramework, the
> character in VoxelCharacterPlugin, the content, the redirect and the shots in the parent. Numbers: G177, D95-D97.

## 1. What exists

Read 2026-11-11 from dev 8370639.

- **A sheath is a slot** with a `DrawSlotTag` (the hand it draws into) and holds one item, like every slot. The
  character has four (`AVCCharacterBase`'s constructor, `AddSheath`): `Equipment.Slot.Sheath.Belt` (`pelvis`, the
  pickaxes), `.HipLeft` (`pelvis`, the swords), `.Back` (`spine_05`, the bow), each into the main hand, and G175's
  `.ShieldBack` (`spine_03`, into the off hand, `bDrawWhenFree`).
- **An equip into an occupied slot** from the bag is a swap (`TryReplaceItem`: the old item takes the new one's bag slot,
  no free slot needed, G90); a direct `TryEquipToSlot` is refused (`SlotOccupied`).
- **G175** (`BlockedSlotTags` on the bow and the pickaxes, `HandBlocked`, `ReconcileBlockedHands`): the shield is on
  the arm whenever the off hand is free (unarmed too) and on its own back sheath while the bow or a pickaxe is out.
- **G177 (the finding this plan closes).** The shield's back sheath and the bow's are two slots on the same back: with
  a bow, the shield and the bow hang on the back together and overlap (the user, 2026-11-11). Nothing says two slots
  share a place on the body.

## 2. The rule (D95)

**One item per place on the body, and the place is the slot.** The shield moves into the back's sheath,
`Equipment.Slot.Sheath.Back`: the back holds one of a bow, a two-handed weapon or a shield, because it is one slot.
`Equipment.Slot.Sheath.ShieldBack` is retired. The rule then needs no new machinery: slot occupancy is already enforced
on the server and predicted on the client by the same predicates (`ValidateEquipFromInventory` → `ValidateEquip`,
D-CR16). What changes:

- **A sheath draws into its item's hand.** The back draws a bow into the main hand and a shield into the off hand. The
  hand is the item's `EquipmentSlotTag` when the owner has that hand, else the sheath's `DrawSlotTag` (the default).
  Set on the equip, replicated with the slot, back to the default when it empties. `GetHeldItem(OffHand)` finds the
  drawn shield as it does now, so `UVCBlockAbility::GetHeldShield` is unchanged.
- **The shield keeps its fit.** Its fragment names `SheathBoneOverride = spine_03`, so the back fit solved for G175
  carries over (`SheathOffset` alone fits it on that bone), top at the shoulders, face to the camera.
- **Old saves and assets.** A gameplay tag redirect `Equipment.Slot.Sheath.ShieldBack` → `.Back`; the snapshot's
  restore falls back to the item's own sheath when a saved slot tag no longer exists.

**Conflicts: swap, not refuse (recommended).** Equipping the bow with a shield on the back puts the shield in the bag,
in the bow's bag slot; equipping the shield with the bow on the back does the reverse. It is the swap the hotbar
already does (a sword for an axe), it needs no free bag slot, and a refusal would make the player unequip by hand
first. A direct equip with no inventory stays refused (`SlotOccupied`, nothing dropped). The refusal results the client
can see stay the existing `EEquipmentResult` values; no new value is needed for the rule.

## 3. The shield beside a one-handed weapon (D96)

**The shield is drawn with the one-hander and sheathed with it** (the user's "equipped with the one handed weapon and
unequipped with" it, read as the draw: in this codebase equipping puts an item in its sheath, drawing puts it in the
hand). The fragment gets `DrawWithSlotTag` (the shield: `Equipment.Slot.MainHand`): a sheathed item that follows the
item in that hand. After every change the authority reconciles (`ReconcileHands`, the renamed reconcile): the shield is
drawn into the off hand when

1. the main hand holds an item (a sword drawn from the hip), and
2. that item does not block the off hand (`BlockedSlotTags`: a pickaxe or a bow does), and
3. the off hand is free (no torch in the off hand slot itself, nothing else drawn into it);

otherwise it is on the back. So: sword drawn, shield on the arm; R sheathes the sword and the shield goes onto the
back; X to the pickaxe sheathes the sword, the shield goes to the back and stays there. Unarmed, the shield is on the
back (G175 had it on the arm). A direct draw or sheathe of the shield (`Equipment.Draw Sheath.Back`) is refused by
`ValidateDraw` on both sides (`IncompatibleSlot`): its state is its lead's. The swap stays instant at the sword's hand-off
(D94's open gap: no shield montage). The weapon cycle and R still skip it (they take the main hand's sheaths).

**Not recommended: coupling in the bag.** The shield does not go to the bag when the sword does, and a shield can be
equipped without a sword: it stays on the back, which is a place it can be sheathed. The rule decides where items go;
the pairing decides only when the shield is in the hand. Say if you meant the bag too, and the unequip of the sword
will take the shield with it.

## 4. What G175 leaves

| Piece | Fate |
|---|---|
| `UItemFragment_Equipment::BlockedSlotTags`, `IsSlotBlocked`, `GetBlockedSlotTags`, `HandBlocked` | **Kept.** They are the hands rule, not the sheaths': a pickaxe still keeps the shield on the back, a bow or a pickaxe still sends a torch to the bag. Comments reworded (no shield beside the bow: the back says so now) |
| `ReconcileBlockedHands` | **Kept and renamed** `ReconcileHands`: its first loop (a sheath drawn into a blocked hand is sheathed) stays; its second (`bDrawWhenFree`) becomes the partner rule above |
| `FEquipmentSlotDefinition::bDrawWhenFree`, `FEquipmentSlot::bDrawWhenFree` | **Removed** (CommonGameFramework, EquipmentPlugin) |
| `Equipment.Slot.Sheath.ShieldBack`, the character's fifth `AddSheath`, `AddSheath`'s `bDrawWhenFree` | **Removed**, with the redirect above |
| `Equipment.Sheath.BlockedHand` | **Replaced** by `Equipment.Sheath.OnePerPlace` and `Equipment.Sheath.DrawsWithLead` (section 6) |
| `VoxelCharacter.EquipmentLoop.SheathSlots` | **Updated**: three sheaths, no ShieldBack; the back draws a shield into the off hand |
| Docs: `EQUIPMENT_SYSTEM.md` "Sheaths" › Blocked hands, `COMMON_TYPES.md`, `ITEM_INVENTORY_SYSTEM.md`, `CHARACTER_SYSTEM.md`, the script docstrings | **Rewritten** to the rule; G175's row in FINDINGS keeps its history, G177 says what replaced it |

## 5. The quiver, reserved (D97, design only)

> **Corrected after the build (2026-11-11):** the right hip is not free. The belt's pickaxe hangs at the right hip,
> behind (seen in PIE on `CharacterTest`, `Saved/QA/sheath/belt_sheet.png`; `Equipment.Slot.Sheath.Belt`'s tag says so:
> "at the right hip"); the draft below read the pickaxe's left-reaching draw clip as its place. Only the tag
> `Equipment.Slot.Quiver` is reserved, which names no place, so nothing built depends on it. The quiver's place is the
> quiver plan's to settle, with the reload clips in hand (where their hand reaches decides it): (a) the right hip, taking
> the belt's place, so a quiver and a pickaxe are not carried together; (b) the small of the back, under a bow (a shield
> hangs there too, so a quiver and a shield would not go together); (c) the right thigh, below the belt.

The draft as agreed: The bow will get a quiver and reload animations next. **The quiver goes on the right hip**, its own place:
`Equipment.Slot.Quiver`, an ordinary worn slot on `pelvis` (not a sheath: it is never drawn into a hand), accepting a
quiver category. The right hip carries nothing today (the swords hang on the left hip and the pickaxe's draw reaches
the left too; to be checked in PIE), and it is the side of the hand that pulls the arrow (the bow rides the left).
Under the rule, bow (back) + quiver (right hip) + sword (left hip) + pickaxe (belt) all fit, one item each; the bow
needs no change. The alternatives fail the rule: a back quiver is a second item on the back, and a left-hip quiver
would take the sword's place, which is the ranged-or-melee split the user rejected. Only the tag is reserved now; the
slot, the item, what it holds (the arrows stay in the bag) and the reload's hand target are the quiver's own plan.

## 6. Build and done

One pass, branches `claude/sheath-loadout` cut from `dev` in each repository:

1. **CommonGameFramework**: retire `Sheath.ShieldBack` (redirect), reserve `Equipment.Slot.Quiver`, drop
   `bDrawWhenFree`; `COMMON_TYPES.md`.
2. **ItemInventoryPlugin**: `DrawWithSlotTag` on `UItemFragment_Equipment`, the `BlockedSlotTags` comment;
   `ITEM_INVENTORY_SYSTEM.md`.
3. **EquipmentPlugin**: the item's draw hand, `ReconcileHands` with the partner rule, the partner's refused direct draw
   in `ValidateDraw`; tests `Equipment.Sheath.OnePerPlace` (shield then bow: swap to the bag; bow then shield; a direct
   equip refused; a full bag still swaps; the client's predicate the same) and `Equipment.Sheath.DrawsWithLead` (drawn
   and sheathed with the sword; stays back with a pickaxe drawn, with a torch in the off hand, unarmed; a direct draw
   refused; the GAS Defense follows the hand); `EQUIPMENT_SYSTEM.md`.
4. **VoxelCharacterPlugin**: three sheaths, `CHARACTER_SYSTEM.md`, `SheathSlots` updated, the snapshot fallback.
5. **Parent**: `configure_melee_items.py` (the shield: `Sheath.Back`, `SheathBoneOverride spine_03`, `DrawWithSlotTag
   MainHand`), the other scripts' comments, the tag redirect in `Config/DefaultGameplayTags.ini`, FINDINGS (G177 closed,
   D95-D97), the console guide if a row changes, this plan frozen.

Done: the rule on the server with the client on the same predicate; the tests above; PIE screenshots on
`/Game/PluginTesting/Tests/Character/CharacterTest` (`vox.Kit`): sword and shield drawn, both sheathed (sword on the
hip, shield on the back), the bow equipped (the shield gone to the bag, the bow alone on the back); the gate from
`Scripts/qa/README.md`; a PR per repository into `dev`, gitlinks, the board.

## 7. For the user

1. D95: the shield in the back's one sheath; a conflicting equip swaps the old item to the bag (recommended) or is
   refused?
2. D96: the shield follows the drawn one-hander only (recommended), or also leaves for the bag when the sword does?
3. D97: the quiver on the right hip (recommended), reserved now and built with the reload?

## 8. Results (2026-11-11)

Built as section 6 says, on `claude/sheath-loadout` in the parent, CommonGameFramework, ItemInventoryPlugin,
EquipmentPlugin and VoxelCharacterPlugin. Beside the plan: the hotbar's use of a shield (`UseItemInSlot`) ran the draw
sequence of the back's sheath, which sheathed the sword in the hand and played the bow's unsheathe for an item that
cannot be drawn on its own; it now runs only for a sheath of the weapon cycle (`GetCarriedWeaponSlots`), and
`VoxelCharacter.EquipmentLoop.SheathSlots` uses a shield from the bag with the sword drawn. The content script found the
shield's `SheathSlotTag` already `Sheath.Back` on load: the tag redirect works on assets.

- **Gate.** `build.ps1` 0 warnings; the headless suite 911 passed, 0 failed (`qa_sheath_loadout.log`; `BlockedHand`
  replaced by `OnePerPlace` and `DrawsWithLead`), no new warning kind against the last full run; `editor_gate.py` 27 of
  27 (with `VoxelEngine.Replication.Framework`), the 132 parity lines as recorded; `build.ps1 -NonUnity -Changed`.
- **PIE** on `CharacterTest` (`Saved/QA/sheath/sheath_loadout_sheet.png`, `Saved/sheath_shots.py`): the longsword and
  then the shield used from the bag: the sword drawn, the shield on the forearm (the back's `DrawSlotTag` the off hand);
  R: the sword on the left hip, the shield on the back, top at the shoulders; the bow used from the bag: the shield to
  the bag in the bow's bag slot, the bow drawn; R: the bow alone on the back.
- **Not done:** the shield's swap has no montage (D94's gap); the quiver (section 5).
