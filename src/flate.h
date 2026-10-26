// Small self-contained deflate/inflate and gzip (RFC 1951/1952), enough for X3 .pck files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace flate {

uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n);

// Raw deflate stream -> out. Returns false on corrupt data; *used gets the bytes consumed.
bool inflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t* used);
// Raw deflate stream of src (LZ77 with hash chains and lazy matching, dynamic/fixed/stored blocks).
void deflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

// gzip members back to back (as Python's gzip.decompress), each checked by CRC32 and ISIZE.
bool gunzip(const uint8_t* src, size_t n, std::vector<uint8_t>& out);
// gzip member with the header the game's .pck files have: no name, xfl 2, OS 0x0b (NTFS).
void gzip(const uint8_t* src, size_t n, uint32_t mtime, std::vector<uint8_t>& out);

}  // namespace flate
