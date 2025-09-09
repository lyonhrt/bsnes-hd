#include <nall/hash/crc32.hpp>
#include "mode7.cpp"

// HD Pack loader/cache groundwork + HD Tile dump deferral
namespace {
  struct HDEntry {
    ::nall::image img;
    bool loaded = false;
    bool present = false; // file exists (png/bmp) but may not be loaded yet
    bool checkedPresence = false; // whether we've checked the filesystem yet
    // Precomputed 8x8 samples for fast lookup: 15-bit color and alpha
    uint16 sample15[64];
    uint8  sampleA[64];
    bool sampleReady = false;
  };

  ::nall::map<string, HDEntry> g_hdCache;
  // Fast key->stem map to avoid regenerating strings during rendering
  ::nall::map<uint64_t, string> g_hdStemByKey;
  // Direct key->entry map to avoid string lookups during rendering
  ::nall::map<uint64_t, HDEntry*> g_hdEntryByKey;
  
  // Manifest-based tilesheet replacement (hash -> precomputed 8x8 samples)
  struct ManifestEntry { uint16 sample15[64]; uint8 sampleA[64]; };
  ::nall::map<uint32_t, ManifestEntry> g_manifestMap;  // hash -> entry
  bool g_manifestLoaded = false;     // whether we've attempted to load a manifest
  bool g_manifestAvailable = false;  // whether a manifest with at least one entry is present
  
  // Cache tile-hash per identity to avoid recomputing
  ::nall::map<uint64_t, uint32_t> g_hdKeyToHash;
  bool g_hdInitialized = false;
  string g_hdBasePath;

  // In-memory pending HD tile dumps; key is full output filename
  struct DumpEntry { uint32_t px[8 * 8]; };
  ::nall::map<string, DumpEntry> g_hdDumpPending;
  // Cache of filenames we've already enqueued or detected as existing on disk this session
  ::nall::set<string> g_hdDumpSeen;
  // Fast integer key for seen tiles to avoid building strings when unnecessary
  ::nall::set<uint64_t> g_hdDumpSeenKeys;
  // Stable insertion order for building tilesheets
  vector<string> g_hdDumpOrder;
  // Per-frame budget to limit how many new tiles we reconstruct and enqueue
  uint g_hdDumpBudget = 0;
  uint g_hdDumpBudgetMax = 64;  // tuneable: tiles per frame

  // HD pack runtime budgets (to smooth FPS with HD pack ON)
  uint g_hdPresenceBudget = 0;      // file existence checks per frame
  uint g_hdPresenceBudgetMax = 16;  // tighter default to reduce I/O
  uint g_hdLoadBudget = 0;          // image loads + precompute per frame
  uint g_hdLoadBudgetMax = 1;       // very conservative to avoid spikes
  uint g_hdSampleRowBudget = 0;     // number of new HD rows we may compute per frame
  uint g_hdSampleRowBudgetMax = 256; // conservative row precompute budget
  uint g_hdHashBudget = 0;          // hashes we can compute per frame
  uint g_hdHashBudgetMax = 64;      // default

  // Mode 7 (BG1) full texture dumping support
  struct M7DumpEntry { uint width; uint height; vector<uint32_t> px; };
  ::nall::map<string, M7DumpEntry> g_m7DumpPending;
  ::nall::set<string> g_m7DumpSeen;
  struct M7BuildState {
    bool active = false;
    uint width = 1024, height = 1024;
    uint nextY = 0;
    string filename;
    vector<uint32_t> px;
  } g_m7Build;

  // forward declarations
  static auto manifestLoad() -> void;

  auto hdInit() -> void {
    if(g_hdInitialized) return;
    //resolve base path regardless of toggle; toggle is checked by callers
    g_hdBasePath = platform->path(SuperFamicom::ID::HDPack);
    g_hdInitialized = true;
    // Attempt to load manifest once
    manifestLoad();
  }

  // Precompute samples from a tilesheet cell into arrays
  static auto sheetPrecomputeSamples(::nall::image& img, uint cols, uint rows, uint col, uint row, uint16 out15[64], uint8 outA[64]) -> bool {
    if(!img) return false;
    if(cols == 0 || rows == 0) return false;
    uint cellW = img.width()  / cols;
    uint cellH = img.height() / rows;
    if(cellW == 0 || cellH == 0) return false;
    uint cellX = col * cellW;
    uint cellY = row * cellH;
    uint stepX = cellW / 8; if(stepX == 0) stepX = 1; if(stepX > 10) stepX = 10;
    uint stepY = cellH / 8; if(stepY == 0) stepY = 1; if(stepY > 10) stepY = 10;
    auto chA = img.alpha();
    auto chR = img.red();
    auto chG = img.green();
    auto chB = img.blue();
    for(uint y = 0; y < 8; y++) {
      for(uint x = 0; x < 8; x++) {
        uint sx = cellX + x * stepX + (stepX >> 1);
        uint sy = cellY + y * stepY + (stepY >> 1);
        if(sx >= cellX + cellW) sx = cellX + cellW - 1;
        if(sy >= cellY + cellH) sy = cellY + cellH - 1;
        auto ptr = img.data() + sy * img.pitch() + sx * img.stride();
        uint64_t px = img.read(ptr);
        auto A = ::nall::image::normalize((px & chA.mask()) >> chA.shift(), chA.depth(), 8);
        auto R = ::nall::image::normalize((px & chR.mask()) >> chR.shift(), chR.depth(), 8);
        auto G = ::nall::image::normalize((px & chG.mask()) >> chG.shift(), chG.depth(), 8);
        auto B = ::nall::image::normalize((px & chB.mask()) >> chB.shift(), chB.depth(), 8);
        outA[y * 8 + x] = (uint8)A;
        uint15 c = (uint15)(((R >> 3) << 0) | ((G >> 3) << 5) | ((B >> 3) << 10));
        out15[y * 8 + x] = (uint16)c;
      }
    }
    return true;
  }

