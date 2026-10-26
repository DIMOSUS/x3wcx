// Deflate/inflate after RFC 1951; the decoder follows the canonical-code counting scheme of zlib's puff.c.
#include "flate.h"

#include <algorithm>
#include <cstring>
#include <queue>

namespace flate {

static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------- inflate

namespace {

struct BitIn {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    int bits(int need) {
        uint32_t v = buf;
        while (cnt < need) {
            if (pos >= n) {
                err = true;
                return 0;
            }
            v |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        buf = v >> need;
        cnt -= need;
        return (int)(v & ((1u << need) - 1));
    }
};

struct Huff {
    short count[16];
    short symbol[288];
};

// Returns 0 for a complete code, >0 incomplete, <0 over-subscribed.
int build(Huff& h, const short* length, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < n; s++) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int s = 0; s < n; s++)
        if (length[s]) h.symbol[offs[length[s]]++] = (short)s;
    return left;
}

int decode(BitIn& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= s.bits(1);
        if (s.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool codes(BitIn& s, std::vector<uint8_t>& out, const Huff& lencode, const Huff& distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + s.bits(kLenExtra[sym]);
            int ds = decode(s, distcode);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = kDistBase[ds] + s.bits(kDistExtra[ds]);
            if (s.err || dist > out.size()) return false;
            size_t from = out.size() - dist;
            for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

}  // namespace

bool inflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t* used) {
    BitIn s{src, n};
    int last;
    do {
        last = s.bits(1);
        int type = s.bits(2);
        if (s.err) return false;
        if (type == 0) {
            s.buf = 0;
            s.cnt = 0;
            if (s.pos + 4 > n) return false;
            unsigned len = src[s.pos] | src[s.pos + 1] << 8;
            unsigned nlen = src[s.pos + 2] | src[s.pos + 3] << 8;
            s.pos += 4;
            if (len != (~nlen & 0xFFFF) || s.pos + len > n) return false;
            out.insert(out.end(), src + s.pos, src + s.pos + len);
            s.pos += len;
        } else if (type == 1) {
            static Huff lencode, distcode;
            static bool ready = false;
            if (!ready) {
                short lengths[288];
                int sym = 0;
                for (; sym < 144; sym++) lengths[sym] = 8;
                for (; sym < 256; sym++) lengths[sym] = 9;
                for (; sym < 280; sym++) lengths[sym] = 7;
                for (; sym < 288; sym++) lengths[sym] = 8;
                build(lencode, lengths, 288);
                for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
                build(distcode, lengths, 30);
                ready = true;
            }
            if (!codes(s, out, lencode, distcode)) return false;
        } else if (type == 2) {
            int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return false;
            short lengths[320] = {};
            for (int i = 0; i < ncode; i++) lengths[kClOrder[i]] = (short)s.bits(3);
            Huff lencode, distcode;
            if (build(lencode, lengths, 19) != 0) return false;
            int index = 0;
            while (index < nlen + ndist) {
                int sym = decode(s, lencode);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[index++] = (short)sym;
                } else {
                    short len = 0;
                    int rep;
                    if (sym == 16) {
                        if (index == 0) return false;
                        len = lengths[index - 1];
                        rep = 3 + s.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + s.bits(3);
                    } else {
                        rep = 11 + s.bits(7);
                    }
                    if (s.err || index + rep > nlen + ndist) return false;
                    while (rep--) lengths[index++] = len;
                }
            }
            if (lengths[256] == 0) return false;
            if (build(lencode, lengths, nlen) < 0) return false;
            if (build(distcode, lengths + nlen, ndist) < 0) return false;
            if (!codes(s, out, lencode, distcode)) return false;
        } else {
            return false;
        }
    } while (!last);
    if (used) *used = s.pos;
    return true;
}

// ---------------------------------------------------------------- deflate

namespace {

struct BitOut {
    std::vector<uint8_t>& out;
    uint64_t buf = 0;
    int cnt = 0;
    void put(uint32_t v, int n) {
        buf |= (uint64_t)v << cnt;
        cnt += n;
        while (cnt >= 8) {
            out.push_back((uint8_t)buf);
            buf >>= 8;
            cnt -= 8;
        }
    }
    void align() {
        if (cnt) put(0, 8 - cnt);
    }
};

// Huffman code lengths limited to maxbits; when the plain tree is too deep the frequencies are halved and it is rebuilt.
void huff_lengths(const uint32_t* freq, int n, int maxbits, uint8_t* len) {
    std::vector<uint32_t> f(freq, freq + n);
    memset(len, 0, n);
    std::vector<int> syms;
    for (int i = 0; i < n; i++)
        if (f[i]) syms.push_back(i);
    if (syms.empty()) return;
    if (syms.size() == 1) {  // a one-symbol code is incomplete, which zlib rejects: give it a 1-bit partner
        len[syms[0]] = 1;
        len[syms[0] == 0 ? 1 : 0] = 1;
        return;
    }
    for (;;) {
        // nodes: leaves 0..k-1, then internal ones
        size_t k = syms.size();
        std::vector<uint64_t> w(2 * k);
        std::vector<int> parent(2 * k, -1);
        typedef std::pair<uint64_t, int> Item;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
        for (size_t i = 0; i < k; i++) {
            w[i] = f[syms[i]];
            q.push(Item(w[i], (int)i));
        }
        int next = (int)k;
        while (q.size() > 1) {
            Item a = q.top();
            q.pop();
            Item b = q.top();
            q.pop();
            w[next] = a.first + b.first;
            parent[a.second] = parent[b.second] = next;
            q.push(Item(w[next], next));
            next++;
        }
        std::vector<int> depth(next, 0);
        for (int i = next - 2; i >= 0; i--) depth[i] = depth[parent[i]] + 1;
        int maxd = 0;
        for (size_t i = 0; i < k; i++) maxd = std::max(maxd, depth[i]);
        if (maxd <= maxbits) {
            for (size_t i = 0; i < k; i++) len[syms[i]] = (uint8_t)depth[i];
            return;
        }
        for (int s : syms) f[s] = (f[s] + 1) / 2;
    }
}

uint32_t reverse_bits(uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

void canonical(const uint8_t* len, int n, uint16_t* code) {
    int bl_count[16] = {};
    for (int i = 0; i < n; i++) bl_count[len[i]]++;
    bl_count[0] = 0;
    int next_code[16] = {};
    int c = 0;
    for (int bits = 1; bits < 16; bits++) {
        c = (c + bl_count[bits - 1]) << 1;
        next_code[bits] = c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? (uint16_t)reverse_bits(next_code[len[i]]++, len[i]) : 0;
}

struct Sym {
    uint16_t lit;   // literal byte, or match length when dist != 0
    uint16_t dist;
};

int len_code(int len) {  // 3..258 -> 0..28
    int c = 0;
    while (c < 28 && kLenBase[c + 1] <= len) c++;
    return c;
}

int dist_code(int dist) {
    int c = 0;
    while (c < 29 && kDistBase[c + 1] <= dist) c++;
    return c;
}

void write_symbols(BitOut& bo, const std::vector<Sym>& syms, const uint8_t* ll, const uint16_t* llc, const uint8_t* dl,
                   const uint16_t* dc) {
    for (const Sym& s : syms) {
        if (!s.dist) {
            bo.put(llc[s.lit], ll[s.lit]);
        } else {
            int lc = len_code(s.lit);
            bo.put(llc[257 + lc], ll[257 + lc]);
            if (kLenExtra[lc]) bo.put(s.lit - kLenBase[lc], kLenExtra[lc]);
            int dcd = dist_code(s.dist);
            bo.put(dc[dcd], dl[dcd]);
            if (kDistExtra[dcd]) bo.put(s.dist - kDistBase[dcd], kDistExtra[dcd]);
        }
    }
    bo.put(llc[256], ll[256]);
}

void write_block(BitOut& bo, const std::vector<Sym>& syms, const uint8_t* raw, size_t rawlen, bool last) {
    uint32_t lf[286] = {}, df[30] = {};
    uint64_t extra = 0;
    for (const Sym& s : syms) {
        if (!s.dist) {
            lf[s.lit]++;
        } else {
            int lc = len_code(s.lit), dcd = dist_code(s.dist);
            lf[257 + lc]++;
            df[dcd]++;
            extra += kLenExtra[lc] + kDistExtra[dcd];
        }
    }
    lf[256] = 1;
    int nd = 0;
    for (int i = 0; i < 30; i++) nd += df[i] != 0;
    if (nd < 2) {  // some decoders want at least two distance codes
        if (!df[0]) df[0] = 1;
        if (!df[1]) df[1] = 1;
    }
    uint8_t ll[286], dl[30];
    huff_lengths(lf, 286, 15, ll);
    huff_lengths(df, 30, 15, dl);

    int hlit = 286;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    int hdist = 30;
    while (hdist > 1 && !dl[hdist - 1]) hdist--;
    // run-length encoding of the code lengths
    std::vector<uint8_t> all(ll, ll + hlit);
    all.insert(all.end(), dl, dl + hdist);
    std::vector<std::pair<uint8_t, uint8_t>> rle;  // (symbol, extra value)
    for (size_t i = 0; i < all.size();) {
        uint8_t v = all[i];
        size_t run = 1;
        while (i + run < all.size() && all[i + run] == v) run++;
        size_t r = run;
        if (v == 0) {
            while (r >= 11) {
                size_t t = std::min<size_t>(r, 138);
                rle.push_back({18, (uint8_t)(t - 11)});
                r -= t;
            }
            if (r >= 3) {
                rle.push_back({17, (uint8_t)(r - 3)});
                r = 0;
            }
            while (r--) rle.push_back({0, 0});
        } else {
            rle.push_back({v, 0});
            r--;
            while (r >= 3) {
                size_t t = std::min<size_t>(r, 6);
                rle.push_back({16, (uint8_t)(t - 3)});
                r -= t;
            }
            while (r--) rle.push_back({v, 0});
        }
        i += run;
    }
    uint32_t cf[19] = {};
    for (auto& e : rle) cf[e.first]++;
    uint8_t cl[19];
    huff_lengths(cf, 19, 7, cl);
    int hclen = 19;
    while (hclen > 4 && !cl[kClOrder[hclen - 1]]) hclen--;

    uint64_t dyn = 3 + 5 + 5 + 4 + 3 * hclen + extra;
    for (auto& e : rle) dyn += cl[e.first] + (e.first == 16 ? 2 : e.first == 17 ? 3 : e.first == 18 ? 7 : 0);
    for (int i = 0; i < 286; i++) dyn += (uint64_t)lf[i] * ll[i];
    for (int i = 0; i < 30; i++) dyn += (uint64_t)df[i] * dl[i];

    static uint8_t fl[288], fd[30];
    static uint16_t flc[288], fdc[30];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 288; i++) fl[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        for (int i = 0; i < 30; i++) fd[i] = 5;
        canonical(fl, 288, flc);
        canonical(fd, 30, fdc);
        ready = true;
    }
    uint64_t fix = 3 + extra;
    for (int i = 0; i < 286; i++) fix += (uint64_t)lf[i] * fl[i];
    for (int i = 0; i < 30; i++) fix += (uint64_t)df[i] * 5;
    uint64_t stored = 3 + 7 + 32 + 8ull * rawlen + 40ull * (rawlen / 65535);

    if (stored < dyn && stored < fix) {
        size_t off = 0;
        do {
            size_t chunk = std::min<size_t>(rawlen - off, 65535);
            bool fin = last && off + chunk == rawlen;
            bo.put(fin ? 1 : 0, 1);
            bo.put(0, 2);
            bo.align();
            bo.put((uint32_t)chunk, 16);
            bo.put((uint32_t)~chunk & 0xFFFF, 16);
            for (size_t i = 0; i < chunk; i++) bo.put(raw[off + i], 8);
            off += chunk;
        } while (off < rawlen);
    } else if (fix <= dyn) {
        bo.put(last ? 1 : 0, 1);
        bo.put(1, 2);
        write_symbols(bo, syms, fl, flc, fd, fdc);
    } else {
        bo.put(last ? 1 : 0, 1);
        bo.put(2, 2);
        bo.put(hlit - 257, 5);
        bo.put(hdist - 1, 5);
        bo.put(hclen - 4, 4);
        for (int i = 0; i < hclen; i++) bo.put(cl[kClOrder[i]], 3);
        uint16_t clc[19];
        canonical(cl, 19, clc);
        for (auto& e : rle) {
            bo.put(clc[e.first], cl[e.first]);
            if (e.first == 16) bo.put(e.second, 2);
            else if (e.first == 17) bo.put(e.second, 3);
            else if (e.first == 18) bo.put(e.second, 7);
        }
        uint16_t llc[286], dc[30];
        canonical(ll, 286, llc);
        canonical(dl, 30, dc);
        write_symbols(bo, syms, ll, llc, dl, dc);
    }
}

}  // namespace

void deflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    const int kWin = 32768, kMask = kWin - 1, kHashBits = 15, kMaxChain = 256, kNice = 258;
    const size_t kBlockSyms = 32768;
    BitOut bo{out};
    std::vector<int32_t> head(1 << kHashBits, -1), prev(kWin, -1);
    size_t ins = 0;
    auto hash = [&](size_t p) {
        return ((src[p] << 10) ^ (src[p + 1] << 5) ^ src[p + 2]) & ((1 << kHashBits) - 1);
    };
    auto insert_upto = [&](size_t p) {
        for (; ins < p; ins++) {
            if (ins + 2 >= n) continue;
            int h = hash(ins);
            prev[ins & kMask] = head[h];
            head[h] = (int32_t)ins;
        }
    };
    auto find = [&](size_t p, int& bestDist) {
        int best = 0;
        if (p + 3 > n) return 0;
        insert_upto(p);
        int maxlen = (int)std::min<size_t>(258, n - p);
        int32_t c = head[hash(p)];
        int chain = kMaxChain;
        while (c >= 0 && chain-- > 0) {
            size_t d = p - (size_t)c;
            if (d == 0 || d > (size_t)kWin) break;
            if (src[c + best] == src[p + best]) {
                int l = 0;
                while (l < maxlen && src[c + l] == src[p + l]) l++;
                if (l > best) {
                    best = l;
                    bestDist = (int)d;
                    if (l >= kNice || l >= maxlen) break;
                }
            }
            int32_t nc = prev[c & kMask];
            if (nc >= c) break;
            c = nc;
        }
        return best >= 3 ? best : 0;
    };

