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
