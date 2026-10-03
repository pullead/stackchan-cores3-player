# Running the host tests

`firmware/tests` is a standalone CMake project: the tests need a C++17 compiler
and nothing from ESP-IDF.  They are the fastest way to check the media core, and
they cover the parts that are hard to judge on a device (parsers, hashing,
play-mode policy, controller state).

## On this machine

There is no compiler on `PATH`, but two usable ones exist:

- a portable MSVC 14.44 in `C:\Users\tei_s\Documents\Nihongo ToastFish\.local\BuildTools`,
- the system Windows SDK in `C:\Program Files (x86)\Windows Kits\10`.

CMake and Ninja come from the ESP-IDF tools, so nothing has to be installed.
The whole suite builds and runs in a few seconds:

```powershell
$bt   = "C:\Users\tei_s\Documents\Nihongo ToastFish\.local\BuildTools"
$msvc = "$bt\VC\Tools\MSVC\14.44.35207"
$sdk  = "C:\Program Files (x86)\Windows Kits\10"
$ver  = "10.0.26100.0"

$env:INCLUDE = "$msvc\include;$sdk\Include\$ver\ucrt;$sdk\Include\$ver\um;$sdk\Include\$ver\shared"
$env:LIB     = "$msvc\lib\x64;$sdk\Lib\$ver\ucrt\x64;$sdk\Lib\$ver\um\x64"
$env:PATH    = "$msvc\bin\HostX64\x64;$sdk\bin\$ver\x64;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;$env:PATH"

$src = "<worktree>\firmware\tests"          # e.g. C:\sc\firmware\tests
$bld = "$src\build-host-verify"

cmake -S $src -B $bld -G Ninja `
  -DCMAKE_MAKE_PROGRAM="C:\Espressif\tools\ninja\1.12.1\ninja.exe" `
  -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_BUILD_TYPE=Debug `
  "-DCMAKE_CXX_FLAGS_DEBUG=/Z7 /Ob0 /Od"
cmake --build $bld
ctest --test-dir $bld --output-on-failure
```

Notes:

- Debug flags are overridden to `/Z7 /Ob0 /Od`: `NDEBUG` must stay undefined or
  the `assert`-based checks silently disappear, and `/Z7` keeps the debug info in
  the objects instead of a PDB.
- `fatfs_config_test` reads `../sdkconfig`, which is generated and therefore
  absent from a fresh worktree.  Copy it from a configured tree, or run
  `idf.py reconfigure` first.
- Under the DSH sandbox, CMake has to be allowed to spawn the compiler and
  capture its output; without that the configure step hangs.

## Layout

Each test is its own executable with a `check()` helper (the newer files) or
plain `assert` (the older ones), registered by `add_test` in
`firmware/tests/CMakeLists.txt`.  Nothing here links against ESP-IDF, so a test
that needs hardware behaviour is a signal that the code under test has the wrong
seam.
