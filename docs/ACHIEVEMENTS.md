# Achievements (Recomp Achievements)

Witch Time integrates with [Recomp Achievements](https://github.com/elripalda/RecompAchievements) (`runtime/achievements.c`, `runtime/game_shim_eh.c`; see [RE_NOTES.md](RE_NOTES.md#12-achievements-recomp-achievements)).

- **Total**: **50** achievements (**1,240** points):
  - **19 Story Chapter Clears** (IDs `1..19`, 190 pts — 10 pts each, any difficulty).
  - **3 Game Completion / Difficulty Achievements** (IDs `20..22`, 150 pts — higher difficulties retroactively unlock lower ones).
  - **2 All-Chapter Mastery Achievements** (IDs `23`, `50`, 100 pts — all Platinum on any difficulty; all Pure Platinum on ∞ Climax).
  - **19 ∞ Climax Pure Platinum Chapter Achievements** (IDs `24..42`, 475 pts — 25 pts each, Pure Platinum on ∞ Climax only).
  - **2 Secret Boss Achievements** (IDs `43..44`, 100 pts — Golem & Rodin).
  - **2 Arsenal & Infernal Demon Achievements** (IDs `45..46`, 100 pts — all 13 Bayonetta 3 weapons & all 12 Infernal Demons).
  - **3 Witch Trial Achievements** (IDs `47..49`, 125 pts — Lost Chapters: Witch Trials 1–3).
- **List**: [achievements.json](achievements.json) is the list RecompLauncher hosts (format in the [spec](https://github.com/elripalda/RecompAchievements/blob/main/docs/SPEC.md)), an `events` list: each unlock goes out as its row's event name (`wt_ach_defs` in `runtime/achievements.h`). The file is written from that table (`tests/achievements_test.c --write`) and the test fails when the two differ. Points are 5, 10, 25 or 50, the only values a list takes. The 19 chapter clears are `progression` and The Single Truth is the `win`, so those 20 make the game Beaten. Event names of a released list are never renamed.
- **Unlocks & saves**: Live unlocks fire through `h_chapter_record_save` (`f_7100b2a6c0`), `h_side_chapter_record_save` (`f_7100b2c740`), `h_weapon_unlock` (`f_7100b2cf90`), and `h_demon_unlock` (`f_7100b2d050`). Pre-existing progress loaded from `SAVE:/SlotData_*.dat` is reconciled from chapter records (`f_7100b29be0`) and save bitmasks (`save_obj + 6492` / `+ 6496`) and fired right after a `save_loaded` message, which the launcher keeps as Casual (an event has no `earlier` flag).
- **Casual mode**: Executing any mutating `WT_DEBUG` command marks the session Casual (`reason: "debug"`).

---

## Game Completion & Mastery (5)

| ID | Title | Pts | Condition |
|---:|---|---:|---|
| 20 | The Single Truth | 50 | Complete all 19 story chapters on any difficulty (Casual, Standard, Expert, or ∞ Climax). |
| 21 | Umbran Expert | 50 | Complete all 19 story chapters on **Expert** (Hard) or higher (also unlocks `#20`). |
| 22 | Infinite Climax | 50 | Complete all 19 story chapters on **∞ Climax** (highest difficulty; retroactively unlocks `#21` and `#20`). |
| 23 | Platinum Witch | 50 | Earn a **Platinum** or **Pure Platinum** award on all 19 story chapters (any difficulty). |
| 50 | Pure Platinum Witch | 50 | Earn a **Pure Platinum** award on all 19 story chapters on **∞ Climax**. |

## Secret Bosses (2)

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 43 | `0x1000000` | The Artifact Unleashed | 50 | Defeat Golem in Extra: The Artifact Unleashed (any difficulty). |
| 44 | `0x10000000` | Devil's Duel | 50 | Defeat Rodin in Devil's Duel. |

## Arsenal & Infernal Demons (2)

| ID | Title | Pts | Objective |
|---:|---|---:|---|
| 45 | Arsenal of Darkness | 50 | Unlock all 13 Bayonetta 3 weapons (excludes Bayonetta 1 & 2 save-data bonuses Scarborough Fair and Love Is Blue). |
| 46 | Infernal Pact | 50 | Unlock all 12 Infernal Demons. |

## Lost Chapters: Witch Trials (3)

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 47 | `0x100000` | Witch Trial 1 Clear | 25 | Complete Lost Chapter: Witch Trial 1. |
| 48 | `0x100001` | Witch Trial 2 Clear | 50 | Complete Lost Chapter: Witch Trial 2. |
| 49 | `0x100002` | Witch Trial 3 Clear | 50 | Complete Lost Chapter: Witch Trial 3. |

## Story Chapter Clears (19)

Unlocked on any difficulty (Casual, Standard, Expert, or ∞ Climax):

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 1 | `0x01` | Prologue Clear | 10 | Complete Prologue: A Chaotic Encounter on any difficulty. |
| 2 | `0x02` | Chapter 1 Clear | 10 | Complete Chapter 1: Scrambling for Answers on any difficulty. |
| 3 | `0x03` | Chapter 2 Clear | 10 | Complete Chapter 2: City on Fire on any difficulty. |
| 4 | `0x04` | Chapter 3 Clear | 10 | Complete Chapter 3: A Sinking Feeling on any difficulty. |
| 5 | `0x05` | Side Chapter 1 Clear | 10 | Complete Side Chapter 1: The Crimson Shadow on any difficulty. |
| 6 | `0x06` | Chapter 4 Clear | 10 | Complete Chapter 4: Worlds Apart on any difficulty. |
| 7 | `0x07` | Chapter 5 Clear | 10 | Complete Chapter 5: Hot Pursuit on any difficulty. |
| 8 | `0x08` | Chapter 6 Clear | 10 | Complete Chapter 6: Off the Rails on any difficulty. |
| 9 | `0x09` | Side Chapter 2 Clear | 10 | Complete Side Chapter 2: The Lynx Strikes on any difficulty. |
| 10 | `0x0a` | Chapter 7 Clear | 10 | Complete Chapter 7: Burning Sands on any difficulty. |
| 11 | `0x0b` | Chapter 8 Clear | 10 | Complete Chapter 8: A Croaking Chorus on any difficulty. |
| 12 | `0x0c` | Chapter 9 Clear | 10 | Complete Chapter 9: Learning to Fly on any difficulty. |
| 13 | `0x0d` | Side Chapter 3 Clear | 10 | Complete Side Chapter 3: High Stakes on any difficulty. |
| 14 | `0x0e` | Chapter 10 Clear | 10 | Complete Chapter 10: Cover of Night on any difficulty. |
| 10 | `0x0f` | Chapter 11 Clear | 10 | Complete Chapter 11: A Familiar Dance on any difficulty. |
| 16 | `0x10` | Chapter 12 Clear | 10 | Complete Chapter 12: The Edge of Madness on any difficulty. |
| 17 | `0x11` | Side Chapter 4 Clear | 10 | Complete Side Chapter 4: The Doctor is Out on any difficulty. |
| 18 | `0x12` | Chapter 13 Clear | 10 | Complete Chapter 13: Kingdom of One on any difficulty. |
| 19 | `0x13` | Chapter 14 Clear | 10 | Complete Chapter 14: The Single Truth on any difficulty. |

## ∞ Climax Pure Platinum Chapter Mastery (19)

Unlocked **only on ∞ Climax** with a **Pure Platinum** award:

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 24 | `0x01` | Prologue: Pure Climax | 25 | Earn a Pure Platinum award in Prologue: A Chaotic Encounter on ∞ Climax. |
| 25 | `0x02` | Chapter 1: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 1: Scrambling for Answers on ∞ Climax. |
| 26 | `0x03` | Chapter 2: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 2: City on Fire on ∞ Climax. |
| 27 | `0x04` | Chapter 3: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 3: A Sinking Feeling on ∞ Climax. |
| 28 | `0x05` | Side Chapter 1: Pure Climax | 25 | Earn a Pure Platinum award in Side Chapter 1: The Crimson Shadow on ∞ Climax. |
| 29 | `0x06` | Chapter 4: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 4: Worlds Apart on ∞ Climax. |
| 30 | `0x07` | Chapter 5: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 5: Hot Pursuit on ∞ Climax. |
| 31 | `0x08` | Chapter 6: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 6: Off the Rails on ∞ Climax. |
| 32 | `0x09` | Side Chapter 2: Pure Climax | 25 | Earn a Pure Platinum award in Side Chapter 2: The Lynx Strikes on ∞ Climax. |
| 33 | `0x0a` | Chapter 7: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 7: Burning Sands on ∞ Climax. |
| 34 | `0x0b` | Chapter 8: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 8: A Croaking Chorus on ∞ Climax. |
| 35 | `0x0c` | Chapter 9: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 9: Learning to Fly on ∞ Climax. |
| 36 | `0x0d` | Side Chapter 3: Pure Climax | 25 | Earn a Pure Platinum award in Side Chapter 3: High Stakes on ∞ Climax. |
| 37 | `0x0e` | Chapter 10: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 10: Cover of Night on ∞ Climax. |
| 38 | `0x0f` | Chapter 11: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 11: A Familiar Dance on ∞ Climax. |
| 39 | `0x10` | Chapter 12: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 12: The Edge of Madness on ∞ Climax. |
| 40 | `0x11` | Side Chapter 4: Pure Climax | 25 | Earn a Pure Platinum award in Side Chapter 4: The Doctor is Out on ∞ Climax. |
| 41 | `0x12` | Chapter 13: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 13: Kingdom of One on ∞ Climax. |
| 42 | `0x13` | Chapter 14: Pure Climax | 25 | Earn a Pure Platinum award in Chapter 14: The Single Truth on ∞ Climax. |
