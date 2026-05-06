# OpenWrt mainline PoC para TP-Link Archer XR500v v1

**Fecha:** 2026-05-06
**Autor:** cristuu + Claude
**Estado:** Design — pendiente review

## Resumen ejecutivo

Lograr **prueba de concepto (PoC)** de OpenWrt mainline corriendo en un Archer XR500v v1 (SoC EcoNet EN751221), con éxito definido como: kernel arranca, llega a `procd init`, abre shell por UART. Sin presión de timeline. Si el PoC funciona, considerar contribución upstream del DTS al fork mantenido por @cjdelisle (target `econet/en751221`).

## Contexto técnico (verificado en sesiones anteriores)

### Hardware
- **SoC:** EcoNet EN751221 (MIPS 34Kc V5.8, dual-core, big-endian, MIPS32r2)
- **RAM:** 256 MB DDR3 (chips SG48001G fecha 2132)
- **NAND:** 128 MB SPI NAND (`SPI_NAND_DEVICE_ID_F50L1G`, mfr 0xc8)
- **Kernel stock:** Linux 3.18.21 (cross-built `gcc 4.9.3` con Buildroot 2015.08.1)
- **WiFi:** MediaTek MT7603 (2.4G) + MT7612 (5G), drivers `mt76` upstream
- **GPON:** OMCI propietario fuera del scope mainline (no se busca en este PoC)

### Bootloader propietario (TrendChip-derived, prompt `bldr>`)
- Comandos relevantes: `xmdm <addr_hex_sin_0x> <len>`, `flash`, `imginfo` (muestra slots `os1:os2:`), `bflag get|set <0|1>`, `go` (autoboot)
- **Hex sin prefijo `0x`** en todos los args
- **Bug confirmado**: `go` desde el prompt manualmente siempre crashea con `Undefined Exception EPC=81fb06e4` después de cualquier `set`. Workaround: cambiar `bflag` y power-cycle, dejar autoboot
- **Header del kernel partition**: 512 bytes propietario + LZMA en offset 0x200. Magic distinto al `2RDH` de OpenWrt; usa `03 00 00 03 cccc 2233...` o `03 00 00 00 ver. 2.0` (TP-Link sysupgrade variant). Validado: el bldr ACEPTA el formato sysupgrade.bin de OpenWrt cjdelisle (load + LZMA decompress OK)

### Layout MTD verificado
| MTD | Size | Label | Notas |
|---|---|---|---|
| mtd0 | 0x07000000 (112M) | ALL | Partition completa NAND |
| mtd1 | 0x40000 (256K) | boot | U-Boot/bldr |
| mtd2 | 0x40000 (256K) | romfile | Config persistente XML plain |
| mtd3 | 0x300000 (3M) | kernel | Slot A kernel |
| mtd4 | 0x1000000 (16M) | rootfs | Slot A rootfs |
| mtd5 | 0x480000 (4.5M) | misc | |
| mtd6 | 0x300000 (3M) | kernel1 | **Slot B kernel** (failsafe) |
| mtd7 | 0x1000000 (16M) | rootfs1 | **Slot B rootfs** (failsafe) |
| mtd8 | 0x4e0000 (4.9M) | others | Calibración WiFi |
| mtd9 | 0x20000 (128K) | bootflag | Selector slot A/B |

### Backup completo guardado
En `~/tools/xr500v/backup_full/` y `/mnt/c/tftp/`. Total 48 MB. md5sums en `backup_md5sums.txt`. Permite restore total partición a partición.

