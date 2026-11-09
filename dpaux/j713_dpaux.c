// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J713 (T8132) ATC PHY DP AUX experiment.
 *
 * Upstream atcphy skips atcphy_enable_dp_aux() on the T8122 generation
 * ("FIXME"), so the lpdptx AUX front end of the Type-C PHY is never powered and
 * the DCP DPTX firmware reports DEVICE_NOT_RESPONDING / INACTIVE_SINK.
 *
 * Load: read-only dump of lpdptx + core DP registers of atc<atc>.
 * debugfs /sys/kernel/debug/j713_dpaux/
 *   dump     read: register snapshot
 *   enable   write 1: lpdptx AUX power-up (T8103 sequence, lpdptx part only)
 *            write 2: core DP lane clocks/reset (T8103 sequence, core part)
 *            write 3: both (core first, as atcphy_enable_dp_aux)
 *            write 4: T8132 aciophy_aux_init (macOS AppleT8132TypeCPhy):
 *                     AUX_SHM PWR_REG (core+0x16400) |= 0x3, |= 0xc,
 *                     = (v & ~0x300) | 0x100; AUX_TOP AUX_CTRL (core+0x16000) &= ~1
 *            write 8: undo the T8103 lpdptx writes (+0x0 bit4, +0x4000 = 0)
 *            write 16: apply DT "apple,tunable-aux-shm" (m1n1 auxshm1 stage2:
 *                      ADT tunable_AUX_SHM, <core-offset mask value> triplets)
 *            write 32: T8132 aux_init, then the T8122-family HBR2 link start
 *                      (Asahi f67312fed2, the public form of macOS
 *                      aciophy_dptx_main_init) on whichever lanes are already
 *                      in DP mode. Does not touch a USB3 lane.
 *   j713_dpaux_hbr2(): same sequence, for the dcpext probe to call from the
 *                      ACTIVATE apcall. That callback runs after the firmware
 *                      has powered dev 0x18 and before the reply lets it sample
 *                      hot-plug.
 *   disable  write 1: lpdptx AUX power-down (T8103 sequence)
 *            write 2: T8132 aciophy_aux_shutdown
 *   poke     write "core|lpdptx OFF VAL": raw 32-bit write (page-checked)
 */
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static int atc = 1;
module_param(atc, int, 0444);
MODULE_PARM_DESC(atc, "ATC instance (0: 0x403000000, 1: 0x40b000000)");

#define CORE_SIZE	0x4c000
#define LPDPTX_OFF	0x50000
#define LPDPTX_SIZE	0x8000

/* core */
#define ACIOPHY_PLL_COMMON_CTRL			0x1028
#define ACIOPHY_PLL_WAIT_FOR_CMN_READY		BIT(24)
#define ACIOPHY_CFG0				0x08
#define ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0	0x7000
#define DP_PMA_BYTECLK_RESET			BIT(0)
#define DPTXPHY_PMA_LANE_RESET_N		BIT(2)
#define DPTXPHY_PMA_LANE_RESET_N_OV		BIT(3)
#define DPTX_PCLK1_SELECT			GENMASK(6, 4)
#define DPTX_PCLK2_SELECT			GENMASK(9, 7)
#define DPRX_PCLK_SELECT			GENMASK(12, 10)
#define DPTX_PCLK1_ENABLE			BIT(13)
#define DPTX_PCLK2_ENABLE			BIT(14)
#define DPRX_PCLK_ENABLE			BIT(15)
#define ACIOPHY_LANE_MODE_T8122			0x60
#define ACIOPHY_CROSSBAR_T8122			0x64

/* T8132 (macOS AppleT8132TypeCPhy block table: 25 AUX_TOP, 26 AUX_SHM) */
#define T8132_AUX_TOP_AUX_CTRL		0x16000
#define  T8132_AUX_CTRL_PWRDN		BIT(0)
#define T8132_AUX_SHM_PWR_REG		0x16400
#define  T8132_SML_IN_OV		BIT(0)
#define  T8132_SML_IN			BIT(1)
#define  T8132_BIG_IN_OV		BIT(2)
#define  T8132_BIG_IN			BIT(3)
#define  T8132_CLAMP_EN_OV		BIT(8)
#define  T8132_CLAMP_EN			BIT(9)

