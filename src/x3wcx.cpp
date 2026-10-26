// x3wcx: Total Commander packer plugin for X3 catalogs (.cat + .dat) and .pck files.
//
// .cat  every byte XOR (0xDB + i) & 0xFF; first line is a .dat name (unused by the game), then "path size" lines.
// .dat  members back to back in index order, every byte XOR 0x33; offsets are the running sum of sizes.
// .pck  gzip; or a key byte k ^ 0xC8 followed by the gzip stream XOR k. Shown as an archive with one file.
//       .pbd (a text model .bod) and .pbb (a binary model .bob) are the same format.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "flate.h"
#include "wcxhead.h"

namespace {

typedef std::wstring wstr;

const uint8_t kDatKey = 0x33;
const size_t kChunk = 1 << 20;
const wchar_t kTmpSuffix[] = L".x3wcx-tmp";
const wchar_t kBakSuffix[] = L".x3wcx-bak";

tProcessDataProc gProcA = nullptr;
tProcessDataProcW gProcW = nullptr;

// Settings live in the ini TC names in PackSetDefaultParams, section [x3wcx].
std::string gIni;
const char kIniSection[] = "x3wcx";
const char kPackOnAdd[] = "PackOnAdd";

// Files added to a catalog go in packed when the game looks them up packed first (default on).
bool pack_on_add() {
    return gIni.empty() || GetPrivateProfileIntA(kIniSection, kPackOnAdd, 1, gIni.c_str()) != 0;
}

// ---------------------------------------------------------------- strings

wstr acp_to_w(const char* s) {
    if (!s) return wstr();
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    wstr w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

std::string w_to_acp(const wstr& w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// Double-null terminated name list (AddList, DeleteList).
std::vector<wstr> split_list_w(const WCHAR* p) {
    std::vector<wstr> out;
    while (p && *p) {
        out.push_back(p);
        p += out.back().size() + 1;
    }
    return out;
}

std::vector<wstr> split_list_a(const char* p) {
    std::vector<wstr> out;
    while (p && *p) {
        out.push_back(acp_to_w(p));
        p += strlen(p) + 1;
    }
    return out;
}

wstr latin1_to_w(const std::string& s) {
    wstr w(s.size(), L'\0');
    for (size_t i = 0; i < s.size(); i++) w[i] = (unsigned char)s[i];
    return w;
}

bool w_to_latin1(const wstr& w, std::string& s) {
    s.resize(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] > 0xFF) return false;
        s[i] = (char)w[i];
    }
    return true;
}

// Catalog path as TC shows it (backslashes) and back.
wstr to_tc(const std::string& path) {
    wstr w = latin1_to_w(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

// The game compares paths case-insensitively (Python's lower() on latin1, as in catx.norm).
std::string norm(const std::string& p) {
    std::string s = p;
    for (char& ch : s) {
        unsigned char c = (unsigned char)ch;
        if (c == '\\') c = '/';
        else if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 0x20;
        ch = (char)c;
    }
    return s;
}

wstr lower_w(wstr s) {
    for (wchar_t& c : s) c = (wchar_t)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)c);
    return s;
}

bool ends_with_i(const wstr& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && lower_w(s.substr(s.size() - n)) == suffix;
}

wstr base_name(const wstr& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == wstr::npos ? p : p.substr(k + 1);
}

wstr dir_name(const wstr& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == wstr::npos ? wstr() : p.substr(0, k);
}

wstr strip_ext(const wstr& p) {
    size_t k = p.find_last_of(L'.');
    size_t s = p.find_last_of(L"\\/");
    return k == wstr::npos || (s != wstr::npos && k < s) ? p : p.substr(0, k);
}

wstr dat_of(const wstr& cat) {
    return strip_ext(cat) + L".dat";
}

void copy_trunc(WCHAR* dst, const wstr& src, size_t cap) {
    size_t n = std::min(src.size(), cap - 1);
    memcpy(dst, src.c_str(), n * sizeof(WCHAR));
    dst[n] = 0;
}

void copy_trunc(char* dst, const std::string& src, size_t cap) {
    size_t n = std::min(src.size(), cap - 1);
    memcpy(dst, src.c_str(), n);
    dst[n] = 0;
}

// ---------------------------------------------------------------- files

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    File() {}
    explicit File(HANDLE x) : h(x) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { close(); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    void close() {
        if (ok()) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
};

HANDLE open_read(const wstr& p) {
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

HANDLE create_write(const wstr& p) {
    return CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool exists(const wstr& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool file_size(HANDLE h, uint64_t& size) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return false;
    size = (uint64_t)li.QuadPart;
    return true;
}

bool seek(HANDLE h, uint64_t off) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
}

bool read_exact(HANDLE h, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, (DWORD)std::min<size_t>(n, 1u << 30), &got, nullptr) || !got) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool write_exact(HANDLE h, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        DWORD put = 0;
        if (!WriteFile(h, p, (DWORD)std::min<size_t>(n, 1u << 30), &put, nullptr) || !put) return false;
        p += put;
        n -= put;
    }
    return true;
}

int read_all(const wstr& p, std::vector<uint8_t>& out) {
    File f(open_read(p));
    if (!f.ok()) return E_EOPEN;
    uint64_t size;
    if (!file_size(f.h, size)) return E_EREAD;
    if (size > 0x7FFFFFFF) return E_NO_MEMORY;
    out.resize((size_t)size);
    return size && !read_exact(f.h, out.data(), out.size()) ? E_EREAD : 0;
}

void make_dirs(const wstr& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    make_dirs(dir_name(dir));
    CreateDirectoryW(dir.c_str(), nullptr);
}

// Writes p via p.x3wcx-tmp and a rename, so a failure leaves the old file whole.
int write_atomic(const wstr& p, const std::vector<uint8_t>& data) {
    wstr tmp = p + kTmpSuffix;
    {
        File f(create_write(tmp));
        if (!f.ok()) return E_ECREATE;
        if (!write_exact(f.h, data.data(), data.size()) || !FlushFileBuffers(f.h)) {
            f.close();
            DeleteFileW(tmp.c_str());
            return E_EWRITE;
        }
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return E_EWRITE;
    }
    return 0;
}

int dos_time(const FILETIME& ft) {
    FILETIME local;
    WORD d = 0, t = 0;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToDosDateTime(&local, &d, &t);
    return (int)((DWORD)d << 16 | t);
}

bool file_time(const wstr& p, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    ft = fa.ftLastWriteTime;
    return true;
}

// ---------------------------------------------------------------- progress

// false when the user pressed Cancel
bool progress(const wstr& name, uint64_t bytes) {
    int n = (int)std::min<uint64_t>(bytes, 0x7FFFFFFF);
    if (gProcW) return gProcW((WCHAR*)name.c_str(), n) != 0;
    if (gProcA) {
        std::string a = w_to_acp(name);
        return gProcA((char*)a.c_str(), n) != 0;
    }
    return true;
}

// Copies size bytes from in to out (out may be null: read only), XOR-ing with kDatKey when xr.
int copy_stream(HANDLE in, HANDLE out, uint64_t size, bool xr, const wstr& name) {
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>(size, kChunk));
    while (size) {
        size_t n = (size_t)std::min<uint64_t>(size, kChunk);
        if (!read_exact(in, buf.data(), n)) return E_EREAD;
        if (xr)
            for (size_t i = 0; i < n; i++) buf[i] ^= kDatKey;
        if (out && !write_exact(out, buf.data(), n)) return E_EWRITE;
        size -= n;
        if (!progress(name, n)) return E_EABORTED;
    }
    return 0;
}

