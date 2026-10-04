// dllmain.cpp : load the real GameAssembly at runtime, redirect every export to it
// and load the mods in autorun around il2cpp init
//
//   autorun\early\*.dll  loaded right before il2cpp_init, for mods that have to hook
//                        the runtime before it starts (il2cpp api is NOT usable yet)
//   autorun\*.dll        loaded right after il2cpp_init succeeded, il2cpp api is ready
//
// both are loaded in name order on unity's main thread, so a mod's DllMain should
// spawn a thread for anything long running instead of blocking the game
//
// mods that need the real GameAssembly (GetModuleHandle(L"GameAssembly.dll") is this
// proxy) can get it with:
//   auto get = (HMODULE(*)())GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), "GL_GetGameAssembly");
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "exports_gen.h"

#ifndef _WIN64
#error GameAssembly is x64 only, the jmp stubs in stubs.asm are x64 MASM
#endif

namespace fs = std::filesystem;

static fs::path g_game_root;
static HMODULE g_orig = nullptr;

// if GameAssembly_orig.dll does not exist we will iterate through the entire game root to find it
// via md5 (kOrigMd5/kOrigSize come from gen_exports.py)
constexpr wchar_t GameAssembly_orig_name[] = L"GameAssembly_orig.dll";

constexpr size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

// stubs.asm does jmp qword ptr [g_real_exports + i*8], filled in by LoadOriginal()
extern "C" void* g_real_exports[kExportCount] = {};

using il2cpp_init_t = int (*)(const char* domain_name);
using il2cpp_init_utf16_t = int (*)(const wchar_t* domain_name);  // Il2CppChar is wchar_t on windows

static void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    int n = swprintf_s(msg, L"[GL_dll_loader] ");
    va_list args;
    va_start(args, fmt);
    // truncate instead of the _s invalid parameter handler killing the game on long paths
    _vsnwprintf_s(msg + n, _countof(msg) - n, _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(msg);
}

static fs::path ModulePath(HMODULE module) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(module, buf.data(), (DWORD)buf.size());
        if (len == 0)
            return {};
        if (len < buf.size()) {
            buf.resize(len);
            return buf;
        }
        if (buf.size() >= 32768)
            return {};
        buf.resize(buf.size() * 2);
    }
}

// anything the original doesn't actually export lands here instead of jumping to null
extern "C" void MissingExport() {
    MessageBoxW(nullptr, L"The game called a GameAssembly export that GameAssembly_orig.dll does not have.\n"
                         L"Re-run gen_exports.py against the current GameAssembly_orig.dll and rebuild.",
                L"GL_dll_loader", MB_ICONERROR);
    ExitProcess(1);
}

// for mods, see the top of the file
extern "C" HMODULE GL_GetGameAssembly() {
    return g_orig;
}

static bool Md5File(const fs::path& file, char (&hex)[33]) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    bool ok = false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_MD5_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<UCHAR> chunk(1 << 20);
        ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(h, chunk.data(), (DWORD)chunk.size(), &read, nullptr)) {
                Log(L"failed to read %ls (error %lu)\n", file.c_str(), GetLastError());
                ok = false;
                break;
            }
            if (read == 0)
                break;
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, chunk.data(), read, 0))) {
                ok = false;
                break;
            }
        }
        UCHAR digest[16];
        ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
        if (ok) {
            for (int i = 0; i < 16; i++)
                sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
        }
        BCryptDestroyHash(hash);
    }
    CloseHandle(h);
    return ok;
}

static bool IsOriginal(const fs::path& file) {
    char hex[33];
    return Md5File(file, hex) && strcmp(hex, kOrigMd5) == 0;
}

static fs::path FindOriginal() {
    fs::path expected = g_game_root / GameAssembly_orig_name;
    std::error_code ec;
    if (fs::is_regular_file(expected, ec)) {
        // still load it on a mismatch, exports are resolved by name so a newer
        // original mostly works, but say why things might break
        if (!IsOriginal(expected))
            Log(L"%ls does not match the md5 this loader was built for (%hs), re-run gen_exports.py and rebuild\n",
                expected.c_str(), kOrigMd5);
        return expected;
    }

    Log(L"%ls not found, searching %ls by md5\n", GameAssembly_orig_name, g_game_root.c_str());

    // any name/extension (GameAssembly.dll.orig, .bak, ...), only files of the right size get hashed
    auto opts = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(g_game_root, opts, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || it->file_size(file_ec) != kOrigSize)
            continue;
        if (IsOriginal(it->path()))
            return it->path();
    }
    if (ec)
        Log(L"md5 search stopped early (error %d)\n", ec.value());
    return {};
}