/* lpdptx */
#define AUX_CFG_BLK_AUX_CTRL		0x0000
#define  BLK_AUX_CTRL_PWRDN		BIT(4)
#define  BLK_AUX_RXOFFSET		GENMASK(25, 22)
#define AUX_CFG_BLK_AUX_LDO_CTRL	0x0008
#define AUX_CFG_BLK_AUX_MARGIN		0x000c
#define  MARGIN_RCAL_RXOFFSET_EN	BIT(5)
#define  AUX_MARGIN_RCAL_TXSWING	GENMASK(10, 6)
#define AUX_SHM_CFG_BLK_AUX_CTRL_REG0	0x0204
#define  CFG_PMA_AUX_SEL_LF_DATA	BIT(15)
#define AUX_SHM_CFG_BLK_AUX_CTRL_REG1	0x0208
#define  CFG_PMA_PHYS_ADJ		GENMASK(22, 20)
#define  CFG_PMA_PHYS_ADJ_OV		BIT(19)
#define AUX_CONTROL			0x4000
#define  AUX_PWN_DOWN			0x10
#define  AUX_CLAMP_EN			0x04
#define  SLEEP_B_BIG_IN			0x02
#define  SLEEP_B_SML_IN			0x01
#define  TXTERM_CODEMSB			0x400
#define  TXTERM_CODE			GENMASK(9, 5)

static void __iomem *core, *lp;
static struct dentry *dir;
static DEFINE_MUTEX(lock);

static void m32(void __iomem *r, u32 clr, u32 set)
{
	u32 o = readl(r), n = (o & ~clr) | set;

	writel(n, r);
	pr_info("j713_dpaux: %s+%#06lx %#010x -> %#010x (rb %#010x)\n",
		(r >= lp && r < lp + LPDPTX_SIZE) ? "lpdptx" : "core",
		(r >= lp && r < lp + LPDPTX_SIZE) ? (unsigned long)(r - lp) : (unsigned long)(r - core),
		o, n, readl(r));
}

static const struct { const char *n; bool lp; u32 off; } regs[] = {
	{ "core LANE_MODE(t8122)", 0, ACIOPHY_LANE_MODE_T8122 },
	{ "core CROSSBAR(t8122)", 0, ACIOPHY_CROSSBAR_T8122 },
	{ "core PLL_COMMON_CTRL", 0, ACIOPHY_PLL_COMMON_CTRL },
	{ "core LANE_DP_TX_DP_CTRL0", 0, ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0 },
	{ "core T8132 AUX_TOP+0x0", 0, T8132_AUX_TOP_AUX_CTRL },
	{ "core T8132 AUX_TOP+0x4", 0, T8132_AUX_TOP_AUX_CTRL + 4 },
	{ "core T8132 AUX_TOP+0x8", 0, T8132_AUX_TOP_AUX_CTRL + 8 },
	{ "core T8132 AUX_TOP+0xc", 0, T8132_AUX_TOP_AUX_CTRL + 0xc },
	{ "core T8132 AUX_TOP+0x10", 0, T8132_AUX_TOP_AUX_CTRL + 0x10 },
	{ "core T8132 AUX_SHM+0x0", 0, T8132_AUX_SHM_PWR_REG },
	{ "core T8132 AUX_SHM+0x4", 0, T8132_AUX_SHM_PWR_REG + 4 },
	{ "core T8132 AUX_SHM+0x8", 0, T8132_AUX_SHM_PWR_REG + 8 },
	{ "core T8132 AUX_SHM+0xc", 0, T8132_AUX_SHM_PWR_REG + 0xc },
	{ "core T8132 AUX_SHM+0x10", 0, T8132_AUX_SHM_PWR_REG + 0x10 },
	{ "lp AUX_CFG_BLK_AUX_CTRL", 1, AUX_CFG_BLK_AUX_CTRL },
	{ "lp +0x04", 1, 0x4 },
	{ "lp AUX_LDO_CTRL", 1, AUX_CFG_BLK_AUX_LDO_CTRL },
	{ "lp AUX_MARGIN", 1, AUX_CFG_BLK_AUX_MARGIN },
	{ "lp AUX_SHM_CTRL_REG0", 1, AUX_SHM_CFG_BLK_AUX_CTRL_REG0 },
	{ "lp AUX_SHM_CTRL_REG1", 1, AUX_SHM_CFG_BLK_AUX_CTRL_REG1 },
	{ "lp AUX_CONTROL", 1, AUX_CONTROL },
	{ "lp +0x4004", 1, 0x4004 },
	{ "lp +0x4008", 1, 0x4008 },
};

