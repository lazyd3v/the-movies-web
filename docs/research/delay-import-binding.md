# Delay-import binding investigation

Status: read-only investigation; delay-IAT binding is **not implemented** by this work. Direct host binding is a proposed next step when actual host implementations of the factories exist. It does not establish graphics, input, media, or playable execution.

## Audited executable and slots

Original `MoviesSE.exe` SHA-256:
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.

PE32 image base is `0x00400000`. Data-directory index 13 has RVA `0x00A4A85C`, size 128 bytes: three 32-byte delay descriptors and a terminating zero descriptor. All three have `grAttrs = 1`, meaning address fields are RVAs.

| DLL / export | Guest delay-IAT slot | Original slot value | Descriptor VA | Module-handle storage VA |
| --- | --- | --- | --- | --- |
| WMVCore.DLL!WMCreateSyncReader | `0x00F87A3C` | `0x00C9B666` | `0x00E4A85C` | `0x010C9FF0` |
| WMVCore.DLL!WMCreateProfileManager | `0x00F87A40` | `0x00AA6035` | `0x00E4A85C` | `0x010C9FF0` |
| DINPUT8.dll!DirectInput8Create | `0x00F87A34` | `0x00AEF120` | `0x00E4A87C` | `0x010CC0E8` |
| d3d9.dll!Direct3DCreate9 | `0x00F87A48` | `0x00AFBA33` | `0x00E4A89C` | `0x010CC128` |

Descriptor fields, as stored (RVAs):

| DLL | Name | IAT | INT | Bound IAT | Unload IAT |
| --- | --- | --- | --- | --- | --- |
| WMVCore.DLL | `0x0097B460` | `0x00B87A3C` | `0x00A4A8E4` | `0x00A4A950` | zero |
| DINPUT8.dll | `0x00984CC0` | `0x00B87A34` | `0x00A4A8DC` | `0x00A4A958` | zero |
| d3d9.dll | `0x00988860` | `0x00B87A48` | `0x00A4A8F0` | `0x00A4A960` | zero |

These values were independently read using `pefile` against the checksum-identified executable. Disassembly confirmed the Direct3D path:

```text
0x009A7AE6  call 0x00AFBA4E
0x00AFBA4E  jmp dword ptr [0x00F87A48]

initial delayed target:
0x00AFBA33  mov eax, 0x00F87A48
0x00AFBA38  jmp 0x00AFBA3D
0x00AFBA3D  push ecx
0x00AFBA3E  push edx
0x00AFBA3F  push eax
0x00AFBA40  push 0x00E4A89C
0x00AFBA45  call 0x00C9B296
0x00AFBA4A  pop edx
0x00AFBA4B  pop ecx
0x00AFBA4C  jmp eax
```

## Existing runtime behavior

The inspected `.cache/recomp-full/bootstrap/runtime/runtime.c` `load_image` reads regular import directory index 1 and replaces its IAT entries with registered host-import addresses. It does not read delay directory 13. Consequently, the four slots above retain their original delay-helper targets.

`native/startup-api.c` currently recognizes the executable and KERNEL32 module in `GetModuleHandleA`; its `GetProcAddress` implementation resolves registered KERNEL32 handlers only. That is not a general Windows DLL loader.

There is a separate dispatch issue: `call_indirect` sets `g_import_target`, logs the import name, and identifies unimplemented imports. `jump_indirect` currently calls the registered function directly and skips those operations. The translated Direct3D thunk uses `jump_indirect`, so a bound factory reached through it would lack those diagnostics and could observe a stale `g_import_target`.

For import-address targets, a proposed fix is to delegate from `jump_indirect` to `call_indirect(c, target)` and return. **Do not push another guest return address.** A tail jump inherits the existing caller's return address and arguments. The host stdcall handler must consume precisely that existing frame; Direct3DCreate9 consumes one argument and the return address, advancing ESP by 8 bytes.

## Proposed implementation boundary

After the hook audit below, bind only actual implemented host factories or explicit unsupported-import traps. Do not advertise successful DLL loading or fabricate COM objects to bypass initialization.

