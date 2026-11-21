# Equipment Sheathing Plan: weapons carried on the body, drawn into the hand

> **Frozen 2026-11-09.** This plan is history: agreed 2026-11-08 as drafted, every decision as recommended (D77-D82,
> §6; numbered D76-D81 until the bow aim fixes took D76 the same day), S1-S4 built 2026-11-08/09. Built: sheath slots
> with a replicated drawn state (EquipmentPlugin), the fragment's sheath fields (ItemInventoryPlugin), the sheath tags
> (CommonGameFramework); the character's three sheaths, R / X / F, the unsheathe and sheathe montages with a hand-off
> measured from each clip, the attacks and the action set on the held weapon, the save, the panel's drawn frame and the
> carried strip (VoxelCharacterPlugin); the fits, the inputs, the replication steps and the QA rows F2-10..13 and
> F13-18..21 (the parent). As it is now: `EQUIPMENT_SYSTEM.md` ("Sheaths", "UI Widgets"), `CHARACTER_SYSTEM.md`
> ("Items, progression and saves" › Carried weapons, "Animation", "Input", "HUD and widgets"), the console guide. The
> one finding the build left open is G162 (the draw over the whole body). Owner as planned: EquipmentPlugin; the
> fragment in ItemInventoryPlugin, the tags in CommonGameFramework, the character in VoxelCharacterPlugin, the content,
> the replication test and the QA rows in the parent.

## 1. What exists

Read 2026-11-08 from the code and the scripts.

- **Slots.** `UEquipmentManagerComponent` holds an `FEquipmentSlot` per `FEquipmentSlotDefinition` (CommonGameFramework:
  tag, accepted tags, name, attach socket, a skeleton correction). The character defines four: `MainHand` (`hand_r`,
  weapons), `OffHand` (`hand_l`, weapons and the torch), `Head` (`head`), `Chest` (`spine_03`)
  (`AVCCharacterBase`'s constructor). An item names one slot, `UItemFragment_Equipment::EquipmentSlotTag`; every weapon
  names `MainHand`.
- **An item is in the bag or in a slot.** Equipping moves it out of the inventory (`TryEquipFromInventory`, a swap when
  the slot is occupied, G90); the hotbar's F (`UseItemInSlot` / `EquipHotbarItem`) equips the selected weapon and the old
  one goes back to the bag. So one weapon is carried at a time, always in the hand.
- **Everything that asks what the player wields reads `MainHand`:** the action set (`RefreshActionSet`,
  `PickActionSet`), the primary action chain (`VCCharacterBase_Surface.cpp`), the melee ability's weapon and wear
  (`Combat/VCMeleeAttackAbility.cpp`), the bow's draw, shot and wear (`Combat/VCRangedAttackAbility.cpp`), the drawn
  grip (`UpdateEquipmentAttachments`). Digging asks the `MiningSpeed` attribute, which the pickaxe's `StatModifiers`
  raise through GAS for as long as it is equipped.
- **Visuals.** The manager attaches each slot's mesh to the body at `GetVisualAttachSocket` with
  `GetVisualAttachOffset` (the item's fit times the slot's correction, or the item's own bone, the bow's `hand_l`), links
  the item's stance layer on attach, and rebuilds visuals per machine in `OnRep_EquipmentSlots` (G153's stale check).
  The character re-seats them per view and composes the body's correction by bone (D71) and the bow's drawn grip;
  `vc.Equip.Nudge` / `vc.Equip.PrintFit` already take a slot argument but print the fit as the item's hand
  `AttachOffset` only.