static int dump_show(struct seq_file *s, void *unused)
{
	int i;

	mutex_lock(&lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(s, "%-26s %s+%#06x = %#010x\n", regs[i].n,
			   regs[i].lp ? "lpdptx" : "core", regs[i].off,
			   readl((regs[i].lp ? lp : core) + regs[i].off));
	seq_puts(s, "lpdptx 0x000-0x00f:");
	for (i = 0; i < 0x10; i += 4)
		seq_printf(s, " %08x", readl(lp + i));
	seq_puts(s, "\nlpdptx 0x200-0x20f:");
	for (i = 0x200; i < 0x210; i += 4)
		seq_printf(s, " %08x", readl(lp + i));
	seq_puts(s, "\nlpdptx 0x4000-0x401f:");
	for (i = 0x4000; i < 0x4020; i += 4)
		seq_printf(s, " %08x", readl(lp + i));
	seq_putc(s, '\n');
	mutex_unlock(&lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dump);

static void enable_core(void)
{
	void __iomem *r = core + ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0;

	m32(r, 0, DPTXPHY_PMA_LANE_RESET_N);
	m32(r, 0, DPTXPHY_PMA_LANE_RESET_N_OV);
	m32(r, DPRX_PCLK_SELECT, FIELD_PREP(DPRX_PCLK_SELECT, 1));
	m32(r, 0, DPRX_PCLK_ENABLE);
	m32(r, DPTX_PCLK1_SELECT, FIELD_PREP(DPTX_PCLK1_SELECT, 1));
	m32(r, 0, DPTX_PCLK1_ENABLE);
	m32(r, DPTX_PCLK2_SELECT, FIELD_PREP(DPTX_PCLK2_SELECT, 1));
	m32(r, 0, DPTX_PCLK2_ENABLE);
	m32(core + ACIOPHY_PLL_COMMON_CTRL, 0, ACIOPHY_PLL_WAIT_FOR_CMN_READY);
}

static void enable_lp(void)
{
	m32(lp + AUX_CONTROL, 0, AUX_CLAMP_EN);
	m32(lp + AUX_CONTROL, 0, SLEEP_B_SML_IN);
	udelay(10);
	m32(lp + AUX_CONTROL, 0, SLEEP_B_BIG_IN);
	udelay(10);
	m32(lp + AUX_CONTROL, AUX_CLAMP_EN, 0);
	m32(lp + AUX_CONTROL, AUX_PWN_DOWN, 0);
	m32(lp + AUX_CONTROL, TXTERM_CODEMSB, 0);
	m32(lp + AUX_CONTROL, TXTERM_CODE, FIELD_PREP(TXTERM_CODE, 0x16));

	m32(lp + AUX_CFG_BLK_AUX_LDO_CTRL, 0, 0x1c00);
	m32(lp + AUX_SHM_CFG_BLK_AUX_CTRL_REG1, CFG_PMA_PHYS_ADJ, FIELD_PREP(CFG_PMA_PHYS_ADJ, 5));
	m32(lp + AUX_SHM_CFG_BLK_AUX_CTRL_REG1, 0, CFG_PMA_PHYS_ADJ_OV);
	m32(lp + AUX_CFG_BLK_AUX_MARGIN, MARGIN_RCAL_RXOFFSET_EN, 0);
	m32(lp + AUX_CFG_BLK_AUX_CTRL, BLK_AUX_CTRL_PWRDN, 0);
	m32(lp + AUX_SHM_CFG_BLK_AUX_CTRL_REG0, 0, CFG_PMA_AUX_SEL_LF_DATA);
	m32(lp + AUX_CFG_BLK_AUX_CTRL, BLK_AUX_RXOFFSET, FIELD_PREP(BLK_AUX_RXOFFSET, 3));
	m32(lp + AUX_CFG_BLK_AUX_MARGIN, AUX_MARGIN_RCAL_TXSWING,
	    FIELD_PREP(AUX_MARGIN_RCAL_TXSWING, 12));
}

static void disable_lp(void)
{
	m32(lp + AUX_CONTROL, 0, AUX_PWN_DOWN);
	m32(lp + AUX_CFG_BLK_AUX_CTRL, 0, BLK_AUX_CTRL_PWRDN);
	m32(lp + AUX_CONTROL, 0, AUX_CLAMP_EN);
	m32(lp + AUX_CONTROL, SLEEP_B_SML_IN, 0);
	udelay(10);
	m32(lp + AUX_CONTROL, SLEEP_B_BIG_IN, 0);
	udelay(10);
}

static void t8132_aux_init(void)
{
	void __iomem *pwr = core + T8132_AUX_SHM_PWR_REG;

	m32(pwr, 0, T8132_SML_IN | T8132_SML_IN_OV);
	udelay(1);
	m32(pwr, 0, T8132_BIG_IN | T8132_BIG_IN_OV);
	udelay(1);
	m32(pwr, T8132_CLAMP_EN | T8132_CLAMP_EN_OV, T8132_CLAMP_EN_OV);
	udelay(1);
	m32(core + T8132_AUX_TOP_AUX_CTRL, T8132_AUX_CTRL_PWRDN, 0);
	udelay(1);
}

static void t8132_aux_shutdown(void)
{
	void __iomem *pwr = core + T8132_AUX_SHM_PWR_REG;

	m32(core + T8132_AUX_TOP_AUX_CTRL, 0, T8132_AUX_CTRL_PWRDN);
	udelay(1);
	m32(pwr, 0, T8132_CLAMP_EN);
	udelay(1);
	m32(pwr, T8132_BIG_IN, 0);
	udelay(1);
	m32(pwr, T8132_SML_IN, 0);
}

static void undo_t8103_lp(void)
{
	m32(lp + AUX_CFG_BLK_AUX_CTRL, 0, BLK_AUX_CTRL_PWRDN);
	m32(lp + AUX_CONTROL, ~0u, 0);
}

/* T8122/T8132 HBR2 link start. Register map from Asahi f67312fed2.
 * AUSCMN_DIG (core+0x800) and AUSPLL_CORE (core+0x2200) already live in
 * apple,tunable-common-b and are applied by the in-tree atcphy driver.
 */
#define AUSPLL_APB_CMD_OVERRIDE		0x2000
#define  AUSPLL_APB_REQ			BIT(0)
#define  AUSPLL_APB_ACK			BIT(1)
#define  AUSPLL_APB_UNK28		BIT(28)
#define  AUSPLL_APB_CMD			GENMASK(27, 3)
#define AUSPLL_FREQ_DESC_A		0x2080
#define  AUSPLL_FD_FREQ_COUNT_TARGET	GENMASK(9, 0)
#define  AUSPLL_FD_KI_MAN		GENMASK(17, 14)
#define  AUSPLL_FD_KI_EXP		GENMASK(21, 18)
#define  AUSPLL_FD_KP_MAN		GENMASK(25, 22)
#define  AUSPLL_FD_KP_EXP		GENMASK(29, 26)
#define AUSPLL_FREQ_DESC_B		0x2084
#define  AUSPLL_FD_FBDIVN_FRAC_DEN	GENMASK(13, 0)
#define  AUSPLL_FD_FBDIVN_FRAC_NUM	GENMASK(27, 14)
#define AUSPLL_FREQ_DESC_C		0x2088
#define  AUSPLL_FD_PCLK_DIV_SEL		GENMASK(13, 9)
#define  AUSPLL_FD_LFSDM_DIV		GENMASK(15, 14)
#define  AUSPLL_FD_LFCLK_CTRL		GENMASK(19, 16)
#define  AUSPLL_FD_VCLK_OP_DIVN		GENMASK(21, 20)
#define  AUSPLL_FD_VCLK_PRE_DIVN	BIT(22)
#define AUSPLL_CLKOUT_MASTER		0x2200
#define  AUSPLL_PCLK_DRVR_EN_T8122	BIT(3)
#define AUSPLL_CLKOUT_DIV		0x2208
#define  AUSPLL_CLKOUT_PLLA_REFBUFCLK_DI GENMASK(20, 16)
#define AUSPLL_BGR_T8122		0x2218
#define  AUSPLL_BGR_CTRL_AVAIL		BIT(0)
#define AUSPLL_FREQ_CFG_T8122		0x2234
#define  AUSPLL_FREQ_REFCLK		GENMASK(1, 0)
#define ACIOPHY_DP_PCLK_STAT_T8122	0x7034
#define  ACIOPHY_AUSPLL_LOCK		BIT(3)
#define ACIOPHY_CFG1			0x000c
#define ACIOPHY_SLEEP_CTRL_T8122	0x010c
#define LANE_STRIDE			0x7000
#define LN0_AUSPMA_RX_TOP		0x9000
#define LN0_AUSPMA_RX_SHM		0xB000
#define LN0_AUSPMA_TX_TOP		0xC000
#define LN0_AUSPMA_TX_SHM		0xD000
#define LN_AUSPMA_TX_SHM_MAIN2		0x0004
#define LN_AUSPMA_TX_SHM_MAIN4		0x000c
#define LN_AUSPMA_TX_SHM_MAIN5		0x0010
#define LN_AUSPMA_TX_SHM_CLKMON1	0x001c
#define LN_AUSPMA_TX_SHM_LDOCLK1	0x0044
#define LN_AUSPMA_TX_TOP_EQ1		0x0050
#define LN_AUSPMA_TX_TOP_EQ17		0x00d0
#define LN_AUSPMA_TX_TOP_EQ18		0x00d4
#define LN_AUSPMA_RX_TOP_TXMODE		0x0160
#define LN_AUSPMA_RX_TOP_AFE1		0x01d4
#define LN_AUSPMA_RX_TOP_DCO1		0x01dc
#define LN_AUSPMA_RX_SHM_CTLE2		0x0030
#define LN_AUSPMA_RX_SHM_DFE101		0x0064
#define LN_AUSPMA_RX_SHM_DFE111		0x0068
#define LN_AUSPMA_RX_SHM_DFE121		0x006c
#define LN_AUSPMA_RX_SHM_SAVOS161	0x00a4
#define LN_AUSPMA_RX_SHM_TERM191	0x00a8
#define LN_AUSPMA_RX_SHM_VREF221	0x00d4
#define LN_AUSPMA_RX_SHM_VREF231	0x00d8
#define LN_AUSPMA_RX_SHM_VREG1		0x00dc

static void __iomem *lane_reg(unsigned int lane, u32 lane0)
{
	return core + lane0 + lane * LANE_STRIDE;
}

static int dp_poll(u32 off, u32 mask, u32 expect)
{
	u32 val;
	int ret = readl_poll_timeout(core + off, val, (val & mask) == expect, 10, 20000);

	pr_info("j713_dpaux: poll core+%#x mask %#x want %#x got %#x %s\n",
		off, mask, expect, val, ret ? "TIMEOUT" : "ok");
	return ret;
}

static int auspll_cmd(u32 command, bool request)
{
	m32(core + AUSPLL_APB_CMD_OVERRIDE,
	    AUSPLL_APB_REQ | AUSPLL_APB_CMD | AUSPLL_APB_UNK28,
	    FIELD_PREP(AUSPLL_APB_CMD, command) | AUSPLL_APB_UNK28 |
	    (request ? AUSPLL_APB_REQ : 0));
	return dp_poll(AUSPLL_APB_CMD_OVERRIDE, AUSPLL_APB_ACK,
		       request ? AUSPLL_APB_ACK : 0);
}

static void hbr2_tx(unsigned int lane)
{
	void __iomem *shm = lane_reg(lane, LN0_AUSPMA_TX_SHM);
	void __iomem *top = lane_reg(lane, LN0_AUSPMA_TX_TOP);

	m32(core + ACIOPHY_SLEEP_CTRL_T8122, 0, BIT(4 + lane));
	m32(core + ACIOPHY_SLEEP_CTRL_T8122, 0, BIT(6 + lane));
	udelay(1);
	m32(core + ACIOPHY_SLEEP_CTRL_T8122, 0, BIT(lane));
	m32(core + ACIOPHY_SLEEP_CTRL_T8122, 0, BIT(2 + lane));
	udelay(1);
	m32(core + ACIOPHY_SLEEP_CTRL_T8122, BIT(8 + lane), 0);
	m32(core + ACIOPHY_SLEEP_CTRL_T8122, 0, BIT(10 + lane));
	udelay(1);

	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, ~0u, 0x0aafb800);
	udelay(1);
	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, 0, BIT(24));
	udelay(1);
	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, 0, BIT(26));
	udelay(1);
	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, GENMASK(10, 6), FIELD_PREP(GENMASK(10, 6), 12));
	udelay(1);
	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, GENMASK(18, 17) | GENMASK(15, 14), 0);
	udelay(1);
	m32(top + LN_AUSPMA_TX_TOP_EQ17, 0, BIT(20));
	m32(top + LN_AUSPMA_TX_TOP_EQ1, GENMASK(3, 0) | GENMASK(15, 14), 0);
	m32(top + LN_AUSPMA_TX_TOP_EQ18, 0, BIT(28));
	m32(top + LN_AUSPMA_TX_TOP_EQ17, GENMASK(18, 0), 1);
	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, 0, BIT(20));
	udelay(1);
	m32(shm + LN_AUSPMA_TX_SHM_LDOCLK1, 0, BIT(22));
	udelay(1);
	m32(shm + LN_AUSPMA_TX_SHM_MAIN2, BIT(24) | BIT(25), BIT(25));
	m32(shm + LN_AUSPMA_TX_SHM_MAIN2, 0, BIT(28) | BIT(29));
	m32(shm + LN_AUSPMA_TX_SHM_MAIN4, GENMASK(25, 24) | BIT(26), BIT(26));
	m32(shm + LN_AUSPMA_TX_SHM_MAIN5, 0, BIT(9) | BIT(10));
	m32(shm + LN_AUSPMA_TX_SHM_CLKMON1, BIT(22) | BIT(23), BIT(23));
}

