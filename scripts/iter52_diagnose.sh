#!/bin/sh
# iter52: post-boot diagnostic. Pegar en el shell del router después de bootear iter52.
# Lee GDM1 RX counters + QDMA RX queue state + switch port 6 stats.
#
# Mientras corre, generá tráfico desde tu PC (ping al router, broadcasts, etc.)
# Si GDM1 RX_PKTS > 0 → frames llegan al SoC desde el switch
# Si GDM1 RX_PKTS = 0 pero switch port 6 TX > 0 → TRGMII RX side roto
# Si GDM1 RX_PKTS > 0 pero kernel rx = 0 → bug GDM→QDMA path

SYS=/sys/devices/platform/1fb50000.ethernet

if [ ! -e $SYS/raw_offset ]; then
    echo "ERROR: raw_offset sysfs no existe — ¿flasheaste el kernel iter52?"
    exit 1
fi

readreg() {
    local off="$1"
    local label="$2"
    echo "$off" > $SYS/raw_offset
    printf "  %-30s @ %-7s = " "$label" "$off"
    cat $SYS/raw_value
}

echo "=== eth0 / GDM1 (port0) regs at 0x0400+ ==="
# fwd_cfg está en offset 0x100 dentro de port0 = absoluto 0x500
readreg 0x500 "GDM1 fwd_cfg"
# rx_len_threshold: típicamente cerca de fwd_cfg
readreg 0x508 "GDM1 rx_len_th"
# bitfield_0 (GDM_RX*_FPORT)
readreg 0x504 "GDM1 bitfield_0"

echo
echo "=== GDM1 counters (struct gdm_counters) ==="
# Counters en gdm struct typically a 0x280+ bytes from start of gdm
# Layout: tx counters first, then rx
# Best-effort offsets — pueden necesitar ajuste leyendo struct gdm en gdm_regs.h
for off in 0x680 0x684 0x688 0x68c 0x690 0x694 0x698 0x69c \
           0x6a0 0x6a4 0x6a8 0x6ac 0x6b0 0x6b4 0x6b8 0x6bc \
           0x6c0 0x6c4 0x6c8 0x6cc 0x6d0; do
    readreg $off "GDM1 cnt[$off]"
done

echo
echo "=== QDMA0 regs at 0x4000+ ==="
readreg 0x4000 "QDMA0 cfg"
readreg 0x4008 "QDMA0 int_status"
readreg 0x400c "QDMA0 int_enable"
# Q chain registers: rx_hwi/cpui/base
# Layout assumed at offset ~0x100-0x300 dentro de qdma_regs
for off in 0x4100 0x4108 0x410c 0x4110 0x4114 0x4118 0x411c \
           0x4180 0x4188 0x418c 0x4190; do
    readreg $off "QDMA0 q[$off]"
done

echo
echo "=== switch_regs (offset 0x8000+ MMIO = 0x0 en sw_offset) ==="
readreg 0x8010 "MFC"
readreg 0x80c8 "MTRAP"
readreg 0xa104 "PCR_P(1)"
readreg 0xa604 "PCR_P(6) CPU"
readreg 0xb008 "PMSR_P(0)"
readreg 0xb108 "PMSR_P(1)"
readreg 0xb208 "PMSR_P(2)"
readreg 0xb308 "PMSR_P(3)"
readreg 0xb408 "PMSR_P(4)"
readreg 0xb608 "PMSR_P(6)"

echo
echo "=== Kernel netdev stats ==="
for nd in eth0 lan1 lan2 lan3 lan4; do
    [ -e /sys/class/net/$nd/statistics/rx_packets ] || continue
    rx=$(cat /sys/class/net/$nd/statistics/rx_packets)
    tx=$(cat /sys/class/net/$nd/statistics/tx_packets)
    drop=$(cat /sys/class/net/$nd/statistics/rx_dropped)
    err=$(cat /sys/class/net/$nd/statistics/rx_errors)
    printf "  %-8s rx=%-8s tx=%-8s drop=%-4s err=%s\n" "$nd" "$rx" "$tx" "$drop" "$err"
done

echo
echo "=== /proc/interrupts (busca QDMA/eth/mt7530 IRQs) ==="
grep -iE "ethernet|qdma|mt7530|gdm|trgmii" /proc/interrupts 2>/dev/null

echo
echo "=== dmesg eth/mt7530 errores ==="
dmesg | grep -iE "(eth|gdm|qdma|mt7530|trgmii).*(err|fail|drop|warn)" | tail -10
echo "(fin)"
