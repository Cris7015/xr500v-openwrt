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