static void hbr2_rxtx(unsigned int lane)
{
	void __iomem *shm = lane_reg(lane, LN0_AUSPMA_RX_SHM);
	void __iomem *top = lane_reg(lane, LN0_AUSPMA_RX_TOP);

	m32(top + LN_AUSPMA_RX_TOP_TXMODE, 0, BIT(0));
	m32(top + LN_AUSPMA_RX_TOP_DCO1, 0, GENMASK(16, 15));
	m32(shm + LN_AUSPMA_RX_SHM_SAVOS161, BIT(0) | BIT(1), BIT(1));
	m32(shm + LN_AUSPMA_RX_SHM_TERM191, BIT(15) | BIT(16), BIT(16));
	m32(shm + LN_AUSPMA_RX_SHM_DFE121, BIT(15) | BIT(16), BIT(16));
	m32(shm + LN_AUSPMA_RX_SHM_VREF221, GENMASK(20, 16) | GENMASK(12, 7), 0x121180);
	m32(shm + LN_AUSPMA_RX_SHM_DFE101, GENMASK(15, 10), 0xb800);
	m32(shm + LN_AUSPMA_RX_SHM_VREF231, GENMASK(1, 0) | BIT(2), BIT(2));
	m32(shm + LN_AUSPMA_RX_SHM_VREG1, GENMASK(4, 3), 0);
	m32(shm + LN_AUSPMA_RX_SHM_VREF221, GENMASK(15, 13), 0xc000);
	udelay(1);

	m32(core + ACIOPHY_CFG1, 0, BIT(4 + lane));
	m32(core + ACIOPHY_CFG1, 0, BIT(6 + lane));
	udelay(1);
	m32(core + ACIOPHY_CFG1, 0, BIT(lane));
	m32(core + ACIOPHY_CFG1, 0, BIT(2 + lane));
	udelay(1);
	m32(core + ACIOPHY_CFG0, 0, BIT(10 + lane));
	m32(core + ACIOPHY_CFG0, 0, BIT(12 + lane));
	udelay(1);
	m32(core + ACIOPHY_CFG0, 0, BIT(6 + lane));
	m32(core + ACIOPHY_CFG0, 0, BIT(8 + lane));
	udelay(1);
	m32(core + ACIOPHY_CFG0, BIT(14 + lane), 0);
	m32(core + ACIOPHY_CFG0, 0, BIT(16 + lane));
	udelay(1);

	m32(shm + LN_AUSPMA_RX_SHM_DFE121, 0, BIT(11));
	m32(top + LN_AUSPMA_RX_TOP_AFE1, BIT(0) | BIT(1), BIT(0));
	udelay(1);
	m32(top + LN_AUSPMA_RX_TOP_AFE1, 0, BIT(1));
	m32(shm + LN_AUSPMA_RX_SHM_DFE121, BIT(9) | BIT(10), BIT(10));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_VREF221, BIT(16) | GENMASK(19, 18),
	    BIT(16) | FIELD_PREP(GENMASK(19, 18), 1));
	m32(shm + LN_AUSPMA_RX_SHM_VREF231, GENMASK(1, 0), FIELD_PREP(GENMASK(1, 0), 1));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_VREF221, GENMASK(19, 18), 0);
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_DFE111, 0, BIT(7) | BIT(8));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_DFE111, 0, BIT(5) | BIT(6));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_VREF231, GENMASK(1, 0), FIELD_PREP(GENMASK(1, 0), 3));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_VREF231, GENMASK(1, 0), FIELD_PREP(GENMASK(1, 0), 2));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_VREF231, GENMASK(1, 0), 0);
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_TERM191, 0, BIT(17) | BIT(18));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_DFE121, BIT(13) | BIT(14), BIT(14));
	m32(top + 0x0210, GENMASK(3, 0) | GENMASK(15, 14), 0);
	m32(top + 0x0248, 0, BIT(28));
	m32(top + 0x0244, GENMASK(18, 0), 0x1f81);
	m32(shm + LN_AUSPMA_RX_SHM_TERM191, GENMASK(20, 19) | BIT(21), BIT(21));
	m32(shm + LN_AUSPMA_RX_SHM_TERM191, BIT(22) | BIT(23), BIT(23));
	udelay(1);
	m32(shm + LN_AUSPMA_RX_SHM_CTLE2, 0, BIT(10) | BIT(11));
	udelay(1);
}

