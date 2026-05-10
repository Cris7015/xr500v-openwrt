#!/usr/bin/env python3
from pathlib import Path

ROOT = Path("/home/cristuu/openwrt")
PATCH = ROOT / "package/kernel/econet-eth/patches/106-en751221-cal-before-dsa-register.patch"

PATCH.write_text("""iter97: run EN751221 GE PHY calibration before DSA registration

iter96 proved that calibration of addr 9..11 can run while DSA/netifd is
already enabling ports. The EN751221 analog calibration and setup_port both
touch paged MDIO/MMD state, so run the probe-time calibration before
dsa_register_switch() exposes user ports.

--- a/gsw/mt7530-mmio.c
+++ b/gsw/mt7530-mmio.c
@@ -66,10 +66,13 @@ static int mt7988_probe(struct platform_device *pdev)
 \tif (ret)
 \t\treturn ret;
 
+\tif (priv->id == ID_EN751221) {
+\t\tdev_warn(&pdev->dev, "ITER97: running GE PHY cal before dsa_register_switch\\n");
+\t\ten751221_run_cal_at_probe(priv);
+\t}
+
 \tret = dsa_register_switch(priv->ds);
 \tif (ret)
 \t\treturn ret;
-\tif (priv->id == ID_EN751221)
-\t\ten751221_run_cal_at_probe(priv);
 \treturn 0;
 }
 
""")

print(f"iter97 patch written: {PATCH}")
