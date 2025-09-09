---
trigger: manual
---

RULE: Implementing HD tile dumping and replacement for any emulator

1. HASHING
- Always hash the raw native tile data + palette ID (if applicable).
- Never hash scaled or rendered HD tiles.
- Each frame of an animation is treated as a separate tile with its own hash.
- Hash uniquely identifies the tile and remains stable across scale, system, or replacement passes.

2. DUMPING
- Save tiles as 32-bit RGBA PNGs with alpha.
- Allow arbitrary integer scaling (2×, 4×, 10×, etc.).
- File naming: hash.png, stored in hd_dump/tiles/.
- Use a memory buffer to store dump candidates; write to disk only when recording stops.
- Detect and ignore tiles affected by palette fades (fade-in/fade-out) to prevent dumping transient tiles.
- At render time, optionally apply in-game fade effects dynamically to HD tiles without modifying original PNGs.

3. MANIFEST / HDNes-style `hires.txt`
- Each tile entry contains:  
  `hash,tile_index,x,y,width,height,plane,palette`
- Optional comment after `;` for notes.
- Support:
  - **Groups / Metatiles:** multiple tiles may form a larger HD replacement; each retains its own hash.
  - **Multi-plane/layer:** separate entries for BG1, BG2, SPRITES, etc.
  - **Animated tiles:** each frame is a separate hash entry; sequences handled externally.
  - **Coordinates:** x,y,width,height to locate tile in sheet or HD PNG.
  - **Scale factor:** recorded per tile or manifest-wide.
  - **Versioning:** include a version field for forward compatibility.
  - **Optional notes** for human-readable metadata.

4. MANIFEST-AWARE BEHAVIOR
- Load the `hires.txt` manifest at ROM load.
- For each entry, record hash, scale factor, plane/layer, palette, coordinates, group info.
- Skip dumping tiles whose hash already exists in the manifest.
- Lock HD scale to the manifest-specified scale; user cannot modify.

5. RENDERING / REPLACEMENT
- Recompute raw tile hash + palette at runtime.
- If a manifest entry exists for the hash, render the corresponding HD PNG at declared scale.
- If no replacement exists, render the native tile.
- Plane-specific replacements affect only their plane/layer.
- Groups and sequential tiles for animation are handled externally by emulator or HD pack loader.
- Scaling, replacement, and hashing remain decoupled.

6. GENERAL / SYSTEM-AGNOSTIC
- Do not hard-code tile sizes; read from system specifications.
- Must work across multiple consoles: NES, PC Engine, Mega Drive, GBA, NeoGeo, etc.
- Fully compatible with HDNes `hires.txt` style while supporting extended features like multi-plane, animation, groups, fade handling, and prescale lock.