A loader implementation should parse optional-header offset `96 + 13 * 8` only after checking the optional-header and data-directory bounds. Bound descriptor iteration by the directory size, require a terminating descriptor, reject reserved attribute bits, and support or explicitly reject the legacy VA-based form (`grAttrs & 1 == 0`). Bound DLL names, INT/IAT arrays, hint/name records, and all guest addresses against the loaded image. Handle ordinal entries deliberately. Normalize DLL case consistently with ordinary import names.

Ignore bound native DLL addresses: they are not host/Wasm function pointers. Register host slots and update only the corresponding delay-IAT entries. The current runtime has a 4096-entry import array without bounds checks in ordinary or dynamic registration; add explicit capacity checks before extending registration to delay imports.

A separate native binding helper after `load_image` could use `add_dynamic_import` for implemented handlers without changing the PE loader immediately. Unsupported entries would still need an explicit policy: leave the original delay-helper path intact, or bind an identifiable trap. Neither policy should pretend an unimplemented factory exists.

## Audit required before early binding

Early IAT binding bypasses execution of the original delay helper. Before adopting it, inspect helper `0x00C9B296`, its references, and any notification/failure-hook variables. Determine whether this game installs hooks and relies on pre-load, pre-GetProcAddress, or end-processing notifications. Their side effects must not be silently skipped.

All three descriptors have zero unload-IAT fields, but that alone does not establish that unload/reload or helper-specific bookkeeping is irrelevant. Audit explicit unload-helper calls, module-handle storage reads, bound-import validation, and any related initialization checks. Keep module-handle storage unchanged unless a real virtual-module abstraction is implemented; setting a nonzero fake HMODULE would create unsupported loader semantics. If module identity is required, implement and test that abstraction deliberately.

Also inspect any failure path that expects delay-load exceptions or a failed LoadLibrary/GetProcAddress result. Replacing such a path with an unconditional trap or eager success changes its behavior. Record the chosen unsupported behavior explicitly.

## Focused validation

- Synthetic PE fixtures: no delay directory; RVA-based and legacy VA-based descriptors; named and ordinal imports; malformed sizes, unterminated names/tables, out-of-image pointers, reserved attributes, and import-capacity exhaustion.
- Audited executable: assert exactly the four slots above are discovered; original executable bytes and guest helper code remain unchanged. Check module-handle and unload fields are preserved unless separately justified.
- ABI harness: invoke a mock implemented factory through the translated-style IAT tail jump. Verify original return address and argument, correct ESP cleanup, EAX result, preserved nonvolatile registers, and correct import name/`g_import_target`.
- Unsupported factory: verify deterministic named failure without fabricated success.
- Hook/unload fixtures when that audit finds relevant behavior; demonstrate that the adopted binding strategy preserves required notifications and state.

## Primary reference

Microsoft's [PE format: delay-load import tables](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#delay-load-import-tables-image-only) specifies descriptor fields, RVA versus legacy VA addressing, and the IAT/INT/bound/unload tables. Executable-specific addresses and runtime limitations above come from local binary/source inspection, not from that specification.

## Follow-up hook and unload audit (original executable)

The original helper at `0x00C9B296` was disassembled through its `ret 8` at `0x00C9B4D6`. It reads these globals:

| Guest address | Role | Direct reads in helper |
| --- | --- | --- |
| `0x010DAA3C` | notification hook pointer | `0x00C9B354`, `0x00C9B378`, `0x00C9B41A`, `0x00C9B4B5` |
| `0x010DAA38` | failure hook pointer | `0x00C9B3A7`, `0x00C9B47E` |
| `0x010DAA34` | unload-list head | `0x00C9B404` and related list helpers |

Notification dispatch passes codes 0 (start), 1 (pre-load), 2 (pre-GetProcAddress), and 5 (end). Failure dispatch passes codes 3 (load failure) and 4 (procedure lookup failure). Both hook pointers are tested for zero before each callback. These are real guest callbacks, not merely diagnostic hooks.

