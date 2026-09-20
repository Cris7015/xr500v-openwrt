#!/bin/bash
# Read-only dump of every MTD partition over SSH, verified by router-side md5.
set -u
host=${1:-192.168.1.1}
ssh_() { ssh -n -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@"$host" "$@"; }
ssh_ 'cat /proc/mtd' > proc_mtd.txt
while read -r dev size erase name; do
  [[ $dev == mtd* ]] || continue
  i=${dev%:}; n=${name//\"/}
  ssh_ "cat /dev/$i" > "${i}_$n.bin"
  r=$(ssh_ "md5sum /dev/$i" | cut -d' ' -f1)
  l=$(md5sum "${i}_$n.bin" | cut -d' ' -f1)
  s=$(stat -c%s "${i}_$n.bin")
  ok=MISMATCH; [[ $r == "$l" && $s -eq $((16#$size)) ]] && ok=OK
  echo "$i $n $s router=$r local=$l $ok"
done < proc_mtd.txt
