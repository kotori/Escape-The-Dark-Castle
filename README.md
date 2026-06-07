# Escape the Dark Castle - Unofficial Allegro 5 Edition

A digital tabletop translation of the atmospheric, cooperative rogue-lite board game *Escape the Dark Castle*. Built natively in C++ using the **Allegro 5** multimedia framework and backed by an embedded SQLite database layer for dynamic asset storage.

---

## 🎮 Core Features

* **Dual-Player Cooperative Crawl:** Command two distinct prisoners (Primary Hero and Companion) working in tandem to escape the shifting cells.
* **SQLite-Driven Game Assets:** Characters, item attributes, text modifiers, and threat rooms are parsed directly from an embedded game database instead of being hardcoded.
* **Turn-Aware Engagement Loop:** Solo and cooperative interaction state routing loops dynamically track who is active to bat or strike during room threats.
* **Bone Dice Simulation System:** True-to-board dice array faces (Might, Cunning, Wisdom, Shield) simulated via rapid pseudo-randomized logic.
* **Dynamic Visual Overlay Panels:** Polished, context-aware interface modals handle weapon re-rolls, branching paths, character metrics, and game statuses.

---

## 🕹️ Game Controls Reference

The engine utilizes a clean, context-sensitive keyboard configuration map that automatically shifts input handlers depending on the active scene or phase state.

### 🌌 1. Main Title Screen


| Key | Action | Description |
| :--- | :--- | :--- |
| **`W` / `S`** or **`UP` / `DOWN`** | Highlight Row | Toggle cursor menu index color frames between choices. |
| **`ENTER` / `SPACEBAR`** | Confirm Action | Advance cleanly into cell slots or exit the runtime binary. |

### 👥 2. Prisoner Character Selection Grid
The selection grid isolates players into independent, turn-based lock-in phases to prevent frame stutters or accidental double choices.


| Key | Action | Description |
| :--- | :--- | :--- |
| **`A` / `D`** or **`LEFT` / `RIGHT`** | Cycle Prisoners | Scroll across the 6 portrait cards horizontally. |
| **`ENTER` / `SPACEBAR`** | Confirm Selection | **Press 1:** Locks Player 1 (Hero) and drops a persistent mask over their choice.<br>**Press 2:** Locks Player 2 (Companion) and initiates the dungeon crawl deck. |

### 🗺️ 3. Active Gameplay & Interactive Tabletop Loops


| Key | Action | Description |
| :--- | :--- | :--- |
| **`1` / `2`** | Flip Chapter Door | Select room leader on an un-flipped door. `1` = Hero, `2` = Companion. |
| **`R`** | Rest Toggle | Cycle recovery state loops between encounters. (*Illegal during Skill Challenges*) |
| **`LEFT` / `RIGHT` Arrows** | Focus Inventory Slot | Dynamically cycles the golden selector cursor over Slot 1 or Slot 2 for the **currently acting player**. |
| **`LEFT SHIFT`** | Consume Gear (P1) | Instantly drink provisions or trigger modifiers from Player 1's focused inventory slot. |
| **`RIGHT SHIFT`** | Consume Gear (P2) | Instantly drink provisions or trigger modifiers from Player 2's focused inventory slot. |
| **`BACKSPACE`** | Discard Gear (P1) | Forcefully drop/free Player 1's highlighted item slot cell before a reward splits. |
| **`DELETE`** | Discard Gear (P2) | Forcefully drop/free Player 2's highlighted item slot cell before a reward splits. |
| **`SPACEBAR`** | Resolve Active Threat | Execute randomized trait rolls during standard combat rounds or active puzzle steps. |
| **`N`** | Advance Room | Safely progress the party to the next chapter card once all live threats are zeroed out. |

### 🛡️ 4. Context-Specific Modal Prompts
* **Weapon Re-Roll Modal (`is_awaiting_reroll_choice`):** Tap **`Y`** to expend a weapon charge and roll your active dice faces again, or tap **`N`** to accept the current face value outcome.
* **Greatsword Cleaving Selector (`is_awaiting_greatsword_choice`):** Use **`A` / `D`** (or **`LEFT` / `RIGHT`**) to move your slicing target cursor over live monster shields, then slam **`SPACEBAR` / `ENTER`** to shatter the selected icon threat.
* **Loot Distribution Overlays (`is_awaiting_loot_choice`):** Tap **`LEFT` / `RIGHT`** to move the target selection highlight between characters, hit **`ENTER` / `SPACEBAR`** to assign the item, or hit **`X`** to discard the treasure into the dust.

---

## 🛠️ Development & Compiling

The production repository utilizes CMake pipelines to resolve local shared library files and compile the executable binary targets.

### Prerequisites (Debian/Ubuntu Linux Layout)
Ensure your native development package pools include the core Allegro 5 development libraries and SQLite3 modules:
```bash
sudo apt-get update
sudo apt-get install build-essential cmake liballegro5-dev libsqlite3-dev
```

### Clean Rebuilding Sequence
To build fresh without caching artifacts throwing off vertical alignments or header variable declarations, run this forced reconstruction pipeline:
```bash
cd ~/devel/escape_dark_castle
mkdir -p build && cd build
rm -rf *
cmake ..
make
```

### Execution
Run the compiled binary output target directly inside your build terminal directory path:
```bash
./escape_dark_castle
```
