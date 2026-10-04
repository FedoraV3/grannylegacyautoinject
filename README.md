# Granny Legacy auto inject

A mod loader for **Granny Legacy**. It replaces the game's `GameAssembly.dll` with a small proxy that loads the real one and passes every call through to it. Then it loads every mod DLL you drop into the `autorun` folder automatically, so you don't need an injector.

## Install

1. Download `GameAssembly.dll` from the [latest release](https://github.com/FedoraV3/grannylegacyautoinject/releases/latest).
2. Open the game folder. In Steam: right-click **Granny Legacy** → **Manage** → **Browse local files**.
3. Rename the game's own `GameAssembly.dll` to **`GameAssembly_orig.dll`**.
4. Copy the downloaded `GameAssembly.dll` into the game folder.
5. Start the game once. It creates the `autorun` folder (and `autorun\early`) next to the exe.
6. Put your mod `.dll` files in `autorun` and restart the game.

The game folder should look like this:

```
Granny_Legacy\
├── Granny Legacy.exe
├── GameAssembly.dll         <- this loader
├── GameAssembly_orig.dll    <- the game's original GameAssembly.dll, renamed
├── UnityPlayer.dll
├── baselib.dll
└── autorun\
    ├── early\               <- mods that must load before il2cpp starts
    │   └── ...
    ├── 01_mymod.dll         <- normal mods
    └── 02_othermod.dll
```

If you named the original something else (for example `GameAssembly.dll.orig`) or moved it into a subfolder, the loader still finds it by its md5 hash. `GameAssembly_orig.dll` next to the exe is the fastest option, though.

No Visual C++ redistributable is needed: the loader is built with the static runtime.

### Uninstall

Delete the loader's `GameAssembly.dll` and rename `GameAssembly_orig.dll` back to `GameAssembly.dll`. Or use **Verify integrity of game files** in Steam.

### After a game update

A Steam update can overwrite `GameAssembly.dll` with the new real one, which removes the loader. Repeat steps 3 and 4. If the update changed GameAssembly, the loader logs an md5 warning and still tries to run, but you should rebuild it against the new version (see [Building](#building)).

## When mods are loaded

| Folder | When | il2cpp API |
| --- | --- | --- |
| `autorun\early\*.dll` | right before `il2cpp_init` | not usable yet. Use this to hook runtime startup |
| `autorun\*.dll` | right after `il2cpp_init` succeeds | ready to use |

- Mods load in name order (`01_core.dll` before `02_whatever.dll`), on the game's main thread.
- Mods in `autorun` are skipped if `il2cpp_init` fails.
- Only files ending in `.dll` are loaded. Anything else in the folders is ignored.

## Writing mods

A mod is a normal x64 DLL. Its `DllMain` runs on the game's main thread, so start a thread for anything slow instead of blocking the game:

```cpp
#include <Windows.h>

DWORD WINAPI ModThread(LPVOID) {
    // your mod
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
```

### Finding the real GameAssembly

`GetModuleHandle(L"GameAssembly.dll")` returns this loader, not the game code. If you use offsets from Il2CppDumper, get the real module with the `GL_GetGameAssembly` export:

```cpp
using GetGameAssembly_t = HMODULE (*)();
auto get = reinterpret_cast<GetGameAssembly_t>(
    GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), "GL_GetGameAssembly"));
uintptr_t base = reinterpret_cast<uintptr_t>(get());
```

The il2cpp exports (`il2cpp_domain_get`, `il2cpp_class_from_name`, ...) can be called on either module. The loader forwards them.

## Logs

The loader writes everything it does to `OutputDebugString` with the prefix `[GL_dll_loader]`: which original it loaded, every mod it loaded or failed to load, and md5 warnings. To see them, run [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) before starting the game.

## Building

Requires Visual Studio 2026 (v145 toolset) with the C++ desktop workload, and Python 3.

1. Optional: put the game's original `GameAssembly.dll` in the folder **above** this repo, named `GameAssembly_orig.dll`.
2. Open `proxydllmodloader.vcxproj` and build **Release | x64**. Output: `x64\Release\GameAssembly.dll`.

The pre-build step runs `gen_exports.py`. It reads the export table of `GameAssembly_orig.dll` and regenerates `exports.def`, `stubs.asm` and `exports_gen.h`. If the DLL isn't there, the committed generated files are used, so the repo builds without the game installed. After a game update, put the new original in place and rebuild. You can also run the script directly:

```
python gen_exports.py path\to\GameAssembly_orig.dll
```

### How it works

- `exports.def` gives the proxy the same 386 export names and ordinals as the original.
- Each export is a one-instruction stub in `stubs.asm` (`jmp qword ptr [g_real_exports + i*8]`). Calls reach the real function with registers and stack untouched.
- `DllMain` loads the original with `LoadLibraryExW` and fills `g_real_exports` with `GetProcAddress`.
- `il2cpp_init` and `il2cpp_init_utf16` are wrapped in C++ (`dllmain.cpp`) so the mods can be loaded around runtime startup.
