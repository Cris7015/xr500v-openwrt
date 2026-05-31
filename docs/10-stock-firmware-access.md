# Stock firmware access & security notes

> Scope: this page documents the **stock (OEM) firmware** that ships in slot A of the owner's Archer XR500v, as observed by reverse-engineering the device the owner physically possesses. It is reference material for understanding how the factory image is built, how it authenticates console/management access, and how it validates firmware uploads — the background needed to replace it with [OpenWrt](11-openwrt-port-build-persistence.md) without bricking the unit. Everything below was recovered from the device's own flash dumps and from disassembly of the OEM binaries; nothing here is theoretical. The single most reusable result is the web-upload signature scheme in §6: reverse-engineering the OEM updater's MD5+salt firmware check makes it possible to install a custom image through the factory web UI, no telnet or UART required.

The short version: the "TP-Link Archer XR500v v1" is not a TP-Link software platform. It is a generic **TCLinux / TrendChip Home Gateway (HGW)** image — the same OEM codebase China Telecom and other Asian carriers deploy — running on an EcoNet/Airoha **EN751221** SoC (MIPS 24Kc, big-endian), kernel `Linux 3.18.21`, with a TP-Link login skin bolted on top. The underlying HGW firmware is intact: its plaintext XML config (`romfile`), its default/undocumented operator service accounts, its restricted-CLI-to-root path, and its MD5+salt firmware-signature check are all standard TrendChip mechanisms that this page describes for the owner's own device. See also [OEM platform & hardware overview](02-hardware-chip-inventory.md) and [Boot, partitions & flashing](03-boot-partitions-flashing.md).

## 1. What the stock firmware actually is

Identified by reading the stock config block (`romfile`, recovered from flash):

```xml
<Entry Vendor="TC" ProductName="HGW" SWVer="TCLinux Fw 7.1.2.7" FWVer="3.10.0.24"
       PubDate="2010/11/11" BatchNum="STC20P0.012.310024" SerialNum="<unit-serial>"
       HWVer="HardWare Ver.0001" GateWay="Family GateWay" Model="HGW"/>
```

| Field | Value | Meaning |
|---|---|---|
| `Vendor` | `TC` | TrendChip (the TCLinux SDK vendor) |
| `ProductName` / `Model` | `HGW` | generic "Home GateWay", not "XR500v" |
| `SWVer` | `TCLinux Fw 7.1.2.7` | TrendChip firmware build string |
| `FWVer` | `3.10.0.24` | OEM firmware version |
| Kernel | `Linux version 3.18.21 ... Buildroot 2015.08.1 #96 Mon Aug 16 2021` | EcoNet 3.18 vendor kernel |
| SoC | EcoNet/Airoha EN751221 | MIPS 24Kc big-endian |

Corroborating tells inside the config: default DNS `168.95.1.1 / 168.95.1.2` (HiNet, Taiwan), default SSIDs `ChinaNet-AP1` / `ChinaNet_AP_5G`, `Country="CN"`, a TR-069 ACS pointed at `http://devacs.edatahome.com:9090/ACS-server/ACS`, and DHCP vendor classes for `HGW`/`HGW-CT` (China Telecom Home Gateway). TP-Link rebadged this for retail and overwrote only the web-login credential layer; the rest of the HGW stack was shipped as-is.

This matters for two practical reasons:
1. The firmware-image format and signature check are TrendChip's, **not** TP-Link's `tplink-v2`/`tclinux-trx` standard formats — which is why naïve OpenWrt images are rejected by the OEM updater (see §6).
2. The operator management accounts and services that ship in the `romfile` remain defined and, for several of them, active — so understanding them is part of securing the device.

## 2. The `romfile` config format

Stock configuration lives in its own flash partition (see [Boot, partitions & flashing](03-boot-partitions-flashing.md)) as a single **plaintext XML document** rooted at `<ROMFILE>` (≈500 lines in the recovered dump). It is not encrypted or signed — it can be read directly from a flash dump. Each subsystem is a child element (`<Wan>`, `<Lan>`, `<Account>`, `<Wlan*>`, `<VoIP>`, `<TR069>`, `<Sys>`, …) with attributes as key/value config.

