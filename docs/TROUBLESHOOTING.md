## Troubleshooting

### Proton

If the Reflex option is not available in-game, it means that the installation is not set up correctly. Make sure that you:

- Copied/symlinked `latencyflex_wine.dll` and `latencyflex_layer.dll` to **both** `lib[64]/wine/x86_64-windows/` and `pfx/drive_c/windows/system32`.
- Copied `latencyflex_layer.so` to `lib[64]/wine/x86_64-unix`. (This is different from `/usr/lib/liblatencyflex_layer.so`)
- Have a version of DXVK-NVAPI supporting LFX or have updated it to a supported version.

#### Getting Logs

Logs will provide helpful insights about what went wrong with the installation. Set `PROTON_LOG=1` and `DXVK_NVAPI_LOG_LEVEL=info` in your launch options.

The log will be created under your home directory as `proton-<appid>.log`.

#### Checking if DXVK-NVAPI is initialized

Check the log for these messages:

```
DXVK_NVAPI_ALLOW_OTHER_DRIVERS is set, reporting also GPUs with non-NVIDIA proprietary driver.
NvAPI Device: AMD RADV NAVI10 (21.99.99)
NvAPI Output: \\.\DISPLAY1
DXVK_NVAPI_DRIVER_VERSION is set to '49729', reporting driver version 497.29.
NvAPI_Initialize: OK
```

If you're not seeing this, re-check if DXVK-NVAPI is enabled and you have overrided the vendor ID and disabled nvapiHack in `dxvk.conf`.

#### Checking if the Wine bridge is loaded

Check the log for messages like this:

```
trace:loaddll:build_module Loaded L"C:\\windows\\system32\\latencyflex_layer.dll" at <address>: builtin
```

If you can't find it, recheck if you have:
- Set up a supported DXVK-NVAPI version
- Put `latencyflex_layer.dll` at **both** `lib[64]/wine/x86_64-windows/` and `pfx/drive_c/windows/system32`

#### Checking if the Wine bridge successfully initialized

If there's a log entry like this:

```
Loading latencyflex_layer.dll failed with error code: 1114
```

It's typically accompanied by a failure reason:

```
../builtin.cpp: Querying MemoryWineUnixFuncs failed c0000135
../builtin.cpp: Look for library loading errors in the log and check if liblatencyflex_layer.so is installed on your system.
```

Recheck if you have put **both** `/usr/lib/liblatencyflex_layer.so` and `lib[64]/wine/x86_64-unix/latencyflex_layer.so` correctly.