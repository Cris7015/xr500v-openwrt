#!/usr/bin/env python3
"""iter59: fix iter58 GE PHY analog cal — bypass phy_*_mmd indirection.

Root cause of iter58 failure: every cal step reported "initial wait fail"
because Linux's phy_read_mmd/phy_write_mmd uses Clause 22 emulation
(regs 13/14) when phydev->is_c45 is false. The EN751221 GE PHY does NOT
implement standard MMD via regs 13/14 — its MMD bank is only accessible
through the switch's PHY_IAC controller in Clause 45 mode.

The driver already registers c45 ops on priv->user_bus
(mt7531_ind_c45_phy_read/write). We just need to call mdiobus_c45_read /
mdiobus_c45_write directly instead of phy_read_mmd / phy_write_mmd in the
cal sequence — bypassing the broken C22 fallback.

This script:
  1. Inserts en751221_mmd_read/write helper wrappers in the ITER58 block
  2. Rewrites every phy_read_mmd(phydev, ...) -> en751221_mmd_read(...)
     and every phy_write_mmd(phydev, ...) -> en751221_mmd_write(...) inside
     the ITER58_PHY_ANALOG_CAL section ONLY (not driver-wide).
"""
import re, sys

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/econet-eth-2026.02.13~c2f855cf/gsw/mt7530.c"

src = open(FN).read()

if "ITER59_C45_BYPASS" in src:
    print("[i] iter59 already applied")
    sys.exit(0)

if "ITER58_PHY_ANALOG_CAL" not in src:
    sys.exit("ERROR: ITER58 block not found, apply iter58 first")

# Insert the C45 bypass helpers right after the constants/table, before
# en751221_anacal_wait. We anchor on the table closing brace.
needle_helpers_anchor = "static u8 en751221_anacal_wait(struct phy_device *phydev, u32 delay)"
helpers = r'''/* iter59: ITER59_C45_BYPASS — direct C45 helpers
 *
 * phy_read_mmd / phy_write_mmd in mainline Linux fall back to Clause 22
 * emulation (writes to regs 13/14 of the PHY) when phydev->is_c45 is
 * false.  The EN751221 GE PHY does not implement MMD via regs 13/14;
 * its analog cal MMD bank can only be reached through the switch's
 * PHY_IAC controller in Clause 45 mode.  The driver already registers
 * read_c45 / write_c45 callbacks on priv->user_bus, so we just call
 * mdiobus_c45_read / mdiobus_c45_write directly — that path drives
 * mt7531_ind_c45_phy_read / mt7531_ind_c45_phy_write on EN751221, which
 * issue native C45 frames on PHY_IAC.
 */
static int en751221_mmd_read(struct phy_device *phydev, int devad, u32 regnum)
{
	return mdiobus_c45_read(phydev->mdio.bus, phydev->mdio.addr,
				devad, regnum);
}

static int en751221_mmd_write(struct phy_device *phydev, int devad,
			      u32 regnum, u16 val)
{
	return mdiobus_c45_write(phydev->mdio.bus, phydev->mdio.addr,
				 devad, regnum, val);
}

'''
src = src.replace(needle_helpers_anchor, helpers + needle_helpers_anchor, 1)

# Now rewrite phy_*_mmd -> en751221_mmd_* ONLY inside the cal block.
START = "/* === ITER58_PHY_ANALOG_CAL"
END   = "/* === END ITER58_PHY_ANALOG_CAL === */"
i_start = src.index(START)
i_end   = src.index(END) + len(END)
block = src[i_start:i_end]

# Substitute calls
block_new = re.sub(r"\bphy_write_mmd\(phydev,",
                   "en751221_mmd_write(phydev,", block)
block_new = re.sub(r"\bphy_read_mmd\(phydev,",
                   "en751221_mmd_read(phydev,", block_new)

# Sanity: at least 20 substitutions expected
n_w = block.count("phy_write_mmd(phydev,")
n_r = block.count("phy_read_mmd(phydev,")
print(f"[i] substitutions in cal block: {n_w} writes, {n_r} reads")
if n_w + n_r < 30:
    sys.exit(f"ERROR: only {n_w + n_r} substitutions (expected >=30)")

# Mark applied
block_new = block_new.replace(
    "/* === ITER58_PHY_ANALOG_CAL",
    "/* === ITER58_PHY_ANALOG_CAL + ITER59_C45_BYPASS",
    1,
)

src = src[:i_start] + block_new + src[i_end:]

open(FN, "w").write(src)
print(f"[+] iter59 applied: {n_w} writes + {n_r} reads now via direct C45")
