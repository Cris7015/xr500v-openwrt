# Iteration Log

Diario de qué se probó, qué pasó, qué se aprendió.

## 2026-05-06: Project bootstrap
- Repo creado, estructura inicial.

## 2026-05-06: Task 4 — Clone repos en Azure
- Clonado cjdelisle/openwrt fork + nuestro repo en Azure VM.
- ⚠️ **El soporte econet está en branch `econet-integration-tree-apr27-2026`, NO en main.**
  - `git checkout econet-integration-tree-apr27-2026` después del clone.
  - Habrá que actualizar scripts/setup para hacer el checkout automático.
- Last commit cjdelisle (en esa branch): `f3605b31fb econet: add EN751627 subtarget and Zyxel EX3301-T0 board`
- DTS disponibles para EN751221: vr1200v-v2, smartfiber_xp8421-b, nokia_g240g-e, zyxel_pmg5617ga, generic
- VR1200v v2 DTS = 128 líneas (nuestro punto de partida para XR500v)
- Feeds update + install OK

## 2026-05-06: Task 5 — Baseline VR1200v build
- Build pipeline VALIDADO en Azure VM (8 vCPU, 32GB RAM)
- Branch: `econet-integration-tree-apr27-2026`
- Imagen output: `openwrt-econet-en751221-tplink_archer-vr1200v-v2-squashfs-sysupgrade.bin`
- Tamaño build: **8,487,580 bytes (8.09 MB)**
- Tamaño referencia local vr1200v_sysupgrade.bin: 7,814,770 bytes (7.45 MB) — diferencia +672KB (8.6%), aceptable
- Sin errores en `make -j8`; warnings solo de python3-pysocks/unidecode en feed packages (benignos)
- Duración total build: ~25 min (VM clock UTC); toolchain GCC 14.3.0 + Linux 6.12.80 + packages
- Outputs adicionales: initramfs-kernel.bin (6.7MB), manifest, sha256sums, profiles.json
- ✅ Pipeline OK — listo para Task 6: crear DTS del XR500v

## 2026-05-06: Task 8 — DTS XR500v creado
- Copiado de en751221_tplink_archer-vr1200v-v2.dts
- compatible string actualizada a "tplink,archer-xr500v"
- model actualizada a "TP-Link Archer XR500v v1"
- Partition layout ajustado al layout real del XR500v (3MB kernel, 16MB rootfs)
  - boot=256K, romfile=256K, kernel=3MB, rootfs=16MB, misc=4.5MB
  - kernel1=3MB, rootfs1=16MB, others=4.9MB, bootflag=128K
  - openwrt_ubi=~62.75MB (0x3000000-0x6ec0000, con reserva BMT)
- NVMEM layout en partición misc eliminado (no conocemos offsets XR500v)
- gmac0 nvmem-cells comentado (MAC address NVMEM no mapeada aún)
- wifi@pcie0/pcie1: mantenidos con compatible=mediatek,mt76 pero sin nvmem-cells (EEPROM/MAC desconocidos)
- CPP preprocessing OK (449 líneas post-include), dtc compilación OK con 1 warning pre-existente en dtsi
- DTS commiteado: 418736a042fac128c8b715f5edacfcd0b25d5e37

## 2026-05-06: Task 9 — Image Makefile registra XR500v
- Bloque Device/tplink_archer-xr500v agregado a target/linux/econet/image/en751221.mk
- KERNEL_SIZE=3072k, IMAGE_SIZE=16384k, BLOCKSIZE=128k
- IMAGES=sysupgrade.bin con recipe: append-kernel | lzma | tclinux-trx
- TARGET_DEVICES += tplink_archer-xr500v
- Decisión de diseño: se usa tclinux-trx en lugar de tplink-v2-header porque el XR500v
  es OEM TCLinux (no firmware TP-Link nativo); TPLINK_HWID desconocido (firmware cifrado).
- Makefile dry-run (-n) OK: "Nothing to be done for all"

## 2026-05-06: Task 11 — Primer build XR500v ✅
- Imagen sysupgrade.bin: 6166960 bytes
- Imagen initramfs-kernel.bin: 5887402 bytes
- Build incremental sin errores
- Total bin/targets/econet/en751221/ tiene tanto VR1200v (baseline) como XR500v (nuestro)

## 2026-05-06: Task 12 — Verificación imagen XR500v ⚠️ CRITICAL FINDING
- Imagen tamaño: 6166960 bytes (5.88 MB)
- Header magic primeros 4 bytes: `32 52 44 48` = **'2RDH' (TRX2 format)**
- Squashfs offset: 0x2f4782 (3098498 bytes)
- LZMA offset: 0x100

### ❌ BLOCKER IDENTIFICADO:
La imagen sysupgrade.bin usa la receta `tclinux-trx` (decidida en Task 9), que genera un envoltorio **TRX2 ('2RDH')**. 
**EL BOOTLOADER RECHAZA EXPLÍCITAMENTE este formato** (notas del task original).

### Comparación con referentes:
| Imagen | Header | TRX2? | LZMA@ | Squashfs@ | Estado |
|--------|--------|-------|-------|-----------|--------|
| VR1200v sysupgrade | `03 00 00 00 'ver. 2.0'` | NO | 0x200 | 0x400000 | ✅ Aceptado por bldr |
| Stock XR500v (mtd3) | `03 00 00 03 ...` | NO | 0x200 | - | ✅ En producción |
| **NEW XR500v sysupgrade** | **'2RDH'** | **YES** | **0x100** | **0x2f4782** | **❌ RECHAZADO** |

### Acción requerida:
- Cambiar imagen recipe en `target/linux/econet/image/en751221.mk`
- Opciones investigadas en Task 9:
  - `tplink-v2-header`: usaba TPLINK_HWID (desconocido, FW cifrado)
  - `tclinux-trx`: produce TRX2 envoltorio (actual, RECHAZADO)
- Necesario: investigar qué recipe genera formato `03 00 00 ...` sin TRX2
  - Posible: cambiar a `append-kernel-lzma | append-squashfs` sin tclinux-trx wrapper
  - O encontrar recipe del VR1200v que generó su formato aceptado