// ---------------------------------------------------------------- catalog

struct Entry {
    std::string path;  // as stored: forward slashes, latin1
    uint64_t size;
    uint64_t off;
};

struct Catalog {
    std::string first;  // the .dat name line, kept as it was
    std::vector<Entry> ents;
    uint64_t total() const { return ents.empty() ? 0 : ents.back().off + ents.back().size; }
};

void cat_crypt(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] ^= (uint8_t)(0xDB + i);
}

bool parse_cat(std::vector<uint8_t> raw, Catalog& c) {
    cat_crypt(raw.data(), raw.size());
    std::string t(raw.begin(), raw.end());
    c.first.clear();
    c.ents.clear();
    bool haveFirst = false;
    uint64_t off = 0;
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t\r\v\f") == std::string::npos) continue;  // catx skips blank lines
        if (!haveFirst) {
            c.first = line;
            haveFirst = true;
            continue;
        }
        size_t sp = line.rfind(' ');
        if (sp == std::string::npos || sp + 1 >= line.size()) return false;
        uint64_t size = 0;
        for (size_t i = sp + 1; i < line.size(); i++) {
            if (line[i] < '0' || line[i] > '9') return false;
            size = size * 10 + (line[i] - '0');
        }
        c.ents.push_back({line.substr(0, sp), size, off});
        off += size;
    }
    return haveFirst;
}

std::vector<uint8_t> encode_cat(const Catalog& c) {
    std::string t = c.first + "\n";
    for (const Entry& e : c.ents) t += e.path + " " + std::to_string(e.size) + "\n";
    std::vector<uint8_t> raw(t.begin(), t.end());
    cat_crypt(raw.data(), raw.size());
    return raw;
}

int load_cat(const wstr& path, Catalog& c) {
    std::vector<uint8_t> raw;
    int r = read_all(path, raw);
    if (r) return r;
    return parse_cat(raw, c) ? 0 : E_BAD_ARCHIVE;
}

// First line, decrypted, looks like "<name>.dat".
bool looks_like_cat(const uint8_t* b, size_t n) {
    std::string line;
    for (size_t i = 0; i < n && i < 512; i++) {
        uint8_t c = b[i] ^ (uint8_t)(0xDB + i);
        if (c == '\n') break;
        if (c == '\r') continue;
        if (c < 0x20 || c == 0x7F) return false;
        line += (char)c;
    }
    if (line.size() < 5 || line.size() >= 512) return false;
    std::string ext = norm(line.substr(line.size() - 4));
    return ext == ".dat";
}