static bool LoadOriginal() {
    fs::path path = FindOriginal();
    if (path.empty()) {
        Log(L"could not find the original GameAssembly (md5 %hs)\n", kOrigMd5);
        return false;
    }

    // the original's dependencies (baselib.dll) live in the game root, which is not
    // necessarily the folder the md5 search found it in, so search both
    DLL_DIRECTORY_COOKIE root_cookie = AddDllDirectory(g_game_root.c_str());
    g_orig = LoadLibraryExW(path.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    DWORD load_err = GetLastError();
    if (root_cookie)
        RemoveDllDirectory(root_cookie);
    if (!g_orig) {
        Log(L"failed to load %ls (error %lu)\n", path.c_str(), load_err);
        return false;
    }

    for (size_t i = 0; i < kExportCount; i++) {
        FARPROC proc = GetProcAddress(g_orig, kExportNames[i]);
        if (!proc) {
            Log(L"%hs is missing from %ls\n", kExportNames[i], path.c_str());
            proc = reinterpret_cast<FARPROC>(&MissingExport);
        }
        g_real_exports[i] = reinterpret_cast<void*>(proc);
    }

    Log(L"redirected %zu exports to %ls\n", kExportCount, path.c_str());
    return true;
}

static void LoadModsFrom(const fs::path& dir) noexcept {
    try {
        /* there must be a directory
           named "autorun" next to this
           dll or else no work!

           now it works
        */
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            fs::create_directories(dir, ec);
            if (ec)
                Log(L"failed to create %ls after finding it was missing (error %d)\n", dir.c_str(), ec.value());
            return;
        }

        std::vector<fs::path> mods;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code file_ec;
            if (it->is_regular_file(file_ec) && _wcsicmp(it->path().extension().c_str(), L".dll") == 0)
                mods.push_back(it->path());
        }
        if (ec)
            Log(L"failed to list %ls (error %d)\n", dir.c_str(), ec.value());
        // load in name order so mods can rely on it (01_core.dll before 02_whatever.dll)
        std::sort(mods.begin(), mods.end());

        // load every .dll in the folder
        for (const auto& mod : mods) {
            if (LoadLibraryExW(mod.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
                Log(L"loaded %ls\n", mod.filename().c_str());
            else
                Log(L"Failed to load %ls (error %lu)\n", mod.c_str(), GetLastError());
        }
    } catch (const std::exception& e) {
        Log(L"error while loading mods from %ls: %hs\n", dir.c_str(), e.what());
    }
}

static std::once_flag g_early_once;
static std::once_flag g_late_once;

static void BeforeIl2cppInit() {
    std::call_once(g_early_once, [] { LoadModsFrom(g_game_root / L"autorun" / L"early"); });
}

static void AfterIl2cppInit(int result) {
    if (!result) {
        Log(L"il2cpp_init failed, not loading mods\n");
        return;
    }
    std::call_once(g_late_once, [] { LoadModsFrom(g_game_root / L"autorun"); });
}

// exported as il2cpp_init / il2cpp_init_utf16 (see exports.def), UnityPlayer calls one
// of them on the main thread once at startup. the original's utf16 version calls its
// own il2cpp_init internally, so only one of these hooks fires and the once flags
// cover any other order
extern "C" int hk_il2cpp_init(const char* domain_name) {
    BeforeIl2cppInit();
    int result = reinterpret_cast<il2cpp_init_t>(g_real_exports[kExport_il2cpp_init])(domain_name);
    AfterIl2cppInit(result);
    return result;
}

extern "C" int hk_il2cpp_init_utf16(const wchar_t* domain_name) {
    BeforeIl2cppInit();
    int result = reinterpret_cast<il2cpp_init_utf16_t>(g_real_exports[kExport_il2cpp_init_utf16])(domain_name);
    AfterIl2cppInit(result);
    return result;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    (void)lpReserved;

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // the table has to be filled before DllMain returns, UnityPlayer starts
        // calling exports right after its LoadLibrary("GameAssembly.dll")
        try {
            g_game_root = ModulePath(hModule).parent_path();
            if (g_game_root.empty()) {
                Log(L"could not get the path of this dll (error %lu)\n", GetLastError());
                return FALSE;
            }
            return LoadOriginal() ? TRUE : FALSE;
        } catch (const std::exception& e) {
            Log(L"failed to load the original GameAssembly: %hs\n", e.what());
            return FALSE;
        }
    default:
        break;
    }
    return TRUE;
}