int j713_dpaux_hbr2(void);

/* Returns 0 when the DP PLL locks. Lanes not already in DP mode are left alone. */
static int t8132_hbr2(void)
{
	u32 lm = readl(core + ACIOPHY_LANE_MODE_T8122);
	u8 pmas = 0;
	unsigned int lane;
	int ret;

	if (((lm >> 0) & 7) == 2 && ((lm >> 3) & 7) == 2)
		pmas |= BIT(0);
	if (((lm >> 6) & 7) == 2 && ((lm >> 9) & 7) == 2)
		pmas |= BIT(1);
	pr_info("j713_dpaux: HBR2 LANE_MODE %#x DP-lane mask %#x\n", lm, pmas);
	t8132_aux_init();
	if (!pmas) {
		pr_err("j713_dpaux: no lane is in DP mode; HBR2 not programmed\n");
		return -ENODEV;
	}

	m32(core + AUSPLL_FREQ_CFG_T8122, AUSPLL_FREQ_REFCLK, 0);
	m32(core + ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0,
	    DPTX_PCLK1_SELECT | DPTX_PCLK2_SELECT | DPRX_PCLK_SELECT,
	    FIELD_PREP(DPTX_PCLK1_SELECT, 1) | FIELD_PREP(DPTX_PCLK2_SELECT, 1) |
	    FIELD_PREP(DPRX_PCLK_SELECT, 1));
	m32(core + AUSPLL_FREQ_DESC_A, ~0u,
	    FIELD_PREP(AUSPLL_FD_FREQ_COUNT_TARGET, 0x1c2) |
	    FIELD_PREP(AUSPLL_FD_KI_MAN, 8) | FIELD_PREP(AUSPLL_FD_KI_EXP, 3) |
	    FIELD_PREP(AUSPLL_FD_KP_MAN, 8) | FIELD_PREP(AUSPLL_FD_KP_EXP, 7));
	m32(core + AUSPLL_FREQ_DESC_B, ~0u,
	    FIELD_PREP(AUSPLL_FD_FBDIVN_FRAC_DEN, 0x3ffe) |
	    FIELD_PREP(AUSPLL_FD_FBDIVN_FRAC_NUM, 0x1fff));
	m32(core + AUSPLL_FREQ_DESC_C, ~0u,
	    FIELD_PREP(AUSPLL_FD_PCLK_DIV_SEL, 0x4) |
	    FIELD_PREP(AUSPLL_FD_LFSDM_DIV, 1) |
	    FIELD_PREP(AUSPLL_FD_LFCLK_CTRL, 0x5) |
	    FIELD_PREP(AUSPLL_FD_VCLK_OP_DIVN, 1) | AUSPLL_FD_VCLK_PRE_DIVN);
	m32(core + AUSPLL_CLKOUT_DIV, AUSPLL_CLKOUT_PLLA_REFBUFCLK_DI,
	    FIELD_PREP(AUSPLL_CLKOUT_PLLA_REFBUFCLK_DI, 7));
	m32(core + AUSPLL_BGR_T8122, 0, AUSPLL_BGR_CTRL_AVAIL);
	m32(core + AUSPLL_CLKOUT_MASTER, 0, AUSPLL_PCLK_DRVR_EN_T8122);
	ret = auspll_cmd(0, true);
	if (ret)
		return ret;
	ret = dp_poll(ACIOPHY_DP_PCLK_STAT_T8122, ACIOPHY_AUSPLL_LOCK, ACIOPHY_AUSPLL_LOCK);
	if (ret)
		return ret;
	ret = auspll_cmd(0x2800, false);
	if (ret)
		return ret;

	for (lane = 0; lane < 2; lane++) {
		if (pmas & BIT(lane))
			m32(lane_reg(lane, LN0_AUSPMA_RX_SHM) + LN_AUSPMA_RX_SHM_DFE101,
			    BIT(2) | BIT(3), BIT(3));
	}
	m32(core + ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0, 0,
	    DPTXPHY_PMA_LANE_RESET_N | DPTXPHY_PMA_LANE_RESET_N_OV);
	udelay(1);
	for (lane = 0; lane < 2; lane++) {
		if (!(pmas & BIT(lane)))
			continue;
		pr_info("j713_dpaux: HBR2 program DP lane %u\n", lane);
		hbr2_tx(lane);
		hbr2_rxtx(lane);
	}
	m32(core + ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0, DP_PMA_BYTECLK_RESET, 0);
	pr_info("j713_dpaux: HBR2 link start done, PCLK_STAT %#x\n",
		readl(core + ACIOPHY_DP_PCLK_STAT_T8122));
	return 0;
}