Both hook globals and the unload-list head lie in the zero-filled tail of `.data`: their offsets from the section start are `0x28DA38`, `0x28DA3C`, and `0x28DA34`; the section has only `0x13B000` raw bytes. Therefore the loader initializes them to zero.

A full original-file search for little-endian absolute references found exactly four references to `0x010DAA3C` and two to `0x010DAA38`. Disassembly identifies all six as the reads listed above. No absolute write or address-materialization reference to either hook was found. The generated code independently shows only those reads. The unload-list head does have writes, confined to the helper/list-management code. This is evidence that this executable does not install delay hooks through ordinary direct references; it is **not proof against arbitrary computed-address writes or memory corruption**.

The module-handle globals (`0x010C9FF0`, `0x010CC0E8`, `0x010CC128`) are also in the zero-filled `.data` tail. No absolute VA references to them were found in the original file. Their descriptor RVAs identify their storage, and the delay helper accesses them through the descriptor. On successful load it stores the module handle using `InterlockedExchange` at `0x00C9B3E3` and may balance a duplicate load with `FreeLibrary` at `0x00C9B414`.

All three descriptors have **both zero timestamps and zero unload-IAT fields**. The helper checks the descriptor timestamp before inspecting a loaded PE module for bound-import reuse (`0x00C9B439`); zero timestamps prevent that branch. Unload-list insertion requires a nonzero unload-IAT field (`0x00C9B3ED`), so these three descriptors cannot insert unload records through the audited helper path.

Two support routines are present:

- `0x00C9B4F5`: `__HrLoadAllImportsForDll`, which walks matching descriptor slots and invokes the helper directly at `0x00C9B58B`.
- `0x00C9B5A5`: `__FUnloadDelayLoadedDLL2`, which searches the unload list, restores the unload IAT, calls `FreeLibrary`, and clears module storage when a qualifying record exists.

A scan of executable-section bytes for direct relative CALL/JMP targets, plus an original-file search for stored absolute function addresses, found **no callers or stored pointers to either support routine**. This does not rule out computed indirect calls. The helper itself has four direct callers: the shared WMV thunk at `0x00AA6047`, DirectInput thunk at `0x00AEF132`, Direct3D thunk at `0x00AFBA45`, and the otherwise unreferenced eager-load support routine at `0x00C9B58B`.

WMCreateSyncReader's initial target `0x00C9B666` loads its slot address then jumps to `0x00AA603F`, the shared WMV helper sequence also used by WMCreateProfileManager. The factory-call thunks themselves are tail jumps through the respective IAT slots.

The original failure behavior is explicit: invalid RVA-mode raises `0xC06D0057`, LoadLibrary failure raises `0xC06D007E`, and GetProcAddress failure raises `0xC06D007F`, through the imported RaiseException. Direct binding must not be described as preserving these loader-failure paths. It replaces the supported dependency-loading boundary with host linkage; actual factory arguments, return values, and factory failures still need faithful handling.

### Conclusion and next implementation gate

For this checksum-identified executable, direct binding of **implemented** factories is a defensible, narrow static-port strategy: hooks start zero with no discovered setters, bound-import validation is inactive, and unload bookkeeping is inactive for all three descriptors. A complete Windows DLL loader is not required merely to call these host factories.

Before binding, verify the executable identity, all four names/slot locations, descriptor attributes, zero timestamps/unload fields, and zero notification/failure hooks and module storage in the loaded image. Reject an unexpected configuration rather than bypassing callbacks. A small factory-dispatch bridge can check the hook globals again when the bound factory is actually invoked, catching any later mutation instead of silently suppressing hooks. Leave original helper instructions, module-handle storage, and unload-list state unchanged.

Bind actual implemented factories; use named strict unsupported traps for the others only as an explicitly documented diagnostic policy. Do not fabricate a DLL handle, COM object, HRESULT success, or loader exception handling. Add the tail-jump import-dispatch fix and ABI tests described above before relying on the new path. A later discovery of hook installation, eager-load invocation, explicit module queries, or unload behavior must reopen this boundary and add the required virtual-module semantics.

No runtime code was changed during this audit.
