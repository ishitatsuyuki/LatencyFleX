// Copyright 2022 Tatsuyuki Ishi
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define __WINESRC__
#include <stdio.h>
#include <vulkan/vk_layer.h>

// Silence keyword conflict in RegisterUserApiHook which uses the name `new` in arguments
#define new
#include <windows.h>
#undef new


// Keep this in sync with __wine_unix_call_funcs.
enum lfx_funcs {
  unix_WaitAndBeginFrame,
  unix_SetTargetFrameTime,
};

// Internal definitions copied out of the wine source tree.
// These APIs are likely unstable: copy these at your own risk. They will require changes when
// upstream modifies the mechanism.
typedef LONG NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);
typedef UINT64 unixlib_handle_t;
typedef enum _MEMORY_INFORMATION_CLASS {
  MemoryWineUnixFuncs = 1000,
} MEMORY_INFORMATION_CLASS;

static HMODULE ntdll_handle;
static unixlib_handle_t binding_handle;
typedef NTSTATUS(WINAPI *PFN_NtQueryVirtualMemory)(HANDLE, LPCVOID, MEMORY_INFORMATION_CLASS, PVOID,
                                                   SIZE_T, SIZE_T *);
static PFN_NtQueryVirtualMemory pNtQueryVirtualMemory;
typedef NTSTATUS(WINAPI *PFN___wine_unix_call)(unixlib_handle_t handle, unsigned int code,
                                               void *args);
static PFN___wine_unix_call __wine_unix_call;
#define UNIX_CALL(func, params) __wine_unix_call(binding_handle, unix_##func, params)

extern "C" VK_LAYER_EXPORT void winelfx_WaitAndBeginFrame() {
  UNIX_CALL(WaitAndBeginFrame, nullptr);
}

extern "C" VK_LAYER_EXPORT void winelfx_SetTargetFrameTime(__int64 target_frame_time) {
  UNIX_CALL(SetTargetFrameTime, &target_frame_time);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(hinst);
    ntdll_handle = GetModuleHandleA("ntdll.dll");
    pNtQueryVirtualMemory = reinterpret_cast<PFN_NtQueryVirtualMemory>(
        GetProcAddress(ntdll_handle, "NtQueryVirtualMemory"));
    NTSTATUS err = pNtQueryVirtualMemory(GetCurrentProcess(), hinst, MemoryWineUnixFuncs,
                                         &binding_handle, sizeof(binding_handle), nullptr);
    if (err) {
      fprintf(stderr, __FILE__ ": Querying MemoryWineUnixFuncs failed %lx\n", err);
      fprintf(stderr, __FILE__ ": Look for library loading errors in the log and check if "
                               "liblatencyflex_layer.so is installed on your system.\n");
      return FALSE;
    }
    __wine_unix_call =
        reinterpret_cast<PFN___wine_unix_call>(GetProcAddress(ntdll_handle, "__wine_unix_call"));
    if (!__wine_unix_call) {
      fprintf(stderr,
              __FILE__ ": Cannot find __wine_unix_call. This Wine version is likely too old\n");
    }
    break;
  }
  return TRUE;
}