int j713_dpaux_hbr2(void)
{
	int err;

	if (!core)
		return -ENODEV;
	mutex_lock(&lock);
	err = t8132_hbr2();
	mutex_unlock(&lock);
	return err;
}
EXPORT_SYMBOL_GPL(j713_dpaux_hbr2);

static void apply_aux_shm_tunable(void)
{
	char path[32];
	struct device_node *np;
	const __be32 *t;
	int len, i;

	snprintf(path, sizeof(path), "/soc/phy@%llx", atc ? 0x40b000000ULL : 0x403000000ULL);
	np = of_find_node_by_path(path);
	if (!np) {
		pr_warn("j713_dpaux: %s not found\n", path);
		return;
	}
	t = of_get_property(np, "apple,tunable-aux-shm", &len);
	if (!t || len % 12) {
		pr_warn("j713_dpaux: %s has no usable apple,tunable-aux-shm (len %d)\n", path, t ? len : -1);
		of_node_put(np);
		return;
	}
	pr_info("j713_dpaux: applying %d AUX_SHM tunables\n", len / 12);
	for (i = 0; i < len / 12; i++) {
		u32 off = be32_to_cpu(t[3 * i]), mask = be32_to_cpu(t[3 * i + 1]);
		u32 val = be32_to_cpu(t[3 * i + 2]);

		if (off < 0x16000 || off >= 0x17000 || (off & 3)) {
			pr_warn("j713_dpaux: skipping tunable at %#x\n", off);
			continue;
		}
		m32(core + off, mask, val);
	}
	of_node_put(np);
}

