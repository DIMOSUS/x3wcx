// Test driver: loads a WCX plugin and calls its exports the way Total Commander does.
//
//   wcxdrv <dll> [--abort-after N] <command> ...
//     caps                                  GetPackerCaps
//     canhandle <file>                      CanYouHandleThisFileW (and the ANSI one): prints 1/0
//     list <arc>                            name<TAB>packed<TAB>unpacked<TAB>attr, via OpenArchiveW/ReadHeaderExW
//     lista <arc>                           the same via OpenArchive/ReadHeader (ANSI, 32-bit sizes)
//     hashes <arc> <tmpdir>                 md5<TAB>name of every member, each extracted by ProcessFileW to a temp file
//     extract <arc> <outdir>                ProcessFileW(PK_EXTRACT) for all members, DestPath = NULL, DestName = full path
//     test <arc>                            ProcessFileW(PK_TEST) for all members
//     pack <arc> <srcdir> <subpath|-> <flags> <name>...    PackFilesW
//     packa <arc> <srcdir> <subpath|-> <flags> <name>...   PackFiles (ANSI)
//     delete <arc> <name>...                DeleteFilesW
// Exit code: the WCX error code (0 = ok). Progress calls and bytes go to stderr as "progress <calls> <bytes>".
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/wcxhead.h"

typedef HANDLE(__stdcall* OpenArchiveW_t)(tOpenArchiveDataW*);
typedef HANDLE(__stdcall* OpenArchive_t)(tOpenArchiveData*);
typedef int(__stdcall* ReadHeaderExW_t)(HANDLE, tHeaderDataExW*);
typedef int(__stdcall* ReadHeader_t)(HANDLE, tHeaderData*);
typedef int(__stdcall* ProcessFileW_t)(HANDLE, int, WCHAR*, WCHAR*);
typedef int(__stdcall* CloseArchive_t)(HANDLE);
typedef void(__stdcall* SetProcessDataProcW_t)(HANDLE, tProcessDataProcW);
typedef int(__stdcall* GetPackerCaps_t)();
typedef int(__stdcall* PackFilesW_t)(WCHAR*, WCHAR*, WCHAR*, WCHAR*, int);
typedef int(__stdcall* PackFiles_t)(char*, char*, char*, char*, int);
typedef int(__stdcall* DeleteFilesW_t)(WCHAR*, WCHAR*);
typedef BOOL(__stdcall* CanYouHandleThisFileW_t)(WCHAR*);
typedef BOOL(__stdcall* CanYouHandleThisFile_t)(char*);

static HMODULE gDll;
static long long gAbortAfter = -1, gCalls = 0, gBytes = 0;

template <class T>
static T fn(const char* name) {
    T p = (T)GetProcAddress(gDll, name);
    if (!p) {
        fprintf(stderr, "missing export %s\n", name);
        ExitProcess(100);
    }
    return p;
}

static int __stdcall on_progress(WCHAR*, int size) {
    gCalls++;
    gBytes += size;
    return gAbortAfter >= 0 && gCalls > gAbortAfter ? 0 : 1;
}

static void out_utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    fwrite(s.data(), 1, s.size(), stdout);
}

static std::string to_acp(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}