    std::vector<Sym> syms;
    size_t blockStart = 0, i = 0;
    while (i < n) {
        int dist = 0;
        int len = find(i, dist);
        if (len && len < 32 && i + 1 < n) {
            int d2 = 0;
            int l2 = find(i + 1, d2);
            if (l2 > len) len = 0;  // lazy: the next position matches longer, emit a literal now
        }
        if (len) {
            syms.push_back({(uint16_t)len, (uint16_t)dist});
            i += len;
        } else {
            syms.push_back({src[i], 0});
            i++;
        }
        if (syms.size() >= kBlockSyms) {
            write_block(bo, syms, src + blockStart, i - blockStart, i >= n);
            syms.clear();
            blockStart = i;
        }
    }
    if (!syms.empty() || n == 0 || blockStart < n) write_block(bo, syms, src + blockStart, i - blockStart, true);
    bo.align();
}

// ---------------------------------------------------------------- gzip

bool gunzip(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) return false;
    int flg = src[3];
    if (flg & 0xE0) return false;
    size_t p = 10;
    if (flg & 4) {
        if (p + 2 > n) return false;
        p += 2 + (src[p] | src[p + 1] << 8);
    }
    if (flg & 8) {
        while (p < n && src[p]) p++;
        p++;
    }
    if (flg & 16) {
        while (p < n && src[p]) p++;
        p++;
    }
    if (flg & 2) p += 2;
    if (p >= n) return false;
    size_t used = 0;
    if (!inflate_raw(src + p, n - p, out, &used)) return false;
    p += used;
    if (p + 8 > n) return false;
    uint32_t crc = src[p] | src[p + 1] << 8 | src[p + 2] << 16 | (uint32_t)src[p + 3] << 24;
    uint32_t isize = src[p + 4] | src[p + 5] << 8 | src[p + 6] << 16 | (uint32_t)src[p + 7] << 24;
    return crc == crc32(0, out.data(), out.size()) && isize == (uint32_t)out.size();
}

void gzip(const uint8_t* src, size_t n, uint32_t mtime, std::vector<uint8_t>& out) {
    out.clear();
    uint8_t hdr[10] = {0x1F, 0x8B, 8, 0, (uint8_t)mtime, (uint8_t)(mtime >> 8), (uint8_t)(mtime >> 16), (uint8_t)(mtime >> 24),
                       2, 0x0B};
    out.insert(out.end(), hdr, hdr + 10);
    deflate_raw(src, n, out);
    uint32_t crc = crc32(0, src, n), sz = (uint32_t)n;
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)(crc >> (8 * i)));
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)(sz >> (8 * i)));
}

}  // namespace flate