// ---------------------------------------------------------------- pck

struct Pck {
    bool masked = false;
    uint8_t key0 = 0;  // first byte of a masked file; the key is key0 ^ 0xC8
    std::vector<uint8_t> data;
};

bool decode_pck(const std::vector<uint8_t>& b, Pck& p) {
    if (flate::gunzip(b.data(), b.size(), p.data)) {
        p.masked = false;
        return true;
    }
    if (b.size() < 2) return false;
    uint8_t k = b[0] ^ 0xC8;
    std::vector<uint8_t> t(b.begin() + 1, b.end());
    for (uint8_t& x : t) x ^= k;
    if (!flate::gunzip(t.data(), t.size(), p.data)) return false;
    p.masked = true;
    p.key0 = b[0];
    return true;
}

std::vector<uint8_t> encode_pck(const std::vector<uint8_t>& data, bool masked, uint8_t key0) {
    std::vector<uint8_t> gz;
    flate::gzip(data.data(), data.size(), (uint32_t)time(nullptr), gz);
    if (!masked) return gz;
    uint8_t k = key0 ^ 0xC8;
    std::vector<uint8_t> out;
    out.reserve(gz.size() + 1);
    out.push_back(key0);
    for (uint8_t x : gz) out.push_back(x ^ k);
    return out;
}

bool looks_like_pck(const uint8_t* b, size_t n) {
    if (n >= 3 && b[0] == 0x1F && b[1] == 0x8B && b[2] == 8) return true;
    if (n < 4) return false;
    uint8_t k = b[0] ^ 0xC8;
    return (b[1] ^ k) == 0x1F && (b[2] ^ k) == 0x8B && (b[3] ^ k) == 8;
}

// The packed extension the game tries first for a file of this type, or null. The exe's lookup lists are
// "pck xml", "pck txt", "pck dds" and "pbb bob pbd bod": within one catalog the packed form wins.
const char* packed_form(const std::string& path) {
    size_t dot = path.rfind('.'), slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return nullptr;
    std::string ext = norm(path.substr(dot));
    if (ext == ".xml" || ext == ".txt" || ext == ".dds") return ".pck";
    if (ext == ".bod") return ".pbd";
    if (ext == ".bob") return ".pbb";
    return nullptr;
}

// .pck, .pbd, .pbb: gzip-packed files of the game
bool packed_ext(const wstr& path) {
    return ends_with_i(path, L".pck") || ends_with_i(path, L".pbd") || ends_with_i(path, L".pbb");
}

// Extension for what a .pck holds, by content: the folder is no guide (dds/ holds textures, director/mdfiles is a
// plain list, types/hq is XML) and is lost anyway when TC opens a .pck from inside a catalog via a temp copy.
const wchar_t* content_ext(const std::vector<uint8_t>& d) {
    auto starts = [&](const char* sig, size_t n) { return d.size() >= n && memcmp(d.data(), sig, n) == 0; };
    if (starts("DDS ", 4)) return L".dds";
    if (starts("\x89PNG\r\n\x1a\n", 8)) return L".png";
    if (starts("\xFF\xD8\xFF", 3)) return L".jpg";
    if (starts("BOB1", 4) || starts("CUT1", 4)) return L".bob";
    if (starts("OggS", 4)) return L".ogg";
    if (starts("RIFF", 4) && d.size() >= 12 && memcmp(d.data() + 8, "WAVE", 4) == 0) return L".wav";
    size_t i = starts("\xEF\xBB\xBF", 3) ? 3 : 0;
    while (i < d.size() && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) i++;
    if (i == d.size()) return nullptr;  // empty: no clue
    if (d[i] == '<') return L".xml";
    size_t n = std::min<size_t>(d.size(), 4096);
    return memchr(d.data(), 0, n) ? L".bin" : L".txt";
}

// Name of the single member: the base name with .bod for .pbd, .bob for .pbb; for .pck an extension from the
// content, and for an empty one from the folder (types -> .txt, else .xml).
wstr pck_member_name(const wstr& pckPath, const std::vector<uint8_t>& data) {
    wstr stem = base_name(strip_ext(pckPath));
    if (ends_with_i(pckPath, L".pbd")) return stem + L".bod";
    if (ends_with_i(pckPath, L".pbb")) return stem + L".bob";
    if (const wchar_t* ext = content_ext(data)) return stem + ext;
    return stem + (lower_w(base_name(dir_name(pckPath))) == L"types" ? L".txt" : L".xml");
}

// ---------------------------------------------------------------- archive kinds

enum Kind { kNone, kCat, kPck };

Kind sniff(const wstr& path) {
    File f(open_read(path));
    if (!f.ok()) return kNone;
    uint8_t b[512];
    DWORD got = 0;
    if (!ReadFile(f.h, b, sizeof b, &got, nullptr)) return kNone;
    if (looks_like_pck(b, got)) return kPck;
    if (looks_like_cat(b, got)) return kCat;
    return kNone;
}