  // Lightweight manifest parser: supports lines of the form:
  //   # filename.png cols=16 rows=16
  //   89ABCDEF col=0 row=1
  static auto manifestLoad() -> void {
    if(g_manifestLoaded) return;
    g_manifestLoaded = true;  // only attempt once per power cycle
    g_manifestAvailable = false;
    string manifest = {g_hdBasePath, "manifest.txt"};
    if(!::nall::file::exists(manifest)) return;
    auto buffer = ::nall::file::read(manifest);
    if(buffer.size() == 0) return;

    auto isSpace = [](char c) -> bool { return c==' '||c=='\t'; };
    auto isHex = [](char c) -> bool { return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
    auto hexVal = [](char c) -> uint32 { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return 10+(c-'a'); if(c>='A'&&c<='F') return 10+(c-'A'); return 0; };

    ::nall::image sheet;
    uint sheetCols = 16, sheetRows = 16;
    string sheetName;
    bool sheetOk = false;

    // iterate lines
    uint pos = 0; uint size = buffer.size();
    while(pos < size) {
      // read one line
      string line;
      while(pos < size) {
        char c = (char)buffer[pos++];
        if(c == '\n') break;
        if(c == '\r') continue;
        line.append(c);
      }
      // trim leading spaces
      uint L = 0; while(L < line.size() && isSpace(line[L])) L++;
      if(L >= line.size()) continue;  // empty
      if(line[L] == '#') {
        // parse header
        L++;
        while(L < line.size() && isSpace(line[L])) L++;
        // filename
        string fname;
        while(L < line.size() && !isSpace(line[L])) fname.append(line[L++]);
        uint cols = sheetCols, rows = sheetRows;
        // parse optional tokens: cols=, rows=
        while(L < line.size()) {
          while(L < line.size() && isSpace(line[L])) L++;
          if(L >= line.size()) break;
          if(line.slice(L).beginsWith("cols=")) {
            L += 5; uint v = 0; while(L < line.size() && line[L] >= '0' && line[L] <= '9') { v = v*10 + (line[L]-'0'); L++; }
            if(v) cols = v;
          } else if(line.slice(L).beginsWith("rows=")) {
            L += 5; uint v = 0; while(L < line.size() && line[L] >= '0' && line[L] <= '9') { v = v*10 + (line[L]-'0'); L++; }
            if(v) rows = v;
          } else {
            // skip unknown token
            while(L < line.size() && !isSpace(line[L])) L++;
          }
        }
        // load sheet
        string path = {g_hdBasePath, fname};
        sheetOk = sheet.load(path);
        sheetCols = cols; sheetRows = rows; sheetName = fname;
        continue;
      }
      // parse mapping: <8hex> col=X row=Y
      uint i = L;
      uint32 hash = 0;
      uint nhex = 0;
      while(i < line.size() && isHex(line[i]) && nhex < 8) { hash = (hash << 4) | hexVal(line[i]); i++; nhex++; }
      if(nhex == 0) continue;  // invalid
      uint col = 0, row = 0; bool haveCol = false, haveRow = false;
      while(i < line.size()) {
        while(i < line.size() && isSpace(line[i])) i++;
        if(i >= line.size()) break;
        if(line.slice(i).beginsWith("col=")) {
          i += 4; uint v = 0; while(i < line.size() && line[i] >= '0' && line[i] <= '9') { v = v*10 + (line[i]-'0'); i++; }
          col = v; haveCol = true;
        } else if(line.slice(i).beginsWith("row=")) {
          i += 4; uint v = 0; while(i < line.size() && line[i] >= '0' && line[i] <= '9') { v = v*10 + (line[i]-'0'); i++; }
          row = v; haveRow = true;
        } else {
          while(i < line.size() && !isSpace(line[i])) i++;
        }
      }
      if(!sheetOk || !haveCol || !haveRow) continue;
      ManifestEntry me = {};
      if(sheetPrecomputeSamples(sheet, sheetCols, sheetRows, col, row, me.sample15, me.sampleA)) {
        g_manifestMap.insert(hash, me);
        g_manifestAvailable = true;
      }
    }
  }

  static inline auto hdMakeKey(uint bgId, uint bppIndex, uint character, uint palette, uint paletteGroup, uint hmirror, uint vmirror) -> uint64_t {
    uint64_t key = 0;
    key |= (uint64)bgId & 0x3ull;
    key |= ((uint64)(character & 0x3ff)) << 2;
    key |= ((uint64)(palette & 0xffff)) << 12;
    key |= ((uint64)(bppIndex & 0x3)) << 28;
    key |= ((uint64)(hmirror & 0x1)) << 30;
    key |= ((uint64)(vmirror & 0x1)) << 31;
    key |= ((uint64)(paletteGroup & 0x7)) << 32;
    return key;
  }

  // (moved to member function below)

  static auto hdPrecomputeSamples(HDEntry& entry) -> void {
    entry.sampleReady = false;
    if(!entry.img) return;
    if(entry.img.width() < 8 || entry.img.height() < 8) return;
    uint scaleX = entry.img.width()  / 8;  if(scaleX == 0) scaleX = 1;  if(scaleX > 10) scaleX = 10;
    uint scaleY = entry.img.height() / 8;  if(scaleY == 0) scaleY = 1;  if(scaleY > 10) scaleY = 10;
    auto chA = entry.img.alpha();
    auto chR = entry.img.red();
    auto chG = entry.img.green();
    auto chB = entry.img.blue();
    for(uint y = 0; y < 8; y++) {
      for(uint x = 0; x < 8; x++) {
        uint sampleX = x * scaleX + (scaleX >> 1);
        uint sampleY = y * scaleY + (scaleY >> 1);
        if(sampleX >= entry.img.width())  sampleX = entry.img.width()  - 1;
        if(sampleY >= entry.img.height()) sampleY = entry.img.height() - 1;
        auto ptr = entry.img.data() + sampleY * entry.img.pitch() + sampleX * entry.img.stride();
        uint64_t px = entry.img.read(ptr);
        auto A = ::nall::image::normalize((px & chA.mask()) >> chA.shift(), chA.depth(), 8);
        auto R = ::nall::image::normalize((px & chR.mask()) >> chR.shift(), chR.depth(), 8);
        auto G = ::nall::image::normalize((px & chG.mask()) >> chG.shift(), chG.depth(), 8);
        auto B = ::nall::image::normalize((px & chB.mask()) >> chB.shift(), chB.depth(), 8);
        entry.sampleA[y * 8 + x] = (uint8)A;
        uint15 c = (uint15)(((R >> 3) << 0) | ((G >> 3) << 5) | ((B >> 3) << 10));
        entry.sample15[y * 8 + x] = (uint16)c;
      }
    }
    entry.sampleReady = true;
  }
}

// Implementations of PPU::Background HD helpers
auto PPU::Background::hdMakeStem(const Tile& tile) const -> string {
  uint bpp = 2u << io.mode;  //2,4,8
  return {
    g_hdBasePath,
    "BG", 1 + id,
    "_C", pad(tile.character, 4, '0'),
    "_PB", pad(tile.palette, 3, '0'),
    "_G", tile.paletteGroup,
    "_B", bpp,
    "_H", (uint)tile.hmirror,
    "_V", (uint)tile.vmirror
  };
}

auto PPU::Background::hdHasOrLoad(const Tile& tile) const -> bool {
  hdInit();
  if(!g_hdBasePath) return false;
  // Build or retrieve the stem string via numeric key to avoid repeated construction
  uint bppIndex = (io.mode == Mode::BPP2) ? 0u : (io.mode == Mode::BPP4) ? 1u : 2u;
  uint64_t key = hdMakeKey(id, bppIndex, tile.character, tile.palette, tile.paletteGroup, tile.hmirror, tile.vmirror);
  string stem;
  if(auto s = g_hdStemByKey.find(key)) stem = s();
  else {
    stem = hdMakeStem(tile);
    g_hdStemByKey.insert(key, stem);
  }
  if(auto e = g_hdCache.find(stem)) {
    // Update entry-by-key mapping
    g_hdEntryByKey.insert(key, &e());
    // If we haven't checked presence yet and have budget, check now
    if(!e().checkedPresence && g_hdPresenceBudget) {
      g_hdPresenceBudget--;
      auto png = string{stem, ".png"};
      auto bmp = string{stem, ".bmp"};
      e().present = ::nall::file::exists(png) || ::nall::file::exists(bmp);
      e().checkedPresence = true;
    }
    return e().present;
  }
  HDEntry entry;
  entry.loaded = false;
  entry.present = false;
  entry.checkedPresence = false;
  // Respect per-frame presence budget to avoid I/O stalls
  if(g_hdPresenceBudget) {
    g_hdPresenceBudget--;
    auto png = string{stem, ".png"};
    if(::nall::file::exists(png)) entry.present = true; else {
      auto bmp = string{stem, ".bmp"};
      if(::nall::file::exists(bmp)) entry.present = true;
    }
    entry.checkedPresence = true;
  } else {
    entry.present = false;  // defer until a later frame
  }
  // do not load here; lazy-load in hdSample on first actual use
  g_hdCache.insert(stem, entry);
  // establish pointer mapping now that value is stored in map
  if(auto e = g_hdCache.find(stem)) g_hdEntryByKey.insert(key, &e());
  return entry.present;
}

auto PPU::Background::hdSample(const Tile& tile, uint x, uint15& outColor) const -> bool {
  if(!configuration.hacks.ppu.useHDPack) return false;
  if(!tile.hd) return false;
  if(!g_hdBasePath) return false;
  // Micro-cache for the current tile row to avoid repeated map lookups and string builds
  struct Cache {
    const void* tilePtr = nullptr;
    uint8 row = 255;
    uint1 hmirror = 0;
    bool valid = false;
    const HDEntry* entry = nullptr;
    uint16 colors[8];
    uint8 presentMask = 0;  // bit i = 1 if pixel i is present (non-zero alpha)
  };
  static Cache cache;

  if(cache.tilePtr != &tile || cache.row != tile.hdRow || cache.hmirror != tile.hmirror || !cache.valid) {
    cache.tilePtr = &tile;
    cache.row = tile.hdRow;
    cache.hmirror = tile.hmirror;
    cache.valid = false;
    cache.entry = nullptr;
    cache.presentMask = 0;

    // Lookup HD entry via direct key mapping
    if(auto p = g_hdEntryByKey.find(tile.hdKey)) {
      auto* pent = p();
      auto& ent = *pent;
      if(!ent.loaded) {
        // Lazy-load PNG/BMP now
        string stem;
        if(auto s = g_hdStemByKey.find(tile.hdKey)) stem = s();
        else stem = hdMakeStem(tile);
        if(ent.present && g_hdLoadBudget) {
          g_hdLoadBudget--;
          auto png = string{stem, ".png"};
          bool ok = ent.img.load(png);
          if(!ok) {
            auto bmp = string{stem, ".bmp"};
            ok = ent.img.load(bmp);
          }
          ent.loaded = ok;
          if(ok) hdPrecomputeSamples(ent);
        }
      }
      if(ent.loaded) {
        if(!ent.sampleReady) {
          if(g_hdLoadBudget) { g_hdLoadBudget--; hdPrecomputeSamples(ent); }
          else { /* defer sample generation to later frame */ }
        }
        if(ent.sampleReady) {
          cache.entry = &ent;
          // Precompute all 8 samples for this row once
          uint y = tile.hdRow;
          if(y < 8) {
            for(uint i = 0; i < 8; i++) {
              uint xWithin = tile.hmirror ? (7u - i) : i;
              uint idx = y * 8 + xWithin;
              if(ent.sampleA[idx] == 0) {
                // not present
              } else {
                cache.colors[i] = ent.sample15[idx];
                cache.presentMask |= (1u << i);
              }
            }
            cache.valid = true;
          }
        }
      }
    }
  }

  if(!cache.valid) return false;
  if(x >= 8) return false;
  if((cache.presentMask & (1u << x)) == 0) return false;
  outColor = (uint15)cache.colors[x];
  return true;
}

auto PPU::Background::hires() const -> bool {
  return ppu.io.bgMode == 5 || ppu.io.bgMode == 6;
}

//V = 0, H = 0
auto PPU::Background::frame() -> void {
  // Reset per-frame budget for dumping new tiles
  g_hdDumpBudget = g_hdDumpBudgetMax;
  // Reset per-frame HD pack budgets
  g_hdPresenceBudget = g_hdPresenceBudgetMax;
  g_hdLoadBudget = g_hdLoadBudgetMax;
  g_hdSampleRowBudget = g_hdSampleRowBudgetMax;
  g_hdHashBudget = g_hdHashBudgetMax;

  // Build Mode 7 base texture incrementally (dump as a single 1024x1024 image)
  if(configuration.hacks.ppu.hdTileDump && ppu.io.bgMode == 7 && id == ID::BG1) {
    auto dir = platform->path(SuperFamicom::ID::HDTileDump);
    if(dir) {
      if(!g_m7Build.active) {
        string fn = {dir, "MODE7_BG1.png"};
        if(g_m7DumpSeen.find(fn) || g_m7DumpPending.find(fn)) {
          // already handled this session
        } else {
          g_m7Build.active = true;
          g_m7Build.width = 1024;
          g_m7Build.height = 1024;
          g_m7Build.nextY = 0;
          g_m7Build.filename = fn;
          g_m7Build.px.resize(g_m7Build.width * g_m7Build.height);
        }
      }

      if(g_m7Build.active) {
        // Process a limited number of rows per frame to avoid stutter
        uint rows = 64;
        if(g_m7Build.nextY + rows > g_m7Build.height) rows = g_m7Build.height - g_m7Build.nextY;
        for(uint r = 0; r < rows; r++) {
          uint Y = g_m7Build.nextY + r;
          for(uint X = 0; X < g_m7Build.width; X++) {
            uint tileX = X >> 3;
            uint tileY = Y >> 3;
            uint16 tileAddress = (tileY << 7) | tileX;  //128x128 tiles
            uint8 tile = (uint8)(ppu.vram[tileAddress] & 0xff);
            uint16 paletteAddress = ((Y & 7) << 3) | (X & 7);
            uint16 word = ppu.vram[(tile << 6) | paletteAddress];
            uint8 pal = (uint8)(word >> 8);
            if(pal == 0) {
              g_m7Build.px[Y * g_m7Build.width + X] = 0x00000000u;
            } else {
              uint15 c15;
              if(ppu.screen.io.directColor && ppu.io.bgMode == 7) c15 = ppu.screen.directColor(pal, 0);
              else c15 = ppu.screen.paletteColor(pal);
              uint8 r5 = (c15 >> 0) & 31;
              uint8 g5 = (c15 >> 5) & 31;
              uint8 b5 = (c15 >> 10) & 31;
              uint8 r8 = (r5 << 3) | (r5 >> 2);
              uint8 g8 = (g5 << 3) | (g5 >> 2);
              uint8 b8 = (b5 << 3) | (b5 >> 2);
              g_m7Build.px[Y * g_m7Build.width + X] = (uint32)0xff << 24 | (uint32)r8 << 16 | (uint32)g8 << 8 | (uint32)b8;
            }
          }
        }
        g_m7Build.nextY += rows;
        if(g_m7Build.nextY >= g_m7Build.height) {
          M7DumpEntry entry;
          entry.width = g_m7Build.width;
          entry.height = g_m7Build.height;
          entry.px = g_m7Build.px;
          g_m7DumpPending.insert(g_m7Build.filename, entry);
          g_m7DumpSeen.insert(g_m7Build.filename);
          g_m7Build.active = false;
          g_m7Build.px.reset();
        }
      }
    }
  }
}

//H = 0
auto PPU::Background::scanline() -> void {
  mosaic.hcounter = ppu.mosaic.size;
  mosaic.hoffset = 0;

  renderingIndex = 0;

  opt.hoffset = 0;
  opt.voffset = 0;

  pixelCounter = io.hoffset & 7;
}

//H = 56
auto PPU::Background::begin() -> void {
  //remove partial tile columns that have been scrolled offscreen
  for(auto& data : tiles[0].data) data >>= pixelCounter << 1;
}

auto PPU::Background::fetchNameTable() -> void {
  if(ppu.vcounter() == 0) return;

  uint nameTableIndex = ppu.hcounter() >> 5 << hires();
  int x = (ppu.hcounter() & ~31) >> 2;

  uint hpixel = x << hires();
  uint vpixel = ppu.vcounter();
  uint hscroll = io.hoffset;
  uint vscroll = io.voffset;

  if(mosaic.enable) vpixel -= ppu.mosaic.voffset();
  if(hires()) {
    hscroll <<= 1;
    if(ppu.io.interlace) {
      vpixel = vpixel << 1 | ppu.field();
      if(mosaic.enable) vpixel -= ppu.mosaic.voffset() + ppu.field();
    }
  }

  bool repeated = false;
  repeat:

  uint hoffset = hpixel + hscroll;
  uint voffset = vpixel + vscroll;

  if(ppu.io.bgMode == 2 || ppu.io.bgMode == 4 || ppu.io.bgMode == 6) {
    auto hlookup = ppu.bg3.opt.hoffset;
    auto vlookup = ppu.bg3.opt.voffset;
    uint valid = 1 << 13 + id;

    if(ppu.io.bgMode == 4) {
      if(hlookup & valid) {
        if(!(hlookup & 0x8000)) {
          hoffset = hpixel + (hlookup & ~7) + (hscroll & 7);
        } else {
          voffset = vpixel + (vlookup);
        }
      }
    } else {
      if(hlookup & valid) hoffset = hpixel + (hlookup & ~7) + (hscroll & 7);
      if(vlookup & valid) voffset = vpixel + (vlookup);
    }
  }

  uint width = 256 << hires();
  uint hsize = width << io.tileSize << io.screenSize.bit(0);
  uint vsize = width << io.tileSize << io.screenSize.bit(1);

  hoffset &= hsize - 1;
  voffset &= vsize - 1;

  uint vtiles = 3 + io.tileSize;
  uint htiles = !hires() ? vtiles : 4;

  uint htile = hoffset >> htiles;
  uint vtile = voffset >> vtiles;

  uint hscreen = io.screenSize.bit(0) ? 32 << 5 : 0;
  uint vscreen = io.screenSize.bit(1) ? 32 << 5 + io.screenSize.bit(0) : 0;

  uint16 offset = (uint5)htile << 0 | (uint5)vtile << 5;
  if(htile & 0x20) offset += hscreen;
  if(vtile & 0x20) offset += vscreen;

  uint16 address = io.screenAddress + offset;
  uint16 attributes = ppu.vram[address];

  auto& tile = tiles[nameTableIndex];
  tile.character = attributes & 0x03ff;
  tile.paletteGroup = attributes >> 10 & 7;
  tile.priority = io.priority[attributes >> 13 & 1];
  tile.hmirror = bool(attributes & 0x4000);
  tile.vmirror = bool(attributes & 0x8000);

  if(htiles == 4 && bool(hoffset & 8) != tile.hmirror) tile.character +=  1;
  if(vtiles == 4 && bool(voffset & 8) != tile.vmirror) tile.character += 16;

  uint characterMask = ppu.vram.mask >> 3 + io.mode;
  uint characterIndex = io.tiledataAddress >> 3 + io.mode;
  uint16 origin = tile.character + characterIndex & characterMask;

  //row within the 8x8 tile after vertical flip is applied
  uint hdRow = voffset & 7;
  if(tile.vmirror) hdRow ^= 7, voffset ^= 7;
  tile.address = (origin << 3 + io.mode) + (voffset & 7);

  uint paletteOffset = ppu.io.bgMode == 0 ? id << 5 : 0;
  uint paletteSize = 2 << io.mode;
  tile.palette = paletteOffset + (tile.paletteGroup << paletteSize);
  //mark HD availability and row for this tile
  tile.hdRow = hdRow;
  {
    uint bppIndex = (io.mode == Mode::BPP2) ? 0u : (io.mode == Mode::BPP4) ? 1u : 2u;
    tile.hdKey = hdMakeKey(id, bppIndex, tile.character, tile.palette, tile.paletteGroup, tile.hmirror, tile.vmirror);
    if(configuration.hacks.ppu.useHDPack) {
      hdInit();
      tile.hd = (g_manifestAvailable || hdHasOrLoad(tile));
    } else {
      tile.hd = 0;
    }
  }

  nameTableIndex++;
  if(hires() && !repeated) {
    repeated = true;
    hpixel += 8;
    goto repeat;
  }

  //dump the full 8x8 tile once attributes are available
  if(configuration.hacks.ppu.hdTileDump) dumpTile(tile);
}

auto PPU::Background::fetchOffset(uint y) -> void {
  if(ppu.vcounter() == 0) return;

  uint characterIndex = ppu.hcounter() >> 5 << hires();
  uint x = characterIndex << 3;

  uint hoffset = x + (io.hoffset & ~7);
  uint voffset = y + (io.voffset);

  uint vtiles = 3 + io.tileSize;
  uint htiles = !hires() ? vtiles : 4;

  uint htile = hoffset >> htiles;
  uint vtile = voffset >> vtiles;

  uint hscreen = io.screenSize.bit(0) ? 32 << 5 : 0;
  uint vscreen = io.screenSize.bit(1) ? 32 << 5 + io.screenSize.bit(0) : 0;

  uint16 offset = (uint5)htile << 0 | (uint5)vtile << 5;
  if(htile & 0x20) offset += hscreen;
  if(vtile & 0x20) offset += vscreen;

  uint16 address = io.screenAddress + offset;
  if(y == 0) opt.hoffset = ppu.vram[address];
  if(y == 8) opt.voffset = ppu.vram[address];
}

auto PPU::Background::fetchCharacter(uint index, bool half) -> void {
  if(ppu.vcounter() == 0) return;

  uint characterIndex = (ppu.hcounter() >> 5 << hires()) + half;

  auto& tile = tiles[characterIndex];
  uint16 data = ppu.vram[tile.address + (index << 3)];

  //reverse bits so that the lowest bit is the left-most pixel
  if(!tile.hmirror) {
    data = data >> 4 & 0x0f0f | data << 4 & 0xf0f0;
    data = data >> 2 & 0x3333 | data << 2 & 0xcccc;
    data = data >> 1 & 0x5555 | data << 1 & 0xaaaa;
  }

  //interleave two bitplanes for faster planar decoding later
  tile.data[index] = (
    ((uint8(data >> 0) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 49) & 0x5555
  | ((uint8(data >> 8) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 48) & 0xaaaa
  );
}

auto PPU::Background::run(bool screen) -> void {
  if(ppu.vcounter() == 0) return;

  if(screen == Screen::Below) {
    output.above.priority = 0;
    output.below.priority = 0;
    if(!hires()) return;
  }

  if(io.mode == Mode::Mode7) return runMode7();

  auto& tile = tiles[renderingIndex];
  uint8 color = 0;
  if(io.mode >= Mode::BPP2) color |= (tile.data[0] & 3) << 0; tile.data[0] >>= 2;
  if(io.mode >= Mode::BPP4) color |= (tile.data[1] & 3) << 2; tile.data[1] >>= 2;
  if(io.mode >= Mode::BPP8) color |= (tile.data[2] & 3) << 4; tile.data[2] >>= 2;
  if(io.mode >= Mode::BPP8) color |= (tile.data[3] & 3) << 6; tile.data[3] >>= 2;

  Pixel pixel;
  pixel.priority = tile.priority;
  pixel.palette = color ? uint(tile.palette + color) : 0;
  pixel.paletteGroup = tile.paletteGroup;
  pixel.hdPresent = 0;
  pixel.hdColor = 0;
  //use HD color if available (sample based on current x within tile row)
  //skip when transparent (color==0) to avoid unnecessary work
  //limit HD sampling to BG1 to significantly reduce per-pixel overhead
  if(configuration.hacks.ppu.useHDPack && tile.hd && color && id == ID::BG1) {
    // Fast path: if this row is already cached on the tile, read directly
    bool rowReady = (bool)tile.hdRowCached && (tile.hdRowCachedIndex == tile.hdRow) && (tile.hdRowCachedHmirror == tile.hmirror);
    if(!rowReady) {
      // Try to build the row cache, but respect budgets to avoid stutter
      if(g_hdSampleRowBudget) {
        g_hdSampleRowBudget--;
        // 1) Prefer manifest mapping by hash, if available
        bool filledFromManifest = false;
        if(g_manifestAvailable) {
          // compute or fetch cached tile hash
          uint32 thash = 0;
          bool hasHash = false;
          if(tile.hdHashCached && tile.hdHashKey == tile.hdKey) {
            thash = tile.hdHash;
            hasHash = true;
          } else if(g_hdHashBudget) {
            if(this->computeTileHash(tile, thash)) {
              g_hdHashBudget--;
              tile.hdHash = thash;
              tile.hdHashKey = tile.hdKey;
              tile.hdHashCached = 1;
              hasHash = true;
            }
          }
          if(hasHash) {
            if(auto me = g_manifestMap.find(thash)) {
              // Fill from manifest entry
              uint yrow = tile.hdRow;
              uint8 presentMask = 0;
              for(uint i = 0; i < 8; i++) {
                uint xWithin = tile.hmirror ? (7u - i) : i;
                uint idx = yrow * 8 + xWithin;
                if(me().sampleA[idx]) { tile.hdRowColors[i] = me().sample15[idx]; presentMask |= (1u << i); }
              }
              tile.hdRowPresentMask = presentMask;
              tile.hdRowCached = 1;
              tile.hdRowCachedIndex = yrow;
              tile.hdRowCachedHmirror = tile.hmirror;
              tile.hdRowCachedKey = tile.hdKey;
              rowReady = true;
              filledFromManifest = true;
            }
          }
        }
        // 2) Fallback: per-stem single-file entry path
        if(!filledFromManifest) if(auto p = g_hdEntryByKey.find(tile.hdKey)) {
          auto& ent = *p();
          // Ensure loaded and sampled (subject to budgets)
          if(!ent.loaded) {
            if(ent.present && g_hdLoadBudget) {
              g_hdLoadBudget--;
              string stem;
              if(auto s = g_hdStemByKey.find(tile.hdKey)) stem = s(); else stem = hdMakeStem(tile);
              auto png = string{stem, ".png"};
              bool ok = ent.img.load(png);
              if(!ok) { auto bmp = string{stem, ".bmp"}; ok = ent.img.load(bmp); }
              ent.loaded = ok;
            }
          }
          if(ent.loaded && !ent.sampleReady) {
            if(g_hdLoadBudget) { g_hdLoadBudget--; hdPrecomputeSamples(ent); }
          }
          if(ent.sampleReady) {
            // Fill the per-tile row cache
            uint yrow = tile.hdRow;
            uint8 presentMask = 0;
            for(uint i = 0; i < 8; i++) {
              uint xWithin = tile.hmirror ? (7u - i) : i;
              uint idx = yrow * 8 + xWithin;
              if(ent.sampleA[idx]) { tile.hdRowColors[i] = ent.sample15[idx]; presentMask |= (1u << i); }
            }
            tile.hdRowPresentMask = presentMask;
            tile.hdRowCached = 1;
            tile.hdRowCachedIndex = yrow;
            tile.hdRowCachedHmirror = tile.hmirror;
            tile.hdRowCachedKey = tile.hdKey;
            rowReady = true;
          }
        }
      }
    }
    if(rowReady) {
      uint bit = 1u << (pixelCounter & 7);
      if(tile.hdRowPresentMask & bit) {
        pixel.hdPresent = 1;
        pixel.hdColor = (uint15)tile.hdRowColors[pixelCounter & 7];
      }
    }
  }
  if(++pixelCounter == 0) renderingIndex++;

  uint x = ppu.hcounter() - 56 >> 2;
  if(x == 0) {
    mosaic.hcounter = ppu.mosaic.size;
    mosaic.pixel = pixel;
  } else if((!hires() || screen == Screen::Below) && --mosaic.hcounter == 0) {
    mosaic.hcounter = ppu.mosaic.size;
    mosaic.pixel = pixel;
  } else if(mosaic.enable) {
    pixel = mosaic.pixel;
  }
  if(screen == Screen::Above) x++;
  if(pixel.palette == 0) return;

  if(!hires() || screen == Screen::Above) if(io.aboveEnable) output.above = pixel;
  if(!hires() || screen == Screen::Below) if(io.belowEnable) output.below = pixel;
}

// Member: compute CRC32 hash of the current 8x8 SNES tile as AARRGGBB pixels
auto PPU::Background::computeTileHash(const Tile& tile, uint32& outHash) const -> bool {
  using namespace nall::Hash;
  CRC32 crc;
  uint characterMask = ppu.vram.mask >> (3 + io.mode);
  uint characterIndex = io.tiledataAddress >> (3 + io.mode);
  uint16 origin = (tile.character + characterIndex) & characterMask;
  uint groups = (io.mode == Mode::BPP2) ? 1u : (io.mode == Mode::BPP4) ? 2u : 4u;
  for(uint y = 0; y < 8; y++) {
    uint yrow = tile.vmirror ? (7 - y) : y;
    uint16 base = (origin << (3 + io.mode)) + yrow;
    uint16 d[4] = {};
    for(uint g = 0; g < groups; g++) {
      uint16 data = ppu.vram[base + (g << 3)];
      if(!tile.hmirror) {
        data = data >> 4 & 0x0f0f | data << 4 & 0xf0f0;
        data = data >> 2 & 0x3333 | data << 2 & 0xcccc;
        data = data >> 1 & 0x5555 | data << 1 & 0xaaaa;
      }
      d[g] = (
        ((uint8(data >> 0) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 49) & 0x5555
      ) | (
        ((uint8(data >> 8) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 48) & 0xaaaa
      );
    }
    for(uint x = 0; x < 8; x++) {
      uint8 color = 0;
      if(io.mode >= Mode::BPP2) { color |= (d[0] & 3) << 0; d[0] >>= 2; }
      if(io.mode >= Mode::BPP4) { color |= (d[1] & 3) << 2; d[1] >>= 2; }
      if(io.mode >= Mode::BPP8) { color |= (d[2] & 3) << 4; d[2] >>= 2; }
      if(io.mode >= Mode::BPP8) { color |= (d[3] & 3) << 6; d[3] >>= 2; }

      uint8 a8 = (color == 0) ? 0 : 255;
      uint16 paletteIndex = color ? (uint16)(tile.palette + color) : 0;
      uint15 c15;
      if(ppu.screen.io.directColor && (ppu.io.bgMode == 3 || ppu.io.bgMode == 4 || ppu.io.bgMode == 7) && id == ID::BG1) {
        c15 = ppu.screen.directColor((uint8)paletteIndex, tile.paletteGroup);
      } else {
        c15 = ppu.screen.paletteColor(paletteIndex);
      }
      uint8 r5 = (c15 >> 0) & 31;
      uint8 g5 = (c15 >> 5) & 31;
      uint8 b5 = (c15 >> 10) & 31;
      uint8 r8 = (r5 << 3) | (r5 >> 2);
      uint8 g8 = (g5 << 3) | (g5 >> 2);
      uint8 b8 = (b5 << 3) | (b5 >> 2);
      crc.input(a8);
      crc.input(r8);
      crc.input(g8);
      crc.input(b8);
    }
  }
  outHash = crc.value();
  return true;
}

auto PPU::Background::power() -> void {
  io = {};
  io.tiledataAddress = (random() & 0x0f) << 12;
  io.screenAddress = (random() & 0xfc) << 8;
  io.screenSize = random();
  io.tileSize = random();
  io.aboveEnable = random();
  io.belowEnable = random();
  io.hoffset = random();
  io.voffset = random();

  output.above = {};
  output.below = {};

  mosaic = {};
  mosaic.enable = random();

  //reset HD pack loader/cache between power cycles / game loads
  g_hdCache.reset();
  g_hdStemByKey.reset();
  g_hdEntryByKey.reset();
  g_manifestMap.reset();
  g_manifestLoaded = false;
  g_manifestAvailable = false;
  g_hdKeyToHash.reset();
  g_hdInitialized = false;
  g_hdBasePath = {};
  g_hdDumpSeen.reset();
  g_m7DumpSeen.reset();
  g_m7Build = {};
  g_hdDumpOrder.reset();
  g_hdDumpBudget = 0; // Introduce a per-frame budget
}

auto PPU::Background::dumpTile(const Tile& tile) -> void {
  // Compose a compact integer key for fast early filtering
  // Key layout (low->high bits):
  //  0..1   : BG id (0..3)
  //  2..11  : tile.character (10 bits)
  // 12..27  : tile.palette (16 bits)
  // 28..29  : bpp index (0:2bpp,1:4bpp,2:8bpp)
  //   30    : hmirror
  //   31    : vmirror
  uint bppIndex = (io.mode == Mode::BPP2) ? 0u : (io.mode == Mode::BPP4) ? 1u : 2u;
  uint64_t key = 0;
  key |= (uint64)id & 0x3ull;
  key |= ((uint64)tile.character & 0x3ffull) << 2;
  key |= ((uint64)tile.palette & 0xffffull) << 12;
  key |= ((uint64)bppIndex & 0x3ull) << 28;
  key |= ((uint64)(tile.hmirror ? 1 : 0)) << 30;
  key |= ((uint64)(tile.vmirror ? 1 : 0)) << 31;

  // If we've already handled this tile, skip immediately with no further work
  if(g_hdDumpSeenKeys.find(key)) return;
  // If we have no budget left for new tiles this frame, defer
  if(g_hdDumpBudget == 0) return;

  //resolve output directory
  auto dir = platform->path(SuperFamicom::ID::HDTileDump);
  if(!dir) return;

  //build a unique filename for the tile (avoid re-writing existing files)
  uint bpp = 2u << io.mode;  //2, 4, or 8
  string filename = {
    dir,
    "BG", 1 + id,
    "_C", pad(tile.character, 4, '0'),
    "_PB", pad(tile.palette, 3, '0'),
    "_G", tile.paletteGroup,
    "_B", bpp,
    "_H", (uint)tile.hmirror,
    "_V", (uint)tile.vmirror,
    ".png"
  };
  // Fast path: skip work if we've already enqueued/written this tile
  if(g_hdDumpSeen.find(filename)) { g_hdDumpSeenKeys.insert(key); return; }
  if(g_hdDumpPending.find(filename)) { g_hdDumpSeen.insert(filename); g_hdDumpSeenKeys.insert(key); return; }

  //reconstruct the full 8x8 tile from VRAM, independent of current scanline
  constexpr uint width = 8, height = 8;
  uint32_t pixels[width * height] = {};

  //compute tile origin address as in fetchNameTable()
  uint characterMask = ppu.vram.mask >> (3 + io.mode);
  uint characterIndex = io.tiledataAddress >> (3 + io.mode);
  uint16 origin = (tile.character + characterIndex) & characterMask;

  //number of interleaved bitplane groups to read per row
  uint groups = (io.mode == Mode::BPP2) ? 1u : (io.mode == Mode::BPP4) ? 2u : 4u;

  for(uint y = 0; y < height; y++) {
    uint yrow = tile.vmirror ? (height - 1 - y) : y;
    uint16 base = (origin << (3 + io.mode)) + yrow;

    //prepare per-row bitplane shift registers (same transform as fetchCharacter)
    uint16 d[4] = {};
    for(uint g = 0; g < groups; g++) {
      uint16 data = ppu.vram[base + (g << 3)];
      if(!tile.hmirror) {
        data = data >> 4 & 0x0f0f | data << 4 & 0xf0f0;
        data = data >> 2 & 0x3333 | data << 2 & 0xcccc;
        data = data >> 1 & 0x5555 | data << 1 & 0xaaaa;
      }
      d[g] = (
        ((uint8(data >> 0) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 49) & 0x5555
      ) | (
        ((uint8(data >> 8) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 48) & 0xaaaa
      );
    }

    for(uint x = 0; x < width; x++) {
      uint8 color = 0;
      if(io.mode >= Mode::BPP2) { color |= (d[0] & 3) << 0; d[0] >>= 2; }
      if(io.mode >= Mode::BPP4) { color |= (d[1] & 3) << 2; d[1] >>= 2; }
      if(io.mode >= Mode::BPP8) { color |= (d[2] & 3) << 4; d[2] >>= 2; }
      if(io.mode >= Mode::BPP8) { color |= (d[3] & 3) << 6; d[3] >>= 2; }

      uint16 paletteIndex = color ? (uint16)(tile.palette + color) : 0;
      uint15 c15;
      if(ppu.screen.io.directColor && (ppu.io.bgMode == 3 || ppu.io.bgMode == 4 || ppu.io.bgMode == 7) && id == ID::BG1) {
        // Match on-screen rendering path for direct color
        c15 = ppu.screen.directColor((uint8)paletteIndex, tile.paletteGroup);
      } else {
        c15 = ppu.screen.paletteColor(paletteIndex);
      }
      uint8 r5 = (c15 >> 0) & 31;
      uint8 g5 = (c15 >> 5) & 31;
      uint8 b5 = (c15 >> 10) & 31;
      uint8 r8 = (r5 << 3) | (r5 >> 2);
      uint8 g8 = (g5 << 3) | (g5 >> 2);
      uint8 b8 = (b5 << 3) | (b5 >> 2);
      uint8 a8 = (color == 0) ? 0 : 255;  //transparent when color index is 0

      //store as 0xAARRGGBB; PNG encoder will serialize rows as RGBA bytes
      pixels[y * width + x] = (uint32)a8 << 24 | (uint32)r8 << 16 | (uint32)g8 << 8 | (uint32)b8;
    }
  }

  // Enqueue into pending dump cache (deduplicate by filename)
  if(!g_hdDumpPending.find(filename)) {
    DumpEntry entry = {};
    for(uint i = 0; i < width * height; i++) entry.px[i] = pixels[i];
    g_hdDumpPending.insert(filename, entry);
    // Remember we've handled this tile to avoid repeated filesystem checks
    g_hdDumpSeen.insert(filename);
    g_hdDumpSeenKeys.insert(key);
    g_hdDumpOrder.append(filename);
    // consume budget for this new tile
    if(g_hdDumpBudget) g_hdDumpBudget--;
  }
}

// Flush all pending tile dumps to disk; called on toggle-off or unload
auto FlushHDTileDumpCache() -> void {
  // Flush background tiles as tilesheets only (16x16 tiles => 128x128 px per sheet)
  if(g_hdDumpPending.size()) {
    constexpr uint tileW = 8, tileH = 8;
    // Tilesheets grouped per BG plane (BG1..BG4)
    auto dir = platform->path(SuperFamicom::ID::HDTileDump);
    if(dir && g_hdDumpOrder.size()) {
      struct Bucket { vector<const DumpEntry*> tiles; };
      Bucket buckets[4];  // BG1..BG4
      for(auto& name : g_hdDumpOrder) {
        // locate BG# in filename
        uint bgIndex = 0;  // 0..3
        // filenames contain "BG<digit>_..."; find that pattern
        for(uint i = 0; i + 2 < name.size(); i++) {
          if(name[i + 0] == 'B' && name[i + 1] == 'G') {
            char d = name[i + 2];
            if(d >= '1' && d <= '4') { bgIndex = (uint)(d - '1'); break; }
          }
        }
        if(auto e = g_hdDumpPending.find(name)) {
          buckets[bgIndex].tiles.append(&e());
        }
      }

      const uint tilesPerRow = 16, tilesPerCol = 16;
      const uint sheetW = tilesPerRow * tileW;
      const uint sheetH = tilesPerCol * tileH;

      for(uint b = 0; b < 4; b++) {
        auto& list = buckets[b].tiles;
        if(list.size() == 0) continue;
        uint sheetIndex = 0;
        for(uint n = 0; n < list.size();) {
          vector<uint32_t> sheet; sheet.resize(sheetW * sheetH);
          // initialize transparent
          for(uint i = 0; i < sheet.size(); i++) sheet[i] = 0x00000000u;
          uint take = (uint)(list.size() - n);
          if(take > 256u) take = 256u;
          for(uint i = 0; i < take; i++) {
            const DumpEntry* de = list[n + i];
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
          string sheetName = {dir, "BG", b + 1, "_sheet_", pad(sheetIndex, 3, '0'), ".png"};
          ::nall::Encode::PNG::create(sheetName, &sheet[0], sheetW << 2, sheetW, sheetH, /*alpha=*/true);
          n += take;
          sheetIndex++;
        }
      }
    }

    g_hdDumpPending.reset();
    g_hdDumpOrder.reset();
  }

  // Flush pending sprites
  FlushSpriteDumpCache();

  // Flush Mode 7 full textures
  if(g_m7DumpPending.size()) {
    for(auto it = g_m7DumpPending.begin(); it != g_m7DumpPending.end(); ++it) {
      auto& node = *it;
      const string& fname = node.key;
      const M7DumpEntry& e = node.value;
      const uint stride = e.width << 2;
      ::nall::Encode::PNG::create(fname, &e.px[0], stride, e.width, e.height, /*alpha=*/true);
    }
    g_m7DumpPending.reset();
  }
}
