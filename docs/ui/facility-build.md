# 原版 UI 规格：设施类别选择（大块地建设 / 改建卡）

Scope: the screen a human sees when a building is put on an empty facility lot (大塊地), and when 改建卡
changes a facility's type. Conventions (anchors, MKF numbering, effects) as in `messages.md`. Asm paths
are relative to `.local/upstream/asm`.

## 1. Screen (`fcn_00440aac`, rich4.asm:21443; window proc `fcn_0043fae4`, rich4.asm:20210)

`fcn_00440aac(cancellable)` saves rect (0,40)–(440,480), draws, then waits for the 0x402 result.

- **Font.** `create_font(0x10, 0xf0f0f0, 0x101010, 3, 1)`: bold 16 px white with a drop shadow.
- **Strip.** `Data.mkf 517:4` (355×83, five 68×68 icons in a frame), opaque at (0x2b,0x117) = **(43,279)**.
  Slot *i* = facility type *i*, top-left **(50+68i, 286)**:

  | Slot | Icon | Name (0x475150, drawn in the box) | Max level (`0x474940`) |
  | --- | --- | --- | --- |
  | 0 | PARK | `公  園` | 1 |
  | 1 | Zzz bed | `旅  館` | 5 |
  | 2 | shopping cart | `購物中心` | 5 |
  | 3 | car | `加油站` | 1 |
  | 4 | DNA | `研究所` | 5 |

- **Box.** `Data.mkf 517:5` (the 249×170 message box) transparent at (220,140); prompt
  `請選擇設施類別` (0x465289) mode 2 at **(220,122)**. No price or other text.
- **Cursor.** WM 0x401 warps the mouse to (220,320); `lParam` (the `cancellable` argument) is kept in
  `0x48c528`, the hovered slot+1 in `0x48c524` (0 = none).
- **Hover** (WM_MOUSEMOVE, zone x 50–389, y 286–353, slot = (x−50)/68):
  - Entering a new slot plays **Effect 0** (0x48231a).
  - The previous slot is restored from the strip (sub-blit `fcn_0045643d`).
  - Three nested **0xffff00** frames are drawn: 68×68 at (50+68i, 286), 66×66 at +1, 64×64 at +2.
  - The box is redrawn with the prompt plus the slot's name, mode 4 at **(220,154)**.
  - Leaving the zone restores the slot and redraws the box with the prompt only.
- **Click** (WM_LBUTTONUP 0x202, only while a slot is hovered): **Effect 1** (0x482322); the result is the
  slot index 0..4.
- **Right-click** (WM_RBUTTONUP 0x205): only if `cancellable`. It plays **Effect 4** (0x482332) and the
  result is −1. Otherwise right-click does nothing, so the choice is forced.
- Computers never see the screen.

## 2. Flows

| Flow | asm | Cancellable | Notes |
| --- | --- | --- | --- |
| Landing on one's own empty facility lot | core_actions:1129 (0x41a22f) | no | No yes/no first. The cost is the lot's land price (+0x22) × price index; if cash is short, the not-enough-cash box (0x46398b) shows instead. Result → type (+0x18), level 1, Effect 0x32 (sound record 0x4823da), cash deducted. Computers: `rand()%4+1`. |
| `fcn_0040b110` "upgrade a building" on a level-0 facility (rich4.asm:4638) | callers: construction company (`請選擇欲加蓋地點` then Data 553, core_actions:2028; owner: twice, core_actions:1824/1830), 天使卡 (card_tianshika:182), 福神 (gods:1269/1639), magic house (magic_house:413), 機器工人 (tool_jiqigongren:83) | no | A human always picks, even for another player's lot. A computer picks `rand()%4+1` on its own lot and type 0 (公園) on others'. |
| 改建卡 on a facility with a building | card_gaijianka:104 | **yes** | The 0x481252 card line bubble comes first. −1 = card not used. Result 0 or 3 caps the level at 1. Computers: `_rich4_get_ai_card_param_value(0)`. |

