struct Background {
  Background(uint id) : id(id) {}

  alwaysinline auto hires() const -> bool;

  //background.cpp
  auto frame() -> void;
  auto scanline() -> void;
  auto begin() -> void;
  auto fetchNameTable() -> void;
  auto fetchOffset(uint y) -> void;
  auto fetchCharacter(uint index, bool half = 0) -> void;
  auto run(bool screen) -> void;
  auto power() -> void;
  struct Tile;
  auto dumpTile(const Tile& tile) -> void;
  // HD pack helpers
  auto hdMakeStem(const Tile& tile) const -> string;
  auto hdHasOrLoad(const Tile& tile) const -> bool;
  auto hdSample(const Tile& tile, uint x, uint15& outColor) const -> bool;
  auto computeTileHash(const Tile& tile, uint32& outHash) const -> bool;  // manifest hashing

  //mode7.cpp
  alwaysinline auto clip(int n) -> int;
  auto runMode7() -> void;

  auto serialize(serializer&) -> void;

  struct ID { enum : uint { BG1, BG2, BG3, BG4 }; };
  const uint id;

  struct Mode { enum : uint { BPP2, BPP4, BPP8, Mode7, Inactive }; };
  struct ScreenSize { enum : uint { Size32x32, Size32x64, Size64x32, Size64x64 }; };
  struct TileSize { enum : uint { Size8x8, Size16x16 }; };
  struct Screen { enum : uint { Above, Below }; };

  struct IO {
    uint16 tiledataAddress;
    uint16 screenAddress;
     uint2 screenSize;
     uint1 tileSize;

     uint8 mode;
     uint8 priority[2];

     uint1 aboveEnable;
     uint1 belowEnable;

    uint16 hoffset;
    uint16 voffset;
  } io;

  struct Pixel {
    uint8 priority;  //0 = none (transparent)
    uint8 palette;
    uint3 paletteGroup;
    //HD pack override (if present, use hdColor instead of paletteColor)
     uint1 hdPresent;
    uint15 hdColor;
  } above, below;

  struct Output {
    Pixel above;
    Pixel below;
  } output;

  struct Mosaic {
     uint1 enable;
    uint16 hcounter;
    uint16 hoffset;
    Pixel  pixel;
  } mosaic;

  struct OffsetPerTile {
    //set in BG3 only; used by BG1 and BG2
    uint16 hoffset;
    uint16 voffset;
  } opt;

  struct Tile {
    uint16 address;
    uint10 character;
     uint8 palette;
     uint3 paletteGroup;
     uint8 priority;
     uint1 hmirror;
     uint1 vmirror;
    uint16 data[4];
     uint1 hd;      //has HD replacement available
     uint3 hdRow;   //0..7 row within tile for current scanline
    uint64 hdKey;   //numeric identity for HD cache lookups
    // Fast per-row cache for HD sampling
     uint1 hdRowCached;         // whether hdRowColors/presentMask are valid
      uint3 hdRowCachedIndex;   // which row (0..7) is cached
      uint1 hdRowCachedHmirror; // whether the cache assumed hmirror
     uint64 hdRowCachedKey;     // which tile identity this cache belongs to
      uint8 hdRowPresentMask;   // bit i set if non-transparent at x=i
     uint16 hdRowColors[8];
    // Cached hash for manifest lookups
     uint1 hdHashCached;
     uint64 hdHashKey;
     uint32 hdHash;
  } tiles[66];

  uint7 renderingIndex;
  uint3 pixelCounter;

  friend class PPU;
};