The config orchestrator daemon (`cfg_manager` / `cos`) parses this file at boot and applies it. One observed hazard:

> Running `/userfs/bin/cfg_manager show` on the live stock unit **rewrote the romfile with factory defaults** ("Romfile format is wrong, we use default romfile to replace current setting romfile"). Do not invoke `cfg_manager` on the stock OEM firmware unless you intend to reset config. Always work from a **dump** of the partition, not the live tool.

Because the format is plaintext, the entire device configuration — including credentials, SIP secrets, WPA keys, and TR-069 ACS credentials — is recoverable by anyone who can read flash (e.g. via the root path in §4). Treat the `romfile` as sensitive.

## 3. Default credentials & the operator management accounts

The `<Account>` block in the stock `romfile` defines three web accounts plus telnet/FTP/console entries. These are the operator HGW defaults; they were present on the owner's retail unit. Passwords are shown as placeholders below — the literal values are vendor defaults and should be changed:

```xml
<Account>
  <Entry0 Active="Yes" username="telecomadmin" web_passwd="<...>" display_mask="FF FF FF FF FF FF FF FF FF" />
  <Entry1 Active="Yes" username="useradmin"    web_passwd="<...>" display_mask="BF 00 0F 08 07 20 03 00 01" />
  <Entry2 Active="Yes" username="user3"        web_passwd="<...>" display_mask="BF 00 07 08 07 10 03 00 01" />
  <TelnetEntry  Active="No"  telnet_username="admin"  telnet_passwd="<...>"  telnet_port="23" />
  <FtpEntry     Active="No"  ftp_username="admin"     ftp_passwd="<...>"     ftp_port="21" />
  <ConsoleEntry Active="Yes" console_username="admin" console_passwd="<...>" />
  <CTDefParaEntry setDefValueFlag="1" />
</Account>
```

| Account | Active | Notes |
|---|---|---|
| `telecomadmin` | Yes | Operator "super-admin" (`display_mask` all-`FF` = full privilege). Ships with a well-known carrier-HGW default password. |
| `useradmin` | Yes | Restricted subscriber admin (limited `display_mask`); vendor-default password. |
| `user3` | Yes | Further restricted account; vendor-default password. |
| telnet `admin` | **No** | Telnet account defined but disabled by default. |
| FTP `admin` | **No** | FTP account defined but disabled by default. |
| console `admin` | Yes | Serial console (UART) login, enabled. Factory-default credentials are `admin` / `1234` (the standard TrendChip HGW default) → drops into the restricted CLI → root via the §4 `reg read` injection. |

The `display_mask` is a TrendChip privilege bitfield: the all-`FF` mask on `telecomadmin` grants every management page, while the subscriber accounts get a reduced set. There is also a separate `<deviceAccount>` block (operator auto-registration credentials, empty on this unit).

### What TP-Link changed
TP-Link's retail layer puts its **own** web login on top — on the owner's device the web admin password is a user-set value (the owner set `<router-password>`). That login uses an RSA-wrapped flow rather than the raw HGW account check:

```
POST /cgi/getParm                       # fetch RSA public key + sequence
POST /cgi/login?UserName=<RSA(admin)>&Passwd=<RSA(base64(pwd))>&Action=1&LoginStatus=0
     Header: TokenID: 0
GET  /index.htm                         # post-login page returns the real session token
```

The security note worth keeping: **TP-Link overrode the front-end login, but the underlying HGW accounts and services in the `romfile` remain defined and, for several of them, active.** The default operator service accounts (`telecomadmin`, `useradmin`) live in the config and in OEM binaries regardless of what password the owner sets in the TP-Link UI, and they carry vendor-default passwords. Anyone running one of these units should change those credentials; they cannot be assumed unknown.

## 4. The restricted CLI and the root path

The stock firmware exposes a TrendChip-style **restricted management CLI** (prompt `TP-LINK(conf)#`) over the standard telnet/console path. It is not a shell — it is a menu of typed command modules:

```
dev  reg  i2c  usb  wan  lan  wlctl  voip  igmp  factory  txctl
```

Login is password-only (the username is hardcoded `admin` internally). Empirically observed protection: **five failed attempts triggers a ~10-minute lockout**, so the CLI is not trivially brute-forceable online.

### Reaching a root shell from the restricted CLI
The `reg` module's `read` subcommand internally calls `system("devmem ...")` via the firmware helper `util_execSystem()`. On this build the `devmem` binary does not exist, so the invoked command **fails**. `util_execSystem()` passes the user-supplied argument into the shell and only blacklists the semicolon `;` — it does **not** filter `||`, `&&`, `|`, backticks, or `$()`. That makes the failing `reg read` the left side of a shell OR, and the right side runs as **root**:

```
reg read 0 || /bin/cat /proc/version
```

Observed output (root context, on the device's own firmware):

```
/bin/sh: devmem: not found
Linux version 3.18.21 (root@...) (gcc 4.9.3, Buildroot 2015.08.1) #96 SMP Mon Aug 16 02:03:53 UTC 2021
```

Notes on the filter, confirmed by fuzzing the CLI on the owner's device:
- `;` and `$(...)` injected into a *quoted* argument are rejected with `"[ util_execSystem ] 190: Parameter contains illegal character!"`.
- `||` and `&&` are **not** filtered, so the OR-fallback works.
- Invoked commands must use **absolute paths** (`/bin/cat`, `/bin/ls`) because the failing branch leaves no useful `PATH`.

The root filesystem confirms how weak the on-box account hashing is. `/bin/cat /etc/passwd` returns:

```
admin:$1$$iC.dUsGpxNNJGeOm1dFio/:0:0:root:/:/bin/sh
nobody:*:99:99:nobody:/:/bin/sh
guest:*:0:0:guest:/var/usbdisk/:/bin/sh
```

`admin` is UID 0 (root) with an MD5-crypt (`$1$`) hash and an **empty salt** — and note `guest` is also GID/UID 0.

### Persistent unauthenticated shell
For RE work the owner stood up a second, unauthenticated root shell so the injection didn't have to be re-run every session:

```
telnetd -l /bin/sh -p 2323     # root shell, no auth, port 2323
```

This `:2323` shell persists until the device reboots. It is the workhorse used for everything in [Boot, partitions & flashing](03-boot-partitions-flashing.md): dumping partitions, and **flashing slot B**. It is, of course, an unauthenticated root service — it exists only because the owner created it on their own hardware for development, and is not part of stock behavior.

The stock firmware also runs **anonymous FTP** on `:21`, jailed to attached USB storage (`/var/vsftp/root/anonymous/...`), which is convenient for moving dump files but is another open service to be aware of.

## 5. Security summary of the stock image

Documented plainly, the factory firmware on this unit has the following exposure characteristics (all verified on the owner's device):

- **Plaintext, unsigned config** — the entire `romfile` (credentials, WPA keys, SIP/TR-069 secrets) is readable XML in flash.
- **Default operator service accounts** — `telecomadmin` and `useradmin` ship with vendor-default passwords baked into the OEM config and binaries; the TP-Link UI password does not remove them, and they should be changed.
- **Restricted-CLI → root** via an unfiltered `||` in `util_execSystem()` behind `reg read`.
- **Weak root hash** — `admin` is UID 0 with empty-salt MD5-crypt; a second UID-0 `guest` account exists.
- **Operator services on by default** — the TR-069 management client points at an external ACS; anonymous FTP is enabled.
- **Old kernel** — Linux 3.18.21 (2021 build) on a 2015 Buildroot toolchain.

None of this is unique to TP-Link's labeling — it is the standard posture of the TrendChip/TCLinux HGW platform. It is part of why replacing the stock image with [OpenWrt](11-openwrt-port-build-persistence.md) is worthwhile, and it is the reverse-engineering surface that made that port possible in the first place.

## 6. Firmware image format & the web-upload signature check

This is the most reusable result on the page. The OEM web updater (`/firmware-upgrade`, served by the `httpd` binary, which calls `rdp_verifyFirmware`) validates the uploaded image **before** writing it. Reverse-engineering that check is what allows an OpenWrt image to be installed through the web UI instead of requiring the telnet/UART path — a meaningfully more complete result than simply observing that custom uploads are rejected.

### Header layout
The stock kernel partition begins with a **512-byte header**. Unlike the standard TP-Link `tplink-v2` header (`03 00 00 00` + `"ver. 2.0"`) or OpenWrt's `tclinux-trx` (`2RDH` magic), the XR500v uses a TrendChip/HGW variant whose first bytes are `03 00 00 03`:

| Offset | Size | Field | Stock value |
|---|---|---|---|
| `0x00` | 4 | header magic | `03 00 00 03` |
| `0x04` | 48 | test/filler pattern (3×16 B `cc..ff`) | `cc cc 22 33 … aa bb cc dd ee ff` |
| `0x34` | 4 | `SW_VER_ID` | `01 b3 18 01` (encodes `7.1.2.7`; upgrade requires ≥ this) |
| `0x38` | 4 | `HW_REV` | `00 05 00 09` |
| `0x3c` | 4 | `HWID` | `00 00 00 01` |
| `0x40` | 16 | **MD5** (salted, over whole file) | `3c bc 67 8a 93 fe 98 f0 10 d3 e5 ba ad 34 9c 2a` |
| `0x50` | 4 | unused | `00 00 00 00` |
| `0x54` | 4 | inner magic | `55 aa 55 aa` |
| `0x58` | 8 | `sig` (not actually checked) | `f1 e2 d3 c4 e5 a6 6a 5e` |
| `0x60`+ | — | TrendChip section (decompress addr, kernel entry, sizes, sub-magic `55 aa 01 01`) | — |

The real hardware identifiers, read from the stock kernel header: **HWID = `0x00000001`**, **HW_REV = `0x00050009`**, **SW_VER_ID = `0x01b31801`**. (Note: the OpenWrt image recipe uses a *dummy* `TPLINK_HWID := 0x0ec60001` for the `tplink-v2` header it builds; the genuine values above are what the OEM web check expects.)

### The MD5 + salt check
The verification routine `rsl_sys_verifyFirmware` (in `libcmm.so`, at file offset `0x1e380`) implements a self-referential salted MD5. Reconstructed from disassembly:

```python
# pseudocode of rsl_sys_verifyFirmware
salt = SALT_A if fw[0x88] == 0 else SALT_B   # select salt by a flag byte at 0x88
stored_md5 = fw[0x40:0x50]                    # save the header's stored digest
fw[0x40:0x50] = salt                          # overwrite the digest field with the salt, in place
computed = md5(fw[0 : fw_size])               # MD5 over the whole file (now with salt in the hash slot)
assert computed == stored_md5                 # must match the value that was stored at 0x40
```

The two salts are constants in the library's `.rodata` (recovered from `libcmm.so`):

| Salt | Selector | Value (hex) | Lib file offset |
|---|---|---|---|
| `SALT_A` | used when `fw[0x88] == 0` | `dcd73aa5c39598fbdcf9e7f40eae4737` | `0xb6e40` |
| `SALT_B` | used when `fw[0x88] != 0` | `8cef335fd5c5cefaac9c28dab2e90f42` | `0xb6e50` |

The `SALT_A` path is the one observed end-to-end (the image built and flashed in §below had `fw[0x88] == 0`). The `SALT_B` trigger condition is inferred from the disassembly of the selector, not exercised on the device.

> These salts and the algorithm are legitimate reverse-engineering of the owner's own device. The "key" is a constant compiled into the firmware's own shipped library, not a third-party secret, so publishing it here is appropriate.

The scheme is **integrity-only, not authenticity** — it is a keyed checksum where the "key" (the salt) ships in cleartext inside the firmware's own library. There is no asymmetric signature; the 8-byte `sig` field at `0x58` is not consulted. So once the salt is known, a correctly-formatted image with a recomputed MD5 passes. Before this was understood, custom images were rejected with `Error code: 4503 = CMM_UPDATE_FILE_VER_ERR` (defined in the stock `/web/locale/en_US/err.js`).

### Producing a web-acceptable OpenWrt image
This is what lets the owner flash OpenWrt through the stock web UI rather than over telnet/UART. The build steps, assuming `fw[0x88] == 0` (so `SALT_A`):

```python
import hashlib
# 1. Build the OpenWrt sysupgrade.bin (kernel + squashfs).
# 2. Run patch_trendchip_header.py to set the TrendChip fields at 0x60+.
# 3. Overwrite bytes 0x00..0x5F with the stock kernel header (copy mtd kernel[:0x60]).
# 4. Optionally set fw[0x34]=SW_VER_ID, fw[0x38]=HW_REV, fw[0x3c]=HWID.
fw[0x40:0x50] = bytes.fromhex("dcd73aa5c39598fbdcf9e7f40eae4737")  # SALT_A in the digest slot
fw[0x40:0x50] = hashlib.md5(bytes(fw)).digest()                    # then store the real MD5
# 5. Upload via http://<router-ip>/firmware-upgrade — accepted; device flashes itself.
```

With that header, the OEM updater accepts a custom (OpenWrt) image and flashes it. This was confirmed working on the owner's device with an OpenWrt build. It removes the dependency on the telnet `:2323` flashing path for the common case — though that path remains the recommended recovery channel.

> Caveat — which channel to flash from: writing **slot B from running OpenWrt** corrupts the NAND (the mainline `en75_bmt` driver lacks the OEM bad-block table, so writes silently mis-map). Production-quality slot-B flashing must be done from the **stock** telnet `:2323` shell or via the verified web-upload above, not from a live OpenWrt session. See [Boot, partitions & flashing](03-boot-partitions-flashing.md) for the full flashing and recovery procedure and the `bflag` slot-selection mechanism.

## 7. Reverse-engineering workflow (for similar HGW/TC3162 devices)

The method used here generalizes to other TrendChip/TCLinux gateways:

1. Locate the web server binary via `/proc/<pid>/exe`; identify its shared libs (`libcmm.so` is the HGW config/verification core).
2. Search the libs for symbols `rdp_verifyFirmware`, `rsl_sys_verifyFirmware`, `cen_md5*`.
3. Disassemble `rsl_sys_verifyFirmware` (capstone + pyelftools).
4. Trace `gp`-relative `.rodata` loads to compute the salt addresses; extract the salt constants.
5. Replicate the salt-selection + in-place-overwrite + MD5 algorithm to forge a passing header.
6. Read the plaintext `romfile` from a flash dump for accounts/config; reach root via the restricted-CLI `||` injection for live inspection.

## Reference binaries recovered

For provenance, the following stock artifacts back the facts above (owner's device, dumped via the root path):

- `~/tools/xr500v/mtd/romfile.txt` — the plaintext OEM XML config (accounts, version block, services).
- `~/tools/xr500v/mtd/libcmm.so` — contains `rsl_sys_verifyFirmware` and both salts.
- `~/tools/xr500v/mtd/httpd` — web server that calls `rdp_verifyFirmware`.
- `~/tools/xr500v/backup_full/mtd3_kernel.bin` — stock kernel header reference (bytes `0x00..0x5F`).
- `~/tools/xr500v/cli_inj.log`, `cli_rce.log`, `cli_fuzz.log` — empirical CLI filter / root-injection / `/etc/passwd` evidence.

## Cross-references
- [OEM platform & hardware overview](02-hardware-chip-inventory.md) — EN751221 SoC and the "rebadged HGW" identification.
- [Boot, partitions & flashing](03-boot-partitions-flashing.md) — partitions, `bflag` A/B selection, the safe slot-B flashing procedure, and UART/TFTP recovery.
- [OpenWrt port, build & persistence](11-openwrt-port-build-persistence.md) — what replaces this firmware, and why the signature/flashing notes here matter for installation.