# Kernel panic RESOLVED - BQL race before QDMA doorbell (2026-06-24)

**TL;DR:** el panic no era el SLIC/PCM ni "la llamada" en si. La llamada VoIP/RTP
solo hacia mas probable el timing. El crash era un `BUG_ON()` de BQL/DQL:
`dql_completed()` recibia una completion de TX antes de que el driver hubiera
contabilizado el enqueue con `netdev_tx_sent_queue()`.

**Fix:** patch `397-bql-account-before-qdma-doorbell.patch`. En `econet_eth`, el
driver ahora guarda `txq`/`bql_len` en el descriptor software, llama
`netdev_tx_sent_queue()` antes de tocar el doorbell QDMA (`tx_cpui`), completa BQL
contra el `txq` guardado y, si el descriptor llena el ring, para la cola antes
del doorbell para cerrar el race wake-before-stop.

**Resultado en HW:** desplegado como KO overlay en el XR500v (`192.168.68.222`),
validado con telefono + Ethernet + speedtests + cambios HWNAT/SWNAT/none. Mas de
44 minutos en llamada sin corte ni panic.

## Sintoma

El router reiniciaba con:

```text
Kernel bug detected[#1]
CPU: 0 PID: 2133 Comm: napi/qdma0_eth-
epc: 8142ac80
ra: 83694d28 [econet_eth@...+0xe000]
Kernel panic - not syncing: Fatal exception in interrupt
```

La traza caia desde el thread NAPI de TX completion de QDMA (`napi/qdma0_eth-`).
El `epc` simboliza a:

```text
lib/dynamic_queue_limits.c: dql_completed()
BUG_ON(count > num_queued - dql->num_completed)
```

El `ra` del modulo cae en el completion path de `econet_eth`, alrededor de
`en75_poll_tx_complete()`, justo donde se llamaba:

```c
netdev_tx_completed_queue(txq, 1, skb->len);
```

## Causa raiz

El orden viejo era:

1. `en75_dev_xmit()` preparaba el skb.
2. `en75_qdma_xmit()` posteaba el descriptor y tocaba el doorbell QDMA.
3. Recién despues, `en75_dev_xmit()` llamaba `netdev_tx_sent_queue(txq, len)`.

En paquetes chicos y frecuentes, el hardware puede completar muy rapido. Entonces
el NAPI de TX done podia correr entre los pasos 2 y 3:

1. QDMA recibe descriptor.
2. QDMA completa el descriptor.
3. `en75_poll_tx_complete()` llama `netdev_tx_completed_queue()`.
4. BQL/DQL ve bytes completados que todavia no estaban registrados como enviados.
5. `dql_completed()` dispara el `BUG_ON()`.

Por eso el bug parecia aparecer "en llamada": RTP genera muchos paquetes chicos,
y al mezclar Ethernet/speedtest/offload se ejercita justo el camino de completions
rapidas. El subsistema VoIP no era la causa primaria.

Tambien habia un race companero: cuando el descriptor llenaba el ring, la cola se
paraba despues del doorbell. Una completion extremadamente rapida podia despertar
antes de que la cola quedara parada.

## Cambio aplicado

Patch en ambos arboles:

- `/home/cristuu/openwrt/package/kernel/econet-eth/patches/397-bql-account-before-qdma-doorbell.patch`
- `/home/cristuu/xr500v-openwrt/package/kernel/econet-eth/patches/397-bql-account-before-qdma-doorbell.patch`

Cambios importantes:

- `en75_qdma_xmit()` ahora recibe `struct netdev_queue *txq` y `bql_len`.
- `struct en75_q_tx_ent` guarda `txq`, `wake_dev` y `bql_len` junto al skb.
- Se llama `netdev_tx_sent_queue(txq, bql_len)` antes de `en75_wreg(...tx_cpui)`.
- Si el descriptor consume el ultimo slot libre, `netif_tx_stop_queue(txq)` ocurre
  antes del doorbell.
- El completion usa el `txq`/`bql_len` guardado, no recalcula desde `skb->dev`.
- Los usuarios internos sin enqueue BQL, como el probe WHNAT sintetico, llaman
  `en75_qdma_xmit(..., NULL, 0)` y quedan fuera de BQL.

## Build y deploy usado

Se reconstruyo solo `kmod-econet-eth`:

```sh
make package/kernel/econet-eth/clean package/kernel/econet-eth/compile V=s -j$(nproc)
```

Artefacto desplegado:

```text
/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/econet-eth-2026.02.13~c2f855cf/ipkg-mips_24kc/kmod-econet-eth/lib/modules/6.12.80/econet-eth.ko
md5: 4e75e1f4a449a2cfcdbe94fa46501e9b
vermagic: 6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

En el router se copio sobre el KO overlay:

```text
/lib/modules/6.12.80/econet-eth.ko
```

Backup antes de reemplazar:

```text
/root/module-backups/econet-eth.ko.pre-bql-397.20260624-164233
```

El router ya usa `/etc/init.d/econet-swap`, que descarga la pila ROM y carga el
KO de `/lib/modules/6.12.80/econet-eth.ko` antes de levantar red. No se hizo
`sysupgrade` para esta prueba.

## Validacion

Despues del reboot:

- `/lib/modules/6.12.80/econet-eth.ko` tenia md5 `4e75e1f4a449a2cfcdbe94fa46501e9b`.
- `/rom/lib/modules/6.12.80/econet-eth.ko` seguia siendo el KO viejo, md5
  `d0437e8d5c7ca92502bded1ea19c1311`.
- `lsmod` mostraba `econet_eth`.
- `ip -br link` mostraba `eth0`, `lan1..lan4`, `br-lan`, con `br-lan` activo.
- `dmesg` no mostro `BUG`, `panic` ni nuevos errores DQL.
- Ping adaptivo desde router a PC (`192.168.68.248`) con `3000/3000`, 0% loss.
- Test real del usuario: telefono + Ethernet + speedtests + cambios entre HWNAT,
  SWNAT y none; mas de 44 minutos en llamada estable.

## Rollback

Si hay que volver al KO previo:

```sh
cp /root/module-backups/econet-eth.ko.pre-bql-397.20260624-164233 /lib/modules/6.12.80/econet-eth.ko
sync
reboot
```

## No perseguir de nuevo

- No era un bug primario de PCM/SLIC/FXS/baresip.
- No era "VoIP rompe el kernel"; VoIP solo disparaba el timing por paquetes RTP.
- No era la corrupcion vieja de qdisc/WiFi.
- No era principalmente HWNAT/SWNAT. Esos modos modifican la carga y el timing,
  pero el BUG estaba en la contabilidad BQL del TX path de `econet_eth`.