Kind kind_for_write(const wstr& path) {
    if (packed_ext(path)) return kPck;
    if (ends_with_i(path, L".cat")) return kCat;
    Kind k = exists(path) ? sniff(path) : kNone;
    return k == kNone ? kCat : k;
}

// ---------------------------------------------------------------- open archive

struct Archive {
    Kind kind = kNone;
    wstr path, dat;
    Catalog cat;
    Pck pck;
    wstr pckName;
    size_t next = 0;
    int dosTime = 0;
    FILETIME ft = {};
    File hdat;
};

Archive* open_arc(const wstr& path, int& result) {
    std::unique_ptr<Archive> a(new Archive);
    a->path = path;
    std::vector<uint8_t> raw;
    result = read_all(path, raw);
    if (result) return nullptr;
    if (file_time(path, a->ft)) a->dosTime = dos_time(a->ft);
    if (looks_like_pck(raw.data(), raw.size()) || packed_ext(path)) {
        if (!decode_pck(raw, a->pck)) {
            result = E_BAD_ARCHIVE;
            return nullptr;
        }
        a->kind = kPck;
        a->pckName = pck_member_name(path, a->pck.data);
    } else if (parse_cat(raw, a->cat)) {
        a->kind = kCat;
        a->dat = dat_of(path);
    } else {
        result = E_UNKNOWN_FORMAT;
        return nullptr;
    }
    result = 0;
    return a.release();
}

// Fills the common fields of the header for the next member; false at the end.
bool next_member(Archive* a, wstr& name, uint64_t& packed, uint64_t& unpacked) {
    if (a->kind == kCat) {
        if (a->next >= a->cat.ents.size()) return false;
        const Entry& e = a->cat.ents[a->next];
        name = to_tc(e.path);
        packed = unpacked = e.size;
    } else {
        if (a->next >= 1) return false;
        name = a->pckName;
        unpacked = a->pck.data.size();
        packed = 0;
        File f(open_read(a->path));
        if (f.ok()) file_size(f.h, packed);
    }
    a->next++;
    return true;
}

int extract(Archive* a, int op, const wstr& dest) {
    if (op == PK_SKIP) return 0;
    if (a->next == 0) return E_NOT_SUPPORTED;
    File out;
    if (op == PK_EXTRACT) {
        out.h = create_write(dest);
        if (!out.ok()) {
            make_dirs(dir_name(dest));
            out.h = create_write(dest);
        }
        if (!out.ok()) return E_ECREATE;
    }
    int r = 0;
    wstr shown = op == PK_EXTRACT ? dest : a->path;
    if (a->kind == kCat) {
        const Entry& e = a->cat.ents[a->next - 1];
        if (!a->hdat.ok()) {
            a->hdat.h = open_read(a->dat);
            if (!a->hdat.ok()) r = E_EOPEN;
        }
        if (!r) r = seek(a->hdat.h, e.off) ? copy_stream(a->hdat.h, out.h == INVALID_HANDLE_VALUE ? nullptr : out.h, e.size, true, shown)
                                           : E_EREAD;
    } else {
        if (out.ok() && !write_exact(out.h, a->pck.data.data(), a->pck.data.size())) r = E_EWRITE;
        if (!r && !progress(shown, a->pck.data.size())) r = E_EABORTED;
    }
    if (out.ok()) {
        if (!r) SetFileTime(out.h, nullptr, nullptr, &a->ft);
        out.close();
        if (r) DeleteFileW(dest.c_str());
    }
    return r;
}

// ---------------------------------------------------------------- catalog writing

// One member of the new catalog: either bytes already in the old .dat, or a file to add.
struct Item {
    std::string path;
    uint64_t oldOff = 0, oldSize = 0;
    wstr src;           // empty: take from the old .dat
    bool pack = false;  // gzip src first (stored as .pck/.pbd/.pbb)
};

// Streams items to out, filling sizes; old members are copied raw (the XOR key does not depend on position).
int write_items(HANDLE oldDat, HANDLE out, const std::vector<Item>& items, const wstr& arcName, Catalog& nc) {
    for (const Item& it : items) {
        uint64_t size;
        int r;
        if (it.src.empty()) {
            size = it.oldSize;
            r = seek(oldDat, it.oldOff) ? copy_stream(oldDat, out, size, false, arcName) : E_EREAD;
        } else if (it.pack) {
            std::vector<uint8_t> data;
            r = read_all(it.src, data);
            if (r) return r;
            std::vector<uint8_t> gz = encode_pck(data, false, 0);  // unmasked, as the game's own
            for (uint8_t& x : gz) x ^= kDatKey;
            size = gz.size();
            if (!write_exact(out, gz.data(), gz.size())) return E_EWRITE;
            if (!progress(it.src, data.size())) return E_EABORTED;
        } else {
            File in(open_read(it.src));
            if (!in.ok()) return E_EOPEN;
            if (!file_size(in.h, size)) return E_EREAD;
            r = copy_stream(in.h, out, size, true, it.src);
        }
        if (r) return r;
        nc.ents.push_back({it.path, size, nc.total()});
    }
    return 0;
}

