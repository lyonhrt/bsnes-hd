#pragma once

#include <nall/file.hpp>
#include <nall/string.hpp>
#include <nall/stdint.hpp>

namespace nall::Encode {

// Minimal PNG encoder for 32-bit RGBA8 images using uncompressed DEFLATE blocks.
// This is sufficient for small tile dumps and avoids adding external dependencies.
// API mirrors BMP::create for drop-in usage.
struct PNG {
  static auto create(const string& filename, const void* data, uint pitch, uint width, uint height, bool /*alpha*/) -> bool {
    if(width == 0 || height == 0) return false;

    // Open output file
    auto fp = file::open(filename, file::mode::write);
    if(!fp) return false;

    // Helpers
    auto writem32 = [&](uint32_t v) { fp.writem<uint32_t>(v, 4); };
    auto write8 = [&](uint8_t v) { fp.write(v); };

    // PNG signature
    static const uint8_t signature[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    for(uint n=0;n<8;n++) write8(signature[n]);

    // Build IHDR chunk (13 bytes)
    uint8_t ihdr[13] = {};
    ihdr[0] = (uint8_t)(width >> 24);
    ihdr[1] = (uint8_t)(width >> 16);
    ihdr[2] = (uint8_t)(width >> 8);
    ihdr[3] = (uint8_t)(width >> 0);
    ihdr[4] = (uint8_t)(height >> 24);
    ihdr[5] = (uint8_t)(height >> 16);
    ihdr[6] = (uint8_t)(height >> 8);
    ihdr[7] = (uint8_t)(height >> 0);
    ihdr[8]  = 8;  // bit depth
    ihdr[9]  = 6;  // color type RGBA
    ihdr[10] = 0;  // compression method
    ihdr[11] = 0;  // filter method
    ihdr[12] = 0;  // interlace method

    writeChunk(fp, "IHDR", ihdr, sizeof ihdr);

    // Prepare zlib-compressed IDAT from uncompressed DEFLATE blocks
    // Each scanline is prefixed with filter byte 0
    const uint rowBytes = 1 + width * 4;  // filter + RGBA
    const uint64_t total = (uint64_t)rowBytes * height;

    // We'll assemble the zlib stream in memory to compute the IDAT chunk length easily
    vector<uint8_t> zlib;
    zlib.reserve(2 + total + total / 65535 * 5 + 4);

    // zlib header: CMF=0x78 (deflate, 32K), FLG=0x01 (FCHECK set for CMF*256+FLG multiple of 31; FLEVEL=0)
    zlib.append(0x78);
    zlib.append(0x01);

    // Adler-32 over the uncompressed data stream
    auto adlerA = 1u;
    auto adlerB = 0u;
    auto adlerUpdate = [&](uint8_t byte){ adlerA = (adlerA + byte) % 65521u; adlerB = (adlerB + adlerA) % 65521u; };

    // Generator over image rows
    auto src = (const uint8_t*)data;
    uint64_t remaining = total;

    // Helper to emit one uncompressed block of length 'len' from the data stream
    auto emitStoreBlock = [&](uint32_t len, bool final){
      // Deflate block header (3 bits): BFINAL | BTYPE=00; here as full byte because we are byte-aligned
      zlib.append(final ? 0x01 : 0x00);
      // LEN (LE) and NLEN (LE)
      uint16_t nlen = (uint16_t)len;
      uint16_t nnlen = (uint16_t)~nlen;
      zlib.append((uint8_t)(nlen & 0xff));
      zlib.append((uint8_t)(nlen >> 8));
      zlib.append((uint8_t)(nnlen & 0xff));
      zlib.append((uint8_t)(nnlen >> 8));

      // Now append 'len' bytes of the uncompressed data stream
      // We generate scanlines on the fly
      static uint currentY = 0; // reset below before first use; static only to silence some compilers in headers
      static uint posInRow = 0;
      // But since this function can be called multiple times, keep these as captured-by-reference from outer scope.
    };

    // We'll instead stream-generate into zlib with an outer loop to avoid large lambdas capturing state by value
    {
      uint y = 0;
      uint pos = 0; // position within current row [0..rowBytes)
      while(remaining > 0) {
        uint32_t chunk = (uint32_t)((remaining > 65535ull) ? 65535ull : remaining);
        bool final = (remaining == chunk);

        // Write store block header
        zlib.append(final ? 0x01 : 0x00);
        uint16_t nlen = (uint16_t)chunk;
        uint16_t nnlen = (uint16_t)~nlen;
        zlib.append((uint8_t)(nlen & 0xff));
        zlib.append((uint8_t)(nlen >> 8));
        zlib.append((uint8_t)(nnlen & 0xff));
        zlib.append((uint8_t)(nnlen >> 8));

        // Produce exactly 'chunk' bytes
        uint32_t produced = 0;
        while(produced < chunk) {
          if(pos == 0) {
            // filter byte at start of row
            zlib.append(0x00);
            adlerUpdate(0x00);
            produced++;
            pos = 1;
            continue;
          }
          // emit pixel bytes in RGBA order
          uint x = (pos - 1) >> 2;        // which pixel (0..width-1)
          uint c = (pos - 1) & 3;         // component within pixel
          const uint8_t* rowPtr = src + y * pitch;
          uint32_t argb = ((const uint32_t*)rowPtr)[x];
          uint8_t a = (uint8_t)(argb >> 24);
          uint8_t r = (uint8_t)(argb >> 16);
          uint8_t g = (uint8_t)(argb >> 8);
          uint8_t b = (uint8_t)(argb >> 0);
          uint8_t out = 0;
          switch(c){
            case 0: out = r; break;
            case 1: out = g; break;
            case 2: out = b; break;
            case 3: out = a; break;
          }
          zlib.append(out);
          adlerUpdate(out);
          produced++;
          pos++;
          if(pos == rowBytes) { pos = 0; y++; }
        }

        remaining -= chunk;
      }
    }

    // Adler-32 (big-endian)
    uint32_t adler = (adlerB << 16) | adlerA;
    zlib.append((uint8_t)(adler >> 24));
    zlib.append((uint8_t)(adler >> 16));
    zlib.append((uint8_t)(adler >> 8));
    zlib.append((uint8_t)(adler >> 0));

    // IDAT chunk with zlib stream
    writeChunk(fp, "IDAT", zlib.data(), (uint32_t)zlib.size());

    // IEND chunk
    writeChunk(fp, "IEND", nullptr, 0);

    return true;
  }

private:
  // Write a PNG chunk: length (BE), type (4 ASCII), data bytes, CRC (BE)
  static auto writeChunk(file_buffer& fp, const char type[4], const uint8_t* data, uint32_t length) -> void {
    // length
    fp.writem<uint32_t>(length, 4);
    // type
    for(uint n=0;n<4;n++) fp.write((uint8_t)type[n]);
    // data
    for(uint32_t n=0;n<length;n++) fp.write(data[n]);
    // CRC32 over type+data
    uint32_t crc = crc32(type, data, length);
    fp.writem<uint32_t>(crc, 4);
  }

  static auto crc32(const char type[4], const uint8_t* data, uint32_t length) -> uint32_t {
    // Standard CRC32 (IEEE 802.3) with polynomial 0xEDB88320
    static uint32_t table[256];
    static bool init = false;
    if(!init) {
      init = true;
      for(uint32_t i=0;i<256;i++) {
        uint32_t c = i;
        for(uint n=0;n<8;n++) c = (c >> 1) ^ (0xEDB88320u & (-(int32_t)(c & 1)));
        table[i] = c;
      }
    }
    uint32_t crc = ~0u;
    for(uint n=0;n<4;n++) crc = (crc >> 8) ^ table[(crc ^ (uint8_t)type[n]) & 0xff];
    for(uint32_t i=0;i<length;i++) crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xff];
    return ~crc;
  }
};

}
