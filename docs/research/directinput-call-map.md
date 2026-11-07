# Original DirectInput8 calls and module preflight

Static disassembly of the audited MoviesSE.exe (SHA256
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`).
Addresses below are original x86 VAs, not Wasm addresses. Interface names, GUIDs
and constants were checked against the [Wine 11.0 DirectInput header](https://github.com/wine-mirror/wine/blob/wine-11.0/include/dinput.h).
This is a bounded call-path audit, not evidence that input already works.

## Module-presence preflight is a filesystem existence query

Renderer preflight `9A6960` calls GetSystemDirectoryA at `9A698C`
(return address `9A6992`) with a 260-byte buffer. Zero return takes the error path.
It builds paths by appending `\\wmvcore.dll` (literal `D71C30`) and
`\\dinput8.dll`, not by calling LoadLibrary in this preflight.

- WMVCore check at `9A6A5A` is reached only if byte global `105BE88` is nonzero;
  otherwise it jumps to the DirectInput section at `9A6BF3`.
- DirectInput check is at `9A6CA2`.
- Both call `9D3660(stringObject, optionalSizeOutput=NULL)`, which passes
  `stringObject[0]` (the character pointer) to `A10220`.
- **`A10220` calls FindFirstFileA at `A1022F`** with a WIN32_FIND_DATAA-sized
  stack buffer. INVALID_HANDLE_VALUE causes GetLastError and return zero.
  Any valid handle is closed with **FindClose at `A1024A`**, then it returns 1.
  This function does **not** examine contents, signatures, size or attributes.
- If that filesystem lookup fails, `9D3660` can try helper `A10810` when byte
  global `10B9351` is set (an alternate resource lookup, not analyzed here).
- Failure displays localized component messages: WMVCore at `9A6B89`,
  DirectInput at **`9A6DD1`**. Successful dialog display does not supply the
  missing capability; the preflight then returns failure.

A browser's virtual system-directory namespace therefore needs consistent
module enumeration and later factory resolution. This is not evidence that
shipping an arbitrary empty DLL makes DirectInput implemented. When a real
built-in DirectInput bridge exists, its advertised module metadata and
LoadLibrary/GetProcAddress resolution should describe that same capability.
This existence check itself does not require loading a Windows DLL's PE bytes.

## Factory and object layout

Factory wrapper: delay IAT slot **`F87A34`**, initial thunk `AEF120`, common
delay helper `C9B296`, callable jump stub **`AEF13B`**.

Method **`A95F90`** initializes a game-owned input manager (`this` saved in EDI).
Its interface fields are:

| Manager offset | Value |
|---:|---|
| `+0C` | IDirectInput8A* |
| `+10` | Keyboard IDirectInputDevice8A* |
| `+14` | Mouse IDirectInputDevice8A* |
| `+18` | Observed first-keyboard-batch discard flag |
| `+19` | COM initialized flag |
| `+1C` onward | Keyboard DIK-offset → game-key lookup table |

The manager first calls CoInitialize(NULL) at `A95FA1` unless its initialized
flag is already set. Negative HRESULT returns without creating interfaces.
At **`A95FCB`** it calls DirectInput8Create with:

- hInstance = GetModuleHandleA(NULL), fetched at `A95FC4`;
- version **`0800`**;
- IID pointer **`D8605C`** = **`BF798030-483A-4DA2-AA99-5D64ED369700`**,
  IID_IDirectInput8A;
- output = manager `+0C`;
- outer IUnknown = NULL.

Factory failure (`HRESULT < 0`) exits initialization. Subsequent COM calls are
stdcall vtable calls with the interface pointer pushed as the first argument.
Slots below are zero based, including the three IUnknown methods.

## Interface calls actually observed

| Interface | Slot / byte offset | Method | Call sites |
|---|---|---|---|
| IDirectInput8A | 3 / `0C` | CreateDevice | `A95FE9` keyboard, `A96055` mouse |
| Device8A | 11 / `2C` | SetDataFormat | `A95FFA` keyboard, `A96066` mouse |
| Device8A | 13 / `34` | SetCooperativeLevel | `A9600B`, `A96077` |
| Device8A | 6 / `18` | SetProperty | `A9603A`, `A960A6` |
| Device8A | 7 / `1C` | Acquire | `A96042`, `A960AE`; update `A9615D`, `A96249` |
| Device8A | 10 / `28` | GetDeviceData | `A96198` keyboard, `A96274` mouse |
| Mouse / keyboard / factory | 2 / `08` | Release | `A960CD`, `A960E1`, `A960F5` |

CreateDevice GUIDs:

- `D85EEC` = **`6F1D2B61-D5A0-11CF-BFC7-444553540000`**, GUID_SysKeyboard.
- `D85EFC` = **`6F1D2B60-D5A0-11CF-BFC7-444553540000`**, GUID_SysMouse.

Both cooperative-level calls use HWND global **`105BEB0`** and flags **5**
(`DISCL_EXCLUSIVE | DISCL_FOREGROUND`). Both SetProperty calls use property
pseudo-GUID **1**, `DIPROP_BUFFERSIZE`. DIPROPDWORD is size `14` hex (20),
header size `10` hex (16), object 0, addressing mode 0 (`DIPH_DEVICE`), data
**`80` hex (128 buffered events)**. The property argument is the special small
integer identifier, not a pointer to 16 readable GUID bytes.

The audited update routine **does not call Poll or GetDeviceState**. Those
methods remain outside this proven slice, not proven absent from the entire
executable. Do not mislabel `Acquire` (`1C`) as Poll or immediate-state input.

## Exact embedded data formats

Both structures are x86 DIDATAFORMAT: six DWORDs `(size, objectSize, flags,
dataSize, objectCount, objectArray)`. Object entries are 16 bytes
`(GUID pointer, byte offset, type, flags)`.

**Keyboard format at `D84C9C`:** `(18, 10, 2, 100, 100, AEE120)`, all values
hexadecimal. Data size 256 bytes; 256 object entries. Entry n has offset n,
GUID_Key `55728220-D33C-11CF-BFC7-444553540000`, type
`8000000C | (n << 8)`, flags 0. These offsets are DirectInput scan-code indices,
not browser keyCode values. Key-down data uses bit `80` hexadecimal.

**Mouse format at `D84A94`:** `(18, 10, 2, 10, 7, AEE0B0)` hexadecimal.
Data size 16 bytes; flags 2 (`DIDF_RELAXIS`); seven objects:

| Object | Offset | GUID | Type |
|---|---:|---|---:|
| X | 0 | GUID_XAxis `A36D02E0-C9F3-11CF-BFC7-444553540000` | `00FFFF03` |
| Y | 4 | GUID_YAxis `A36D02E1-C9F3-11CF-BFC7-444553540000` | `00FFFF03` |
| Z/wheel | 8 | GUID_ZAxis `A36D02E2-C9F3-11CF-BFC7-444553540000` | `80FFFF03` |
| Button 0 | 12 | NULL | `00FFFF0C` |
| Button 1 | 13 | NULL | `00FFFF0C` |
| Button 2 | 14 | NULL | `80FFFF0C` |
| Button 3 | 15 | NULL | `80FFFF0C` |

These are the classic four-button DIMOUSESTATE offsets, not the eight-button
DIMOUSESTATE2 layout. Axis event values are relative signed deltas carried in
DWORDs; absolute canvas coordinates do not fulfill this format.

## Buffered event consumption and failure paths

Manager update **`A96130`** acquires each device then requests buffered events.
Both GetDeviceData calls pass **record size 20**, a zeroed array large enough for
128 records, in/out count initialized to **128**, and flags **0** (consume, not
peek). DIDEVICEOBJECTDATA is `(dwOfs, dwData, dwTimeStamp, dwSequence, uAppData)`;
all five fields are 32-bit in this guest ABI.

For keyboard acquisition failure, `A96168` invokes a **game-manager virtual
method at slot 4**, then skips keyboard events. This is not a DirectInput slot.
Negative GetDeviceData HRESULT sets event count zero. Manager flag `+18`
explicitly discards one keyboard batch and clears itself. The event loop reads
dwOfs, bounds it below 256, maps it through manager `+1C`, extracts dwData bit 7,
and calls game-manager virtual callbacks. The manager's slots 0/3 used here are
also **not** DirectInput interface methods.

The mouse Acquire HRESULT is not tested immediately. GetDeviceData failure
again forces count zero. Its switch (table `A962FC`, index bytes `A96310`)
handles offsets 0/4/8 as relative X/Y/Z, calls manager slot 1 with axis IDs
1/2/3 and the raw delta. Offsets 12..15 route button transitions through
manager slot 0 with game button ID `offset + 67` hexadecimal. Other offsets
are discarded. Empty queues must write count zero; success with a stale count
would fabricate input events.

Cleanup `A960C0` releases mouse, keyboard, factory in that order, clears their
pointers, then tail-calls CoUninitialize when its initialization flag is set.
No explicit Unacquire call was observed in this cleanup slice.

## First implementation boundary

Implement the factory, two devices, exact data-format/property validation,
foreground acquisition and a bounded consuming event queue before advertising
module presence. Connect the same real game HWND used by CreateDevice and the
window callback bridge. Map browser physical keys to DIK offsets; map captured
pointer movement to relative signed axis events and preserve button transitions,
focus-loss/reacquisition and queue-overflow semantics. Instrument observed HRESULTs
and queue counts before extending to Poll/GetDeviceState or other devices.

This audit covers one factory caller and its initialization/update/destruction
family. It does not establish complete alias coverage, runtime event ordering,
required queue-overflow handling, or full browser pointer-lock behavior.