bool move(const wstr& from, const wstr& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// Puts datTmp/catTmp in place of dat/cat, keeping the old pair as *.x3wcx-bak until both renames succeed.
int commit_pair(const wstr& dat, const wstr& cat) {
    wstr datTmp = dat + kTmpSuffix, catTmp = cat + kTmpSuffix, datBak = dat + kBakSuffix, catBak = cat + kBakSuffix;
    bool hadDat = exists(dat), hadCat = exists(cat);
    if (hadDat && !move(dat, datBak)) return E_EWRITE;
    if (!move(datTmp, dat)) {
        if (hadDat) move(datBak, dat);
        return E_EWRITE;
    }
    if (hadCat && !move(cat, catBak)) {
        move(dat, datTmp);
        if (hadDat) move(datBak, dat);
        return E_EWRITE;
    }
    if (!move(catTmp, cat)) {
        if (hadCat) move(catBak, cat);
        move(dat, datTmp);
        if (hadDat) move(datBak, dat);
        return E_EWRITE;
    }
    DeleteFileW(datBak.c_str());
    DeleteFileW(catBak.c_str());
    return 0;
}

// Writes a fresh .dat and .cat next to the old ones, then swaps them in.
int rewrite_cat(const wstr& cat, const Catalog& old, const std::vector<Item>& items) {
    wstr dat = dat_of(cat), datTmp = dat + kTmpSuffix, catTmp = cat + kTmpSuffix;
    File oldDat;
    bool needOld = std::any_of(items.begin(), items.end(), [](const Item& i) { return i.src.empty(); });
    if (needOld) {
        oldDat.h = open_read(dat);
        if (!oldDat.ok()) return E_EOPEN;
    }
    Catalog nc;
    nc.first = old.first;
    int r;
    {
        File out(create_write(datTmp));
        if (!out.ok()) return E_ECREATE;
        r = write_items(oldDat.h, out.h, items, cat, nc);
        if (!r && !FlushFileBuffers(out.h)) r = E_EWRITE;
    }
    oldDat.close();
    std::vector<uint8_t> idx = encode_cat(nc);
    if (!r) {
        File out(create_write(catTmp));
        if (!out.ok()) r = E_ECREATE;
        else if (!write_exact(out.h, idx.data(), idx.size()) || !FlushFileBuffers(out.h)) r = E_EWRITE;
    }
    if (!r) r = commit_pair(dat, cat);
    DeleteFileW(datTmp.c_str());
    DeleteFileW(catTmp.c_str());
    return r;
}

// Appends items (all new files) to the end of the .dat in place, then replaces the .cat atomically.
// A failure truncates the .dat back; the old .cat never points past its old end either way.
int append_cat(const wstr& cat, const Catalog& old, const std::vector<Item>& items) {
    wstr dat = dat_of(cat);
    uint64_t end = old.total();
    // only an empty catalog may be missing its .dat; otherwise do not leave a stray empty one behind
    File f(CreateFileW(dat.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, old.ents.empty() ? OPEN_ALWAYS : OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f.ok()) return E_EOPEN;
    uint64_t size;
    if (!file_size(f.h, size)) return E_EREAD;
    if (size < end) return E_BAD_ARCHIVE;
    if (!seek(f.h, end)) return E_EWRITE;
    Catalog nc = old;
    int r = write_items(INVALID_HANDLE_VALUE, f.h, items, cat, nc);
    if (!r && !SetEndOfFile(f.h)) r = E_EWRITE;
    if (!r && !FlushFileBuffers(f.h)) r = E_EWRITE;
    if (!r) r = write_atomic(cat, encode_cat(nc));
    if (r && seek(f.h, end)) SetEndOfFile(f.h);
    return r;
}

struct Source {
    wstr src;   // file on disk
    wstr path;  // path inside the archive, forward slashes
};

wstr join(const wstr& dir, const wstr& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    return last == L'\\' || last == L'/' ? dir + name : dir + L"\\" + name;
}

// Files of an AddList with their archive paths; directories (trailing backslash) are skipped.
int collect_sources(const wstr& subPath, const wstr& srcPath, const std::vector<wstr>& list, int flags, std::vector<Source>& out) {
    for (const wstr& name : list) {
        if (name.empty() || name.back() == L'\\' || name.back() == L'/') continue;
        wstr inArc = flags & PK_PACK_SAVE_PATHS ? name : base_name(name);
        if (!subPath.empty()) inArc = join(subPath, inArc);
        std::replace(inArc.begin(), inArc.end(), L'\\', L'/');
        while (!inArc.empty() && inArc[0] == L'/') inArc.erase(0, 1);
        if (inArc.empty()) return E_BAD_DATA;
        out.push_back({join(srcPath, name), inArc});
    }
    return 0;
}

// *.x3wcx-bak next to the catalog means a swap was cut short (crash between the two renames): the backup may be
// the only whole copy, so nothing is written until the user sorts it out.
bool interrupted_swap(const wstr& cat) {
    return exists(cat + kBakSuffix) || exists(dat_of(cat) + kBakSuffix);
}

int pack_cat(const wstr& cat, const std::vector<Source>& srcs) {
    if (interrupted_swap(cat)) return E_ECREATE;
    Catalog old;
    bool fresh = !exists(cat);
    if (!fresh) {
        uint64_t size = 0;
        File f(open_read(cat));
        if (f.ok()) file_size(f.h, size);
        fresh = size == 0;
    }
    if (fresh) {
        if (exists(dat_of(cat))) return E_ECREATE;  // a lone .dat: do not overwrite what we cannot read
        std::string first;
        w_to_latin1(base_name(dat_of(cat)), first);
        old.first = first.empty() ? "x3wcx.dat" : first;
    } else {
        int r = load_cat(cat, old);
        if (r) return r;
    }
    std::vector<std::string> paths(srcs.size());  // catalog paths are latin1
    for (size_t i = 0; i < srcs.size(); i++)
        if (!w_to_latin1(srcs[i].path, paths[i])) return E_BAD_DATA;
    // types the game looks up packed first go in packed, unless this catalog already holds that very file unpacked
    std::vector<bool> packIt(srcs.size(), false);
    if (pack_on_add()) {
        for (size_t i = 0; i < srcs.size(); i++) {
            const char* to = packed_form(paths[i]);
            if (!to) continue;
            std::string n = norm(paths[i]);
            if (std::any_of(old.ents.begin(), old.ents.end(), [&](const Entry& e) { return norm(e.path) == n; })) continue;
            paths[i] = paths[i].substr(0, paths[i].rfind('.')) + to;
            packIt[i] = true;
        }
    }
    // the last of equal paths wins; a path already in the catalog is replaced in its place
    std::vector<int> replaceBy(old.ents.size(), -1);
    std::vector<bool> isNew(srcs.size(), true);
    for (size_t i = 0; i < srcs.size(); i++) {
        std::string n = norm(paths[i]);
        for (size_t j = i + 1; j < srcs.size(); j++)
            if (norm(paths[j]) == n) isNew[i] = false;
        if (!isNew[i]) continue;
        for (size_t k = 0; k < old.ents.size(); k++)
            if (norm(old.ents[k].path) == n) {
                replaceBy[k] = (int)i;
                isNew[i] = false;
            }
    }
    bool replacing = std::any_of(replaceBy.begin(), replaceBy.end(), [](int x) { return x >= 0; });
    std::vector<Item> added;
    for (size_t i = 0; i < srcs.size(); i++)
        if (isNew[i]) added.push_back({paths[i], 0, 0, srcs[i].src, packIt[i]});
    if (!replacing) return added.empty() ? 0 : append_cat(cat, old, added);
    std::vector<Item> items;
    for (size_t k = 0; k < old.ents.size(); k++) {
        const Entry& e = old.ents[k];
        if (replaceBy[k] >= 0) items.push_back({e.path, 0, 0, srcs[replaceBy[k]].src, packIt[replaceBy[k]]});
        else items.push_back({e.path, e.off, e.size, wstr()});
    }
    items.insert(items.end(), added.begin(), added.end());
    return rewrite_cat(cat, old, items);
}

bool matches(const std::string& path, const wstr& pattern) {
    std::string p;
    if (!w_to_latin1(pattern, p)) return false;
    p = norm(p);
    std::string n = norm(path);
    if (p == "*.*" || p == "*") return true;
    for (const char* tail : {"/*.*", "/*"}) {
        size_t t = strlen(tail);
        if (p.size() > t && p.compare(p.size() - t, t, tail) == 0) {
            std::string dir = p.substr(0, p.size() - t + 1);
            return n.compare(0, dir.size(), dir) == 0;
        }
    }
    return n == p;
}

int delete_cat(const wstr& cat, const std::vector<wstr>& list) {
    if (interrupted_swap(cat)) return E_ECREATE;
    Catalog old;
    int r = load_cat(cat, old);
    if (r) return r;
    std::vector<bool> del(old.ents.size(), false);
    size_t ndel = 0, firstDel = old.ents.size();
    for (size_t k = 0; k < old.ents.size(); k++)
        for (const wstr& pat : list)
            if (matches(old.ents[k].path, pat)) {
                del[k] = true;
                ndel++;
                firstDel = std::min(firstDel, k);
                break;
            }
    if (!ndel) return 0;
    if (firstDel + ndel == old.ents.size()) {
        // only the tail goes: the new index is a prefix of the old one, so write it first and then cut the .dat
        Catalog nc = old;
        nc.ents.resize(firstDel);
        r = write_atomic(cat, encode_cat(nc));
        if (r) return r;
        File f(CreateFileW(dat_of(cat).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (f.ok() && seek(f.h, nc.total())) SetEndOfFile(f.h);
        return 0;
    }
    std::vector<Item> items;
    for (size_t k = 0; k < old.ents.size(); k++)
        if (!del[k]) items.push_back({old.ents[k].path, old.ents[k].off, old.ents[k].size, wstr()});
    return rewrite_cat(cat, old, items);
}

int pack_pck(const wstr& pck, const std::vector<Source>& srcs) {
    if (srcs.empty()) return E_NO_FILES;
    if (srcs.size() > 1) return E_TOO_MANY_FILES;
    Pck p;
    if (exists(pck)) {
        std::vector<uint8_t> raw;
        int r = read_all(pck, raw);
        if (r) return r;
        if (!raw.empty() && !decode_pck(raw, p)) return E_BAD_ARCHIVE;
        // an existing .pck takes only its own member back (any extension, any case), not a file dropped in by mistake
        if (!raw.empty() && lower_w(base_name(strip_ext(srcs[0].path))) != lower_w(base_name(strip_ext(pck)))) return E_NOT_SUPPORTED;
    }
    std::vector<uint8_t> data;
    int r = read_all(srcs[0].src, data);
    if (r) return r;
    if (!progress(srcs[0].src, data.size())) return E_EABORTED;
    return write_atomic(pck, encode_pck(data, p.masked, p.key0));
}

int pack_files(const wstr& packed, const wstr& subPath, const wstr& srcPath, const std::vector<wstr>& list, int flags) {
    std::vector<Source> srcs;
    int r = collect_sources(subPath, srcPath, list, flags, srcs);
    if (r) return r;
    r = kind_for_write(packed) == kPck ? pack_pck(packed, srcs) : (srcs.empty() ? 0 : pack_cat(packed, srcs));
    if (!r && (flags & PK_PACK_MOVE_FILES)) {
        for (const Source& s : srcs) DeleteFileW(s.src.c_str());
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (!it->empty() && it->back() == L'\\') RemoveDirectoryW(join(srcPath, it->substr(0, it->size() - 1)).c_str());
    }
    return r;
}

int delete_files(const wstr& packed, const std::vector<wstr>& list) {
    Kind k = kind_for_write(packed);
    if (k == kPck) return E_NOT_SUPPORTED;
    return delete_cat(packed, list);
}

// gzip flags of a .pck (after unmasking); the game's files have none, while gzip.exe stores the file name.
int pck_flags(const wstr& path) {
    File f(open_read(path));
    uint8_t b[5] = {};
    DWORD got = 0;
    if (!f.ok() || !ReadFile(f.h, b, sizeof b, &got, nullptr) || got < 5) return -1;
    return b[0] == 0x1F && b[1] == 0x8B ? b[3] : b[4] ^ b[0] ^ 0xC8;
}

bool can_handle(const wstr& path) {
    Kind k = sniff(path);
    if (k == kCat) return exists(dat_of(path));
    return k == kPck && (packed_ext(path) || pck_flags(path) == 0);
}

wstr dest_path(const wstr& destPath, const wstr& destName) {
    return destPath.empty() ? destName : join(destPath, destName);
}

}  // namespace

// ---------------------------------------------------------------- exports

extern "C" {

HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* d) {
    int r = 0;
    Archive* a = open_arc(d->ArcName ? d->ArcName : L"", r);
    d->OpenResult = r;
    d->CmtSize = 0;
    d->CmtState = 0;
    return a ? (HANDLE)a : nullptr;
}

HANDLE __stdcall OpenArchive(tOpenArchiveData* d) {
    int r = 0;
    Archive* a = open_arc(acp_to_w(d->ArcName), r);
    d->OpenResult = r;
    d->CmtSize = 0;
    d->CmtState = 0;
    return a ? (HANDLE)a : nullptr;
}

int __stdcall ReadHeaderExW(HANDLE h, tHeaderDataExW* hd) {
    Archive* a = (Archive*)h;
    wstr name;
    uint64_t packed, unpacked;
    if (!next_member(a, name, packed, unpacked)) return E_END_ARCHIVE;
    memset(hd, 0, sizeof *hd);
    copy_trunc(hd->ArcName, a->path, 1024);
    copy_trunc(hd->FileName, name, 1024);
    hd->PackSize = (unsigned)packed;
    hd->PackSizeHigh = (unsigned)(packed >> 32);
    hd->UnpSize = (unsigned)unpacked;
    hd->UnpSizeHigh = (unsigned)(unpacked >> 32);
    hd->FileTime = a->dosTime;
    hd->FileAttr = FILE_ATTRIBUTE_ARCHIVE;
    return 0;
}

int __stdcall ReadHeaderEx(HANDLE h, tHeaderDataEx* hd) {
    Archive* a = (Archive*)h;
    wstr name;
    uint64_t packed, unpacked;
    if (!next_member(a, name, packed, unpacked)) return E_END_ARCHIVE;
    memset(hd, 0, sizeof *hd);
    copy_trunc(hd->ArcName, w_to_acp(a->path), 1024);
    copy_trunc(hd->FileName, w_to_acp(name), 1024);
    hd->PackSize = (unsigned)packed;
    hd->PackSizeHigh = (unsigned)(packed >> 32);
    hd->UnpSize = (unsigned)unpacked;
    hd->UnpSizeHigh = (unsigned)(unpacked >> 32);
    hd->FileTime = a->dosTime;
    hd->FileAttr = FILE_ATTRIBUTE_ARCHIVE;
    return 0;
}

int __stdcall ReadHeader(HANDLE h, tHeaderData* hd) {
    Archive* a = (Archive*)h;
    wstr name;
    uint64_t packed, unpacked;
    if (!next_member(a, name, packed, unpacked)) return E_END_ARCHIVE;
    memset(hd, 0, sizeof *hd);
    copy_trunc(hd->ArcName, w_to_acp(a->path), 260);
    copy_trunc(hd->FileName, w_to_acp(name), 260);
    hd->PackSize = (int)std::min<uint64_t>(packed, 0x7FFFFFFF);
    hd->UnpSize = (int)std::min<uint64_t>(unpacked, 0x7FFFFFFF);
    hd->FileTime = a->dosTime;
    hd->FileAttr = FILE_ATTRIBUTE_ARCHIVE;
    return 0;
}

int __stdcall ProcessFileW(HANDLE h, int op, WCHAR* destPath, WCHAR* destName) {
    return extract((Archive*)h, op, dest_path(destPath ? destPath : L"", destName ? destName : L""));
}

int __stdcall ProcessFile(HANDLE h, int op, char* destPath, char* destName) {
    return extract((Archive*)h, op, dest_path(acp_to_w(destPath), acp_to_w(destName)));
}

int __stdcall CloseArchive(HANDLE h) {
    delete (Archive*)h;
    return 0;
}

void __stdcall SetChangeVolProc(HANDLE, tChangeVolProc) {}
void __stdcall SetChangeVolProcW(HANDLE, tChangeVolProcW) {}

void __stdcall SetProcessDataProc(HANDLE, tProcessDataProc p) {
    gProcA = p;
}

void __stdcall SetProcessDataProcW(HANDLE, tProcessDataProcW p) {
    gProcW = p;
}

int __stdcall GetPackerCaps() {
    return PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE | PK_CAPS_OPTIONS | PK_CAPS_BY_CONTENT |
           PK_CAPS_SEARCHTEXT;
}

void __stdcall PackSetDefaultParams(PackDefaultParamStruct* dps) {
    if (dps && dps->size >= (int)sizeof(PackDefaultParamStruct)) gIni = dps->DefaultIniName;
}

// The "Configure" button in TC's packer dialog: one Yes/No question.
void __stdcall ConfigurePacker(HWND parent, HINSTANCE) {
    bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    const wchar_t* text =
        ru ? L"Сжимать файлы при добавлении в каталог (.cat)?\n\n"
             L".xml, .txt, .dds → .pck,  .bod → .pbd,  .bob → .pbb\n\n"
             L"Игра ищет эти файлы сначала в сжатом виде. Файл, который уже лежит в каталоге несжатым, "
             L"заменяется как есть.\n\nСейчас: "
           : L"Pack files when adding them to a catalog (.cat)?\n\n"
             L".xml, .txt, .dds → .pck,  .bod → .pbd,  .bob → .pbb\n\n"
             L"The game looks these up packed first. A file the catalog already holds unpacked is replaced as it is."
             L"\n\nNow: ";
    wstr msg = wstr(text) + (pack_on_add() ? (ru ? L"да" : L"yes") : (ru ? L"нет" : L"no"));
    int r = MessageBoxW(parent, msg.c_str(), L"x3wcx", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return;
    if (gIni.empty()) {
        MessageBoxW(parent, ru ? L"TC не передал файл настроек." : L"TC gave no settings file.", L"x3wcx", MB_ICONWARNING);
        return;
    }
    WritePrivateProfileStringA(kIniSection, kPackOnAdd, r == IDYES ? "1" : "0", gIni.c_str());
}

int __stdcall PackFilesW(WCHAR* packedFile, WCHAR* subPath, WCHAR* srcPath, WCHAR* addList, int flags) {
    return pack_files(packedFile, subPath ? subPath : L"", srcPath ? srcPath : L"", split_list_w(addList), flags);
}

int __stdcall PackFiles(char* packedFile, char* subPath, char* srcPath, char* addList, int flags) {
    return pack_files(acp_to_w(packedFile), acp_to_w(subPath), acp_to_w(srcPath), split_list_a(addList), flags);
}

int __stdcall DeleteFilesW(WCHAR* packedFile, WCHAR* deleteList) {
    return delete_files(packedFile, split_list_w(deleteList));
}

int __stdcall DeleteFiles(char* packedFile, char* deleteList) {
    return delete_files(acp_to_w(packedFile), split_list_a(deleteList));
}

BOOL __stdcall CanYouHandleThisFileW(WCHAR* fileName) {
    return can_handle(fileName ? fileName : L"");
}

BOOL __stdcall CanYouHandleThisFile(char* fileName) {
    return can_handle(acp_to_w(fileName));
}

}  // extern "C"
