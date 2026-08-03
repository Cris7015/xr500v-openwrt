# Latency weirdness - Windows through XR500v vs router/local switch (2026-06-24)

## TL;DR

Hay dos fenomenos distintos:

1. **Picos locales PC -> XR500v lan2 de 15-16 ms:** eran del lado Windows/Realtek.
   La NIC tenia `Interrupt Moderation` activado. Al desactivarlo, `ping
   192.168.50.1` paso a 0-1 ms estable.
2. **Ping a Internet desde Windows por XR500v sigue alto:** Windows mide
   ~42-44 ms a Google, mientras el XR500v hacia el mismo IP mide ~28 ms y el
   probe `ICMPPHASE` del driver tambien ve ~27-28 ms para el ping forwardeado
   de Windows. Esto queda como caso raro abierto.

Cuando la PC se desconecta del XR500v y vuelve al switch/red comun, el ping al
mismo IP de Google baja a ~22 ms. Por eso no es "Google esta lento" solamente:
la diferencia aparece cuando la PC sale por el XR500v como router de prueba.

## Topologia durante la prueba

- PC Windows en `lan2` del XR500v.
- PC: `192.168.50.248/24`.
- XR500v `lan2`: `192.168.50.1/24`.
- XR500v management `br-lan`: `192.168.68.222/24`.
- XR500v WAN: `pppoe-wan`, IP publica `<public IP redacted>`.
- `lan2` esta aislado para tomar la conexion PPPoE por el XR500v.

## Hallazgo 1: pico local por Realtek/Windows

Antes de tocar la NIC, Windows tenia picos contra el propio gateway:

```text
PS> ping 192.168.50.1
Respuesta desde 192.168.50.1: tiempo=16ms
Respuesta desde 192.168.50.1: tiempo=15ms
Respuesta desde 192.168.50.1: tiempo<1ms
Respuesta desde 192.168.50.1: tiempo=15ms
```

Pero el probe del router para esos pings locales mostraba que el XR500v
respondia en alrededor de 1 ms:

```text
XR500v ICMPPHASE ... us=1021/14/354/1390
XR500v ICMPPHASE ... us=714/14/293/1022
XR500v ICMPPHASE ... us=856/10/265/1132
```

Formato del probe:

```text
us = irq->poll / poll->rx / rx->tx / irq->tx
```

Entonces para `us=1021/14/354/1390`:

- IRQ RX -> NAPI poll: `1021 us`
- NAPI poll -> skb RX: `14 us`
- RX -> TX post: `354 us`
- IRQ RX -> TX post total: `1390 us`

La demora de 15-16 ms no estaba en el router. Estaba despues de que el router
ya habia posteado la respuesta TX, del lado PC/NIC/Windows.

La NIC Realtek tenia:

```text
Realtek PCIe GbE Family Controller
Moderacion de interrupcion: Activado
```

Al desactivar `Interrupt Moderation`, la latencia local quedo bien:

```text
PS> ping 192.168.50.1
Respuesta desde 192.168.50.1: tiempo=1ms
Respuesta desde 192.168.50.1: tiempo=1ms
Respuesta desde 192.168.50.1: tiempo=1ms
Respuesta desde 192.168.50.1: tiempo<1ms
```

Conclusion de este punto: **el path local PC <-> XR500v lan2 quedo sano**.

## Hallazgo 2: Internet por XR500v sigue midiendo alto en Windows

Con la PC por XR500v:

```text
PS> ping 142.251.129.78
Respuesta desde 142.251.129.78: tiempo=42ms
Respuesta desde 142.251.129.78: tiempo=43ms
Respuesta desde 142.251.129.78: tiempo=44ms
Respuesta desde 142.251.129.78: tiempo=43ms
```

Desde el XR500v hacia el mismo IP:

```text
root@OpenWrt:~# ping google.com -4
PING google.com (142.251.129.78): 56 data bytes
64 bytes from 142.251.129.78: seq=0 ttl=121 time=28.443 ms
64 bytes from 142.251.129.78: seq=1 ttl=121 time=27.842 ms
64 bytes from 142.251.129.78: seq=2 ttl=121 time=28.030 ms
64 bytes from 142.251.129.78: seq=3 ttl=121 time=28.297 ms
64 bytes from 142.251.129.78: seq=4 ttl=121 time=28.030 ms
```