static ssize_t enable_write(struct file *f, const char __user *ub, size_t n, loff_t *pos)
{
	u32 v;
	int ret = kstrtou32_from_user(ub, n, 0, &v);

	if (ret)
		return ret;
	if (v != 32 && (!v || v > 31))
		return -EINVAL;
	mutex_lock(&lock);
	pr_info("j713_dpaux: atc%d enable %u\n", atc, v);
	if (v == 32) {
		int err = t8132_hbr2();

		if (err)
			pr_err("j713_dpaux: HBR2 failed: %d\n", err);
	} else {
		if (v & 2)
			enable_core();
		if (v & 1)
			enable_lp();
		if (v & 8)
			undo_t8103_lp();
		if (v & 16)
			apply_aux_shm_tunable();
		if (v & 4)
			t8132_aux_init();
	}
	mutex_unlock(&lock);
	return n;
}

static ssize_t disable_write(struct file *f, const char __user *ub, size_t n, loff_t *pos)
{
	u32 v;
	int ret = kstrtou32_from_user(ub, n, 0, &v);

	if (ret)
		return ret;
	if (v != 1 && v != 2)
		return -EINVAL;
	mutex_lock(&lock);
	pr_info("j713_dpaux: atc%d disable %u\n", atc, v);
	if (v == 1)
		disable_lp();
	else
		t8132_aux_shutdown();
	mutex_unlock(&lock);
	return n;
}

