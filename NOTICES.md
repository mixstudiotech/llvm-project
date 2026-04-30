# LLDB iOS Debugging Notes

Date: 2026-05-01

## Environment

- Host repo: `F:\llvm-project`
- LLDB binary: `F:\llvm-project\.build\x64\bin\lldb.exe`
- Test app executable: `C:\Users\Station\AppData\Local\ycode\DerivedData\test\Build\Products\Debug-iphoneos\test.app\test`
- Test app dSYM DWARF: `C:\Users\Station\AppData\Local\ycode\DerivedData\test\Build\Products\Debug-iphoneos\test.app.dSYM\Contents\Resources\DWARF\test`
- Device support cache: `C:\Users\Station\AppData\Local\Mix\DeviceSupport\00008112-000125440E45401E_26.3`
- Device UDID used for tests: `00008112-000125440E45401E`
- Test process: `test`, pid `1656`
- Fast attach env: `LLDB_MIXDEV_FAST_ATTACH=1`

## Fast Attach Changes

The current fast attach path is guarded by `LLDB_MIXDEV_FAST_ATTACH=1`.

Changed files:

- `lldb/source/Plugins/Process/gdb-remote/ProcessGDBRemote.cpp`
- `lldb/source/Plugins/DynamicLoader/MacOSX-DYLD/DynamicLoaderDarwin.cpp`
- `lldb/source/Plugins/DynamicLoader/MacOSX-DYLD/DynamicLoaderMacOS.cpp`

Implemented optimizations:

- Use fallback ARM64/ARM64e register definitions when target XML cannot be parsed, avoiding the slow `qRegisterInfo0...` scan.
- Skip remote `qSymbol` lookup serving during fast attach.
- Skip `qStructuredDataPlugins` probing during fast attach.
- Use local architecture signal definitions instead of querying `jSignalsInfo`.
- Skip dyld notification breakpoint setup and repeated dynamic-loader retry work during fast attach.
- Request `jGetLoadedDynamicLibrariesInfos` with `report_load_commands:false`.
- Load only the main executable module from the minimal image list and set its ASLR slide manually.
- Preserve source/symbol breakpoint support by restoring the executable module and setting its load address.

Observed attach-only timing on the same test process:

- Original baseline before optimizations: about `12229ms`
- After register fallback: about `5622ms`
- After skipping qSymbol / structured data / dyld notification work: about `3583ms`
- After minimal image-list path: about `2687ms`

Packet behavior after the latest optimization:

- `qRegisterInfo_count=0`
- `qSymbol_count=0`
- `jGetLoadedDynamicLibrariesInfos_count=1`
- The remaining image-list request includes `report_load_commands:false`
- Attach-only packet count observed: `send_packets=38`

## Smoke Test Results

Verified as working:

- `platform select remote-ios`
- `platform connect ios://00008112-000125440E45401E`
- `target create` for the local iOS app executable
- `target symbols add` for the app dSYM
- device support search path mapping for `/private/preboot/Cryptexes/OS`, `/System`, and `/usr`
- `process attach --pid 1656`
- `process detach`
- main executable slide: `test` loaded at `0x0000000100490000`
- thread listing
- register read for `pc` and `sp`
- backtrace on the current stopped thread
- memory read at the current PC
- C++ expression evaluation, for example `expression -l c++ -- 1 + 2`
- instruction stepping with `thread step-inst`
- Swift symbol lookup by address for `Renderer.updateGameState`
- Swift symbol breakpoint resolution at `0x000000010049a4cc`
- software breakpoint insertion and removal via `Z0` / `z0`

Representative verified values:

- Current stop PC during attach tests: `0x0000000234d24cd4`
- Single-step result PC: `0x0000000234d282f8`
- `Renderer.updateGameState` resolved address: `0x000000010049a4cc`
- `expression -l c++ -- 1 + 2` returned `(int) $0 = 3`

## Known Limitations

- `platform process list` returned no processes on the `remote-ios` platform in this test environment.
- Fast attach intentionally avoids building full LLDB modules for system dylibs. This is a speed tradeoff.
- `frame variable` produced no local variables while stopped in a system frame. It still needs validation after stopping inside app source.
- A real hit of the `Renderer.updateGameState` breakpoint was previously observed, but the latest run did not hit it within the timeout because the app did not appear to execute that render/update path during the test window.

## Useful LLDB Command Shape

```powershell
$env:LLDB_MIXDEV_FAST_ATTACH='1'
$lldb='F:\llvm-project\.build\x64\bin\lldb.exe'
$exe=Join-Path $env:LOCALAPPDATA 'ycode\DerivedData\test\Build\Products\Debug-iphoneos\test.app\test'
$dwarf=Join-Path $env:LOCALAPPDATA 'ycode\DerivedData\test\Build\Products\Debug-iphoneos\test.app.dSYM\Contents\Resources\DWARF\test'
$root=Join-Path $env:LOCALAPPDATA 'Mix\DeviceSupport\00008112-000125440E45401E_26.3'

& $lldb --batch `
  -o 'settings set target.memory-module-load-level minimal' `
  -o 'settings set symbols.load-on-demand true' `
  -o 'settings set symbols.enable-external-lookup false' `
  -o 'platform select remote-ios' `
  -o 'platform connect ios://00008112-000125440E45401E' `
  -o "target create `"$exe`"" `
  -o "target symbols add `"$dwarf`"" `
  -o "target modules search-paths add /private/preboot/Cryptexes/OS `"$(Join-Path $root 'private\preboot\Cryptexes\OS')`"" `
  -o "target modules search-paths add /System `"$(Join-Path $root 'System')`"" `
  -o "target modules search-paths add /usr `"$(Join-Path $root 'usr')`"" `
  -o 'process attach --pid 1656' `
  -o 'thread list' `
  -o 'register read pc sp' `
  -o 'process detach'
```