### Acceso operacional
- UART vía CP2102 a 115200 8N1 (3 cables: GND, TXD↔RX router, RXD↔TX router)
- Red: posible tras reactivar `ifconfig br0:1 192.168.68.99 netmask 255.255.255.0` + `/usr/sbin/telnetd -l /bin/sh -p 2323` desde shell UART
- Tftpd64 en host Windows (`C:\tftp\`) para servir/recibir archivos al router

## Objetivo del PoC

**Definición de éxito (fase final):**
1. Imagen OpenWrt mainline construida con DTS específico para XR500v
2. Imagen flasheada en slot B (mtd6 + mtd7), slot A intacto
3. Power-cycle + autoboot con `bflag=1` → kernel arranca
4. Vemos por UART: bootlog Linux completo sin crashes
5. Llegamos a prompt `root@OpenWrt:/#` o equivalente
6. `dmesg`, `cat /proc/mtd`, `ls /sys/class/net/` ejecutables

**Fuera de scope para PoC:**
- WiFi funcional (drivers mt76 cargados pero radios no necesariamente up)
- Switch/DSA configurado correctamente
- LEDs y botones mapeados
- LuCI web interface
- GPON/xDSL (totalmente fuera de scope)

**Posibles fases siguientes (no comprometidas):**
- Fase B: Ethernet usable como router cableado
- Fase C: WiFi 2.4G/5G funcional
- Fase D: PR upstream a cjdelisle/openwrt o `openwrt/openwrt`

## Arquitectura del workflow

### Build environment
**Azure VM Standard_D8s_v6** (8 vCPU, 32 GB RAM) en `centralus` o `eastus`. Ubuntu 24.04 LTS. Costo aprox. $0.40/hora pay-as-you-go (consider Spot ~$0.05-0.10/hora si la fase de iteración es larga). Setup esperado: 10-15 min para instalar dependencias OpenWrt + clonar fork de cjdelisle.

**Decisión rationale:**
- WSL 4-cores: build inicial ~45 min, iteración ~10 min — vivible pero lento
- Azure D8s_v6: build inicial ~12 min, iteración ~2 min — saca el cuello de CPU
- Vultr 32-vCPU: aún corriendo CADO-NFS factoring (15h+ uptime), no se toca

### Flash + debug local (WSL)
- VM Azure compila → `.bin/.trx` final
- `scp` desde Azure VM a WSL (~6-7 MB, segundos)
- WSL serve via Tftpd64 / TFTP nativo
- Router (firmware stock): `tftp -g` baja a `/tmp`, `mtd -f -e kernel1 write` flashea slot B
- UART vía CP2102 captura bootlog en tiempo real (picocom o `cat /dev/ttyUSB0`)

### Estrategia de iteración
1. Editar DTS / Makefile target en Azure VM (vía SSH directo, sin scp ida y vuelta)
2. `make -j8 V=s` (verbose) si hay errores
3. Imagen final → scp a WSL → flash → boot test
4. **Cada iteración ~5-10 min total**

### Recovery seguro
Slot A (firmware stock) **nunca se toca**. Si slot B no arranca:
- Power cycle 30s
- Intercept bldr con tecla rápida
- `bflag set 0`
- Power cycle otra vez sin intercept
- Bldr autoboot lee bflag=0 → arranca slot A normal

Si bldr quedó en estado raro: power cycle largo (30+s para descargar DRAM completamente).

Si todo falla: TFTP-restaurar mtd6/mtd7 desde el backup completo, vuelve al estado pre-experimento.

## Plan de fases del PoC

### Fase 0 — Setup (estimado 1 día)
- [ ] Crear Azure VM D8s_v6 en region apropiada
- [ ] Setup deps OpenWrt build (`build-essential`, `gawk`, `unzip`, etc.)
- [ ] Clone `cjdelisle/openwrt` fork
- [ ] Build inicial **sin cambios** target VR1200v v2 (validar pipeline produce imagen idéntica a la oficial)
- [ ] Verificar SSH key persistente para retomar sesiones

### Fase 1 — Crear device XR500v (estimado 1-2 días)
- [ ] `target/linux/econet/dts/en751221_tplink_archer-xr500v.dts` copiado de VR1200v v2
- [ ] Modificar `compatible`, `model`, partition layout (3MB kernel + 16MB rootfs vs 4MB+12MB del VR1200v si difiere)
- [ ] Agregar entry en `target/linux/econet/en751221/profiles/` o equivalente
- [ ] Modificar `target/linux/econet/image/Makefile` para nuevo device
- [ ] `make menuconfig` → seleccionar XR500v → `make -j8`
- [ ] Output esperado: `bin/targets/econet/en751221/openwrt-econet-en751221-tplink_archer-xr500v-squashfs-sysupgrade.bin`

### Fase 2 — Primer boot test (estimado 1 día con retries)
- [ ] scp imagen a WSL
- [ ] Extraer kernel y rootfs sections (igual al procedimiento ya validado)
- [ ] TFTP-flash a slot B
- [ ] `bflag set 1` + power cycle + autoboot
- [ ] Capturar bootlog UART
- [ ] **Resultado esperado**: kernel arranca y tira algún error específico (no crash genérico EPC=81fb06e4 que es del header)

### Fase 3 — Debug iterativo (estimado 1-7 días según severidad)
Errores comunes esperados y su diagnóstico:
- `BADVADDR=0xc0000000` o similar → DTS apunta a periférico que no existe en XR500v. Comentar líneas problemáticas.
- Kernel no monta rootfs → partition layout DTS != flash real
- `Bus error` durante init → driver intentando hardware no presente
- `mtd: error reading offset X` → partition offsets/sizes mal definidos

Estrategia: **commits pequeños y aislados**. Cada cambio de DTS = un commit. Si rompe, revert es trivial.

Aclaración sobre "días": muchos de estos pueden resolverse en horas. La estimación pesimista asume múltiples problemas en cadena.

### Fase 4 — Validación PoC (estimado < 1 día)
- [ ] Shell `root@OpenWrt:/#` accesible vía UART
- [ ] `dmesg | tail -50` sin errores fatales
- [ ] `cat /proc/mtd` muestra layout correcto
- [ ] `cat /proc/cpuinfo` muestra MIPS 34Kc V5.8 dual-core
- [ ] `ls /sys/class/net/` lista interfaces (aunque no estén up)
- [ ] `free -m` razonable (~250 MB RAM)
- [ ] Reboot funciona (kernel se reinicia limpio)
- [ ] **🏁 PoC declarado exitoso. Documentar.**

## Uso de Codex (subagent)

Triggers para delegar a Codex:
1. **Reverse engineering del header propietario del kernel** — si el bldr rechaza la imagen producida por OpenWrt y necesitamos entender qué bytes específicos espera
2. **Análisis de crash dumps MIPS** — descodificar `EPC` / `CAUSE` registros para ubicar la función exacta donde crashea (requiere disassembler MIPS)
3. **Diff semántico DTS VR1200v vs XR500v** — entender qué nodos son SoC-genérico (heredables) vs device-específico (deben adaptarse)
4. **Búsqueda profunda de docs EcoNet/TrendChip** — cuando los términos sean técnicos y necesite contexto de chips/registros

Triggers que NO requieren Codex:
- Edits mecánicos de DTS
- Setup de build env
- Operaciones de flash ya validadas
- Búsqueda general en internet

## Riesgos y mitigaciones

| Riesgo | Probabilidad | Impacto | Mitigación |
|---|---|---|---|
| Build OpenWrt falla por toolchain | Bajo | Medio | Usar Dockerfile de cjdelisle como referencia |
| Imagen producida no la acepta el bldr | Medio | Alto | Investigar header con Codex; tenemos backup; podemos bypass con `mtd -f` |
| Kernel arranca pero crashea por DTS incorrecto | Alto | Bajo | Es lo esperado, debug iterativo en Fase 3 |
| Slot A se rompe accidentalmente | Muy bajo | Crítico | Slot A nunca se toca por design. Procedimiento de flash ya validado solo escribe slot B |
| Pérdida UART durante boot | Bajo | Bajo | UART es hardware-only, no se rompe por software del router |
| Costo Azure se va de las manos | Medio | Bajo | Apagar VM cuando no se usa, considerar Spot, monitorear quincenalmente |

## Estrategia de persistencia del código

**Source of truth**: repo privado en GitHub (`Cris7015/xr500v-openwrt-poc` o nombre similar).

**Contenido del repo**:
- DTS files custom para XR500v
- Patches a aplicar sobre el fork de cjdelisle (NO el openwrt source completo)
- Build scripts custom y helpers
- Docs/notas/specs (este documento incluido)
- `.gitignore` que excluye `bin/`, `build_dir/`, `staging_dir/`, etc. de OpenWrt

**Workflow de sync**:
- Azure VM clona el repo, hace edits, commit + push a GitHub tras cada cambio significativo
- WSL local clona el mismo repo, pull periódico (manual o cron)
- Si Azure se suspende o se pierde por cualquier motivo, todo el código está en GitHub
- WSL puede continuar el build local (más lento) si Azure muere
- Reproducibilidad: con el repo + cjdelisle's fork de openwrt, cualquiera puede rebuildear

**Auth**: GitHub CLI (`gh`) autenticado vía browser flow. Token en `~/.config/gh/hosts.yml`. Para Azure VM necesitaremos repetir `gh auth login` cuando creemos la VM.

**Privacidad**: repo en privado durante desarrollo. Si el PoC funciona y decidimos contribución upstream (D), se hace cherry-pick limpio a fork público o PR a `cjdelisle/openwrt` / `openwrt/openwrt`.

## Decisiones tomadas

1. **Goal**: PoC kernel-boot (opción A) inicialmente, con posible expansión a B/C/D
2. **DTS approach**: Fork del VR1200v v2 + modify (option A)
3. **Build env**: Azure D8s_v6 en `centralus` o `eastus`
4. **Codex**: usar como subagent para tareas técnicas pesadas específicas
5. **Workflow flash**: igual al validado en sesión anterior (mtd write desde firmware stock + bflag para slot switch)
6. **Recovery**: slot A intocado, backups completos de 9 mtds disponibles
7. **No modificar Vultr** mientras CADO-NFS esté activo
8. **Code persistence**: GitHub privado como source-of-truth, sync bidireccional Azure↔WSL