Buying an **unowned** facility lot and **upgrading** a facility use the yes/no box (`messages.md` §2.2),
not this screen.

## 3. Native (`tools/screens/facility.{hpp,cpp}`)

- Hooked from `draw_decision` for `DecisionKind::construct_facility` and for the 改建卡 `action_target`
  whose prompt is `請選擇設施類別`. Option ids stay `210 + option index`; slot *i* maps to the option
  whose value is *i*.
- **Touch.**
  - The first tap on a slot selects it, which shows what hover showed: the yellow frames and the name.
    It plays Effect 0.
  - A second tap on the same slot, or **確定** (id 207), commits with Effect 1.
  - **取消**, Android Back or Escape answer the cancel option with Effect 4. The cancel option and
    button exist only for 改建卡, which the core now offers as `{"取消", -1}`, matching the original
    right-click.
- **Divergence (rules, left unchanged).**
  - The original lets a human build a **公園** (slot 0) on landing and in the `fcn_0040b110` flows.
  - The core's `construct_facility` options are only 1..4, and value 0 means "give up" there. The park
    slot is therefore drawn dimmed and inert.
  - Allowing it needs a core change and a new test.
- Review shots: `--overlay-gallery` writes `gallery-rules-facility-{type,type-selected,rebuild,rebuild-selected}.ppm` and `gallery-rules-research{,-selected,-locked}.ppm`.

## 4. Research Institute tool picker (`fcn_0044101d`, `fcn_004402d7`)

Triggered at `loc_0041b0b3` after landing on a facility tile (`4000 < code < 6000`) and running property-god effects (`0x40f381`) and flight bombardment (`0x448a7e`), whenever:
- `facility.owner_code == current_player + 1`,
- the player is not sleepwalking (`days_sleep_walking == 0`),
- `facility.facility_type == 4` (研究所), `facility.level != 0`, and `(facility.temporary_state & 0x0f) == 0` (not sealed by 查封卡).

- **Box.** `Data.mkf 517:5` transparent at (220,140); prompt `請選擇欲開發道具` (`0x465298`) mode 2 at (220,122). Selected/hovered tool name from `0x47ff22` at (220,154).
- **Strip.** `Data.mkf 517:7` (400×89), opaque at (20,280). Five 66×54 slots at `(35+76i, 297)` displaying `Panel.mkf 11:(10+i)` centered at `(68+76i, 324)`:
  1. Level 1: `機器工人` (tool 9)
  2. Level 2: `時光機` (tool 10)
  3. Level 3: `傳送機` (tool 11)
  4. Level 4: `工程車` (tool 12)
  5. Level 5: `核子飛彈` (tool 13)
- **Locked slots** (`i >= facility.level`): greyscaled (`0x4553fe`); hovering/tapping previews the tool name, clicking cannot commit.
- **Hover / selection:** two nested `0xffff00` outlines (`71×59` at `(32+76i, 294)` and `69×57` at `+1`), Effect 0.
- **Commit / cancel:** clicking an unlocked slot plays Effect 1 and writes `facility[+0x1d] = slot + 1` (`research_tool`) and `facility[+0x1e] = 5` (`research_days`). Right-click / Escape / `取消` plays Effect 4 and leaves existing research unchanged. Computers and 託管 players skip the UI and set `research_tool = level`, `research_days = 5`.
- **Turn-start tick (`fcn_0041c84f`, `0x41cd8c..0x41ce38`):** on the owner's turn start, each owned Research Institute with `research_days > 0` clears `research_days = 0` if `research_tool > level`, otherwise decrements `research_days`; on reaching 0 it displays `"%s開發完成！"` (`0x463b68`, 1500 ms) and grants tool `research_tool + 8` via `receive_tool` (capped at 9, independent of shared `tool_supply`).
- **Tile popup (`fcn_00417559`, `0x417ac2..0x417adf`):** shows the developing tool's name on line 4 (`dy = 54`) whenever `research_days != 0`.