static std::wstring from_acp(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

static bool md5_file(const std::wstring& path, std::wstring& hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    std::vector<unsigned char> buf(1 << 20);
    DWORD got;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) BCryptHashData(h, buf.data(), got, 0);
    CloseHandle(f);
    unsigned char d[16];
    BCryptFinishHash(h, d, 16, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    wchar_t s[33];
    for (int i = 0; i < 16; i++) swprintf(s + 2 * i, 3, L"%02x", d[i]);
    hex = s;
    return true;
}

static HANDLE open_w(const std::wstring& arc, int mode, int& res) {
    tOpenArchiveDataW od = {};
    std::wstring a = arc;
    od.ArcName = &a[0];
    od.OpenMode = mode;
    HANDLE h = fn<OpenArchiveW_t>("OpenArchiveW")(&od);
    res = od.OpenResult;
    if (!h && !res) res = 99;
    if (h) fn<SetProcessDataProcW_t>("SetProcessDataProcW")(h, on_progress);
    return h;
}

// Walks the archive; for each member calls visit(name, header) which returns the ProcessFileW operation and destination.
template <class V>
static int walk(const std::wstring& arc, int mode, V visit) {
    int res;
    HANDLE h = open_w(arc, mode, res);
    if (!h) return res;
    auto rh = fn<ReadHeaderExW_t>("ReadHeaderExW");
    auto pf = fn<ProcessFileW_t>("ProcessFileW");
    int r;
    tHeaderDataExW hd;
    while ((r = rh(h, &hd)) == 0) {
        std::wstring dest;
        int op = visit(hd, dest);
        r = pf(h, op, nullptr, dest.empty() ? nullptr : &dest[0]);
        if (r) break;
        if (op == PK_EXTRACT) visit(hd, dest = L"\x01done");
    }
    fn<CloseArchive_t>("CloseArchive")(h);
    return r == E_END_ARCHIVE ? 0 : r;
}

static std::vector<wchar_t> make_list(int argc, wchar_t** argv, int from) {
    std::vector<wchar_t> list;
    for (int i = from; i < argc; i++) {
        std::wstring s = argv[i];
        list.insert(list.end(), s.begin(), s.end());
        list.push_back(0);
    }
    list.push_back(0);
    list.push_back(0);
    return list;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: wcxdrv <dll> [--abort-after N] <command> ...\n");
        return 2;
    }
    gDll = LoadLibraryW(argv[1]);
    if (!gDll) {
        fprintf(stderr, "cannot load %ls (%lu)\n", argv[1], GetLastError());
        return 101;
    }
    int a = 2;
    if (std::wstring(argv[a]) == L"--abort-after") {
        gAbortAfter = _wtoi64(argv[a + 1]);
        a += 2;
    }
    std::wstring cmd = argv[a++];
    int r = 0;
    if (cmd == L"caps") {
        printf("%d\n", fn<GetPackerCaps_t>("GetPackerCaps")());
    } else if (cmd == L"canhandle") {
        BOOL w = fn<CanYouHandleThisFileW_t>("CanYouHandleThisFileW")(argv[a]);
        std::string n = to_acp(argv[a]);
        BOOL an = fn<CanYouHandleThisFile_t>("CanYouHandleThisFile")(&n[0]);
        printf("%d %d\n", w ? 1 : 0, an ? 1 : 0);
    } else if (cmd == L"list") {
        r = walk(argv[a], PK_OM_LIST, [](tHeaderDataExW& hd, std::wstring&) {
            unsigned long long p = (unsigned long long)hd.PackSizeHigh << 32 | hd.PackSize;
            unsigned long long u = (unsigned long long)hd.UnpSizeHigh << 32 | hd.UnpSize;
            out_utf8(std::wstring(hd.FileName) + L"\t" + std::to_wstring(p) + L"\t" + std::to_wstring(u) + L"\t" +
                     std::to_wstring(hd.FileAttr) + L"\n");
            return PK_SKIP;
        });
    } else if (cmd == L"lista") {
        tOpenArchiveData od = {};
        std::string arc = to_acp(argv[a]);
        od.ArcName = &arc[0];
        od.OpenMode = PK_OM_LIST;
        HANDLE h = fn<OpenArchive_t>("OpenArchive")(&od);
        if (!h) return od.OpenResult ? od.OpenResult : 99;
        tHeaderData hd;
        while ((r = fn<ReadHeader_t>("ReadHeader")(h, &hd)) == 0) {
            out_utf8(from_acp(hd.FileName) + L"\t" + std::to_wstring(hd.PackSize) + L"\t" + std::to_wstring(hd.UnpSize) + L"\n");
            if ((r = ((int(__stdcall*)(HANDLE, int, char*, char*))fn<FARPROC>("ProcessFile"))(h, PK_SKIP, nullptr, nullptr))) break;
        }
        fn<CloseArchive_t>("CloseArchive")(h);
        if (r == E_END_ARCHIVE) r = 0;
    } else if (cmd == L"hashes") {
        std::wstring tmp = std::wstring(argv[a + 1]) + L"\\member.bin";
        r = walk(argv[a], PK_OM_EXTRACT, [&](tHeaderDataExW& hd, std::wstring& dest) {
            if (dest == L"\x01done") {
                std::wstring hex;
                if (!md5_file(tmp, hex)) hex = L"?";
                out_utf8(hex + L"\t" + hd.FileName + L"\n");
                DeleteFileW(tmp.c_str());
                return 0;
            }
            dest = tmp;
            return PK_EXTRACT;
        });
    } else if (cmd == L"extract") {
        std::wstring out = argv[a + 1];
        r = walk(argv[a], PK_OM_EXTRACT, [&](tHeaderDataExW& hd, std::wstring& dest) {
            if (dest == L"\x01done") return 0;
            dest = out + L"\\" + hd.FileName;
            return PK_EXTRACT;
        });
    } else if (cmd == L"test") {
        r = walk(argv[a], PK_OM_EXTRACT, [](tHeaderDataExW&, std::wstring&) { return PK_TEST; });
    } else if (cmd == L"pack" || cmd == L"packa") {
        std::wstring arc = argv[a], src = argv[a + 1], sub = argv[a + 2];
        int flags = _wtoi(argv[a + 3]);
        std::vector<wchar_t> list = make_list(argc, argv, a + 4);
        if (cmd == L"pack") {
            fn<SetProcessDataProcW_t>("SetProcessDataProcW")(INVALID_HANDLE_VALUE, on_progress);
            r = fn<PackFilesW_t>("PackFilesW")(&arc[0], sub == L"-" ? nullptr : &sub[0], &src[0], list.data(), flags);
        } else {
            std::string aarc = to_acp(arc), asrc = to_acp(src), asub = to_acp(sub);
            std::string alist;
            for (int i = a + 4; i < argc; i++) alist += to_acp(argv[i]) + '\0';
            alist += '\0';
            r = fn<PackFiles_t>("PackFiles")(&aarc[0], sub == L"-" ? nullptr : &asub[0], &asrc[0], &alist[0], flags);
        }
    } else if (cmd == L"delete") {
        std::wstring arc = argv[a];
        std::vector<wchar_t> list = make_list(argc, argv, a + 1);
        fn<SetProcessDataProcW_t>("SetProcessDataProcW")(INVALID_HANDLE_VALUE, on_progress);
        r = fn<DeleteFilesW_t>("DeleteFilesW")(&arc[0], list.data());
    } else {
        fprintf(stderr, "unknown command\n");
        return 2;
    }
    fflush(stdout);
    fprintf(stderr, "progress %lld %lld\n", gCalls, gBytes);
    return r;
}