Y el probe `ICMPPHASE` para los pings de Windows tambien vio ~27-28 ms:

```text
XR500v ICMPPHASE id=1 seq=301 ... us=50/13/27215/27280
XR500v ICMPPHASE id=1 seq=302 ... us=41/14/28255/28311
XR500v ICMPPHASE id=1 seq=303 ... us=39/14/28253/28307
XR500v ICMPPHASE id=1 seq=304 ... us=52/14/27563/27630
```

Interpretacion:

- El router recibe el echo request de la PC.
- Lo forwardea/NATea por PPPoE.
- Recibe el echo reply desde Google.
- Postea el reply TX hacia `lan2`.
- Todo eso, segun el probe, tarda ~27-28 ms.

Pero Windows reporta ~43 ms. La diferencia de ~15 ms queda entre:

```text
driver postea TX hacia lan2
-> switch/QDMA/DSA/NIC/Windows reciben/procesan
-> ping.exe registra el reply
```

Esto es lo raro pendiente, porque para ping local `192.168.50.1` ese tramo ya no
parece meter 15 ms despues de desactivar la moderacion de interrupcion.

## Comparacion contra red comun

Cuando la PC se desconecta del XR500v y se conecta al switch/red comun:

```text
PS> ping 142.251.129.78
Respuesta desde 142.251.129.78: tiempo=22ms
Respuesta desde 142.251.129.78: tiempo=22ms
Respuesta desde 142.251.129.78: tiempo=22ms
Respuesta desde 142.251.129.78: tiempo=22ms
```

Esto prueba que la PC/Windows puede medir bien en esa maquina y que el destino
Google no esta inherentemente en 43 ms. La diferencia aparece en la combinacion:

```text
Windows PC -> XR500v lan2 -> NAT/PPPoE del XR500v -> Internet
```

## Cosas descartadas o parcialmente descartadas

- **LAN local rota:** no. Despues del cambio en Realtek, `PC -> 192.168.50.1`
  queda 0-1 ms.
- **QDMA RX/NAPI lento:** no para estos eventos. `irq->poll` y `poll->rx` estan
  en decenas o cientos de microsegundos en los logs relevantes.
- **Google/IP distinto:** no alcanza como explicacion. Se comparo el mismo IP
  `142.251.129.78`.
- **Filtros NDIS como unica causa:** se probaron sospechas por bindings de
  Windows, pero el usuario reporto que no cambio el resultado. Aun asi, siguen
  existiendo muchos filtros en la NIC, por lo que no queda 100% descartado a
  nivel Windows.

## Sospechas abiertas

1. **Diferencia real de ruta/peering por PPPoE del XR500v vs red comun.**
   El router por si mismo ya mide ~28 ms, no 22 ms. Eso explica parte de la
   diferencia contra la red comun, pero no todo el salto Windows 43 ms.

2. **Demora despues del TX post del driver solo para replies forwardeados.**
   `ICMPPHASE` termina en `en75_qdma_xmit()`/TX post, no en TX completion ni en
   "bit realmente salio por cable". Para cerrar esto hay que extender el probe a
   TX completion y, si se puede, distinguir egress `lan2`.

3. **Interaccion Windows/NIC con trafico forwardeado/NATeado.**
   La moderacion de interrupcion local ya se arreglo, pero puede quedar algun
   comportamiento del stack Windows/filtros/driver que no aparece en el ping al
   gateway.

4. **ICMP no representa TCP/UDP offloadeado.**
   El flowtable de OpenWrt solo offloadea `tcp,udp`; ICMP no entra en HWNAT.
   Por eso `ping` puede verse peor que trafico real de juegos/VoIP/speedtest.

## Proximo test bueno

Extender `ICMPPHASE` para medir tambien TX completion:

```text
RX request time
TX post reply time
TX complete reply time
```

Si `TX complete - TX post` tambien queda bajo, entonces el XR500v queda casi
absuelto y el diferencial esta despues del hardware/router. Si aparece un delta
de ~15 ms en TX completion para replies forwardeados hacia `lan2`, entonces hay
un bug/cola dentro del TX path del XR500v que el probe actual no ve.

Tambien conviene medir latencia con UDP/TCP, no solo ICMP, porque TCP/UDP puede
usar flow offload/HWNAT y tomar otro camino.