- **Animation.** `UVCActionAnimSet` has no equip action (`EVCAnimAction`: Attack, Dig, Chop, Place, Use, HitReact,
  Death, Revive, Draw (the bow's string), Release). The Essential Animation packs ship what this needs, UE5 skeleton:
  SwordShield `SSH_Sword_Unsheathe_Hip` / `_Sheathe_Hip` / `_Unsheathe_Back` / `_Sheathe_Back`, the same four for `2H`,
  BowAndArrow `A_Unsheathe_Back` / `A_Sheathe_Back` (and B variants), plus `*_Equip_01..04` (from nothing to the hand).
  No pickaxe clip.
- **The save** stores each occupied slot (`FVCSnapshotEquipmentEntry`: slot tag and the item as JSON) and restores it
  through the inventory and `TryEquipFromInventory`. **NPCs** hold one `HeldItem` (a definition, no manager): a static
  mesh on the hand and its set; they never change it.
- **UI.** The equipment panel lists every `AvailableSlots` entry (code-built; click to move between bag and slot). The
  hotbar is the bag's first slots; an equipped item is not on it.

## 2. Principles

1. **One home per item.** A carried weapon has exactly one slot, its sheath; drawing it changes its state, not its
   place. No reservation, no item in two places, nothing lost when the bag is full.
2. **Ask for the hand, not for a slot.** Every reader of "what is in the main hand" asks the manager
   (`GetHeldItem(HandTag)`), which knows a hand slot's own item or the sheath drawn into it.
3. **An owner without sheaths behaves as today.** Sheaths are opt-in data: an owner whose slots have none (the NPCs, any
   other game) equips into `EquipmentSlotTag` as before, and an item without a sheath goes into the hand as before.
4. **The server decides, the owner animates at input** (D49): the owner's montage plays on the press, the server's
   state follows the montage's own timing read from the asset, the others see the multicast and the replicated state.
5. **Fits are data**: a sheath's skeleton correction on the slot, the item's fit on its fragment, the body's correction
   by bone on the appearance (D71); tuned live with `vc.Equip.Nudge` on `CharacterTest`, printed for the scripts.

## 3. Design

### 3.1 The model (D77)

- **Sheath slots** are equipment slots of their own, `Equipment.Slot.Sheath.*` (CommonGameFramework), whose definition
  names the hand they are drawn into: `FEquipmentSlotDefinition::DrawSlotTag` (set = a sheath; `MainHand` for all of
  ours). The character adds them beside the four it has (§3.2).
- **The item names its sheath and its fit there** (`UItemFragment_Equipment`, ItemInventoryPlugin): `SheathSlotTag`
  (a sheath slot or a parent of several, matched as `EquipmentSlotTag` is), `SheathOffset` (the fit on the sheath's bone,
  in the slot's corrected frame) and `SheathBoneOverride` (as `AttachBoneOverride`). `EquipmentSlotTag` keeps meaning the
  hand it is used in, and its `AttachOffset` / `AttachBoneOverride` the fit in that hand.
- **Drawn is a state of the sheath slot**: `FEquipmentSlot::bDrawn` (replicated). A drawn sheath's visual rides the hand
  (its item's hand bone and fit), a sheathed one the sheath (its sheath bone and fit). **One item per hand**: drawing a
  sheath sheathes the one drawn into the same hand in the same server step; a non-sheath item in the hand slot itself
  goes to the bag first (refused with `NoInventorySpace` when it cannot).
- **Where an equip lands.** `FindTargetSlot` picks the item's sheath when the owner has one, else its
  `EquipmentSlotTag` slot as today. An equip of a sheathable item asked into its hand (the panel's Main Hand, an old save)
  goes into its sheath, drawn.
- **GAS follows the hand.** A sheath slot grants its item's abilities and effects (`GrantedAbilities`, `PassiveEffects`,
  `StatModifiers`) while drawn and revokes them on sheathing; `OnEquipEffects` still fire once on the equip. So a belted
  pickaxe does not dig, a sheathed sword gives nothing; armour is unchanged. The stance layer is linked while drawn only.
- **Queries**: `GetHeldSlotTag(HandTag)` (the hand slot when occupied, else the sheath drawn into it, else none),
  `GetHeldItem(HandTag)`, `IsDrawn(SlotTag)`; `GetVisualAttachSocket` / `GetVisualAttachOffset` answer by the state.
- **Requests**: `TryDraw(SheathSlotTag)`, `TrySheathe(HandTag)`, `TryEquipFromInventory(..., bDraw)` for the hotbar's
  one-press equip-and-draw (one request, so the client's predicate does not wait for the equip to replicate). Each has
  a server RPC and one predicate both sides run (`ValidateDraw`, D-CR16); a refusal answers `OnOperationFailed`.
- **Delegates**: `OnDrawnChanged(SlotTag, bDrawn)` on every machine (on clients from `OnRep_EquipmentSlots`), and
  `OnEquipmentChanged` after it, so the character's action set refresh needs no new binding.
- **Timing hook**: the manager flips the state when it is told to (`SetDrawn` on the authority after its checks); *when*
  is the owner's business (the character waits for the montage's hand-off, §3.3). The framework knows no animation.

Rejected: a wielded flag on the main-hand slot (one carried weapon only, not what is asked); moving the item from the
sheath into `MainHand` on a draw (no reader changes, but the sheath must stay reserved while its item is out, every equip
path learns the reservation, and the visual is destroyed and rebuilt on each draw, which fights the hand-off).

### 3.2 The sheaths (D78)

| Slot | Bone | Items | Unsheathe / Sheathe clips |
|---|---|---|---|
| `Equipment.Slot.Sheath.HipLeft` | `pelvis` | the swords (one-handed; one carried, a second swaps it to the bag) | SwordShield `SSH_Sword_Unsheathe_Hip` / `_Sheathe_Hip` |
| `Equipment.Slot.Sheath.Back` | `spine_05` | the bow; a two-hander when one exists | BowAndArrow `A_Unsheathe_Back` / `A_Sheathe_Back`; 2H `_Back` for a two-hander |
| `Equipment.Slot.Sheath.Belt` | `pelvis` (the right hip, hanging) | the pickaxes | stand-in: 2H `Sword_Unsheathe_Hip` / `_Sheathe_Hip` (no pickaxe clip, D51's rule) |

`HipRight` is left out until an item needs it (a second one-hander). The torch stays an off-hand item.

### 3.3 Drawing and sheathing (D79)

- **Input.** `R` (`IA_Sheathe`, free today): with a weapon drawn, sheathe it; with none, draw the last drawn one (else
  the first occupied sheath). `X` (`IA_CycleWeapon`): sheathe what is drawn and draw the next carried weapon (Belt, Hip,
  Back order). F on a weapon in the bag equips it into its sheath (the sheath's old item to the bag) and draws it, the
  weapon drawn before going back to its own sheath: today's F, with the old weapon kept on the body. No auto-sheathe.
- **Montages.** `EVCAnimAction::Unsheathe` and `Sheathe` (not "Draw": that is the bow's string). `UVCActionAnimSet`
  gets `Unsheathe` / `Sheathe` maps keyed by the sheath slot tag (the item's set chooses; a set without an entry for that
  sheath uses its first). `UVCAnimNotify_ItemHandOff` marks the frame the hand reaches the grip (unsheathe) or lets go
  at the sheath (sheathe); `GetHandOffTime` reads it from the asset as `GetHitTime` does (40% without one).
  `setup_character_layers.py` builds `AM_<Set>_Unsheathe_<Sheath>` / `AM_<Set>_Sheathe_<Sheath>` from the clips of §3.2,
  over the upper body (`UpperBody` slot, as the bow's draw, so the legs keep walking), the hand-off fraction per clip in
  its table, checked by eye.
- **The hand-off on screen.** Each machine re-seats the visual at its own montage's hand-off: while an Unsheathe of a
  slot plays before its hand-off the visual stays at the sheath, while a Sheathe plays before its hand-off it stays in
  the hand, otherwise it follows `bDrawn` (`UpdateEquipmentAttachments`, as the drawn grip blends by its montage today).
  A machine with no montage playing (a late joiner) shows the state.
- **When the weapon can be used.** The server sets `bDrawn` at the unsheathe's hand-off time (a timer from the
  montage's asset time, as the melee hit); a sheathe takes it away at once. The owner ignores the primary action while
  its own unsheathe or sheathe plays. Death, a downing or a hit react during the unsheathe cancels it (the timer cleared,
  still sheathed).

### 3.4 What follows the held item (D80)

- **The character**: the action set, the primary action chain and the drawn grip read `GetHeldItem(MainHand)`;
  everything sheathed is `Unarmed` (punches, edit mode by hand). The melee and ranged abilities read the held slot for
  their weapon and its wear (`ApplyDurabilityLoss(GetHeldSlotTag(MainHand))`).
- **First person**: sheathed visuals hang on the body and hide with it (`UpdateEquipmentVisualVisibility`, D57).
- **The save**: the sheath slots save as slots; `FVCSnapshotEquipmentEntry` gains `bDrawn`, and the restore draws it
  (no montage). An old save's `MainHand` sword lands in its sheath, drawn (§3.1's rule).
- **NPCs**: unchanged. Their `HeldItem` stays in the hand; drawing on aggro is a follow-up (§8).

### 3.5 The UI (D81)

- **The equipment panel** lists the sheath slots with no code change ("Left Hip", "Back", "Belt"); the drawn one gets the
  gold "in hand" frame (`UEquipmentSlotWidget::SetDrawn`), and the Main Hand slot shows the held item (dimmed when it is
  a drawn sheath's, its home named in the tooltip).
- **The carried strip**: a code-built `UVCCarriedWeaponsWidget` beside the hotbar, one small icon per occupied sheath,
  the drawn one lit, the R / X hints under it. Display only.

### 3.6 Multiplayer (D82)

`EquipmentSlots` replicate as today with `bDrawn` in the slot. Requests: `ServerRPC_RequestDraw` /
`ServerRPC_RequestSheathe` (reliable; the predicate again on the server). The owner plays its montage at input and the
server multicasts it to the others (`PlayActionForEveryone`, the owner and a dedicated server skipping, D49); the
server flips the state at the hand-off; `OnRep_EquipmentSlots` sees a state change without an item change and re-seats
the visual (no rebuild: the mesh, its load and its light stay), links or unlinks the layer, fires `OnDrawnChanged`.
GAS changes on the authority only, as now. `VoxelEngine.Replication.Framework` gets a step: the client's draw reaches
the server, the client sees the drawn slot, the visual on `hand_r` and the set changed.

## 4. Phases

| Phase | Repos | Size |
|---|---|---|
| S1 The framework | CGF, IIP, EquipmentPlugin, parent gitlinks | M |
| S2 The character | VoxelCharacterPlugin, parent (input assets) | L |
| S3 The content and the fit | parent scripts, VoxelCharacterPlugin (notify only if needed) | M |
| S4 The UI, the replication step, the close | EquipmentPlugin, VoxelCharacterPlugin, parent | M |

### S1: The framework (M)
- CommonGameFramework: the three `Equipment.Slot.Sheath.*` tags; `FEquipmentSlotDefinition::DrawSlotTag`.
- ItemInventoryPlugin: `SheathSlotTag`, `SheathOffset`, `SheathBoneOverride` on `UItemFragment_Equipment`; its doc.
- EquipmentPlugin: `bDrawn` on the slot; `TryDraw` / `TrySheathe` / `SetDrawn`, the RPCs and `ValidateDraw`; the
  hand rule; `FindTargetSlot` and the hand-to-sheath mapping; `TryEquipFromInventory`'s draw; GAS and the layer on the
  drawn state for sheath slots; the visual's socket and offset by state; `OnRep_EquipmentSlots`'s re-seat branch;
  `GetHeldSlotTag` / `GetHeldItem` / `IsDrawn`; `OnDrawnChanged`; `Equipment.Equip` lands in the sheath and
  `Equipment.Draw <Slot>` / `Equipment.Sheathe` console commands. Tests `Equipment.Sheath.*` (target slot, draw and
  sheathe results, one per hand, the bag-full refusal, GAS on draw, held queries, the stale re-seat predicate, a client's
  request as an RPC). Docs: `EQUIPMENT_SYSTEM.md` ("Sheaths"), the plugin's instructions.

**S1 result (2026-11-08).** As designed (§3.1). The client's equip-and-draw checks only the equip at home (the draw's
check needs the equip done) and the server answers a refused draw through `OnOperationFailed`; a draw asked of a sheath
already drawn is `Success`. An item equipped into a sheath always arrives sheathed, also when its slot's previous
occupant was drawn. Tests `Equipment.Sheath.TargetSlot`, `DrawAndSheathe`, `GASWhileDrawn`, `AttachByState`,
`FromInventoryDraw`, `ClientRequests`. Gates: the build and the non-unity build of the changed modules clean, the
headless suite 876/876, the in-editor gate 27/27. PRs: CommonGameFramework#21, ItemInventoryPlugin#23,
EquipmentPlugin#18, UE5_PluginDev#374.

### S2: The character (L)
- The sheath slot definitions on `AVCCharacterBase`; `IA_Sheathe` (R) and `IA_CycleWeapon` (X) by
  `Scripts/add_input_action.py`; F's equip-and-draw; `GetHeldItem(MainHand)` in the action set, the primary chain, the
  drawn grip; the melee and ranged abilities' weapon and wear (Combat/*: after the bow-aiming session's PRs merge, a
  mechanical change, coordinated in the notes folder).
- `EVCAnimAction::Unsheathe` / `Sheathe`, the set's maps, `UVCAnimNotify_ItemHandOff` and `GetHandOffTime`; the owner's
  montage at input, the multicast, the server's hand-off timer and its cancels; the visual hand-off in
  `UpdateEquipmentAttachments`; the input gate.
- The save's `bDrawn` and the old-save mapping; `vc.Equip.Nudge` / `vc.Equip.PrintFit` on a sheath slot printing
  `SheathOffset` (and the body's entry for the sheath bone); `vox.Equip` lands in the sheath, `vox.Sheathe`.
- Tests `VoxelCharacter.EquipmentLoop.Sheath*` (the set follows the drawn item, Unarmed when sheathed, the pickaxe's
  mining speed only drawn, the save round trip with a drawn and a sheathed weapon, the hand-off time from a montage).
  Docs: `CHARACTER_SYSTEM.md` ("Items, progression and saves", "The primary action", "Input", "Animation").

**S2 result (2026-11-09).** The sequence lives in `VCCharacterBase_Sheath.cpp`: `RunSheathSequence` sheathes the drawn
weapon, then after that montage's length unsheathes the wanted one; the owner plays both at input, the server plays them
to the rest (`PlaySheathActionForEveryone`, its own multicast carrying the weapon's set and the sheath, since an
unsheathe starts under the Unarmed set), sheathes at once and draws at the hand-off by a timer. The visual's hand-off is
`UVCActionMontageComponent::IsSheathHandOffPending` read by `UpdateEquipmentAttachments` each tick while a montage plays
(`UpdateSheathHandOff`). Differences from §3.3: a hit react does not cancel an unsheathe (death and downing do; the
timer checks `IsIncapacitated`); the montages play in the default slot over the whole body, not the upper body, since
the Unarmed graph has no upper-body slot (G162). The melee and ranged abilities read `GetHeldItem` / wear
`GetHeldSlotTag` (two lines each, beside the bow aim fixes' open VoxelCharacterPlugin#85, which touches other lines of the
ranged file); `UnequipSlotToInventory` resolves a hand to the slot holding what is in it (the panel's double-click on
Main Hand, `vox.Unequip`). Tests `VoxelCharacter.EquipmentLoop.SheathSlots`, `SheathActionSet`, `SheathSaveEntry`,
`SheathHandOffTime`.

### S3: The content and the fit (M)
- `setup_character_layers.py`: the Unsheathe / Sheathe montages and the hand-off notify per §3.2, the sets' maps.
- `configure_item_visuals.py` / `configure_ranged_items.py`: each weapon's `SheathSlotTag` and `SheathOffset`; the
  sheath slots' corrections in the character's constructor.
- The fit on `CharacterTest`, both bodies (`vc.Appearance`), `vc.Equip.Nudge` per sheath: at rest, walking, running, a
  draw and a sheathe of each; Kellan's `AttachCorrections` for `pelvis` / `spine_05` if he needs them. Screenshots in
  `Saved/Screenshots/Claudius/sheath_*`.

**S3 result (2026-11-09).**
- **The hand-off measured, not judged by eye.** `setup_character_layers.py` reads each clip's bone poses
  (`AnimationLibrary.get_bone_poses_for_time`, composed down the Mannequin's chains) and puts `UVCAnimNotify_ItemHandOff`
  where the weapon's hand (`hand_r`; the bow's `hand_l`) comes nearest the sheath's bone (`thigh_l` for the hip, `spine_05`
  for the back), within 10-80% of an unsheathe and 20-95% of a sheathe; the report keeps the fraction and the distance
  (`Saved/CharacterLayersSetup.json`). SwordShield's hip: out at 0.228 (24 uu), in at 0.5; the back (TwoHand, Bow): out
  at 0.263, in at 0.512; the bow's left hand 32 uu off the spine at its hand-off. The montages: `AM_<Set>_Unsheathe_<Sheath>`
  and `AM_<Set>_Sheathe_<Sheath>` for SwordShield (HipLeft), TwoHand (Back), Tool (Belt) and Bow (Back).
- **The belt moved to the small of the back.** The pickaxe's stand-in clip (the two-handed sword's hip draw) reaches for
  the left hip; a pickaxe on the right hip would have left the hand closing on air. It lies across the small of the back,
  its handle's end at the left hip, the head to the right, where that hand meets it.
- **Fits computed from a wanted pose.** Rather than nudging, `Saved/claudius_sheath_fit.py` (a PIE helper) takes the
  pose wanted in the body's frame (an anchor bone, offsets along forward / right / up, the long axis and +X) and turns it
  into the sheath bone's space on the low-poly body; the results are `configure_item_visuals.py`'s `SHEATHS` and
  `configure_ranged_items.py`'s `BOW_SHEATH_FIT`. The sword hangs at the left hip, hilt forward, blade down and back; the
  bow crosses the back, its top over the right shoulder. Kellan wears the same fits a few cm off his slimmer hips: no
  `AttachCorrections` entry was needed to read right, one per bone can tighten it later.
- **Checked in PIE on `CharacterTest`** (screenshots `Saved/Screenshots/Claudius/fit2_*`, `draw_t*`, `kellan_*`,
  `bow_t*`): F on the pickaxe, the sword and the bow carries each and draws it, the one before going back (about 2 s per
  switch: the sheathe, then the unsheathe to its hand-off); the sword is in the right hand 0.3 s into its draw; the bow
  comes off the back into the left hand.
- The inputs: `IA_Sheathe` (R) and `IA_CycleWeapon` (X) by `add_input_action.py`. This worktree's
  `Content/CharacterAnimation` was a junction into the main checkout's pack; it became a private copy before the layers
  script ran, so the main checkout's pack is untouched until its own run after the merge.

### S4: The UI, the replication step, the close (M)
- `UEquipmentSlotWidget::SetDrawn`, the Main Hand slot's held item; `UVCCarriedWeaponsWidget` on the HUD.
- The `VoxelEngine.Replication.Framework` step (§3.6).
- QA rows: F2 (carry and draw by key, F's equip-and-draw keeping the old weapon on the body, the panel's sheaths, the
  bag-full refusal), F13 (the hand-off on both bodies, sheathed in first person, the bow from the back then drawing the
  string); `QA_TEST_PLAN.md` regenerated.
- The console guide's rows; FINDINGS; this plan frozen with a results paragraph in `EQUIPMENT_SYSTEM.md` and
  `CHARACTER_SYSTEM.md`.

**S4 result (2026-11-09).** The panel: `UEquipmentSlotWidget` frames a drawn sheath blue (`BackgroundColorFor`; gold
stays the click-to-move grab) and a hand slot shows the weapon drawn into it, dimmed. The strip: `UVCCarriedWeaponsWidget`
above the hotbar, one icon per carried weapon in the sheaths' order, the drawn one lit, the R / X hint; rebound with the
vitals. The replication test's steps 18 and 19: a dagger carried in a hip sheath, drawn at the client's request (the
sword in the hand slot goes to the bag) and sheathed, each state reaching the client and its `OnRep` acting on it. Tests
`Equipment.Sheath.SlotWidgetColour`, `VoxelCharacter.EquipmentLoop.SheathCarriedStrip`. Gates: the headless suite
882/882, the in-editor gate 27/27 with the replication test, parity identical. QA rows F2-10..13, F13-18..21; F2-04's
expected result now keeps the old weapon on the body.

Each phase: a PR per repository into `dev`, the gates of `Scripts/qa/README.md` (S1 and S2 the headless suites and the
editor gate with the replication test; S3 the content scripts on a clean run and the fit shots; S4 the editor gate).

## 5. Tests and gates

Headless: `Equipment.*`, `VoxelCharacter.*`. In the editor: `VoxelEngine.Replication.Framework` (dedicated server and a
client), `Scripts/qa/editor_gate.py`. By hand: the F2 and F13 rows on `CharacterTest` with both bodies.

## 6. Decisions

| ID | Question | Recommendation |
|---|---|---|
| D77 | The model: sheath slots with a drawn state on the slot, a sheathed flag on the main hand, or sheath slots whose item moves into the main hand on a draw | **Sheath slots with `bDrawn`** (§3.1): one home per item, the visual persists through the hand-off, GAS and the stance layer follow the drawn state; readers ask `GetHeldItem(MainHand)`; an owner with no sheaths behaves as today |
| D78 | Which sheaths, on which bones, for which items | **Three**: `HipLeft` (the swords, `pelvis`), `Back` (the bow, `spine_05`), `Belt` (the pickaxes, `pelvis` at the right hip); `HipRight` when an item needs it (§3.2) |
| D79 | How a draw and a sheathe are asked for and animated | **R toggles, X cycles, F on a bag weapon equips and draws it**; no auto-sheathe; `Unsheathe` / `Sheathe` montages per sheath from the packs' clips with a hand-off notify, the visual re-seated at it on every machine, the weapon usable from the hand-off (§3.3) |
| D80 | What follows the held item; the save; the NPCs | **Everything that reads the main hand reads the held item**; all sheathed = `Unarmed`; GAS of a sheath only while drawn; the save keeps `bDrawn` and maps an old main-hand weapon to its sheath drawn; **NPCs unchanged** (§3.4) |
| D81 | The UI | **The panel lists the sheaths and frames the drawn one; the Main Hand slot shows the held item; a carried strip beside the hotbar** (display only) (§3.5) |
| D82 | Multiplayer | **The server flips `bDrawn` at the hand-off (sheathing at once), the owner's montage at input, the multicast to the others (D49)**; `OnRep` re-seats without a rebuild; a step in the replication test (§3.6) |

## 7. Risks

- **The bow-aiming session's files.** The melee and ranged abilities (Combat/*) and the drawn grip in
  `AVCCharacterBase` are being changed in parallel; S2's reader changes there land after that session's PRs merge, as
  a rebase onto `dev`. The rest of S2 stays in the equipment bridge (`UpdateEquipmentAttachments`, the hotbar and equip
  flow, `RefreshActionSet`).
- **The clips' hand-off frame.** The packs' unsheathe clips reach a hilt on bones our skeletons lack (`weapon_r`,
  `weapon_l`); the hand-off fraction is read by eye per clip, and the bow's back clip ends on `hand_l` through its
  `AttachBoneOverride`, where the drawn grip (C1) takes over.
- **Two bodies.** Kellan's hips and back sit elsewhere than the Mannequin's; the per-bone corrections (D71) carry it, a
  correction per sheath bone on his appearance if needed.
- **GAS churn.** A draw grants and a sheathe revokes; abilities the player state grants (the attacks) are untouched,
  and an item ability active at a sheathe ends with it, as at an unequip today.
- **The bow and the torch on `hand_l`** clash today already (both on the left hand); unchanged here, a finding if it
  matters in the pass.

## 8. Not in scope

NPCs drawing on aggro; auto-sheathe after idle; a weapon wheel; a shield or a second one-hander (`HipRight`) sheathed;
dual wielding; quivers and arrows on the body; first-person arms (D57); new animation packs.

## Inputs

- `EQUIPMENT_SYSTEM.md`, `ITEM_INVENTORY_SYSTEM.md`, `CHARACTER_SYSTEM.md` ("The bow", "Animation", "Input",
  "Items, progression and saves"); `CHARACTER_REFINEMENT_PLAN.md` C1 (fits, D71), `RANGED_COMBAT_PLAN.md` (frozen).
- `Plugins/VoxelCharacterPlugin/Source/VoxelCharacterPlugin/Private/Core/VCCharacterBase.cpp` (slots, the equipment
  bridge, the hotbar flow, the snapshot), `VCEquipFitConsole.cpp`, `Combat/VCMeleeAttackAbility.cpp`,
  `Combat/VCRangedAttackAbility.cpp`.
- `Scripts/setup_character_layers.py`, `configure_item_visuals.py`, `configure_ranged_items.py`, `add_input_action.py`;
  `Content/EssentialAnimation/{SwordShield,BowAndArrow}/Animation/UE5/Sequence/**/Equip`.
- `Source/VoxelEngine/Private/Testing/FrameworkReplicationTest.cpp`; `Tools/QA/qa_rows.py`.