static ssize_t poke_write(struct file *f, const char __user *ub, size_t n, loff_t *pos)
{
	char buf[64], which[8];
	u32 off, val;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, n))
		return -EFAULT;
	buf[n] = 0;
	if (sscanf(buf, "%7s %x %x", which, &off, &val) != 3 || (off & 3))
		return -EINVAL;
	mutex_lock(&lock);
	if (!strcmp(which, "lpdptx") && off < LPDPTX_SIZE)
		m32(lp + off, ~0u, val);
	else if (!strcmp(which, "core") && off < CORE_SIZE)
		m32(core + off, ~0u, val);
	else
		n = -EINVAL;
	mutex_unlock(&lock);
	return n;
}

static const struct file_operations enable_fops = {
	.owner = THIS_MODULE, .write = enable_write, .llseek = noop_llseek,
};
static const struct file_operations disable_fops = {
	.owner = THIS_MODULE, .write = disable_write, .llseek = noop_llseek,
};
static const struct file_operations poke_fops = {
	.owner = THIS_MODULE, .write = poke_write, .llseek = noop_llseek,
};

static int __init dpaux_init(void)
{
	phys_addr_t base;
	int i;

	if (atc == 0)
		base = 0x403000000ULL;
	else if (atc == 1)
		base = 0x40b000000ULL;
	else
		return -EINVAL;
	core = ioremap(base, CORE_SIZE);
	lp = ioremap(base + LPDPTX_OFF, LPDPTX_SIZE);
	if (!core || !lp) {
		if (core)
			iounmap(core);
		if (lp)
			iounmap(lp);
		return -ENOMEM;
	}
	pr_info("j713_dpaux: atc%d core %pa lpdptx +%#x (read-only until enable)\n",
		atc, &base, LPDPTX_OFF);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		pr_info("j713_dpaux: %-26s %s+%#06x = %#010x\n", regs[i].n,
			regs[i].lp ? "lpdptx" : "core", regs[i].off,
			readl((regs[i].lp ? lp : core) + regs[i].off));
	dir = debugfs_create_dir("j713_dpaux", NULL);
	debugfs_create_file("dump", 0400, dir, NULL, &dump_fops);
	debugfs_create_file("enable", 0200, dir, NULL, &enable_fops);
	debugfs_create_file("disable", 0200, dir, NULL, &disable_fops);
	debugfs_create_file("poke", 0200, dir, NULL, &poke_fops);
	return 0;
}

static void __exit dpaux_exit(void)
{
	debugfs_remove_recursive(dir);
	iounmap(lp);
	iounmap(core);
}

module_init(dpaux_init);
module_exit(dpaux_exit);
MODULE_DESCRIPTION("J713 ATC PHY DP AUX experiment");
MODULE_LICENSE("Dual MIT/GPL");
