#include "oam.cpp"

namespace {
  struct SpriteDumpEntry { uint32_t px[8 * 8]; };
  ::nall::map<string, SpriteDumpEntry> g_spriteDumpPending;
  ::nall::set<string> g_spriteDumpSeen;
  ::nall::set<uint64_t> g_spriteDumpSeenKeys;
  vector<string> g_spriteDumpOrder;
  uint g_spriteDumpBudget = 0;
  uint g_spriteDumpBudgetMax = 64;  // tiles per frame
}

auto PPU::Object::dumpSpriteTile(const OAM::Object& sprite, uint tx) -> void {
    // Early-out if no budget to process new tiles this frame
    if(g_spriteDumpBudget == 0) return;

    // Compose a fast integer key to deduplicate quickly without strings
    // Fields: type=1(spr), character (10 bits), palette (4 bits), hflip, vflip, tx (4 bits)
    uint tileWidth = sprite.width() >> 3;
    uint bppIndex = 1;  // 4bpp for sprites
    uint64_t key = 0;
    key |= 1ull << 60;  // mark as sprite
    key |= ((uint64)(sprite.character & 0x3ff)) << 0;
    key |= ((uint64)(sprite.palette & 0x0f)) << 12;
    key |= ((uint64)(bppIndex & 0x3)) << 16;
    key |= ((uint64)(sprite.hflip ? 1 : 0)) << 18;
    key |= ((uint64)(sprite.vflip ? 1 : 0)) << 19;
    key |= ((uint64)(tx & 0x0f)) << 20;

    if(g_spriteDumpSeenKeys.find(key)) return;

    // Resolve output directory
    auto dir = platform->path(SuperFamicom::ID::HDTileDump);
    if(!dir) return;

    // Build filename: SPR_Cxxxx_PBxxx_Hx_Vx.png
    uint bpp = 4;
    uint basePalette = 128 + (sprite.palette << 4);
    string filename = {
      dir,
      "SPR",
      "_C", pad(sprite.character, 4, '0'),
      "_TX", pad(tx, 2, '0'),
      "_PB", pad(basePalette, 3, '0'),
      "_B", bpp,
      "_H", (uint)sprite.hflip,
      "_V", (uint)sprite.vflip,
      ".png"
    };

    // Fast path: skip work if we've already handled this tile name
    if(g_spriteDumpSeen.find(filename)) { g_spriteDumpSeenKeys.insert(key); return; }
    if(g_spriteDumpPending.find(filename)) { g_spriteDumpSeen.insert(filename); g_spriteDumpSeenKeys.insert(key); return; }

    // Reconstruct 8x8 tile pixels from VRAM
    constexpr uint width = 8, height = 8;
    uint32_t pixels[width * height] = {};

    // Compute tile base address following bsnes' fetch() logic, but without y contribution
    uint16 tiledataAddress = io.tiledataAddress;
    if(sprite.nameselect) tiledataAddress += (1 + io.nameselect) << 12;
    uint16 chrx = (sprite.character & 15);
    uint16 chryPage = ((sprite.character >> 4) & 15) << 4;
    uint mx = !sprite.hflip ? tx : tileWidth - 1 - tx;
    uint baseIndex = chryPage + ((chrx + mx) & 15);  // tile index within page
    uint16 pos = tiledataAddress + (baseIndex << 4);

    for(uint yrow = 0; yrow < height; yrow++) {
      uint yaddr = sprite.vflip ? (height - 1 - yrow) : yrow;
      uint16 address = (pos & 0xfff0) + yaddr;
      uint16 data0 = ppu.vram[address + 0];
      uint16 data1 = ppu.vram[address + 8];

      // Reverse bit order so LSB corresponds to left-most pixel when no hflip
      if(!sprite.hflip) {
        auto rev = [](uint16 v) -> uint16 {
          v = (uint16)((v >> 4) & 0x0f0f) | (uint16)((v << 4) & 0xf0f0);
          v = (uint16)((v >> 2) & 0x3333) | (uint16)((v << 2) & 0xcccc);
          v = (uint16)((v >> 1) & 0x5555) | (uint16)((v << 1) & 0xaaaa);
          return v;
        };
        data0 = rev(data0);
        data1 = rev(data1);
      }

      uint16 d0 = (
        ((uint8(data0 >> 0) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 49) & 0x5555
      ) | (
        ((uint8(data0 >> 8) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 48) & 0xaaaa
      );
      uint16 d1 = (
        ((uint8(data1 >> 0) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 49) & 0x5555
      ) | (
        ((uint8(data1 >> 8) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 48) & 0xaaaa
      );

      for(uint x = 0; x < width; x++) {
        uint8 color = 0;
        color |= (d0 & 3) << 0; d0 >>= 2;
        color |= (d1 & 3) << 2; d1 >>= 2;

        uint16 paletteIndex = color ? (uint16)(basePalette + color) : 0;
        uint15 c15 = ppu.screen.paletteColor(paletteIndex);
        uint8 r5 = (c15 >> 0) & 31;
        uint8 g5 = (c15 >> 5) & 31;
        uint8 b5 = (c15 >> 10) & 31;
        uint8 r8 = (r5 << 3) | (r5 >> 2);
        uint8 g8 = (g5 << 3) | (g5 >> 2);
        uint8 b8 = (b5 << 3) | (b5 >> 2);
        uint8 a8 = (color == 0) ? 0 : 255;

        pixels[yrow * width + x] = (uint32)a8 << 24 | (uint32)r8 << 16 | (uint32)g8 << 8 | (uint32)b8;
      }
    }

    // Enqueue sprite tile into pending map, record as seen, and consume budget
    if(!g_spriteDumpPending.find(filename)) {
      SpriteDumpEntry entry = {};
      for(uint i = 0; i < width * height; i++) entry.px[i] = pixels[i];
      g_spriteDumpPending.insert(filename, entry);
      g_spriteDumpSeen.insert(filename);
      g_spriteDumpSeenKeys.insert(key);
      g_spriteDumpOrder.append(filename);
      if(g_spriteDumpBudget) g_spriteDumpBudget--;
    }
}

// Exposed to background.cpp's FlushHDTileDumpCache()
auto FlushSpriteDumpCache() -> void {
  if(g_spriteDumpPending.size() == 0) return;

  // Build sprite tilesheets: 16x16 tiles per sheet => 128x128 px per sheet
  auto dir = platform->path(SuperFamicom::ID::HDTileDump);
  if(dir && g_spriteDumpOrder.size()) {
    constexpr uint tileW = 8, tileH = 8;
    const uint tilesPerRow = 16, tilesPerCol = 16;
    const uint sheetW = tilesPerRow * tileW;
    const uint sheetH = tilesPerCol * tileH;

    // Collect in insertion order
    vector<const SpriteDumpEntry*> list;
    for(auto& name : g_spriteDumpOrder) {
      if(auto e = g_spriteDumpPending.find(name)) list.append(&e());
    }

    if(list.size()) {
      uint sheetIndex = 0;
      for(uint n = 0; n < list.size();) {
        vector<uint32_t> sheet; sheet.resize(sheetW * sheetH);
        for(uint i = 0; i < sheet.size(); i++) sheet[i] = 0x00000000u;
        uint take = (uint)(list.size() - n); if(take > 256u) take = 256u;
        for(uint i = 0; i < take; i++) {
          const SpriteDumpEntry* de = list[n + i];
          uint idx = i;
          uint col = idx % tilesPerRow;
          uint row = idx / tilesPerRow;
          uint dstX = col * tileW;
          uint dstY = row * tileH;
          for(uint y = 0; y < tileH; y++) {
            auto* src = &de->px[y * tileW + 0];
            auto* dst = &sheet[(dstY + y) * sheetW + dstX];
            for(uint x = 0; x < tileW; x++) dst[x] = src[x];
          }
        }
        string sheetName = {dir, "SPR_sheet_", pad(sheetIndex, 3, '0'), ".png"};
        ::nall::Encode::PNG::create(sheetName, &sheet[0], sheetW << 2, sheetW, sheetH, /*alpha=*/true);
        n += take;
        sheetIndex++;
      }
    }
  }

  g_spriteDumpPending.reset();
  g_spriteDumpOrder.reset();
}

auto PPU::Object::addressReset() -> void {
  ppu.io.oamAddress = ppu.io.oamBaseAddress;
  setFirstSprite();
}

auto PPU::Object::setFirstSprite() -> void {
  io.firstSprite = !ppu.io.oamPriority ? 0 : uint(ppu.io.oamAddress >> 2);
}

auto PPU::Object::frame() -> void {
  io.timeOver = false;
  io.rangeOver = false;
  // reset per-frame sprite dump budget
  g_spriteDumpBudget = g_spriteDumpBudgetMax;
}

auto PPU::Object::scanline() -> void {
  latch.firstSprite = io.firstSprite;

  t.x = 0;
  t.y = ppu.vcounter();
  t.itemCount = 0;
  t.tileCount = 0;

  t.active = !t.active;
  auto oamItem = t.item[t.active];
  auto oamTile = t.tile[t.active];

  for(uint n : range(32)) oamItem[n].valid = false;
  for(uint n : range(34)) oamTile[n].valid = false;

  if(t.y == ppu.vdisp() && !ppu.io.displayDisable) addressReset();
  if(t.y >= ppu.vdisp() - 1 || ppu.io.displayDisable) return;
}

auto PPU::Object::evaluate(uint7 index) -> void {
  if(ppu.io.displayDisable) return;
  if(t.itemCount > 32) return;

  auto oamItem = t.item[t.active];
  auto oamTile = t.tile[t.active];

  uint7 sprite = latch.firstSprite + index;
  if(!onScanline(oam.object[sprite])) return;
  ppu.latch.oamAddress = sprite;

  if(t.itemCount++ < 32) {
    oamItem[t.itemCount - 1] = {true, sprite};
  }
}

auto PPU::Object::onScanline(PPU::OAM::Object& sprite) -> bool {
  if(sprite.x > 256 && sprite.x + sprite.width() - 1 < 512) return false;
  uint height = sprite.height() >> io.interlace;
  if(t.y >= sprite.y && t.y < sprite.y + height) return true;
  if(sprite.y + height >= 256 && t.y < (sprite.y + height & 255)) return true;
  return false;
}

auto PPU::Object::run() -> void {
  output.above.priority = 0;
  output.below.priority = 0;

  auto oamTile = t.tile[!t.active];
  uint x = t.x++;

  for(uint n : range(34)) {
    const auto& tile = oamTile[n];
    if(!tile.valid) break;

    int px = x - (int9)tile.x;
    if(px & ~7) continue;

    uint color = 0, shift = tile.hflip ? px : 7 - px;
    color += tile.data >> shift +  0 & 1;
    color += tile.data >> shift +  7 & 2;
    color += tile.data >> shift + 14 & 4;
    color += tile.data >> shift + 21 & 8;

    if(color) {
      if(io.aboveEnable) {
        output.above.palette = tile.palette + color;
        output.above.priority = io.priority[tile.priority];
      }

      if(io.belowEnable) {
        output.below.palette = tile.palette + color;
        output.below.priority = io.priority[tile.priority];
      }
    }
  }
}

auto PPU::Object::fetch() -> void {
  auto oamItem = t.item[t.active];
  auto oamTile = t.tile[t.active];

  for(uint i : reverse(range(32))) {
    if(!oamItem[i].valid) continue;

    if(ppu.io.displayDisable || ppu.vcounter() >= ppu.vdisp() - 1) {
      ppu.step(8);
      continue;
    }

    ppu.latch.oamAddress = 0x0200 + (oamItem[i].index >> 2);
    const auto& sprite = oam.object[oamItem[i].index];

    uint tileWidth = sprite.width() >> 3;
    int x = sprite.x;
    int y = t.y - sprite.y & 0xff;
    if(io.interlace) y <<= 1;

    if(sprite.vflip) {
      if(sprite.width() == sprite.height()) {
        y = sprite.height() - 1 - y;
      } else if(y < sprite.width()) {
        y = sprite.width() - 1 - y;
      } else {
        y = sprite.width() + (sprite.width() - 1) - (y - sprite.width());
      }
    }

    if(io.interlace) {
      y = !sprite.vflip ? y + ppu.field() : y - ppu.field();
    }

    x &= 511;
    y &= 255;

    uint16 tiledataAddress = io.tiledataAddress;
    if(sprite.nameselect) tiledataAddress += 1 + io.nameselect << 12;
    uint16 chrx =  (sprite.character & 15);
    uint16 chry = ((sprite.character >> 4) + (y >> 3) & 15) << 4;

    for(uint tx : range(tileWidth)) {
      uint sx = x + (tx << 3) & 511;
      if(x != 256 && sx >= 256 && sx + 7 < 512) continue;
      if(t.tileCount++ >= 34) break;

      uint n = t.tileCount - 1;
      oamTile[n].valid = true;
      oamTile[n].x = sx;
      oamTile[n].priority = sprite.priority;
      oamTile[n].palette = 128 + (sprite.palette << 4);
      oamTile[n].hflip = sprite.hflip;

      uint mx = !sprite.hflip ? tx : tileWidth - 1 - tx;
      uint pos = tiledataAddress + ((chry + (chrx + mx & 15)) << 4);
      uint16 address = (pos & 0xfff0) + (y & 7);

      if(!ppu.io.displayDisable)
      oamTile[n].data  = ppu.vram[address + 0] <<  0;
      ppu.step(4);

      if(!ppu.io.displayDisable)
      oamTile[n].data |= ppu.vram[address + 8] << 16;
      ppu.step(4);

      // Deferred sprite tile dump (deduplicated, budget limited)
      if(configuration.hacks.ppu.hdTileDump) {
        dumpSpriteTile(sprite, tx);
      }
    }
  }

  io.timeOver  |= (t.tileCount > 34);
  io.rangeOver |= (t.itemCount > 32);
}

auto PPU::Object::power() -> void {
  for(auto& object : oam.object) {
    object.x = 0;
    object.y = 0;
    object.character = 0;
    object.nameselect = 0;
    object.vflip = 0;
    object.hflip = 0;
    object.priority = 0;
    object.palette = 0;
    object.size = 0;
  }

  t.x = 0;
  t.y = 0;

  t.itemCount = 0;
  t.tileCount = 0;

  t.active = 0;
  for(uint p : range(2)) {
    for(uint n : range(32)) {
      t.item[p][n].valid = false;
      t.item[p][n].index = 0;
    }
    for(uint n : range(34)) {
      t.tile[p][n].valid = false;
      t.tile[p][n].x = 0;
      t.tile[p][n].priority = 0;
      t.tile[p][n].palette = 0;
      t.tile[p][n].hflip = 0;
      t.tile[p][n].data = 0;
    }
  }

  io.aboveEnable = random();
  io.belowEnable = random();
  io.interlace = random();

  io.baseSize = random();
  io.nameselect = random();
  io.tiledataAddress = (random() & 7) << 13;
  io.firstSprite = 0;

  for(auto& p : io.priority) p = 0;

  io.timeOver = false;
  io.rangeOver = false;

  latch = {};

  output.above.palette = 0;
  output.above.priority = 0;
  output.below.palette = 0;
  output.below.priority = 0;

  // reset sprite dump caches and budget
  g_spriteDumpSeen.reset();
  g_spriteDumpSeenKeys.reset();
  g_spriteDumpPending.reset();
  g_spriteDumpOrder.reset();
  g_spriteDumpBudget = 0;
}
