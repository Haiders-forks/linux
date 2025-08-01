// SPDX-License-Identifier: GPL-2.0-only
/*
 * r8169.c: RealTek 8169/8168/8101 ethernet driver.
 *
 * Copyright (c) 2002 ShuChen <shuchen@realtek.com.tw>
 * Copyright (c) 2003 - 2007 Francois Romieu <romieu@fr.zoreil.com>
 * Copyright (c) a lot of people too. Please respect their work.
 *
 * See MAINTAINERS file for support contact information.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/bitfield.h>
#include <linux/prefetch.h>
#include <linux/ipv6.h>
#include <linux/unaligned.h>
#include <net/ip6_checksum.h>
#include <net/netdev_queues.h>

#include "r8169.h"
#include "r8169_firmware.h"

#define FIRMWARE_8168D_1	"rtl_nic/rtl8168d-1.fw"
#define FIRMWARE_8168D_2	"rtl_nic/rtl8168d-2.fw"
#define FIRMWARE_8168E_1	"rtl_nic/rtl8168e-1.fw"
#define FIRMWARE_8168E_2	"rtl_nic/rtl8168e-2.fw"
#define FIRMWARE_8168E_3	"rtl_nic/rtl8168e-3.fw"
#define FIRMWARE_8168F_1	"rtl_nic/rtl8168f-1.fw"
#define FIRMWARE_8168F_2	"rtl_nic/rtl8168f-2.fw"
#define FIRMWARE_8105E_1	"rtl_nic/rtl8105e-1.fw"
#define FIRMWARE_8402_1		"rtl_nic/rtl8402-1.fw"
#define FIRMWARE_8411_1		"rtl_nic/rtl8411-1.fw"
#define FIRMWARE_8411_2		"rtl_nic/rtl8411-2.fw"
#define FIRMWARE_8106E_1	"rtl_nic/rtl8106e-1.fw"
#define FIRMWARE_8106E_2	"rtl_nic/rtl8106e-2.fw"
#define FIRMWARE_8168G_2	"rtl_nic/rtl8168g-2.fw"
#define FIRMWARE_8168G_3	"rtl_nic/rtl8168g-3.fw"
#define FIRMWARE_8168H_2	"rtl_nic/rtl8168h-2.fw"
#define FIRMWARE_8168FP_3	"rtl_nic/rtl8168fp-3.fw"
#define FIRMWARE_8107E_2	"rtl_nic/rtl8107e-2.fw"
#define FIRMWARE_8125A_3	"rtl_nic/rtl8125a-3.fw"
#define FIRMWARE_8125B_2	"rtl_nic/rtl8125b-2.fw"
#define FIRMWARE_8125D_1	"rtl_nic/rtl8125d-1.fw"
#define FIRMWARE_8125D_2	"rtl_nic/rtl8125d-2.fw"
#define FIRMWARE_8125BP_2	"rtl_nic/rtl8125bp-2.fw"
#define FIRMWARE_8126A_2	"rtl_nic/rtl8126a-2.fw"
#define FIRMWARE_8126A_3	"rtl_nic/rtl8126a-3.fw"
#define FIRMWARE_8127A_1	"rtl_nic/rtl8127a-1.fw"

#define TX_DMA_BURST	7	/* Maximum PCI burst, '7' is unlimited */
#define InterFrameGap	0x03	/* 3 means InterFrameGap = the shortest one */

#define R8169_REGS_SIZE		256
#define R8169_RX_BUF_SIZE	(SZ_16K - 1)
#define NUM_TX_DESC	256	/* Number of Tx descriptor registers */
#define NUM_RX_DESC	256	/* Number of Rx descriptor registers */
#define R8169_TX_RING_BYTES	(NUM_TX_DESC * sizeof(struct TxDesc))
#define R8169_RX_RING_BYTES	(NUM_RX_DESC * sizeof(struct RxDesc))
#define R8169_TX_STOP_THRS	(MAX_SKB_FRAGS + 1)
#define R8169_TX_START_THRS	(2 * R8169_TX_STOP_THRS)

#define OCP_STD_PHY_BASE	0xa400

#define RTL_CFG_NO_GBIT	1

/* write/read MMIO register */
#define RTL_W8(tp, reg, val8)	writeb((val8), tp->mmio_addr + (reg))
#define RTL_W16(tp, reg, val16)	writew((val16), tp->mmio_addr + (reg))
#define RTL_W32(tp, reg, val32)	writel((val32), tp->mmio_addr + (reg))
#define RTL_R8(tp, reg)		readb(tp->mmio_addr + (reg))
#define RTL_R16(tp, reg)		readw(tp->mmio_addr + (reg))
#define RTL_R32(tp, reg)		readl(tp->mmio_addr + (reg))

#define JUMBO_4K	(4 * SZ_1K - VLAN_ETH_HLEN - ETH_FCS_LEN)
#define JUMBO_6K	(6 * SZ_1K - VLAN_ETH_HLEN - ETH_FCS_LEN)
#define JUMBO_7K	(7 * SZ_1K - VLAN_ETH_HLEN - ETH_FCS_LEN)
#define JUMBO_9K	(9 * SZ_1K - VLAN_ETH_HLEN - ETH_FCS_LEN)
#define JUMBO_16K	(SZ_16K - VLAN_ETH_HLEN - ETH_FCS_LEN)

static const struct rtl_chip_info {
	u16 mask;
	u16 val;
	enum mac_version mac_version;
	const char *name;
	const char *fw_name;
} rtl_chip_infos[] = {
	/* 8127A family. */
	{ 0x7cf, 0x6c9,	RTL_GIGA_MAC_VER_80, "RTL8127A", FIRMWARE_8127A_1 },

	/* 8126A family. */
	{ 0x7cf, 0x64a,	RTL_GIGA_MAC_VER_70, "RTL8126A", FIRMWARE_8126A_3 },
	{ 0x7cf, 0x649,	RTL_GIGA_MAC_VER_70, "RTL8126A", FIRMWARE_8126A_2 },

	/* 8125BP family. */
	{ 0x7cf, 0x681,	RTL_GIGA_MAC_VER_66, "RTL8125BP", FIRMWARE_8125BP_2 },

	/* 8125D family. */
	{ 0x7cf, 0x689,	RTL_GIGA_MAC_VER_64, "RTL8125D", FIRMWARE_8125D_2 },
	{ 0x7cf, 0x688,	RTL_GIGA_MAC_VER_64, "RTL8125D", FIRMWARE_8125D_1 },

	/* 8125B family. */
	{ 0x7cf, 0x641,	RTL_GIGA_MAC_VER_63, "RTL8125B", FIRMWARE_8125B_2 },

	/* 8125A family. */
	{ 0x7cf, 0x609,	RTL_GIGA_MAC_VER_61, "RTL8125A", FIRMWARE_8125A_3 },

	/* RTL8117 */
	{ 0x7cf, 0x54b,	RTL_GIGA_MAC_VER_52, "RTL8168fp/RTL8117" },
	{ 0x7cf, 0x54a,	RTL_GIGA_MAC_VER_52, "RTL8168fp/RTL8117",
	  FIRMWARE_8168FP_3 },

	/* 8168EP family. */
	{ 0x7cf, 0x502,	RTL_GIGA_MAC_VER_51, "RTL8168ep/8111ep" },

	/* 8168H family. */
	{ 0x7cf, 0x541,	RTL_GIGA_MAC_VER_46, "RTL8168h/8111h",
	  FIRMWARE_8168H_2 },
	/* Realtek calls it RTL8168M, but it's handled like RTL8168H */
	{ 0x7cf, 0x6c0,	RTL_GIGA_MAC_VER_46, "RTL8168M", FIRMWARE_8168H_2 },

	/* 8168G family. */
	{ 0x7cf, 0x5c8,	RTL_GIGA_MAC_VER_44, "RTL8411b", FIRMWARE_8411_2 },
	{ 0x7cf, 0x509,	RTL_GIGA_MAC_VER_42, "RTL8168gu/8111gu",
	  FIRMWARE_8168G_3 },
	{ 0x7cf, 0x4c0,	RTL_GIGA_MAC_VER_40, "RTL8168g/8111g",
	  FIRMWARE_8168G_2 },

	/* 8168F family. */
	{ 0x7c8, 0x488,	RTL_GIGA_MAC_VER_38, "RTL8411", FIRMWARE_8411_1 },
	{ 0x7cf, 0x481,	RTL_GIGA_MAC_VER_36, "RTL8168f/8111f",
	  FIRMWARE_8168F_2 },
	{ 0x7cf, 0x480,	RTL_GIGA_MAC_VER_35, "RTL8168f/8111f",
	  FIRMWARE_8168F_1 },

	/* 8168E family. */
	{ 0x7c8, 0x2c8,	RTL_GIGA_MAC_VER_34, "RTL8168evl/8111evl",
	  FIRMWARE_8168E_3 },
	{ 0x7cf, 0x2c1,	RTL_GIGA_MAC_VER_32, "RTL8168e/8111e",
	  FIRMWARE_8168E_1 },
	{ 0x7c8, 0x2c0,	RTL_GIGA_MAC_VER_33, "RTL8168e/8111e",
	  FIRMWARE_8168E_2 },

	/* 8168D family. */
	{ 0x7cf, 0x281,	RTL_GIGA_MAC_VER_25, "RTL8168d/8111d",
	  FIRMWARE_8168D_1 },
	{ 0x7c8, 0x280,	RTL_GIGA_MAC_VER_26, "RTL8168d/8111d",
	  FIRMWARE_8168D_2 },

	/* 8168DP family. */
	{ 0x7cf, 0x28a,	RTL_GIGA_MAC_VER_28, "RTL8168dp/8111dp" },
	{ 0x7cf, 0x28b,	RTL_GIGA_MAC_VER_31, "RTL8168dp/8111dp" },

	/* 8168C family. */
	{ 0x7cf, 0x3c9,	RTL_GIGA_MAC_VER_23, "RTL8168cp/8111cp" },
	{ 0x7cf, 0x3c8,	RTL_GIGA_MAC_VER_18, "RTL8168cp/8111cp" },
	{ 0x7c8, 0x3c8,	RTL_GIGA_MAC_VER_24, "RTL8168cp/8111cp" },
	{ 0x7cf, 0x3c0,	RTL_GIGA_MAC_VER_19, "RTL8168c/8111c" },
	{ 0x7cf, 0x3c2,	RTL_GIGA_MAC_VER_20, "RTL8168c/8111c" },
	{ 0x7cf, 0x3c3,	RTL_GIGA_MAC_VER_21, "RTL8168c/8111c" },
	{ 0x7c8, 0x3c0,	RTL_GIGA_MAC_VER_22, "RTL8168c/8111c" },

	/* 8168B family. */
	{ 0x7c8, 0x380,	RTL_GIGA_MAC_VER_17, "RTL8168b/8111b" },
	/* This one is very old and rare, support has been removed.
	 * { 0x7c8, 0x300, RTL_GIGA_MAC_VER_11, "RTL8168b/8111b" },
	 */

	/* 8101 family. */
	{ 0x7c8, 0x448,	RTL_GIGA_MAC_VER_39, "RTL8106e", FIRMWARE_8106E_1 },
	{ 0x7c8, 0x440,	RTL_GIGA_MAC_VER_37, "RTL8402", FIRMWARE_8402_1 },
	{ 0x7cf, 0x409,	RTL_GIGA_MAC_VER_29, "RTL8105e", FIRMWARE_8105E_1 },
	{ 0x7c8, 0x408,	RTL_GIGA_MAC_VER_30, "RTL8105e", FIRMWARE_8105E_1 },
	{ 0x7cf, 0x349,	RTL_GIGA_MAC_VER_08, "RTL8102e" },
	{ 0x7cf, 0x249,	RTL_GIGA_MAC_VER_08, "RTL8102e" },
	{ 0x7cf, 0x348,	RTL_GIGA_MAC_VER_07, "RTL8102e" },
	{ 0x7cf, 0x248,	RTL_GIGA_MAC_VER_07, "RTL8102e" },
	{ 0x7cf, 0x240,	RTL_GIGA_MAC_VER_14, "RTL8401" },
	{ 0x7c8, 0x348,	RTL_GIGA_MAC_VER_09, "RTL8102e/RTL8103e" },
	{ 0x7c8, 0x248,	RTL_GIGA_MAC_VER_09, "RTL8102e/RTL8103e" },
	{ 0x7c8, 0x340,	RTL_GIGA_MAC_VER_10, "RTL8101e/RTL8100e" },

	/* 8110 family. */
	{ 0xfc8, 0x980,	RTL_GIGA_MAC_VER_06, "RTL8169sc/8110sc" },
	{ 0xfc8, 0x180,	RTL_GIGA_MAC_VER_05, "RTL8169sc/8110sc" },
	{ 0xfc8, 0x100,	RTL_GIGA_MAC_VER_04, "RTL8169sb/8110sb" },
	{ 0xfc8, 0x040,	RTL_GIGA_MAC_VER_03, "RTL8110s" },
	{ 0xfc8, 0x008,	RTL_GIGA_MAC_VER_02, "RTL8169s" },

	/* Catch-all */
	{ 0x000, 0x000,	RTL_GIGA_MAC_NONE }
};

static const struct pci_device_id rtl8169_pci_tbl[] = {
	{ PCI_VDEVICE(REALTEK,	0x2502) },
	{ PCI_VDEVICE(REALTEK,	0x2600) },
	{ PCI_VDEVICE(REALTEK,	0x8129) },
	{ PCI_VDEVICE(REALTEK,	0x8136), RTL_CFG_NO_GBIT },
	{ PCI_VDEVICE(REALTEK,	0x8161) },
	{ PCI_VDEVICE(REALTEK,	0x8162) },
	{ PCI_VDEVICE(REALTEK,	0x8167) },
	{ PCI_VDEVICE(REALTEK,	0x8168) },
	{ PCI_VDEVICE(NCUBE,	0x8168) },
	{ PCI_VDEVICE(REALTEK,	0x8169) },
	{ PCI_VDEVICE(DLINK,	0x4300) },
	{ PCI_VDEVICE(DLINK,	0x4302) },
	{ PCI_VDEVICE(AT,	0xc107) },
	{ PCI_VDEVICE(USR,	0x0116) },
	{ PCI_VENDOR_ID_LINKSYS, 0x1032, PCI_ANY_ID, 0x0024 },
	{ 0x0001, 0x8168, PCI_ANY_ID, 0x2410 },
	{ PCI_VDEVICE(REALTEK,	0x8125) },
	{ PCI_VDEVICE(REALTEK,	0x8126) },
	{ PCI_VDEVICE(REALTEK,	0x8127) },
	{ PCI_VDEVICE(REALTEK,	0x3000) },
	{ PCI_VDEVICE(REALTEK,	0x5000) },
	{ PCI_VDEVICE(REALTEK,	0x0e10) },
	{}
};

MODULE_DEVICE_TABLE(pci, rtl8169_pci_tbl);

enum rtl_registers {
	MAC0		= 0,	/* Ethernet hardware address. */
	MAC4		= 4,
	MAR0		= 8,	/* Multicast filter. */
	CounterAddrLow		= 0x10,
	CounterAddrHigh		= 0x14,
	TxDescStartAddrLow	= 0x20,
	TxDescStartAddrHigh	= 0x24,
	TxHDescStartAddrLow	= 0x28,
	TxHDescStartAddrHigh	= 0x2c,
	FLASH		= 0x30,
	ERSR		= 0x36,
	ChipCmd		= 0x37,
	TxPoll		= 0x38,
	IntrMask	= 0x3c,
	IntrStatus	= 0x3e,

	TxConfig	= 0x40,
#define	TXCFG_AUTO_FIFO			(1 << 7)	/* 8111e-vl */
#define	TXCFG_EMPTY			(1 << 11)	/* 8111e-vl */

	RxConfig	= 0x44,
#define	RX128_INT_EN			(1 << 15)	/* 8111c and later */
#define	RX_MULTI_EN			(1 << 14)	/* 8111c only */
#define	RXCFG_FIFO_SHIFT		13
					/* No threshold before first PCI xfer */
#define	RX_FIFO_THRESH			(7 << RXCFG_FIFO_SHIFT)
#define	RX_EARLY_OFF			(1 << 11)
#define	RX_PAUSE_SLOT_ON		(1 << 11)	/* 8125b and later */
#define	RXCFG_DMA_SHIFT			8
					/* Unlimited maximum PCI burst. */
#define	RX_DMA_BURST			(7 << RXCFG_DMA_SHIFT)

	Cfg9346		= 0x50,
	Config0		= 0x51,
	Config1		= 0x52,
	Config2		= 0x53,
#define PME_SIGNAL			(1 << 5)	/* 8168c and later */

	Config3		= 0x54,
	Config4		= 0x55,
	Config5		= 0x56,
	PHYAR		= 0x60,
	PHYstatus	= 0x6c,
	RxMaxSize	= 0xda,
	CPlusCmd	= 0xe0,
	IntrMitigate	= 0xe2,

#define RTL_COALESCE_TX_USECS	GENMASK(15, 12)
#define RTL_COALESCE_TX_FRAMES	GENMASK(11, 8)
#define RTL_COALESCE_RX_USECS	GENMASK(7, 4)
#define RTL_COALESCE_RX_FRAMES	GENMASK(3, 0)

#define RTL_COALESCE_T_MAX	0x0fU
#define RTL_COALESCE_FRAME_MAX	(RTL_COALESCE_T_MAX * 4)

	RxDescAddrLow	= 0xe4,
	RxDescAddrHigh	= 0xe8,
	EarlyTxThres	= 0xec,	/* 8169. Unit of 32 bytes. */

#define NoEarlyTx	0x3f	/* Max value : no early transmit. */

	MaxTxPacketSize	= 0xec,	/* 8101/8168. Unit of 128 bytes. */

#define TxPacketMax	(8064 >> 7)
#define EarlySize	0x27

	FuncEvent	= 0xf0,
	FuncEventMask	= 0xf4,
	FuncPresetState	= 0xf8,
	IBCR0           = 0xf8,
	IBCR2           = 0xf9,
	IBIMR0          = 0xfa,
	IBISR0          = 0xfb,
	FuncForceEvent	= 0xfc,
};

enum rtl8168_8101_registers {
	CSIDR			= 0x64,
	CSIAR			= 0x68,
#define	CSIAR_FLAG			0x80000000
#define	CSIAR_WRITE_CMD			0x80000000
#define	CSIAR_BYTE_ENABLE		0x0000f000
#define	CSIAR_ADDR_MASK			0x00000fff
	PMCH			= 0x6f,
#define D3COLD_NO_PLL_DOWN		BIT(7)
#define D3HOT_NO_PLL_DOWN		BIT(6)
#define D3_NO_PLL_DOWN			(BIT(7) | BIT(6))
	EPHYAR			= 0x80,
#define	EPHYAR_FLAG			0x80000000
#define	EPHYAR_WRITE_CMD		0x80000000
#define	EPHYAR_REG_MASK			0x1f
#define	EPHYAR_REG_SHIFT		16
#define	EPHYAR_DATA_MASK		0xffff
	DLLPR			= 0xd0,
#define	PFM_EN				(1 << 6)
#define	TX_10M_PS_EN			(1 << 7)
	DBG_REG			= 0xd1,
#define	FIX_NAK_1			(1 << 4)
#define	FIX_NAK_2			(1 << 3)
	TWSI			= 0xd2,
	MCU			= 0xd3,
#define	NOW_IS_OOB			(1 << 7)
#define	TX_EMPTY			(1 << 5)
#define	RX_EMPTY			(1 << 4)
#define	RXTX_EMPTY			(TX_EMPTY | RX_EMPTY)
#define	EN_NDP				(1 << 3)
#define	EN_OOB_RESET			(1 << 2)
#define	LINK_LIST_RDY			(1 << 1)
	EFUSEAR			= 0xdc,
#define	EFUSEAR_FLAG			0x80000000
#define	EFUSEAR_WRITE_CMD		0x80000000
#define	EFUSEAR_READ_CMD		0x00000000
#define	EFUSEAR_REG_MASK		0x03ff
#define	EFUSEAR_REG_SHIFT		8
#define	EFUSEAR_DATA_MASK		0xff
	MISC_1			= 0xf2,
#define	PFM_D3COLD_EN			(1 << 6)
};

enum rtl8168_registers {
	LED_CTRL		= 0x18,
	LED_FREQ		= 0x1a,
	EEE_LED			= 0x1b,
	ERIDR			= 0x70,
	ERIAR			= 0x74,
#define ERIAR_FLAG			0x80000000
#define ERIAR_WRITE_CMD			0x80000000
#define ERIAR_READ_CMD			0x00000000
#define ERIAR_ADDR_BYTE_ALIGN		4
#define ERIAR_TYPE_SHIFT		16
#define ERIAR_EXGMAC			(0x00 << ERIAR_TYPE_SHIFT)
#define ERIAR_MSIX			(0x01 << ERIAR_TYPE_SHIFT)
#define ERIAR_ASF			(0x02 << ERIAR_TYPE_SHIFT)
#define ERIAR_OOB			(0x02 << ERIAR_TYPE_SHIFT)
#define ERIAR_MASK_SHIFT		12
#define ERIAR_MASK_0001			(0x1 << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_0011			(0x3 << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_0100			(0x4 << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_0101			(0x5 << ERIAR_MASK_SHIFT)
#define ERIAR_MASK_1111			(0xf << ERIAR_MASK_SHIFT)
	EPHY_RXER_NUM		= 0x7c,
	OCPDR			= 0xb0,	/* OCP GPHY access */
#define OCPDR_WRITE_CMD			0x80000000
#define OCPDR_READ_CMD			0x00000000
#define OCPDR_REG_MASK			0x7f
#define OCPDR_GPHY_REG_SHIFT		16
#define OCPDR_DATA_MASK			0xffff
	OCPAR			= 0xb4,
#define OCPAR_FLAG			0x80000000
#define OCPAR_GPHY_WRITE_CMD		0x8000f060
#define OCPAR_GPHY_READ_CMD		0x0000f060
	GPHY_OCP		= 0xb8,
	RDSAR1			= 0xd0,	/* 8168c only. Undocumented on 8168dp */
	MISC			= 0xf0,	/* 8168e only. */
#define TXPLA_RST			(1 << 29)
#define DISABLE_LAN_EN			(1 << 23) /* Enable GPIO pin */
#define PWM_EN				(1 << 22)
#define RXDV_GATED_EN			(1 << 19)
#define EARLY_TALLY_EN			(1 << 16)
};

enum rtl8125_registers {
	LEDSEL0			= 0x18,
	INT_CFG0_8125		= 0x34,
#define INT_CFG0_ENABLE_8125		BIT(0)
#define INT_CFG0_CLKREQEN		BIT(3)
	IntrMask_8125		= 0x38,
	IntrStatus_8125		= 0x3c,
	INT_CFG1_8125		= 0x7a,
	LEDSEL2			= 0x84,
	LEDSEL1			= 0x86,
	TxPoll_8125		= 0x90,
	LEDSEL3			= 0x96,
	MAC0_BKP		= 0x19e0,
	RSS_CTRL_8125		= 0x4500,
	Q_NUM_CTRL_8125		= 0x4800,
	EEE_TXIDLE_TIMER_8125	= 0x6048,
};

#define LEDSEL_MASK_8125	0x23f

#define RX_VLAN_INNER_8125	BIT(22)
#define RX_VLAN_OUTER_8125	BIT(23)
#define RX_VLAN_8125		(RX_VLAN_INNER_8125 | RX_VLAN_OUTER_8125)

#define RX_FETCH_DFLT_8125	(8 << 27)

enum rtl_register_content {
	/* InterruptStatusBits */
	SYSErr		= 0x8000,
	PCSTimeout	= 0x4000,
	SWInt		= 0x0100,
	TxDescUnavail	= 0x0080,
	RxFIFOOver	= 0x0040,
	LinkChg		= 0x0020,
	RxOverflow	= 0x0010,
	TxErr		= 0x0008,
	TxOK		= 0x0004,
	RxErr		= 0x0002,
	RxOK		= 0x0001,

	/* RxStatusDesc */
	RxRWT	= (1 << 22),
	RxRES	= (1 << 21),
	RxRUNT	= (1 << 20),
	RxCRC	= (1 << 19),

	/* ChipCmdBits */
	StopReq		= 0x80,
	CmdReset	= 0x10,
	CmdRxEnb	= 0x08,
	CmdTxEnb	= 0x04,
	RxBufEmpty	= 0x01,

	/* TXPoll register p.5 */
	HPQ		= 0x80,		/* Poll cmd on the high prio queue */
	NPQ		= 0x40,		/* Poll cmd on the low prio queue */
	FSWInt		= 0x01,		/* Forced software interrupt */

	/* Cfg9346Bits */
	Cfg9346_Lock	= 0x00,
	Cfg9346_Unlock	= 0xc0,

	/* rx_mode_bits */
	AcceptErr	= 0x20,
	AcceptRunt	= 0x10,
#define RX_CONFIG_ACCEPT_ERR_MASK	0x30
	AcceptBroadcast	= 0x08,
	AcceptMulticast	= 0x04,
	AcceptMyPhys	= 0x02,
	AcceptAllPhys	= 0x01,
#define RX_CONFIG_ACCEPT_OK_MASK	0x0f
#define RX_CONFIG_ACCEPT_MASK		0x3f

	/* TxConfigBits */
	TxInterFrameGapShift = 24,
	TxDMAShift = 8,	/* DMA burst value (0-7) is shift this many bits */

	/* Config1 register p.24 */
	LEDS1		= (1 << 7),
	LEDS0		= (1 << 6),
	Speed_down	= (1 << 4),
	MEMMAP		= (1 << 3),
	IOMAP		= (1 << 2),
	VPD		= (1 << 1),
	PMEnable	= (1 << 0),	/* Power Management Enable */

	/* Config2 register p. 25 */
	ClkReqEn	= (1 << 7),	/* Clock Request Enable */
	MSIEnable	= (1 << 5),	/* 8169 only. Reserved in the 8168. */
	PCI_Clock_66MHz = 0x01,
	PCI_Clock_33MHz = 0x00,

	/* Config3 register p.25 */
	MagicPacket	= (1 << 5),	/* Wake up when receives a Magic Packet */
	LinkUp		= (1 << 4),	/* Wake up when the cable connection is re-established */
	Jumbo_En0	= (1 << 2),	/* 8168 only. Reserved in the 8168b */
	Rdy_to_L23	= (1 << 1),	/* L23 Enable */
	Beacon_en	= (1 << 0),	/* 8168 only. Reserved in the 8168b */

	/* Config4 register */
	Jumbo_En1	= (1 << 1),	/* 8168 only. Reserved in the 8168b */

	/* Config5 register p.27 */
	BWF		= (1 << 6),	/* Accept Broadcast wakeup frame */
	MWF		= (1 << 5),	/* Accept Multicast wakeup frame */
	UWF		= (1 << 4),	/* Accept Unicast wakeup frame */
	Spi_en		= (1 << 3),
	LanWake		= (1 << 1),	/* LanWake enable/disable */
	PMEStatus	= (1 << 0),	/* PME status can be reset by PCI RST# */
	ASPM_en		= (1 << 0),	/* ASPM enable */

	/* CPlusCmd p.31 */
	EnableBist	= (1 << 15),	// 8168 8101
	Mac_dbgo_oe	= (1 << 14),	// 8168 8101
	EnAnaPLL	= (1 << 14),	// 8169
	Normal_mode	= (1 << 13),	// unused
	Force_half_dup	= (1 << 12),	// 8168 8101
	Force_rxflow_en	= (1 << 11),	// 8168 8101
	Force_txflow_en	= (1 << 10),	// 8168 8101
	Cxpl_dbg_sel	= (1 << 9),	// 8168 8101
	ASF		= (1 << 8),	// 8168 8101
	PktCntrDisable	= (1 << 7),	// 8168 8101
	Mac_dbgo_sel	= 0x001c,	// 8168
	RxVlan		= (1 << 6),
	RxChkSum	= (1 << 5),
	PCIDAC		= (1 << 4),
	PCIMulRW	= (1 << 3),
#define INTT_MASK	GENMASK(1, 0)
#define CPCMD_MASK	(Normal_mode | RxVlan | RxChkSum | INTT_MASK)

	/* rtl8169_PHYstatus */
	TBI_Enable	= 0x80,
	TxFlowCtrl	= 0x40,
	RxFlowCtrl	= 0x20,
	_1000bpsF	= 0x10,
	_100bps		= 0x08,
	_10bps		= 0x04,
	LinkStatus	= 0x02,
	FullDup		= 0x01,

	/* ResetCounterCommand */
	CounterReset	= 0x1,

	/* DumpCounterCommand */
	CounterDump	= 0x8,

	/* magic enable v2 */
	MagicPacket_v2	= (1 << 16),	/* Wake up when receives a Magic Packet */
};

enum rtl_desc_bit {
	/* First doubleword. */
	DescOwn		= (1 << 31), /* Descriptor is owned by NIC */
	RingEnd		= (1 << 30), /* End of descriptor ring */
	FirstFrag	= (1 << 29), /* First segment of a packet */
	LastFrag	= (1 << 28), /* Final segment of a packet */
};

/* Generic case. */
enum rtl_tx_desc_bit {
	/* First doubleword. */
	TD_LSO		= (1 << 27),		/* Large Send Offload */
#define TD_MSS_MAX			0x07ffu	/* MSS value */

	/* Second doubleword. */
	TxVlanTag	= (1 << 17),		/* Add VLAN tag */
};

/* 8169, 8168b and 810x except 8102e. */
enum rtl_tx_desc_bit_0 {
	/* First doubleword. */
#define TD0_MSS_SHIFT			16	/* MSS position (11 bits) */
	TD0_TCP_CS	= (1 << 16),		/* Calculate TCP/IP checksum */
	TD0_UDP_CS	= (1 << 17),		/* Calculate UDP/IP checksum */
	TD0_IP_CS	= (1 << 18),		/* Calculate IP checksum */
};

/* 8102e, 8168c and beyond. */
enum rtl_tx_desc_bit_1 {
	/* First doubleword. */
	TD1_GTSENV4	= (1 << 26),		/* Giant Send for IPv4 */
	TD1_GTSENV6	= (1 << 25),		/* Giant Send for IPv6 */
#define GTTCPHO_SHIFT			18
#define GTTCPHO_MAX			0x7f

	/* Second doubleword. */
#define TCPHO_SHIFT			18
#define TCPHO_MAX			0x3ff
#define TD1_MSS_SHIFT			18	/* MSS position (11 bits) */
	TD1_IPv6_CS	= (1 << 28),		/* Calculate IPv6 checksum */
	TD1_IPv4_CS	= (1 << 29),		/* Calculate IPv4 checksum */
	TD1_TCP_CS	= (1 << 30),		/* Calculate TCP/IP checksum */
	TD1_UDP_CS	= (1 << 31),		/* Calculate UDP/IP checksum */
};

enum rtl_rx_desc_bit {
	/* Rx private */
	PID1		= (1 << 18), /* Protocol ID bit 1/2 */
	PID0		= (1 << 17), /* Protocol ID bit 0/2 */

#define RxProtoUDP	(PID1)
#define RxProtoTCP	(PID0)
#define RxProtoIP	(PID1 | PID0)
#define RxProtoMask	RxProtoIP

	IPFail		= (1 << 16), /* IP checksum failed */
	UDPFail		= (1 << 15), /* UDP/IP checksum failed */
	TCPFail		= (1 << 14), /* TCP/IP checksum failed */

#define RxCSFailMask	(IPFail | UDPFail | TCPFail)

	RxVlanTag	= (1 << 16), /* VLAN tag available */
};

#define RTL_GSO_MAX_SIZE_V1	32000
#define RTL_GSO_MAX_SEGS_V1	24
#define RTL_GSO_MAX_SIZE_V2	64000
#define RTL_GSO_MAX_SEGS_V2	64

struct TxDesc {
	__le32 opts1;
	__le32 opts2;
	__le64 addr;
};

struct RxDesc {
	__le32 opts1;
	__le32 opts2;
	__le64 addr;
};

struct ring_info {
	struct sk_buff	*skb;
	u32		len;
};

struct rtl8169_counters {
	__le64	tx_packets;
	__le64	rx_packets;
	__le64	tx_errors;
	__le32	rx_errors;
	__le16	rx_missed;
	__le16	align_errors;
	__le32	tx_one_collision;
	__le32	tx_multi_collision;
	__le64	rx_unicast;
	__le64	rx_broadcast;
	__le32	rx_multicast;
	__le16	tx_aborted;
	__le16	tx_underrun;
	/* new since RTL8125 */
	__le64 tx_octets;
	__le64 rx_octets;
	__le64 rx_multicast64;
	__le64 tx_unicast64;
	__le64 tx_broadcast64;
	__le64 tx_multicast64;
	__le32 tx_pause_on;
	__le32 tx_pause_off;
	__le32 tx_pause_all;
	__le32 tx_deferred;
	__le32 tx_late_collision;
	__le32 tx_all_collision;
	__le32 tx_aborted32;
	__le32 align_errors32;
	__le32 rx_frame_too_long;
	__le32 rx_runt;
	__le32 rx_pause_on;
	__le32 rx_pause_off;
	__le32 rx_pause_all;
	__le32 rx_unknown_opcode;
	__le32 rx_mac_error;
	__le32 tx_underrun32;
	__le32 rx_mac_missed;
	__le32 rx_tcam_dropped;
	__le32 tdu;
	__le32 rdu;
};

struct rtl8169_tc_offsets {
	bool	inited;
	__le64	tx_errors;
	__le32	tx_multi_collision;
	__le16	tx_aborted;
	__le16	rx_missed;
};

enum rtl_flag {
	RTL_FLAG_TASK_RESET_PENDING,
	RTL_FLAG_TASK_TX_TIMEOUT,
	RTL_FLAG_MAX
};

enum rtl_dash_type {
	RTL_DASH_NONE,
	RTL_DASH_DP,
	RTL_DASH_EP,
	RTL_DASH_25_BP,
};

struct rtl8169_private {
	void __iomem *mmio_addr;	/* memory map physical address */
	struct pci_dev *pci_dev;
	struct net_device *dev;
	struct phy_device *phydev;
	struct napi_struct napi;
	enum mac_version mac_version;
	enum rtl_dash_type dash_type;
	u32 cur_rx; /* Index into the Rx descriptor buffer of next Rx pkt. */
	u32 cur_tx; /* Index into the Tx descriptor buffer of next Rx pkt. */
	u32 dirty_tx;
	struct TxDesc *TxDescArray;	/* 256-aligned Tx descriptor ring */
	struct RxDesc *RxDescArray;	/* 256-aligned Rx descriptor ring */
	dma_addr_t TxPhyAddr;
	dma_addr_t RxPhyAddr;
	struct page *Rx_databuff[NUM_RX_DESC];	/* Rx data buffers */
	struct ring_info tx_skb[NUM_TX_DESC];	/* Tx data buffers */
	u16 cp_cmd;
	u16 tx_lpi_timer;
	u32 irq_mask;
	int irq;
	struct clk *clk;

	struct {
		DECLARE_BITMAP(flags, RTL_FLAG_MAX);
		struct work_struct work;
	} wk;

	raw_spinlock_t mac_ocp_lock;
	struct mutex led_lock;	/* serialize LED ctrl RMW access */

	unsigned supports_gmii:1;
	unsigned aspm_manageable:1;
	unsigned dash_enabled:1;
	dma_addr_t counters_phys_addr;
	struct rtl8169_counters *counters;
	struct rtl8169_tc_offsets tc_offset;
	u32 saved_wolopts;

	const char *fw_name;
	struct rtl_fw *rtl_fw;

	struct r8169_led_classdev *leds;

	u32 ocp_base;
};

typedef void (*rtl_generic_fct)(struct rtl8169_private *tp);

MODULE_AUTHOR("Realtek and the Linux r8169 crew <netdev@vger.kernel.org>");
MODULE_DESCRIPTION("RealTek RTL-8169 Gigabit Ethernet driver");
MODULE_SOFTDEP("pre: realtek");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FIRMWARE_8168D_1);
MODULE_FIRMWARE(FIRMWARE_8168D_2);
MODULE_FIRMWARE(FIRMWARE_8168E_1);
MODULE_FIRMWARE(FIRMWARE_8168E_2);
MODULE_FIRMWARE(FIRMWARE_8168E_3);
MODULE_FIRMWARE(FIRMWARE_8105E_1);
MODULE_FIRMWARE(FIRMWARE_8168F_1);
MODULE_FIRMWARE(FIRMWARE_8168F_2);
MODULE_FIRMWARE(FIRMWARE_8402_1);
MODULE_FIRMWARE(FIRMWARE_8411_1);
MODULE_FIRMWARE(FIRMWARE_8411_2);
MODULE_FIRMWARE(FIRMWARE_8106E_1);
MODULE_FIRMWARE(FIRMWARE_8106E_2);
MODULE_FIRMWARE(FIRMWARE_8168G_2);
MODULE_FIRMWARE(FIRMWARE_8168G_3);
MODULE_FIRMWARE(FIRMWARE_8168H_2);
MODULE_FIRMWARE(FIRMWARE_8168FP_3);
MODULE_FIRMWARE(FIRMWARE_8107E_2);
MODULE_FIRMWARE(FIRMWARE_8125A_3);
MODULE_FIRMWARE(FIRMWARE_8125B_2);
MODULE_FIRMWARE(FIRMWARE_8125D_1);
MODULE_FIRMWARE(FIRMWARE_8125D_2);
MODULE_FIRMWARE(FIRMWARE_8125BP_2);
MODULE_FIRMWARE(FIRMWARE_8126A_2);
MODULE_FIRMWARE(FIRMWARE_8126A_3);
MODULE_FIRMWARE(FIRMWARE_8127A_1);

static inline struct device *tp_to_dev(struct rtl8169_private *tp)
{
	return &tp->pci_dev->dev;
}

static void rtl_lock_config_regs(struct rtl8169_private *tp)
{
	RTL_W8(tp, Cfg9346, Cfg9346_Lock);
}

static void rtl_unlock_config_regs(struct rtl8169_private *tp)
{
	RTL_W8(tp, Cfg9346, Cfg9346_Unlock);
}

static void rtl_pci_commit(struct rtl8169_private *tp)
{
	/* Read an arbitrary register to commit a preceding PCI write */
	RTL_R8(tp, ChipCmd);
}

static void rtl_mod_config2(struct rtl8169_private *tp, u8 clear, u8 set)
{
	u8 val;

	val = RTL_R8(tp, Config2);
	RTL_W8(tp, Config2, (val & ~clear) | set);
}

static void rtl_mod_config5(struct rtl8169_private *tp, u8 clear, u8 set)
{
	u8 val;

	val = RTL_R8(tp, Config5);
	RTL_W8(tp, Config5, (val & ~clear) | set);
}

static void r8169_mod_reg8_cond(struct rtl8169_private *tp, int reg,
				u8 bits, bool cond)
{
	u8 val, old_val;

	old_val = RTL_R8(tp, reg);
	if (cond)
		val = old_val | bits;
	else
		val = old_val & ~bits;
	if (val != old_val)
		RTL_W8(tp, reg, val);
}

static bool rtl_is_8125(struct rtl8169_private *tp)
{
	return tp->mac_version >= RTL_GIGA_MAC_VER_61;
}

static bool rtl_is_8168evl_up(struct rtl8169_private *tp)
{
	return tp->mac_version >= RTL_GIGA_MAC_VER_34 &&
	       tp->mac_version != RTL_GIGA_MAC_VER_39 &&
	       tp->mac_version <= RTL_GIGA_MAC_VER_52;
}

static bool rtl_supports_eee(struct rtl8169_private *tp)
{
	return tp->mac_version >= RTL_GIGA_MAC_VER_34 &&
	       tp->mac_version != RTL_GIGA_MAC_VER_37 &&
	       tp->mac_version != RTL_GIGA_MAC_VER_39;
}

static void rtl_read_mac_from_reg(struct rtl8169_private *tp, u8 *mac, int reg)
{
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		mac[i] = RTL_R8(tp, reg + i);
}

struct rtl_cond {
	bool (*check)(struct rtl8169_private *);
	const char *msg;
};

static bool rtl_loop_wait(struct rtl8169_private *tp, const struct rtl_cond *c,
			  unsigned long usecs, int n, bool high)
{
	int i;

	for (i = 0; i < n; i++) {
		if (c->check(tp) == high)
			return true;
		fsleep(usecs);
	}

	if (net_ratelimit())
		netdev_err(tp->dev, "%s == %d (loop: %d, delay: %lu).\n",
			   c->msg, !high, n, usecs);
	return false;
}

static bool rtl_loop_wait_high(struct rtl8169_private *tp,
			       const struct rtl_cond *c,
			       unsigned long d, int n)
{
	return rtl_loop_wait(tp, c, d, n, true);
}

static bool rtl_loop_wait_low(struct rtl8169_private *tp,
			      const struct rtl_cond *c,
			      unsigned long d, int n)
{
	return rtl_loop_wait(tp, c, d, n, false);
}

#define DECLARE_RTL_COND(name)				\
static bool name ## _check(struct rtl8169_private *);	\
							\
static const struct rtl_cond name = {			\
	.check	= name ## _check,			\
	.msg	= #name					\
};							\
							\
static bool name ## _check(struct rtl8169_private *tp)

int rtl8168_led_mod_ctrl(struct rtl8169_private *tp, u16 mask, u16 val)
{
	struct device *dev = tp_to_dev(tp);
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	mutex_lock(&tp->led_lock);
	RTL_W16(tp, LED_CTRL, (RTL_R16(tp, LED_CTRL) & ~mask) | val);
	mutex_unlock(&tp->led_lock);

	pm_runtime_put_sync(dev);

	return 0;
}

int rtl8168_get_led_mode(struct rtl8169_private *tp)
{
	struct device *dev = tp_to_dev(tp);
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = RTL_R16(tp, LED_CTRL);

	pm_runtime_put_sync(dev);

	return ret;
}

static int rtl8125_get_led_reg(int index)
{
	static const int led_regs[] = { LEDSEL0, LEDSEL1, LEDSEL2, LEDSEL3 };

	return led_regs[index];
}

int rtl8125_set_led_mode(struct rtl8169_private *tp, int index, u16 mode)
{
	int reg = rtl8125_get_led_reg(index);
	struct device *dev = tp_to_dev(tp);
	int ret;
	u16 val;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	mutex_lock(&tp->led_lock);
	val = RTL_R16(tp, reg) & ~LEDSEL_MASK_8125;
	RTL_W16(tp, reg, val | mode);
	mutex_unlock(&tp->led_lock);

	pm_runtime_put_sync(dev);

	return 0;
}

int rtl8125_get_led_mode(struct rtl8169_private *tp, int index)
{
	int reg = rtl8125_get_led_reg(index);
	struct device *dev = tp_to_dev(tp);
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = RTL_R16(tp, reg);

	pm_runtime_put_sync(dev);

	return ret;
}

void r8169_get_led_name(struct rtl8169_private *tp, int idx,
			char *buf, int buf_len)
{
	struct pci_dev *pdev = tp->pci_dev;
	char pdom[8], pfun[8];
	int domain;

	domain = pci_domain_nr(pdev->bus);
	if (domain)
		snprintf(pdom, sizeof(pdom), "P%d", domain);
	else
		pdom[0] = '\0';

	if (pdev->multifunction)
		snprintf(pfun, sizeof(pfun), "f%d", PCI_FUNC(pdev->devfn));
	else
		pfun[0] = '\0';

	snprintf(buf, buf_len, "en%sp%ds%d%s-%d::lan", pdom, pdev->bus->number,
		 PCI_SLOT(pdev->devfn), pfun, idx);
}

static void r8168fp_adjust_ocp_cmd(struct rtl8169_private *tp, u32 *cmd, int type)
{
	/* based on RTL8168FP_OOBMAC_BASE in vendor driver */
	if (type == ERIAR_OOB && tp->mac_version == RTL_GIGA_MAC_VER_52)
		*cmd |= 0xf70 << 18;
}

DECLARE_RTL_COND(rtl_eriar_cond)
{
	return RTL_R32(tp, ERIAR) & ERIAR_FLAG;
}

static void _rtl_eri_write(struct rtl8169_private *tp, int addr, u32 mask,
			   u32 val, int type)
{
	u32 cmd = ERIAR_WRITE_CMD | type | mask | addr;

	if (WARN(addr & 3 || !mask, "addr: 0x%x, mask: 0x%08x\n", addr, mask))
		return;

	RTL_W32(tp, ERIDR, val);
	r8168fp_adjust_ocp_cmd(tp, &cmd, type);
	RTL_W32(tp, ERIAR, cmd);

	rtl_loop_wait_low(tp, &rtl_eriar_cond, 100, 100);
}

static void rtl_eri_write(struct rtl8169_private *tp, int addr, u32 mask,
			  u32 val)
{
	_rtl_eri_write(tp, addr, mask, val, ERIAR_EXGMAC);
}

static u32 _rtl_eri_read(struct rtl8169_private *tp, int addr, int type)
{
	u32 cmd = ERIAR_READ_CMD | type | ERIAR_MASK_1111 | addr;

	r8168fp_adjust_ocp_cmd(tp, &cmd, type);
	RTL_W32(tp, ERIAR, cmd);

	return rtl_loop_wait_high(tp, &rtl_eriar_cond, 100, 100) ?
		RTL_R32(tp, ERIDR) : ~0;
}

static u32 rtl_eri_read(struct rtl8169_private *tp, int addr)
{
	return _rtl_eri_read(tp, addr, ERIAR_EXGMAC);
}

static void rtl_w0w1_eri(struct rtl8169_private *tp, int addr, u32 p, u32 m)
{
	u32 val = rtl_eri_read(tp, addr);

	rtl_eri_write(tp, addr, ERIAR_MASK_1111, (val & ~m) | p);
}

static void rtl_eri_set_bits(struct rtl8169_private *tp, int addr, u32 p)
{
	rtl_w0w1_eri(tp, addr, p, 0);
}

static void rtl_eri_clear_bits(struct rtl8169_private *tp, int addr, u32 m)
{
	rtl_w0w1_eri(tp, addr, 0, m);
}

static bool rtl_ocp_reg_failure(u32 reg)
{
	return WARN_ONCE(reg & 0xffff0001, "Invalid ocp reg %x!\n", reg);
}

DECLARE_RTL_COND(rtl_ocp_gphy_cond)
{
	return RTL_R32(tp, GPHY_OCP) & OCPAR_FLAG;
}

static void r8168_phy_ocp_write(struct rtl8169_private *tp, u32 reg, u32 data)
{
	if (rtl_ocp_reg_failure(reg))
		return;

	RTL_W32(tp, GPHY_OCP, OCPAR_FLAG | (reg << 15) | data);

	rtl_loop_wait_low(tp, &rtl_ocp_gphy_cond, 25, 10);
}

static int r8168_phy_ocp_read(struct rtl8169_private *tp, u32 reg)
{
	if (rtl_ocp_reg_failure(reg))
		return 0;

	RTL_W32(tp, GPHY_OCP, reg << 15);

	return rtl_loop_wait_high(tp, &rtl_ocp_gphy_cond, 25, 10) ?
		(RTL_R32(tp, GPHY_OCP) & 0xffff) : -ETIMEDOUT;
}

static void __r8168_mac_ocp_write(struct rtl8169_private *tp, u32 reg, u32 data)
{
	if (rtl_ocp_reg_failure(reg))
		return;

	RTL_W32(tp, OCPDR, OCPAR_FLAG | (reg << 15) | data);
}

static void r8168_mac_ocp_write(struct rtl8169_private *tp, u32 reg, u32 data)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&tp->mac_ocp_lock, flags);
	__r8168_mac_ocp_write(tp, reg, data);
	raw_spin_unlock_irqrestore(&tp->mac_ocp_lock, flags);
}

static u16 __r8168_mac_ocp_read(struct rtl8169_private *tp, u32 reg)
{
	if (rtl_ocp_reg_failure(reg))
		return 0;

	RTL_W32(tp, OCPDR, reg << 15);

	return RTL_R32(tp, OCPDR);
}

static u16 r8168_mac_ocp_read(struct rtl8169_private *tp, u32 reg)
{
	unsigned long flags;
	u16 val;

	raw_spin_lock_irqsave(&tp->mac_ocp_lock, flags);
	val = __r8168_mac_ocp_read(tp, reg);
	raw_spin_unlock_irqrestore(&tp->mac_ocp_lock, flags);

	return val;
}

static void r8168_mac_ocp_modify(struct rtl8169_private *tp, u32 reg, u16 mask,
				 u16 set)
{
	unsigned long flags;
	u16 data;

	raw_spin_lock_irqsave(&tp->mac_ocp_lock, flags);
	data = __r8168_mac_ocp_read(tp, reg);
	__r8168_mac_ocp_write(tp, reg, (data & ~mask) | set);
	raw_spin_unlock_irqrestore(&tp->mac_ocp_lock, flags);
}

/* Work around a hw issue with RTL8168g PHY, the quirk disables
 * PHY MCU interrupts before PHY power-down.
 */
static void rtl8168g_phy_suspend_quirk(struct rtl8169_private *tp, int value)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_40:
		if (value & BMCR_RESET || !(value & BMCR_PDOWN))
			rtl_eri_set_bits(tp, 0x1a8, 0xfc000000);
		else
			rtl_eri_clear_bits(tp, 0x1a8, 0xfc000000);
		break;
	default:
		break;
	}
};

static void r8168g_mdio_write(struct rtl8169_private *tp, int reg, int value)
{
	if (reg == 0x1f) {
		tp->ocp_base = value ? value << 4 : OCP_STD_PHY_BASE;
		return;
	}

	if (tp->ocp_base != OCP_STD_PHY_BASE)
		reg -= 0x10;

	if (tp->ocp_base == OCP_STD_PHY_BASE && reg == MII_BMCR)
		rtl8168g_phy_suspend_quirk(tp, value);

	r8168_phy_ocp_write(tp, tp->ocp_base + reg * 2, value);
}

static int r8168g_mdio_read(struct rtl8169_private *tp, int reg)
{
	if (reg == 0x1f)
		return tp->ocp_base == OCP_STD_PHY_BASE ? 0 : tp->ocp_base >> 4;

	if (tp->ocp_base != OCP_STD_PHY_BASE)
		reg -= 0x10;

	return r8168_phy_ocp_read(tp, tp->ocp_base + reg * 2);
}

static void mac_mcu_write(struct rtl8169_private *tp, int reg, int value)
{
	if (reg == 0x1f) {
		tp->ocp_base = value << 4;
		return;
	}

	r8168_mac_ocp_write(tp, tp->ocp_base + reg, value);
}

static int mac_mcu_read(struct rtl8169_private *tp, int reg)
{
	return r8168_mac_ocp_read(tp, tp->ocp_base + reg);
}

DECLARE_RTL_COND(rtl_phyar_cond)
{
	return RTL_R32(tp, PHYAR) & 0x80000000;
}

static void r8169_mdio_write(struct rtl8169_private *tp, int reg, int value)
{
	RTL_W32(tp, PHYAR, 0x80000000 | (reg & 0x1f) << 16 | (value & 0xffff));

	rtl_loop_wait_low(tp, &rtl_phyar_cond, 25, 20);
	/*
	 * According to hardware specs a 20us delay is required after write
	 * complete indication, but before sending next command.
	 */
	udelay(20);
}

static int r8169_mdio_read(struct rtl8169_private *tp, int reg)
{
	int value;

	RTL_W32(tp, PHYAR, 0x0 | (reg & 0x1f) << 16);

	value = rtl_loop_wait_high(tp, &rtl_phyar_cond, 25, 20) ?
		RTL_R32(tp, PHYAR) & 0xffff : -ETIMEDOUT;

	/*
	 * According to hardware specs a 20us delay is required after read
	 * complete indication, but before sending next command.
	 */
	udelay(20);

	return value;
}

DECLARE_RTL_COND(rtl_ocpar_cond)
{
	return RTL_R32(tp, OCPAR) & OCPAR_FLAG;
}

#define R8168DP_1_MDIO_ACCESS_BIT	0x00020000

static void r8168dp_2_mdio_start(struct rtl8169_private *tp)
{
	RTL_W32(tp, 0xd0, RTL_R32(tp, 0xd0) & ~R8168DP_1_MDIO_ACCESS_BIT);
}

static void r8168dp_2_mdio_stop(struct rtl8169_private *tp)
{
	RTL_W32(tp, 0xd0, RTL_R32(tp, 0xd0) | R8168DP_1_MDIO_ACCESS_BIT);
}

static void r8168dp_2_mdio_write(struct rtl8169_private *tp, int reg, int value)
{
	r8168dp_2_mdio_start(tp);

	r8169_mdio_write(tp, reg, value);

	r8168dp_2_mdio_stop(tp);
}

static int r8168dp_2_mdio_read(struct rtl8169_private *tp, int reg)
{
	int value;

	/* Work around issue with chip reporting wrong PHY ID */
	if (reg == MII_PHYSID2)
		return 0xc912;

	r8168dp_2_mdio_start(tp);

	value = r8169_mdio_read(tp, reg);

	r8168dp_2_mdio_stop(tp);

	return value;
}

static void rtl_writephy(struct rtl8169_private *tp, int location, int val)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_28:
	case RTL_GIGA_MAC_VER_31:
		r8168dp_2_mdio_write(tp, location, val);
		break;
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_LAST:
		r8168g_mdio_write(tp, location, val);
		break;
	default:
		r8169_mdio_write(tp, location, val);
		break;
	}
}

static int rtl_readphy(struct rtl8169_private *tp, int location)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_28:
	case RTL_GIGA_MAC_VER_31:
		return r8168dp_2_mdio_read(tp, location);
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_LAST:
		return r8168g_mdio_read(tp, location);
	default:
		return r8169_mdio_read(tp, location);
	}
}

DECLARE_RTL_COND(rtl_ephyar_cond)
{
	return RTL_R32(tp, EPHYAR) & EPHYAR_FLAG;
}

static void rtl_ephy_write(struct rtl8169_private *tp, int reg_addr, int value)
{
	RTL_W32(tp, EPHYAR, EPHYAR_WRITE_CMD | (value & EPHYAR_DATA_MASK) |
		(reg_addr & EPHYAR_REG_MASK) << EPHYAR_REG_SHIFT);

	rtl_loop_wait_low(tp, &rtl_ephyar_cond, 10, 100);

	udelay(10);
}

static u16 rtl_ephy_read(struct rtl8169_private *tp, int reg_addr)
{
	RTL_W32(tp, EPHYAR, (reg_addr & EPHYAR_REG_MASK) << EPHYAR_REG_SHIFT);

	return rtl_loop_wait_high(tp, &rtl_ephyar_cond, 10, 100) ?
		RTL_R32(tp, EPHYAR) & EPHYAR_DATA_MASK : ~0;
}

static u32 r8168dp_ocp_read(struct rtl8169_private *tp, u16 reg)
{
	RTL_W32(tp, OCPAR, 0x0fu << 12 | (reg & 0x0fff));
	return rtl_loop_wait_high(tp, &rtl_ocpar_cond, 100, 20) ?
		RTL_R32(tp, OCPDR) : ~0;
}

static u32 r8168ep_ocp_read(struct rtl8169_private *tp, u16 reg)
{
	return _rtl_eri_read(tp, reg, ERIAR_OOB);
}

static void r8168dp_ocp_write(struct rtl8169_private *tp, u8 mask, u16 reg,
			      u32 data)
{
	RTL_W32(tp, OCPDR, data);
	RTL_W32(tp, OCPAR, OCPAR_FLAG | ((u32)mask & 0x0f) << 12 | (reg & 0x0fff));
	rtl_loop_wait_low(tp, &rtl_ocpar_cond, 100, 20);
}

static void r8168ep_ocp_write(struct rtl8169_private *tp, u8 mask, u16 reg,
			      u32 data)
{
	_rtl_eri_write(tp, reg, ((u32)mask & 0x0f) << ERIAR_MASK_SHIFT,
		       data, ERIAR_OOB);
}

static void r8168dp_oob_notify(struct rtl8169_private *tp, u8 cmd)
{
	rtl_eri_write(tp, 0xe8, ERIAR_MASK_0001, cmd);

	r8168dp_ocp_write(tp, 0x1, 0x30, 0x00000001);
}

#define OOB_CMD_RESET		0x00
#define OOB_CMD_DRIVER_START	0x05
#define OOB_CMD_DRIVER_STOP	0x06

static u16 rtl8168_get_ocp_reg(struct rtl8169_private *tp)
{
	return (tp->mac_version == RTL_GIGA_MAC_VER_31) ? 0xb8 : 0x10;
}

DECLARE_RTL_COND(rtl_dp_ocp_read_cond)
{
	u16 reg;

	reg = rtl8168_get_ocp_reg(tp);

	return r8168dp_ocp_read(tp, reg) & 0x00000800;
}

DECLARE_RTL_COND(rtl_ep_ocp_read_cond)
{
	return r8168ep_ocp_read(tp, 0x124) & 0x00000001;
}

DECLARE_RTL_COND(rtl_ocp_tx_cond)
{
	return RTL_R8(tp, IBISR0) & 0x20;
}

static void rtl8168ep_stop_cmac(struct rtl8169_private *tp)
{
	RTL_W8(tp, IBCR2, RTL_R8(tp, IBCR2) & ~0x01);
	rtl_loop_wait_high(tp, &rtl_ocp_tx_cond, 50000, 2000);
	RTL_W8(tp, IBISR0, RTL_R8(tp, IBISR0) | 0x20);
	RTL_W8(tp, IBCR0, RTL_R8(tp, IBCR0) & ~0x01);
}

static void rtl8168dp_driver_start(struct rtl8169_private *tp)
{
	r8168dp_oob_notify(tp, OOB_CMD_DRIVER_START);
	if (tp->dash_enabled)
		rtl_loop_wait_high(tp, &rtl_dp_ocp_read_cond, 10000, 10);
}

static void rtl8168ep_driver_start(struct rtl8169_private *tp)
{
	r8168ep_ocp_write(tp, 0x01, 0x180, OOB_CMD_DRIVER_START);
	r8168ep_ocp_write(tp, 0x01, 0x30, r8168ep_ocp_read(tp, 0x30) | 0x01);
	if (tp->dash_enabled)
		rtl_loop_wait_high(tp, &rtl_ep_ocp_read_cond, 10000, 30);
}

static void rtl8125bp_driver_start(struct rtl8169_private *tp)
{
	r8168ep_ocp_write(tp, 0x01, 0x14, OOB_CMD_DRIVER_START);
	r8168ep_ocp_write(tp, 0x01, 0x18, 0x00);
	r8168ep_ocp_write(tp, 0x01, 0x10, 0x01);
}

static void rtl8168_driver_start(struct rtl8169_private *tp)
{
	if (tp->dash_type == RTL_DASH_DP)
		rtl8168dp_driver_start(tp);
	else if (tp->dash_type == RTL_DASH_25_BP)
		rtl8125bp_driver_start(tp);
	else
		rtl8168ep_driver_start(tp);
}

static void rtl8168dp_driver_stop(struct rtl8169_private *tp)
{
	r8168dp_oob_notify(tp, OOB_CMD_DRIVER_STOP);
	if (tp->dash_enabled)
		rtl_loop_wait_low(tp, &rtl_dp_ocp_read_cond, 10000, 10);
}

static void rtl8168ep_driver_stop(struct rtl8169_private *tp)
{
	rtl8168ep_stop_cmac(tp);
	r8168ep_ocp_write(tp, 0x01, 0x180, OOB_CMD_DRIVER_STOP);
	r8168ep_ocp_write(tp, 0x01, 0x30, r8168ep_ocp_read(tp, 0x30) | 0x01);
	if (tp->dash_enabled)
		rtl_loop_wait_low(tp, &rtl_ep_ocp_read_cond, 10000, 10);
}

static void rtl8125bp_driver_stop(struct rtl8169_private *tp)
{
	r8168ep_ocp_write(tp, 0x01, 0x14, OOB_CMD_DRIVER_STOP);
	r8168ep_ocp_write(tp, 0x01, 0x18, 0x00);
	r8168ep_ocp_write(tp, 0x01, 0x10, 0x01);
}

static void rtl8168_driver_stop(struct rtl8169_private *tp)
{
	if (tp->dash_type == RTL_DASH_DP)
		rtl8168dp_driver_stop(tp);
	else if (tp->dash_type == RTL_DASH_25_BP)
		rtl8125bp_driver_stop(tp);
	else
		rtl8168ep_driver_stop(tp);
}

static bool r8168dp_check_dash(struct rtl8169_private *tp)
{
	u16 reg = rtl8168_get_ocp_reg(tp);

	return r8168dp_ocp_read(tp, reg) & BIT(15);
}

static bool r8168ep_check_dash(struct rtl8169_private *tp)
{
	return r8168ep_ocp_read(tp, 0x128) & BIT(0);
}

static bool rtl_dash_is_enabled(struct rtl8169_private *tp)
{
	switch (tp->dash_type) {
	case RTL_DASH_DP:
		return r8168dp_check_dash(tp);
	case RTL_DASH_EP:
	case RTL_DASH_25_BP:
		return r8168ep_check_dash(tp);
	default:
		return false;
	}
}

static enum rtl_dash_type rtl_get_dash_type(struct rtl8169_private *tp)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_28:
	case RTL_GIGA_MAC_VER_31:
		return RTL_DASH_DP;
	case RTL_GIGA_MAC_VER_51 ... RTL_GIGA_MAC_VER_52:
		return RTL_DASH_EP;
	case RTL_GIGA_MAC_VER_66:
		return RTL_DASH_25_BP;
	default:
		return RTL_DASH_NONE;
	}
}

static void rtl_set_d3_pll_down(struct rtl8169_private *tp, bool enable)
{
	if (tp->mac_version >= RTL_GIGA_MAC_VER_25 &&
	    tp->mac_version != RTL_GIGA_MAC_VER_28 &&
	    tp->mac_version != RTL_GIGA_MAC_VER_31 &&
	    tp->mac_version != RTL_GIGA_MAC_VER_38)
		r8169_mod_reg8_cond(tp, PMCH, D3_NO_PLL_DOWN, !enable);
}

static void rtl_reset_packet_filter(struct rtl8169_private *tp)
{
	rtl_eri_clear_bits(tp, 0xdc, BIT(0));
	rtl_eri_set_bits(tp, 0xdc, BIT(0));
}

DECLARE_RTL_COND(rtl_efusear_cond)
{
	return RTL_R32(tp, EFUSEAR) & EFUSEAR_FLAG;
}

u8 rtl8168d_efuse_read(struct rtl8169_private *tp, int reg_addr)
{
	RTL_W32(tp, EFUSEAR, (reg_addr & EFUSEAR_REG_MASK) << EFUSEAR_REG_SHIFT);

	return rtl_loop_wait_high(tp, &rtl_efusear_cond, 100, 300) ?
		RTL_R32(tp, EFUSEAR) & EFUSEAR_DATA_MASK : ~0;
}

static u32 rtl_get_events(struct rtl8169_private *tp)
{
	if (rtl_is_8125(tp))
		return RTL_R32(tp, IntrStatus_8125);
	else
		return RTL_R16(tp, IntrStatus);
}

static void rtl_ack_events(struct rtl8169_private *tp, u32 bits)
{
	if (rtl_is_8125(tp))
		RTL_W32(tp, IntrStatus_8125, bits);
	else
		RTL_W16(tp, IntrStatus, bits);
}

static void rtl_irq_disable(struct rtl8169_private *tp)
{
	if (rtl_is_8125(tp))
		RTL_W32(tp, IntrMask_8125, 0);
	else
		RTL_W16(tp, IntrMask, 0);
}

static void rtl_irq_enable(struct rtl8169_private *tp)
{
	if (rtl_is_8125(tp))
		RTL_W32(tp, IntrMask_8125, tp->irq_mask);
	else
		RTL_W16(tp, IntrMask, tp->irq_mask);
}

static void rtl8169_irq_mask_and_ack(struct rtl8169_private *tp)
{
	rtl_irq_disable(tp);
	rtl_ack_events(tp, 0xffffffff);
	rtl_pci_commit(tp);
}

static void rtl_link_chg_patch(struct rtl8169_private *tp)
{
	struct phy_device *phydev = tp->phydev;

	if (tp->mac_version == RTL_GIGA_MAC_VER_34 ||
	    tp->mac_version == RTL_GIGA_MAC_VER_38) {
		if (phydev->speed == SPEED_1000) {
			rtl_eri_write(tp, 0x1bc, ERIAR_MASK_1111, 0x00000011);
			rtl_eri_write(tp, 0x1dc, ERIAR_MASK_1111, 0x00000005);
		} else if (phydev->speed == SPEED_100) {
			rtl_eri_write(tp, 0x1bc, ERIAR_MASK_1111, 0x0000001f);
			rtl_eri_write(tp, 0x1dc, ERIAR_MASK_1111, 0x00000005);
		} else {
			rtl_eri_write(tp, 0x1bc, ERIAR_MASK_1111, 0x0000001f);
			rtl_eri_write(tp, 0x1dc, ERIAR_MASK_1111, 0x0000003f);
		}
		rtl_reset_packet_filter(tp);
	} else if (tp->mac_version == RTL_GIGA_MAC_VER_35 ||
		   tp->mac_version == RTL_GIGA_MAC_VER_36) {
		if (phydev->speed == SPEED_1000) {
			rtl_eri_write(tp, 0x1bc, ERIAR_MASK_1111, 0x00000011);
			rtl_eri_write(tp, 0x1dc, ERIAR_MASK_1111, 0x00000005);
		} else {
			rtl_eri_write(tp, 0x1bc, ERIAR_MASK_1111, 0x0000001f);
			rtl_eri_write(tp, 0x1dc, ERIAR_MASK_1111, 0x0000003f);
		}
	} else if (tp->mac_version == RTL_GIGA_MAC_VER_37) {
		if (phydev->speed == SPEED_10) {
			rtl_eri_write(tp, 0x1d0, ERIAR_MASK_0011, 0x4d02);
			rtl_eri_write(tp, 0x1dc, ERIAR_MASK_0011, 0x0060a);
		} else {
			rtl_eri_write(tp, 0x1d0, ERIAR_MASK_0011, 0x0000);
		}
	}
}

#define WAKE_ANY (WAKE_PHY | WAKE_MAGIC | WAKE_UCAST | WAKE_BCAST | WAKE_MCAST)

static void rtl8169_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	wol->supported = WAKE_ANY;
	wol->wolopts = tp->saved_wolopts;
}

static void __rtl8169_set_wol(struct rtl8169_private *tp, u32 wolopts)
{
	rtl_unlock_config_regs(tp);

	if (rtl_is_8168evl_up(tp)) {
		if (wolopts & WAKE_MAGIC)
			rtl_eri_set_bits(tp, 0x0dc, MagicPacket_v2);
		else
			rtl_eri_clear_bits(tp, 0x0dc, MagicPacket_v2);
	} else if (rtl_is_8125(tp)) {
		if (wolopts & WAKE_MAGIC)
			r8168_mac_ocp_modify(tp, 0xc0b6, 0, BIT(0));
		else
			r8168_mac_ocp_modify(tp, 0xc0b6, BIT(0), 0);
	} else {
		r8169_mod_reg8_cond(tp, Config3, MagicPacket,
				    wolopts & WAKE_MAGIC);
	}

	r8169_mod_reg8_cond(tp, Config3, LinkUp, wolopts & WAKE_PHY);
	if (rtl_is_8125(tp))
		r8168_mac_ocp_modify(tp, 0xe0c6, 0x3f,
				     wolopts & WAKE_PHY ? 0x13 : 0);
	r8169_mod_reg8_cond(tp, Config5, UWF, wolopts & WAKE_UCAST);
	r8169_mod_reg8_cond(tp, Config5, BWF, wolopts & WAKE_BCAST);
	r8169_mod_reg8_cond(tp, Config5, MWF, wolopts & WAKE_MCAST);
	r8169_mod_reg8_cond(tp, Config5, LanWake, wolopts);

	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_02 ... RTL_GIGA_MAC_VER_06:
		r8169_mod_reg8_cond(tp, Config1, PMEnable, wolopts);
		break;
	case RTL_GIGA_MAC_VER_34:
	case RTL_GIGA_MAC_VER_37:
	case RTL_GIGA_MAC_VER_39 ... RTL_GIGA_MAC_VER_LAST:
		r8169_mod_reg8_cond(tp, Config2, PME_SIGNAL, wolopts);
		break;
	default:
		break;
	}

	rtl_lock_config_regs(tp);

	device_set_wakeup_enable(tp_to_dev(tp), wolopts);

	if (!tp->dash_enabled) {
		rtl_set_d3_pll_down(tp, !wolopts);
		tp->dev->ethtool->wol_enabled = wolopts ? 1 : 0;
	}
}

static int rtl8169_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (wol->wolopts & ~WAKE_ANY)
		return -EINVAL;

	tp->saved_wolopts = wol->wolopts;
	__rtl8169_set_wol(tp, tp->saved_wolopts);

	return 0;
}

static void rtl8169_get_drvinfo(struct net_device *dev,
				struct ethtool_drvinfo *info)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	struct rtl_fw *rtl_fw = tp->rtl_fw;

	strscpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
	strscpy(info->bus_info, pci_name(tp->pci_dev), sizeof(info->bus_info));
	BUILD_BUG_ON(sizeof(info->fw_version) < sizeof(rtl_fw->version));
	if (rtl_fw)
		strscpy(info->fw_version, rtl_fw->version,
			sizeof(info->fw_version));
}

static int rtl8169_get_regs_len(struct net_device *dev)
{
	return R8169_REGS_SIZE;
}

static netdev_features_t rtl8169_fix_features(struct net_device *dev,
	netdev_features_t features)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (dev->mtu > TD_MSS_MAX)
		features &= ~NETIF_F_ALL_TSO;

	if (dev->mtu > ETH_DATA_LEN &&
	    tp->mac_version > RTL_GIGA_MAC_VER_06)
		features &= ~(NETIF_F_CSUM_MASK | NETIF_F_ALL_TSO);

	return features;
}

static void rtl_set_rx_config_features(struct rtl8169_private *tp,
				       netdev_features_t features)
{
	u32 rx_config = RTL_R32(tp, RxConfig);

	if (features & NETIF_F_RXALL)
		rx_config |= RX_CONFIG_ACCEPT_ERR_MASK;
	else
		rx_config &= ~RX_CONFIG_ACCEPT_ERR_MASK;

	if (rtl_is_8125(tp)) {
		if (features & NETIF_F_HW_VLAN_CTAG_RX)
			rx_config |= RX_VLAN_8125;
		else
			rx_config &= ~RX_VLAN_8125;
	}

	RTL_W32(tp, RxConfig, rx_config);
}

static int rtl8169_set_features(struct net_device *dev,
				netdev_features_t features)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	rtl_set_rx_config_features(tp, features);

	if (features & NETIF_F_RXCSUM)
		tp->cp_cmd |= RxChkSum;
	else
		tp->cp_cmd &= ~RxChkSum;

	if (!rtl_is_8125(tp)) {
		if (features & NETIF_F_HW_VLAN_CTAG_RX)
			tp->cp_cmd |= RxVlan;
		else
			tp->cp_cmd &= ~RxVlan;
	}

	RTL_W16(tp, CPlusCmd, tp->cp_cmd);
	rtl_pci_commit(tp);

	return 0;
}

static inline u32 rtl8169_tx_vlan_tag(struct sk_buff *skb)
{
	return (skb_vlan_tag_present(skb)) ?
		TxVlanTag | swab16(skb_vlan_tag_get(skb)) : 0x00;
}

static void rtl8169_rx_vlan_tag(struct RxDesc *desc, struct sk_buff *skb)
{
	u32 opts2 = le32_to_cpu(desc->opts2);

	if (opts2 & RxVlanTag)
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), swab16(opts2 & 0xffff));
}

static void rtl8169_get_regs(struct net_device *dev, struct ethtool_regs *regs,
			     void *p)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	u32 __iomem *data = tp->mmio_addr;
	u32 *dw = p;
	int i;

	for (i = 0; i < R8169_REGS_SIZE; i += 4)
		memcpy_fromio(dw++, data++, 4);
}

static const char rtl8169_gstrings[][ETH_GSTRING_LEN] = {
	"tx_packets",
	"rx_packets",
	"tx_errors",
	"rx_errors",
	"rx_missed",
	"align_errors",
	"tx_single_collisions",
	"tx_multi_collisions",
	"unicast",
	"broadcast",
	"multicast",
	"tx_aborted",
	"tx_underrun",
};

static int rtl8169_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(rtl8169_gstrings);
	default:
		return -EOPNOTSUPP;
	}
}

DECLARE_RTL_COND(rtl_counters_cond)
{
	return RTL_R32(tp, CounterAddrLow) & (CounterReset | CounterDump);
}

static void rtl8169_do_counters(struct rtl8169_private *tp, u32 counter_cmd)
{
	u32 cmd = lower_32_bits(tp->counters_phys_addr);

	RTL_W32(tp, CounterAddrHigh, upper_32_bits(tp->counters_phys_addr));
	rtl_pci_commit(tp);
	RTL_W32(tp, CounterAddrLow, cmd);
	RTL_W32(tp, CounterAddrLow, cmd | counter_cmd);

	rtl_loop_wait_low(tp, &rtl_counters_cond, 10, 1000);
}

static void rtl8169_update_counters(struct rtl8169_private *tp)
{
	u8 val = RTL_R8(tp, ChipCmd);

	/*
	 * Some chips are unable to dump tally counters when the receiver
	 * is disabled. If 0xff chip may be in a PCI power-save state.
	 */
	if (val & CmdRxEnb && val != 0xff)
		rtl8169_do_counters(tp, CounterDump);
}

static void rtl8169_init_counter_offsets(struct rtl8169_private *tp)
{
	struct rtl8169_counters *counters = tp->counters;

	/*
	 * rtl8169_init_counter_offsets is called from rtl_open.  On chip
	 * versions prior to RTL_GIGA_MAC_VER_19 the tally counters are only
	 * reset by a power cycle, while the counter values collected by the
	 * driver are reset at every driver unload/load cycle.
	 *
	 * To make sure the HW values returned by @get_stats64 match the SW
	 * values, we collect the initial values at first open(*) and use them
	 * as offsets to normalize the values returned by @get_stats64.
	 *
	 * (*) We can't call rtl8169_init_counter_offsets from rtl_init_one
	 * for the reason stated in rtl8169_update_counters; CmdRxEnb is only
	 * set at open time by rtl_hw_start.
	 */

	if (tp->tc_offset.inited)
		return;

	if (tp->mac_version >= RTL_GIGA_MAC_VER_19) {
		rtl8169_do_counters(tp, CounterReset);
	} else {
		rtl8169_update_counters(tp);
		tp->tc_offset.tx_errors = counters->tx_errors;
		tp->tc_offset.tx_multi_collision = counters->tx_multi_collision;
		tp->tc_offset.tx_aborted = counters->tx_aborted;
		tp->tc_offset.rx_missed = counters->rx_missed;
	}

	tp->tc_offset.inited = true;
}

static void rtl8169_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	struct rtl8169_counters *counters;

	counters = tp->counters;
	rtl8169_update_counters(tp);

	data[0] = le64_to_cpu(counters->tx_packets);
	data[1] = le64_to_cpu(counters->rx_packets);
	data[2] = le64_to_cpu(counters->tx_errors);
	data[3] = le32_to_cpu(counters->rx_errors);
	data[4] = le16_to_cpu(counters->rx_missed);
	data[5] = le16_to_cpu(counters->align_errors);
	data[6] = le32_to_cpu(counters->tx_one_collision);
	data[7] = le32_to_cpu(counters->tx_multi_collision);
	data[8] = le64_to_cpu(counters->rx_unicast);
	data[9] = le64_to_cpu(counters->rx_broadcast);
	data[10] = le32_to_cpu(counters->rx_multicast);
	data[11] = le16_to_cpu(counters->tx_aborted);
	data[12] = le16_to_cpu(counters->tx_underrun);
}

static void rtl8169_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	switch(stringset) {
	case ETH_SS_STATS:
		memcpy(data, rtl8169_gstrings, sizeof(rtl8169_gstrings));
		break;
	}
}

/*
 * Interrupt coalescing
 *
 * > 1 - the availability of the IntrMitigate (0xe2) register through the
 * >     8169, 8168 and 810x line of chipsets
 *
 * 8169, 8168, and 8136(810x) serial chipsets support it.
 *
 * > 2 - the Tx timer unit at gigabit speed
 *
 * The unit of the timer depends on both the speed and the setting of CPlusCmd
 * (0xe0) bit 1 and bit 0.
 *
 * For 8169
 * bit[1:0] \ speed        1000M           100M            10M
 * 0 0                     320ns           2.56us          40.96us
 * 0 1                     2.56us          20.48us         327.7us
 * 1 0                     5.12us          40.96us         655.4us
 * 1 1                     10.24us         81.92us         1.31ms
 *
 * For the other
 * bit[1:0] \ speed        1000M           100M            10M
 * 0 0                     5us             2.56us          40.96us
 * 0 1                     40us            20.48us         327.7us
 * 1 0                     80us            40.96us         655.4us
 * 1 1                     160us           81.92us         1.31ms
 */

/* rx/tx scale factors for all CPlusCmd[0:1] cases */
struct rtl_coalesce_info {
	u32 speed;
	u32 scale_nsecs[4];
};

/* produce array with base delay *1, *8, *8*2, *8*2*2 */
#define COALESCE_DELAY(d) { (d), 8 * (d), 16 * (d), 32 * (d) }

static const struct rtl_coalesce_info rtl_coalesce_info_8169[] = {
	{ SPEED_1000,	COALESCE_DELAY(320) },
	{ SPEED_100,	COALESCE_DELAY(2560) },
	{ SPEED_10,	COALESCE_DELAY(40960) },
	{ 0 },
};

static const struct rtl_coalesce_info rtl_coalesce_info_8168_8136[] = {
	{ SPEED_1000,	COALESCE_DELAY(5000) },
	{ SPEED_100,	COALESCE_DELAY(2560) },
	{ SPEED_10,	COALESCE_DELAY(40960) },
	{ 0 },
};
#undef COALESCE_DELAY

/* get rx/tx scale vector corresponding to current speed */
static const struct rtl_coalesce_info *
rtl_coalesce_info(struct rtl8169_private *tp)
{
	const struct rtl_coalesce_info *ci;

	if (tp->mac_version <= RTL_GIGA_MAC_VER_06)
		ci = rtl_coalesce_info_8169;
	else
		ci = rtl_coalesce_info_8168_8136;

	/* if speed is unknown assume highest one */
	if (tp->phydev->speed == SPEED_UNKNOWN)
		return ci;

	for (; ci->speed; ci++) {
		if (tp->phydev->speed == ci->speed)
			return ci;
	}

	return ERR_PTR(-ELNRNG);
}

static int rtl_get_coalesce(struct net_device *dev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_coal,
			    struct netlink_ext_ack *extack)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	const struct rtl_coalesce_info *ci;
	u32 scale, c_us, c_fr;
	u16 intrmit;

	if (rtl_is_8125(tp))
		return -EOPNOTSUPP;

	memset(ec, 0, sizeof(*ec));

	/* get rx/tx scale corresponding to current speed and CPlusCmd[0:1] */
	ci = rtl_coalesce_info(tp);
	if (IS_ERR(ci))
		return PTR_ERR(ci);

	scale = ci->scale_nsecs[tp->cp_cmd & INTT_MASK];

	intrmit = RTL_R16(tp, IntrMitigate);

	c_us = FIELD_GET(RTL_COALESCE_TX_USECS, intrmit);
	ec->tx_coalesce_usecs = DIV_ROUND_UP(c_us * scale, 1000);

	c_fr = FIELD_GET(RTL_COALESCE_TX_FRAMES, intrmit);
	/* ethtool_coalesce states usecs and max_frames must not both be 0 */
	ec->tx_max_coalesced_frames = (c_us || c_fr) ? c_fr * 4 : 1;

	c_us = FIELD_GET(RTL_COALESCE_RX_USECS, intrmit);
	ec->rx_coalesce_usecs = DIV_ROUND_UP(c_us * scale, 1000);

	c_fr = FIELD_GET(RTL_COALESCE_RX_FRAMES, intrmit);
	ec->rx_max_coalesced_frames = (c_us || c_fr) ? c_fr * 4 : 1;

	return 0;
}

/* choose appropriate scale factor and CPlusCmd[0:1] for (speed, usec) */
static int rtl_coalesce_choose_scale(struct rtl8169_private *tp, u32 usec,
				     u16 *cp01)
{
	const struct rtl_coalesce_info *ci;
	u16 i;

	ci = rtl_coalesce_info(tp);
	if (IS_ERR(ci))
		return PTR_ERR(ci);

	for (i = 0; i < 4; i++) {
		if (usec <= ci->scale_nsecs[i] * RTL_COALESCE_T_MAX / 1000U) {
			*cp01 = i;
			return ci->scale_nsecs[i];
		}
	}

	return -ERANGE;
}

static int rtl_set_coalesce(struct net_device *dev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_coal,
			    struct netlink_ext_ack *extack)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	u32 tx_fr = ec->tx_max_coalesced_frames;
	u32 rx_fr = ec->rx_max_coalesced_frames;
	u32 coal_usec_max, units;
	u16 w = 0, cp01 = 0;
	int scale;

	if (rtl_is_8125(tp))
		return -EOPNOTSUPP;

	if (rx_fr > RTL_COALESCE_FRAME_MAX || tx_fr > RTL_COALESCE_FRAME_MAX)
		return -ERANGE;

	coal_usec_max = max(ec->rx_coalesce_usecs, ec->tx_coalesce_usecs);
	scale = rtl_coalesce_choose_scale(tp, coal_usec_max, &cp01);
	if (scale < 0)
		return scale;

	/* Accept max_frames=1 we returned in rtl_get_coalesce. Accept it
	 * not only when usecs=0 because of e.g. the following scenario:
	 *
	 * - both rx_usecs=0 & rx_frames=0 in hardware (no delay on RX)
	 * - rtl_get_coalesce returns rx_usecs=0, rx_frames=1
	 * - then user does `ethtool -C eth0 rx-usecs 100`
	 *
	 * Since ethtool sends to kernel whole ethtool_coalesce settings,
	 * if we want to ignore rx_frames then it has to be set to 0.
	 */
	if (rx_fr == 1)
		rx_fr = 0;
	if (tx_fr == 1)
		tx_fr = 0;

	/* HW requires time limit to be set if frame limit is set */
	if ((tx_fr && !ec->tx_coalesce_usecs) ||
	    (rx_fr && !ec->rx_coalesce_usecs))
		return -EINVAL;

	w |= FIELD_PREP(RTL_COALESCE_TX_FRAMES, DIV_ROUND_UP(tx_fr, 4));
	w |= FIELD_PREP(RTL_COALESCE_RX_FRAMES, DIV_ROUND_UP(rx_fr, 4));

	units = DIV_ROUND_UP(ec->tx_coalesce_usecs * 1000U, scale);
	w |= FIELD_PREP(RTL_COALESCE_TX_USECS, units);
	units = DIV_ROUND_UP(ec->rx_coalesce_usecs * 1000U, scale);
	w |= FIELD_PREP(RTL_COALESCE_RX_USECS, units);

	RTL_W16(tp, IntrMitigate, w);

	/* Meaning of PktCntrDisable bit changed from RTL8168e-vl */
	if (rtl_is_8168evl_up(tp)) {
		if (!rx_fr && !tx_fr)
			/* disable packet counter */
			tp->cp_cmd |= PktCntrDisable;
		else
			tp->cp_cmd &= ~PktCntrDisable;
	}

	tp->cp_cmd = (tp->cp_cmd & ~INTT_MASK) | cp01;
	RTL_W16(tp, CPlusCmd, tp->cp_cmd);
	rtl_pci_commit(tp);

	return 0;
}

static void rtl_set_eee_txidle_timer(struct rtl8169_private *tp)
{
	unsigned int timer_val = READ_ONCE(tp->dev->mtu) + ETH_HLEN + 0x20;

	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_46:
	case RTL_GIGA_MAC_VER_48:
		tp->tx_lpi_timer = timer_val;
		r8168_mac_ocp_write(tp, 0xe048, timer_val);
		break;
	case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_LAST:
		tp->tx_lpi_timer = timer_val;
		RTL_W16(tp, EEE_TXIDLE_TIMER_8125, timer_val);
		break;
	default:
		break;
	}
}

static unsigned int r8169_get_tx_lpi_timer_us(struct rtl8169_private *tp)
{
	unsigned int speed = tp->phydev->speed;
	unsigned int timer = tp->tx_lpi_timer;

	if (!timer || speed == SPEED_UNKNOWN)
		return 0;

	/* tx_lpi_timer value is in bytes */
	return DIV_ROUND_CLOSEST(timer * BITS_PER_BYTE, speed);
}

static int rtl8169_get_eee(struct net_device *dev, struct ethtool_keee *data)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	int ret;

	if (!rtl_supports_eee(tp))
		return -EOPNOTSUPP;

	ret = phy_ethtool_get_eee(tp->phydev, data);
	if (ret)
		return ret;

	data->tx_lpi_timer = r8169_get_tx_lpi_timer_us(tp);

	return 0;
}

static int rtl8169_set_eee(struct net_device *dev, struct ethtool_keee *data)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (!rtl_supports_eee(tp))
		return -EOPNOTSUPP;

	return phy_ethtool_set_eee(tp->phydev, data);
}

static void rtl8169_get_ringparam(struct net_device *dev,
				  struct ethtool_ringparam *data,
				  struct kernel_ethtool_ringparam *kernel_data,
				  struct netlink_ext_ack *extack)
{
	data->rx_max_pending = NUM_RX_DESC;
	data->rx_pending = NUM_RX_DESC;
	data->tx_max_pending = NUM_TX_DESC;
	data->tx_pending = NUM_TX_DESC;
}

static void rtl8169_get_pause_stats(struct net_device *dev,
				    struct ethtool_pause_stats *pause_stats)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (!rtl_is_8125(tp))
		return;

	rtl8169_update_counters(tp);
	pause_stats->tx_pause_frames = le32_to_cpu(tp->counters->tx_pause_on);
	pause_stats->rx_pause_frames = le32_to_cpu(tp->counters->rx_pause_on);
}

static void rtl8169_get_pauseparam(struct net_device *dev,
				   struct ethtool_pauseparam *data)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	bool tx_pause, rx_pause;

	phy_get_pause(tp->phydev, &tx_pause, &rx_pause);

	data->autoneg = tp->phydev->autoneg;
	data->tx_pause = tx_pause ? 1 : 0;
	data->rx_pause = rx_pause ? 1 : 0;
}

static int rtl8169_set_pauseparam(struct net_device *dev,
				  struct ethtool_pauseparam *data)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (dev->mtu > ETH_DATA_LEN)
		return -EOPNOTSUPP;

	phy_set_asym_pause(tp->phydev, data->rx_pause, data->tx_pause);

	return 0;
}

static void rtl8169_get_eth_mac_stats(struct net_device *dev,
				      struct ethtool_eth_mac_stats *mac_stats)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	rtl8169_update_counters(tp);

	mac_stats->FramesTransmittedOK =
		le64_to_cpu(tp->counters->tx_packets);
	mac_stats->SingleCollisionFrames =
		le32_to_cpu(tp->counters->tx_one_collision);
	mac_stats->MultipleCollisionFrames =
		le32_to_cpu(tp->counters->tx_multi_collision);
	mac_stats->FramesReceivedOK =
		le64_to_cpu(tp->counters->rx_packets);
	mac_stats->AlignmentErrors =
		le16_to_cpu(tp->counters->align_errors);
	mac_stats->FramesLostDueToIntMACXmitError =
		le64_to_cpu(tp->counters->tx_errors);
	mac_stats->BroadcastFramesReceivedOK =
		le64_to_cpu(tp->counters->rx_broadcast);
	mac_stats->MulticastFramesReceivedOK =
		le32_to_cpu(tp->counters->rx_multicast);

	if (!rtl_is_8125(tp))
		return;

	mac_stats->AlignmentErrors =
		le32_to_cpu(tp->counters->align_errors32);
	mac_stats->OctetsTransmittedOK =
		le64_to_cpu(tp->counters->tx_octets);
	mac_stats->LateCollisions =
		le32_to_cpu(tp->counters->tx_late_collision);
	mac_stats->FramesAbortedDueToXSColls =
		le32_to_cpu(tp->counters->tx_aborted32);
	mac_stats->OctetsReceivedOK =
		le64_to_cpu(tp->counters->rx_octets);
	mac_stats->FramesLostDueToIntMACRcvError =
		le32_to_cpu(tp->counters->rx_mac_error);
	mac_stats->MulticastFramesXmittedOK =
		le64_to_cpu(tp->counters->tx_multicast64);
	mac_stats->BroadcastFramesXmittedOK =
		le64_to_cpu(tp->counters->tx_broadcast64);
	mac_stats->MulticastFramesReceivedOK =
		le64_to_cpu(tp->counters->rx_multicast64);
	mac_stats->FrameTooLongErrors =
		le32_to_cpu(tp->counters->rx_frame_too_long);
}

static void rtl8169_get_eth_ctrl_stats(struct net_device *dev,
				       struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (!rtl_is_8125(tp))
		return;

	rtl8169_update_counters(tp);

	ctrl_stats->UnsupportedOpcodesReceived =
		le32_to_cpu(tp->counters->rx_unknown_opcode);
}

static const struct ethtool_ops rtl8169_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_MAX_FRAMES,
	.get_drvinfo		= rtl8169_get_drvinfo,
	.get_regs_len		= rtl8169_get_regs_len,
	.get_link		= ethtool_op_get_link,
	.get_coalesce		= rtl_get_coalesce,
	.set_coalesce		= rtl_set_coalesce,
	.get_regs		= rtl8169_get_regs,
	.get_wol		= rtl8169_get_wol,
	.set_wol		= rtl8169_set_wol,
	.get_strings		= rtl8169_get_strings,
	.get_sset_count		= rtl8169_get_sset_count,
	.get_ethtool_stats	= rtl8169_get_ethtool_stats,
	.get_ts_info		= ethtool_op_get_ts_info,
	.nway_reset		= phy_ethtool_nway_reset,
	.get_eee		= rtl8169_get_eee,
	.set_eee		= rtl8169_set_eee,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_ringparam		= rtl8169_get_ringparam,
	.get_pause_stats	= rtl8169_get_pause_stats,
	.get_pauseparam		= rtl8169_get_pauseparam,
	.set_pauseparam		= rtl8169_set_pauseparam,
	.get_eth_mac_stats	= rtl8169_get_eth_mac_stats,
	.get_eth_ctrl_stats	= rtl8169_get_eth_ctrl_stats,
};

static const struct rtl_chip_info *rtl8169_get_chip_version(u16 xid, bool gmii)
{
	/* Chips combining a 1Gbps MAC with a 100Mbps PHY */
	static const struct rtl_chip_info rtl8106eus_info = {
		.mac_version = RTL_GIGA_MAC_VER_43,
		.name = "RTL8106eus",
		.fw_name = FIRMWARE_8106E_2,
	};
	static const struct rtl_chip_info rtl8107e_info = {
		.mac_version = RTL_GIGA_MAC_VER_48,
		.name = "RTL8107e",
		.fw_name = FIRMWARE_8107E_2,
	};
	const struct rtl_chip_info *p = rtl_chip_infos;

	while ((xid & p->mask) != p->val)
		p++;

	if (p->mac_version == RTL_GIGA_MAC_VER_42 && !gmii)
		return &rtl8106eus_info;
	if (p->mac_version == RTL_GIGA_MAC_VER_46 && !gmii)
		return &rtl8107e_info;

	return p;
}

static void rtl_release_firmware(struct rtl8169_private *tp)
{
	if (tp->rtl_fw) {
		rtl_fw_release_firmware(tp->rtl_fw);
		kfree(tp->rtl_fw);
		tp->rtl_fw = NULL;
	}
}

void r8169_apply_firmware(struct rtl8169_private *tp)
{
	int val;

	/* TODO: release firmware if rtl_fw_write_firmware signals failure. */
	if (tp->rtl_fw) {
		rtl_fw_write_firmware(tp, tp->rtl_fw);
		/* At least one firmware doesn't reset tp->ocp_base. */
		tp->ocp_base = OCP_STD_PHY_BASE;

		/* PHY soft reset may still be in progress */
		phy_read_poll_timeout(tp->phydev, MII_BMCR, val,
				      !(val & BMCR_RESET),
				      50000, 600000, true);
	}
}

static void rtl8168_config_eee_mac(struct rtl8169_private *tp)
{
	/* Adjust EEE LED frequency */
	if (tp->mac_version != RTL_GIGA_MAC_VER_38)
		RTL_W8(tp, EEE_LED, RTL_R8(tp, EEE_LED) & ~0x07);

	rtl_eri_set_bits(tp, 0x1b0, 0x0003);
}

static void rtl8125a_config_eee_mac(struct rtl8169_private *tp)
{
	r8168_mac_ocp_modify(tp, 0xe040, 0, BIT(1) | BIT(0));
	r8168_mac_ocp_modify(tp, 0xeb62, 0, BIT(2) | BIT(1));
}

static void rtl8125b_config_eee_mac(struct rtl8169_private *tp)
{
	r8168_mac_ocp_modify(tp, 0xe040, 0, BIT(1) | BIT(0));
}

static void rtl_rar_exgmac_set(struct rtl8169_private *tp, const u8 *addr)
{
	rtl_eri_write(tp, 0xe0, ERIAR_MASK_1111, get_unaligned_le32(addr));
	rtl_eri_write(tp, 0xe4, ERIAR_MASK_1111, get_unaligned_le16(addr + 4));
	rtl_eri_write(tp, 0xf0, ERIAR_MASK_1111, get_unaligned_le16(addr) << 16);
	rtl_eri_write(tp, 0xf4, ERIAR_MASK_1111, get_unaligned_le32(addr + 2));
}

u16 rtl8168h_2_get_adc_bias_ioffset(struct rtl8169_private *tp)
{
	u16 data1, data2, ioffset;

	r8168_mac_ocp_write(tp, 0xdd02, 0x807d);
	data1 = r8168_mac_ocp_read(tp, 0xdd02);
	data2 = r8168_mac_ocp_read(tp, 0xdd00);

	ioffset = (data2 >> 1) & 0x7ff8;
	ioffset |= data2 & 0x0007;
	if (data1 & BIT(7))
		ioffset |= BIT(15);

	return ioffset;
}

static void rtl_schedule_task(struct rtl8169_private *tp, enum rtl_flag flag)
{
	set_bit(flag, tp->wk.flags);
	if (!schedule_work(&tp->wk.work))
		clear_bit(flag, tp->wk.flags);
}

static void rtl8169_init_phy(struct rtl8169_private *tp)
{
	r8169_hw_phy_config(tp, tp->phydev, tp->mac_version);

	if (tp->mac_version <= RTL_GIGA_MAC_VER_06) {
		pci_write_config_byte(tp->pci_dev, PCI_LATENCY_TIMER, 0x40);
		pci_write_config_byte(tp->pci_dev, PCI_CACHE_LINE_SIZE, 0x08);
		/* set undocumented MAC Reg C+CR Offset 0x82h */
		RTL_W8(tp, 0x82, 0x01);
	}

	if (tp->mac_version == RTL_GIGA_MAC_VER_05 &&
	    tp->pci_dev->subsystem_vendor == PCI_VENDOR_ID_GIGABYTE &&
	    tp->pci_dev->subsystem_device == 0xe000)
		phy_write_paged(tp->phydev, 0x0001, 0x10, 0xf01b);

	/* We may have called phy_speed_down before */
	phy_speed_up(tp->phydev);

	genphy_soft_reset(tp->phydev);
}

static void rtl_rar_set(struct rtl8169_private *tp, const u8 *addr)
{
	rtl_unlock_config_regs(tp);

	RTL_W32(tp, MAC4, get_unaligned_le16(addr + 4));
	rtl_pci_commit(tp);

	RTL_W32(tp, MAC0, get_unaligned_le32(addr));
	rtl_pci_commit(tp);

	if (tp->mac_version == RTL_GIGA_MAC_VER_34)
		rtl_rar_exgmac_set(tp, addr);

	rtl_lock_config_regs(tp);
}

static int rtl_set_mac_address(struct net_device *dev, void *p)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	int ret;

	ret = eth_mac_addr(dev, p);
	if (ret)
		return ret;

	rtl_rar_set(tp, dev->dev_addr);

	return 0;
}

static void rtl_init_rxcfg(struct rtl8169_private *tp)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_02 ... RTL_GIGA_MAC_VER_06:
	case RTL_GIGA_MAC_VER_10 ... RTL_GIGA_MAC_VER_17:
		RTL_W32(tp, RxConfig, RX_FIFO_THRESH | RX_DMA_BURST);
		break;
	case RTL_GIGA_MAC_VER_18 ... RTL_GIGA_MAC_VER_24:
	case RTL_GIGA_MAC_VER_34 ... RTL_GIGA_MAC_VER_36:
	case RTL_GIGA_MAC_VER_38:
		RTL_W32(tp, RxConfig, RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST);
		break;
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_52:
		RTL_W32(tp, RxConfig, RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF);
		break;
	case RTL_GIGA_MAC_VER_61:
		RTL_W32(tp, RxConfig, RX_FETCH_DFLT_8125 | RX_DMA_BURST);
		break;
	case RTL_GIGA_MAC_VER_63 ... RTL_GIGA_MAC_VER_LAST:
		RTL_W32(tp, RxConfig, RX_FETCH_DFLT_8125 | RX_DMA_BURST |
			RX_PAUSE_SLOT_ON);
		break;
	default:
		RTL_W32(tp, RxConfig, RX128_INT_EN | RX_DMA_BURST);
		break;
	}
}

static void rtl8169_init_ring_indexes(struct rtl8169_private *tp)
{
	tp->dirty_tx = tp->cur_tx = tp->cur_rx = 0;
}

static void rtl_jumbo_config(struct rtl8169_private *tp)
{
	bool jumbo = tp->dev->mtu > ETH_DATA_LEN;
	int readrq = 4096;

	if (jumbo && tp->mac_version >= RTL_GIGA_MAC_VER_17 &&
	    tp->mac_version <= RTL_GIGA_MAC_VER_26)
		readrq = 512;

	rtl_unlock_config_regs(tp);
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_17:
		r8169_mod_reg8_cond(tp, Config4, BIT(0), jumbo);
		break;
	case RTL_GIGA_MAC_VER_18 ... RTL_GIGA_MAC_VER_26:
		r8169_mod_reg8_cond(tp, Config3, Jumbo_En0, jumbo);
		r8169_mod_reg8_cond(tp, Config4, Jumbo_En1, jumbo);
		break;
	case RTL_GIGA_MAC_VER_28:
		r8169_mod_reg8_cond(tp, Config3, Jumbo_En0, jumbo);
		break;
	case RTL_GIGA_MAC_VER_31 ... RTL_GIGA_MAC_VER_33:
		RTL_W8(tp, MaxTxPacketSize, jumbo ? 0x24 : 0x3f);
		r8169_mod_reg8_cond(tp, Config3, Jumbo_En0, jumbo);
		r8169_mod_reg8_cond(tp, Config4, BIT(0), jumbo);
		break;
	default:
		break;
	}
	rtl_lock_config_regs(tp);

	if (pci_is_pcie(tp->pci_dev) && tp->supports_gmii)
		pcie_set_readrq(tp->pci_dev, readrq);

	/* Chip doesn't support pause in jumbo mode */
	if (jumbo) {
		linkmode_clear_bit(ETHTOOL_LINK_MODE_Pause_BIT,
				   tp->phydev->advertising);
		linkmode_clear_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT,
				   tp->phydev->advertising);
		phy_start_aneg(tp->phydev);
	}
}

DECLARE_RTL_COND(rtl_chipcmd_cond)
{
	return RTL_R8(tp, ChipCmd) & CmdReset;
}

static void rtl_hw_reset(struct rtl8169_private *tp)
{
	RTL_W8(tp, ChipCmd, CmdReset);

	rtl_loop_wait_low(tp, &rtl_chipcmd_cond, 100, 100);
}

static void rtl_request_firmware(struct rtl8169_private *tp)
{
	struct rtl_fw *rtl_fw;

	/* firmware loaded already or no firmware available */
	if (tp->rtl_fw || !tp->fw_name)
		return;

	rtl_fw = kzalloc(sizeof(*rtl_fw), GFP_KERNEL);
	if (!rtl_fw)
		return;

	rtl_fw->phy_write = rtl_writephy;
	rtl_fw->phy_read = rtl_readphy;
	rtl_fw->mac_mcu_write = mac_mcu_write;
	rtl_fw->mac_mcu_read = mac_mcu_read;
	rtl_fw->fw_name = tp->fw_name;
	rtl_fw->dev = tp_to_dev(tp);

	if (rtl_fw_request_firmware(rtl_fw))
		kfree(rtl_fw);
	else
		tp->rtl_fw = rtl_fw;
}

static void rtl_rx_close(struct rtl8169_private *tp)
{
	RTL_W32(tp, RxConfig, RTL_R32(tp, RxConfig) & ~RX_CONFIG_ACCEPT_MASK);
}

DECLARE_RTL_COND(rtl_npq_cond)
{
	return RTL_R8(tp, TxPoll) & NPQ;
}

DECLARE_RTL_COND(rtl_txcfg_empty_cond)
{
	return RTL_R32(tp, TxConfig) & TXCFG_EMPTY;
}

DECLARE_RTL_COND(rtl_rxtx_empty_cond)
{
	return (RTL_R8(tp, MCU) & RXTX_EMPTY) == RXTX_EMPTY;
}

DECLARE_RTL_COND(rtl_rxtx_empty_cond_2)
{
	/* IntrMitigate has new functionality on RTL8125 */
	return (RTL_R16(tp, IntrMitigate) & 0x0103) == 0x0103;
}

static void rtl_wait_txrx_fifo_empty(struct rtl8169_private *tp)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_52:
		rtl_loop_wait_high(tp, &rtl_txcfg_empty_cond, 100, 42);
		rtl_loop_wait_high(tp, &rtl_rxtx_empty_cond, 100, 42);
		break;
	case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_61:
		rtl_loop_wait_high(tp, &rtl_rxtx_empty_cond, 100, 42);
		break;
	case RTL_GIGA_MAC_VER_63 ... RTL_GIGA_MAC_VER_LAST:
		RTL_W8(tp, ChipCmd, RTL_R8(tp, ChipCmd) | StopReq);
		rtl_loop_wait_high(tp, &rtl_rxtx_empty_cond, 100, 42);
		rtl_loop_wait_high(tp, &rtl_rxtx_empty_cond_2, 100, 42);
		break;
	default:
		break;
	}
}

static void rtl_disable_rxdvgate(struct rtl8169_private *tp)
{
	RTL_W32(tp, MISC, RTL_R32(tp, MISC) & ~RXDV_GATED_EN);
}

static void rtl_enable_rxdvgate(struct rtl8169_private *tp)
{
	RTL_W32(tp, MISC, RTL_R32(tp, MISC) | RXDV_GATED_EN);
	fsleep(2000);
	rtl_wait_txrx_fifo_empty(tp);
}

static void rtl_wol_enable_rx(struct rtl8169_private *tp)
{
	if (tp->mac_version >= RTL_GIGA_MAC_VER_25)
		RTL_W32(tp, RxConfig, RTL_R32(tp, RxConfig) |
			AcceptBroadcast | AcceptMulticast | AcceptMyPhys);

	if (tp->mac_version >= RTL_GIGA_MAC_VER_40)
		rtl_disable_rxdvgate(tp);
}

static void rtl_prepare_power_down(struct rtl8169_private *tp)
{
	if (tp->dash_enabled)
		return;

	if (tp->mac_version == RTL_GIGA_MAC_VER_32 ||
	    tp->mac_version == RTL_GIGA_MAC_VER_33)
		rtl_ephy_write(tp, 0x19, 0xff64);

	if (device_may_wakeup(tp_to_dev(tp))) {
		phy_speed_down(tp->phydev, false);
		rtl_wol_enable_rx(tp);
	}
}

static void rtl_set_tx_config_registers(struct rtl8169_private *tp)
{
	u32 val = TX_DMA_BURST << TxDMAShift |
		  InterFrameGap << TxInterFrameGapShift;

	if (rtl_is_8168evl_up(tp))
		val |= TXCFG_AUTO_FIFO;

	RTL_W32(tp, TxConfig, val);
}

static void rtl_set_rx_max_size(struct rtl8169_private *tp)
{
	/* Low hurts. Let's disable the filtering. */
	RTL_W16(tp, RxMaxSize, R8169_RX_BUF_SIZE + 1);
}

static void rtl_set_rx_tx_desc_registers(struct rtl8169_private *tp)
{
	/*
	 * Magic spell: some iop3xx ARM board needs the TxDescAddrHigh
	 * register to be written before TxDescAddrLow to work.
	 * Switching from MMIO to I/O access fixes the issue as well.
	 */
	RTL_W32(tp, TxDescStartAddrHigh, ((u64) tp->TxPhyAddr) >> 32);
	RTL_W32(tp, TxDescStartAddrLow, ((u64) tp->TxPhyAddr) & DMA_BIT_MASK(32));
	RTL_W32(tp, RxDescAddrHigh, ((u64) tp->RxPhyAddr) >> 32);
	RTL_W32(tp, RxDescAddrLow, ((u64) tp->RxPhyAddr) & DMA_BIT_MASK(32));
}

static void rtl8169_set_magic_reg(struct rtl8169_private *tp)
{
	u32 val;

	if (tp->mac_version == RTL_GIGA_MAC_VER_05)
		val = 0x000fff00;
	else if (tp->mac_version == RTL_GIGA_MAC_VER_06)
		val = 0x00ffff00;
	else
		return;

	if (RTL_R8(tp, Config2) & PCI_Clock_66MHz)
		val |= 0xff;

	RTL_W32(tp, 0x7c, val);
}

static void rtl_set_rx_mode(struct net_device *dev)
{
	u32 rx_mode = AcceptBroadcast | AcceptMyPhys | AcceptMulticast;
	/* Multicast hash filter */
	u32 mc_filter[2] = { 0xffffffff, 0xffffffff };
	struct rtl8169_private *tp = netdev_priv(dev);
	u32 tmp;

	if (dev->flags & IFF_PROMISC) {
		rx_mode |= AcceptAllPhys;
	} else if (!(dev->flags & IFF_MULTICAST)) {
		rx_mode &= ~AcceptMulticast;
	} else if (dev->flags & IFF_ALLMULTI ||
		   tp->mac_version == RTL_GIGA_MAC_VER_35) {
		/* accept all multicasts */
	} else if (netdev_mc_empty(dev)) {
		rx_mode &= ~AcceptMulticast;
	} else {
		struct netdev_hw_addr *ha;

		mc_filter[1] = mc_filter[0] = 0;
		netdev_for_each_mc_addr(ha, dev) {
			u32 bit_nr = eth_hw_addr_crc(ha) >> 26;
			mc_filter[bit_nr >> 5] |= BIT(bit_nr & 31);
		}

		if (tp->mac_version > RTL_GIGA_MAC_VER_06) {
			tmp = mc_filter[0];
			mc_filter[0] = swab32(mc_filter[1]);
			mc_filter[1] = swab32(tmp);
		}
	}

	RTL_W32(tp, MAR0 + 4, mc_filter[1]);
	RTL_W32(tp, MAR0 + 0, mc_filter[0]);

	tmp = RTL_R32(tp, RxConfig);
	RTL_W32(tp, RxConfig, (tmp & ~RX_CONFIG_ACCEPT_OK_MASK) | rx_mode);
}

DECLARE_RTL_COND(rtl_csiar_cond)
{
	return RTL_R32(tp, CSIAR) & CSIAR_FLAG;
}

static void rtl_csi_write(struct rtl8169_private *tp, int addr, int value)
{
	u32 func = PCI_FUNC(tp->pci_dev->devfn);

	RTL_W32(tp, CSIDR, value);
	RTL_W32(tp, CSIAR, CSIAR_WRITE_CMD | (addr & CSIAR_ADDR_MASK) |
		CSIAR_BYTE_ENABLE | func << 16);

	rtl_loop_wait_low(tp, &rtl_csiar_cond, 10, 100);
}

static u32 rtl_csi_read(struct rtl8169_private *tp, int addr)
{
	u32 func = PCI_FUNC(tp->pci_dev->devfn);

	RTL_W32(tp, CSIAR, (addr & CSIAR_ADDR_MASK) | func << 16 |
		CSIAR_BYTE_ENABLE);

	return rtl_loop_wait_high(tp, &rtl_csiar_cond, 10, 100) ?
		RTL_R32(tp, CSIDR) : ~0;
}

static void rtl_csi_mod(struct rtl8169_private *tp, int addr,
			u32 mask, u32 set)
{
	u32 val;

	WARN(addr % 4, "Invalid CSI address %#x\n", addr);

	netdev_notice_once(tp->dev,
		"No native access to PCI extended config space, falling back to CSI\n");

	val = rtl_csi_read(tp, addr);
	rtl_csi_write(tp, addr, (val & ~mask) | set);
}

static void rtl_disable_zrxdc_timeout(struct rtl8169_private *tp)
{
	struct pci_dev *pdev = tp->pci_dev;
	int rc;
	u8 val;

#define RTL_GEN3_RELATED_OFF	0x0890
#define RTL_GEN3_ZRXDC_NONCOMPL	0x1
	if (pdev->cfg_size > RTL_GEN3_RELATED_OFF) {
		rc = pci_read_config_byte(pdev, RTL_GEN3_RELATED_OFF, &val);
		if (rc == PCIBIOS_SUCCESSFUL) {
			val &= ~RTL_GEN3_ZRXDC_NONCOMPL;
			rc = pci_write_config_byte(pdev, RTL_GEN3_RELATED_OFF,
						   val);
			if (rc == PCIBIOS_SUCCESSFUL)
				return;
		}
	}

	rtl_csi_mod(tp, RTL_GEN3_RELATED_OFF, RTL_GEN3_ZRXDC_NONCOMPL, 0);
}

static void rtl_set_aspm_entry_latency(struct rtl8169_private *tp, u8 val)
{
	struct pci_dev *pdev = tp->pci_dev;

	/* According to Realtek the value at config space address 0x070f
	 * controls the L0s/L1 entrance latency. We try standard ECAM access
	 * first and if it fails fall back to CSI.
	 * bit 0..2: L0: 0 = 1us, 1 = 2us .. 6 = 7us, 7 = 7us (no typo)
	 * bit 3..5: L1: 0 = 1us, 1 = 2us .. 6 = 64us, 7 = 64us
	 */
	if (pdev->cfg_size > 0x070f &&
	    pci_write_config_byte(pdev, 0x070f, val) == PCIBIOS_SUCCESSFUL)
		return;

	rtl_csi_mod(tp, 0x070c, 0xff000000, val << 24);
}

static void rtl_set_def_aspm_entry_latency(struct rtl8169_private *tp)
{
	/* L0 7us, L1 16us */
	rtl_set_aspm_entry_latency(tp, 0x27);
}

struct ephy_info {
	unsigned int offset;
	u16 mask;
	u16 bits;
};

static void __rtl_ephy_init(struct rtl8169_private *tp,
			    const struct ephy_info *e, int len)
{
	u16 w;

	while (len-- > 0) {
		w = (rtl_ephy_read(tp, e->offset) & ~e->mask) | e->bits;
		rtl_ephy_write(tp, e->offset, w);
		e++;
	}
}

#define rtl_ephy_init(tp, a) __rtl_ephy_init(tp, a, ARRAY_SIZE(a))

static void rtl_disable_clock_request(struct rtl8169_private *tp)
{
	pcie_capability_clear_word(tp->pci_dev, PCI_EXP_LNKCTL,
				   PCI_EXP_LNKCTL_CLKREQ_EN);
}

static void rtl_enable_clock_request(struct rtl8169_private *tp)
{
	pcie_capability_set_word(tp->pci_dev, PCI_EXP_LNKCTL,
				 PCI_EXP_LNKCTL_CLKREQ_EN);
}

static void rtl_pcie_state_l2l3_disable(struct rtl8169_private *tp)
{
	/* work around an issue when PCI reset occurs during L2/L3 state */
	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Rdy_to_L23);
}

static void rtl_enable_exit_l1(struct rtl8169_private *tp)
{
	/* Bits control which events trigger ASPM L1 exit:
	 * Bit 12: rxdv
	 * Bit 11: ltr_msg
	 * Bit 10: txdma_poll
	 * Bit  9: xadm
	 * Bit  8: pktavi
	 * Bit  7: txpla
	 */
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_34 ... RTL_GIGA_MAC_VER_36:
		rtl_eri_set_bits(tp, 0xd4, 0x1f00);
		break;
	case RTL_GIGA_MAC_VER_37 ... RTL_GIGA_MAC_VER_38:
		rtl_eri_set_bits(tp, 0xd4, 0x0c00);
		break;
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_LAST:
		r8168_mac_ocp_modify(tp, 0xc0ac, 0, 0x1f80);
		break;
	default:
		break;
	}
}

static void rtl_disable_exit_l1(struct rtl8169_private *tp)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_34 ... RTL_GIGA_MAC_VER_38:
		rtl_eri_clear_bits(tp, 0xd4, 0x1f00);
		break;
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_LAST:
		r8168_mac_ocp_modify(tp, 0xc0ac, 0x1f80, 0);
		break;
	default:
		break;
	}
}

static void rtl_hw_aspm_clkreq_enable(struct rtl8169_private *tp, bool enable)
{
	u8 val8;

	if (tp->mac_version < RTL_GIGA_MAC_VER_32)
		return;

	/* Don't enable ASPM in the chip if OS can't control ASPM */
	if (enable && tp->aspm_manageable) {
		/* On these chip versions ASPM can even harm
		 * bus communication of other PCI devices.
		 */
		if (tp->mac_version == RTL_GIGA_MAC_VER_42 ||
		    tp->mac_version == RTL_GIGA_MAC_VER_43)
			return;

		rtl_mod_config5(tp, 0, ASPM_en);
		switch (tp->mac_version) {
		case RTL_GIGA_MAC_VER_70:
		case RTL_GIGA_MAC_VER_80:
			val8 = RTL_R8(tp, INT_CFG0_8125) | INT_CFG0_CLKREQEN;
			RTL_W8(tp, INT_CFG0_8125, val8);
			break;
		default:
			rtl_mod_config2(tp, 0, ClkReqEn);
			break;
		}

		switch (tp->mac_version) {
		case RTL_GIGA_MAC_VER_46 ... RTL_GIGA_MAC_VER_48:
		case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_LAST:
			/* reset ephy tx/rx disable timer */
			r8168_mac_ocp_modify(tp, 0xe094, 0xff00, 0);
			/* chip can trigger L1.2 */
			r8168_mac_ocp_modify(tp, 0xe092, 0x00ff, BIT(2));
			break;
		default:
			break;
		}
	} else {
		switch (tp->mac_version) {
		case RTL_GIGA_MAC_VER_46 ... RTL_GIGA_MAC_VER_48:
		case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_LAST:
			r8168_mac_ocp_modify(tp, 0xe092, 0x00ff, 0);
			break;
		default:
			break;
		}

		switch (tp->mac_version) {
		case RTL_GIGA_MAC_VER_70:
		case RTL_GIGA_MAC_VER_80:
			val8 = RTL_R8(tp, INT_CFG0_8125) & ~INT_CFG0_CLKREQEN;
			RTL_W8(tp, INT_CFG0_8125, val8);
			break;
		default:
			rtl_mod_config2(tp, ClkReqEn, 0);
			break;
		}
		rtl_mod_config5(tp, ASPM_en, 0);
	}
}

static void rtl_set_fifo_size(struct rtl8169_private *tp, u16 rx_stat,
			      u16 tx_stat, u16 rx_dyn, u16 tx_dyn)
{
	/* Usage of dynamic vs. static FIFO is controlled by bit
	 * TXCFG_AUTO_FIFO. Exact meaning of FIFO values isn't known.
	 */
	rtl_eri_write(tp, 0xc8, ERIAR_MASK_1111, (rx_stat << 16) | rx_dyn);
	rtl_eri_write(tp, 0xe8, ERIAR_MASK_1111, (tx_stat << 16) | tx_dyn);
}

static void rtl8168g_set_pause_thresholds(struct rtl8169_private *tp,
					  u8 low, u8 high)
{
	/* FIFO thresholds for pause flow control */
	rtl_eri_write(tp, 0xcc, ERIAR_MASK_0001, low);
	rtl_eri_write(tp, 0xd0, ERIAR_MASK_0001, high);
}

static void rtl_hw_start_8168b(struct rtl8169_private *tp)
{
	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);
}

static void __rtl_hw_start_8168cp(struct rtl8169_private *tp)
{
	RTL_W8(tp, Config1, RTL_R8(tp, Config1) | Speed_down);

	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);

	rtl_disable_clock_request(tp);
}

static void rtl_hw_start_8168cp_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168cp[] = {
		{ 0x01, 0,	0x0001 },
		{ 0x02, 0x0800,	0x1000 },
		{ 0x03, 0,	0x0042 },
		{ 0x06, 0x0080,	0x0000 },
		{ 0x07, 0,	0x2000 }
	};

	rtl_set_def_aspm_entry_latency(tp);

	rtl_ephy_init(tp, e_info_8168cp);

	__rtl_hw_start_8168cp(tp);
}

static void rtl_hw_start_8168cp_2(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);

	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);
}

static void rtl_hw_start_8168cp_3(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);

	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);

	/* Magic. */
	RTL_W8(tp, DBG_REG, 0x20);
}

static void rtl_hw_start_8168c_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168c_1[] = {
		{ 0x02, 0x0800,	0x1000 },
		{ 0x03, 0,	0x0002 },
		{ 0x06, 0x0080,	0x0000 }
	};

	rtl_set_def_aspm_entry_latency(tp);

	RTL_W8(tp, DBG_REG, 0x06 | FIX_NAK_1 | FIX_NAK_2);

	rtl_ephy_init(tp, e_info_8168c_1);

	__rtl_hw_start_8168cp(tp);
}

static void rtl_hw_start_8168c_2(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168c_2[] = {
		{ 0x01, 0,	0x0001 },
		{ 0x03, 0x0400,	0x0020 }
	};

	rtl_set_def_aspm_entry_latency(tp);

	rtl_ephy_init(tp, e_info_8168c_2);

	__rtl_hw_start_8168cp(tp);
}

static void rtl_hw_start_8168c_4(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);

	__rtl_hw_start_8168cp(tp);
}

static void rtl_hw_start_8168d(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);

	rtl_disable_clock_request(tp);
}

static void rtl_hw_start_8168d_4(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168d_4[] = {
		{ 0x0b, 0x0000,	0x0048 },
		{ 0x19, 0x0020,	0x0050 },
		{ 0x0c, 0x0100,	0x0020 },
		{ 0x10, 0x0004,	0x0000 },
	};

	rtl_set_def_aspm_entry_latency(tp);

	rtl_ephy_init(tp, e_info_8168d_4);

	rtl_enable_clock_request(tp);
}

static void rtl_hw_start_8168e_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168e_1[] = {
		{ 0x00, 0x0200,	0x0100 },
		{ 0x00, 0x0000,	0x0004 },
		{ 0x06, 0x0002,	0x0001 },
		{ 0x06, 0x0000,	0x0030 },
		{ 0x07, 0x0000,	0x2000 },
		{ 0x00, 0x0000,	0x0020 },
		{ 0x03, 0x5800,	0x2000 },
		{ 0x03, 0x0000,	0x0001 },
		{ 0x01, 0x0800,	0x1000 },
		{ 0x07, 0x0000,	0x4000 },
		{ 0x1e, 0x0000,	0x2000 },
		{ 0x19, 0xffff,	0xfe6c },
		{ 0x0a, 0x0000,	0x0040 }
	};

	rtl_set_def_aspm_entry_latency(tp);

	rtl_ephy_init(tp, e_info_8168e_1);

	rtl_disable_clock_request(tp);

	/* Reset tx FIFO pointer */
	RTL_W32(tp, MISC, RTL_R32(tp, MISC) | TXPLA_RST);
	RTL_W32(tp, MISC, RTL_R32(tp, MISC) & ~TXPLA_RST);

	rtl_mod_config5(tp, Spi_en, 0);
}

static void rtl_hw_start_8168e_2(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168e_2[] = {
		{ 0x09, 0x0000,	0x0080 },
		{ 0x19, 0x0000,	0x0224 },
		{ 0x00, 0x0000,	0x0004 },
		{ 0x0c, 0x3df0,	0x0200 },
	};

	rtl_set_def_aspm_entry_latency(tp);

	rtl_ephy_init(tp, e_info_8168e_2);

	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_1111, 0x0000);
	rtl_set_fifo_size(tp, 0x10, 0x10, 0x02, 0x06);
	rtl_eri_set_bits(tp, 0x1d0, BIT(1));
	rtl_reset_packet_filter(tp);
	rtl_eri_set_bits(tp, 0x1b0, BIT(4));
	rtl_eri_write(tp, 0xcc, ERIAR_MASK_1111, 0x00000050);
	rtl_eri_write(tp, 0xd0, ERIAR_MASK_1111, 0x07ff0060);

	rtl_disable_clock_request(tp);

	RTL_W8(tp, MCU, RTL_R8(tp, MCU) & ~NOW_IS_OOB);

	rtl8168_config_eee_mac(tp);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) | PFM_EN);
	RTL_W32(tp, MISC, RTL_R32(tp, MISC) | PWM_EN);
	rtl_mod_config5(tp, Spi_en, 0);
}

static void rtl_hw_start_8168f(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);

	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_1111, 0x0000);
	rtl_set_fifo_size(tp, 0x10, 0x10, 0x02, 0x06);
	rtl_reset_packet_filter(tp);
	rtl_eri_set_bits(tp, 0x1b0, BIT(4));
	rtl_eri_set_bits(tp, 0x1d0, BIT(4) | BIT(1));
	rtl_eri_write(tp, 0xcc, ERIAR_MASK_1111, 0x00000050);
	rtl_eri_write(tp, 0xd0, ERIAR_MASK_1111, 0x00000060);

	rtl_disable_clock_request(tp);

	RTL_W8(tp, MCU, RTL_R8(tp, MCU) & ~NOW_IS_OOB);
	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) | PFM_EN);
	RTL_W32(tp, MISC, RTL_R32(tp, MISC) | PWM_EN);
	rtl_mod_config5(tp, Spi_en, 0);

	rtl8168_config_eee_mac(tp);
}

static void rtl_hw_start_8168f_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168f_1[] = {
		{ 0x06, 0x00c0,	0x0020 },
		{ 0x08, 0x0001,	0x0002 },
		{ 0x09, 0x0000,	0x0080 },
		{ 0x19, 0x0000,	0x0224 },
		{ 0x00, 0x0000,	0x0008 },
		{ 0x0c, 0x3df0,	0x0200 },
	};

	rtl_hw_start_8168f(tp);

	rtl_ephy_init(tp, e_info_8168f_1);
}

static void rtl_hw_start_8411(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168f_1[] = {
		{ 0x06, 0x00c0,	0x0020 },
		{ 0x0f, 0xffff,	0x5200 },
		{ 0x19, 0x0000,	0x0224 },
		{ 0x00, 0x0000,	0x0008 },
		{ 0x0c, 0x3df0,	0x0200 },
	};

	rtl_hw_start_8168f(tp);
	rtl_pcie_state_l2l3_disable(tp);

	rtl_ephy_init(tp, e_info_8168f_1);
}

static void rtl_hw_start_8168g(struct rtl8169_private *tp)
{
	rtl_set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
	rtl8168g_set_pause_thresholds(tp, 0x38, 0x48);

	rtl_set_def_aspm_entry_latency(tp);

	rtl_reset_packet_filter(tp);
	rtl_eri_write(tp, 0x2f8, ERIAR_MASK_0011, 0x1d8f);

	rtl_disable_rxdvgate(tp);

	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);

	rtl8168_config_eee_mac(tp);

	rtl_w0w1_eri(tp, 0x2fc, 0x01, 0x06);
	rtl_eri_clear_bits(tp, 0x1b0, BIT(12));

	rtl_pcie_state_l2l3_disable(tp);
}

static void rtl_hw_start_8168g_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168g_1[] = {
		{ 0x00, 0x0008,	0x0000 },
		{ 0x0c, 0x3ff0,	0x0820 },
		{ 0x1e, 0x0000,	0x0001 },
		{ 0x19, 0x8000,	0x0000 }
	};

	rtl_hw_start_8168g(tp);
	rtl_ephy_init(tp, e_info_8168g_1);
}

static void rtl_hw_start_8168g_2(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168g_2[] = {
		{ 0x00, 0x0008,	0x0000 },
		{ 0x0c, 0x3ff0,	0x0820 },
		{ 0x19, 0xffff,	0x7c00 },
		{ 0x1e, 0xffff,	0x20eb },
		{ 0x0d, 0xffff,	0x1666 },
		{ 0x00, 0xffff,	0x10a3 },
		{ 0x06, 0xffff,	0xf050 },
		{ 0x04, 0x0000,	0x0010 },
		{ 0x1d, 0x4000,	0x0000 },
	};

	rtl_hw_start_8168g(tp);
	rtl_ephy_init(tp, e_info_8168g_2);
}

static void rtl8411b_fix_phy_down(struct rtl8169_private *tp)
{
	static const u16 fix_data[] = {
/* 0xf800 */ 0xe008, 0xe00a, 0xe00c, 0xe00e, 0xe027, 0xe04f, 0xe05e, 0xe065,
/* 0xf810 */ 0xc602, 0xbe00, 0x0000, 0xc502, 0xbd00, 0x074c, 0xc302, 0xbb00,
/* 0xf820 */ 0x080a, 0x6420, 0x48c2, 0x8c20, 0xc516, 0x64a4, 0x49c0, 0xf009,
/* 0xf830 */ 0x74a2, 0x8ca5, 0x74a0, 0xc50e, 0x9ca2, 0x1c11, 0x9ca0, 0xe006,
/* 0xf840 */ 0x74f8, 0x48c4, 0x8cf8, 0xc404, 0xbc00, 0xc403, 0xbc00, 0x0bf2,
/* 0xf850 */ 0x0c0a, 0xe434, 0xd3c0, 0x49d9, 0xf01f, 0xc526, 0x64a5, 0x1400,
/* 0xf860 */ 0xf007, 0x0c01, 0x8ca5, 0x1c15, 0xc51b, 0x9ca0, 0xe013, 0xc519,
/* 0xf870 */ 0x74a0, 0x48c4, 0x8ca0, 0xc516, 0x74a4, 0x48c8, 0x48ca, 0x9ca4,
/* 0xf880 */ 0xc512, 0x1b00, 0x9ba0, 0x1b1c, 0x483f, 0x9ba2, 0x1b04, 0xc508,
/* 0xf890 */ 0x9ba0, 0xc505, 0xbd00, 0xc502, 0xbd00, 0x0300, 0x051e, 0xe434,
/* 0xf8a0 */ 0xe018, 0xe092, 0xde20, 0xd3c0, 0xc50f, 0x76a4, 0x49e3, 0xf007,
/* 0xf8b0 */ 0x49c0, 0xf103, 0xc607, 0xbe00, 0xc606, 0xbe00, 0xc602, 0xbe00,
/* 0xf8c0 */ 0x0c4c, 0x0c28, 0x0c2c, 0xdc00, 0xc707, 0x1d00, 0x8de2, 0x48c1,
/* 0xf8d0 */ 0xc502, 0xbd00, 0x00aa, 0xe0c0, 0xc502, 0xbd00, 0x0132
	};
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&tp->mac_ocp_lock, flags);
	for (i = 0; i < ARRAY_SIZE(fix_data); i++)
		__r8168_mac_ocp_write(tp, 0xf800 + 2 * i, fix_data[i]);
	raw_spin_unlock_irqrestore(&tp->mac_ocp_lock, flags);
}

static void rtl_hw_start_8411_2(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8411_2[] = {
		{ 0x00, 0x0008,	0x0000 },
		{ 0x0c, 0x37d0,	0x0820 },
		{ 0x1e, 0x0000,	0x0001 },
		{ 0x19, 0x8021,	0x0000 },
		{ 0x1e, 0x0000,	0x2000 },
		{ 0x0d, 0x0100,	0x0200 },
		{ 0x00, 0x0000,	0x0080 },
		{ 0x06, 0x0000,	0x0010 },
		{ 0x04, 0x0000,	0x0010 },
		{ 0x1d, 0x0000,	0x4000 },
	};

	rtl_hw_start_8168g(tp);

	rtl_ephy_init(tp, e_info_8411_2);

	/* The following Realtek-provided magic fixes an issue with the RX unit
	 * getting confused after the PHY having been powered-down.
	 */
	r8168_mac_ocp_write(tp, 0xFC28, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC2A, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC2C, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC2E, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC30, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC32, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC34, 0x0000);
	r8168_mac_ocp_write(tp, 0xFC36, 0x0000);
	mdelay(3);
	r8168_mac_ocp_write(tp, 0xFC26, 0x0000);

	rtl8411b_fix_phy_down(tp);

	r8168_mac_ocp_write(tp, 0xFC26, 0x8000);

	r8168_mac_ocp_write(tp, 0xFC2A, 0x0743);
	r8168_mac_ocp_write(tp, 0xFC2C, 0x0801);
	r8168_mac_ocp_write(tp, 0xFC2E, 0x0BE9);
	r8168_mac_ocp_write(tp, 0xFC30, 0x02FD);
	r8168_mac_ocp_write(tp, 0xFC32, 0x0C25);
	r8168_mac_ocp_write(tp, 0xFC34, 0x00A9);
	r8168_mac_ocp_write(tp, 0xFC36, 0x012D);
}

static void rtl_hw_start_8168h_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168h_1[] = {
		{ 0x1e, 0x0800,	0x0001 },
		{ 0x1d, 0x0000,	0x0800 },
		{ 0x05, 0xffff,	0x2089 },
		{ 0x06, 0xffff,	0x5881 },
		{ 0x04, 0xffff,	0x854a },
		{ 0x01, 0xffff,	0x068b }
	};
	int rg_saw_cnt;

	rtl_ephy_init(tp, e_info_8168h_1);

	rtl_set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
	rtl8168g_set_pause_thresholds(tp, 0x38, 0x48);

	rtl_set_def_aspm_entry_latency(tp);

	rtl_reset_packet_filter(tp);

	rtl_eri_set_bits(tp, 0xdc, 0x001c);

	rtl_eri_write(tp, 0x5f0, ERIAR_MASK_0011, 0x4f87);

	rtl_disable_rxdvgate(tp);

	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);

	rtl8168_config_eee_mac(tp);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~PFM_EN);
	RTL_W8(tp, MISC_1, RTL_R8(tp, MISC_1) & ~PFM_D3COLD_EN);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~TX_10M_PS_EN);

	rtl_eri_clear_bits(tp, 0x1b0, BIT(12));

	rtl_pcie_state_l2l3_disable(tp);

	rg_saw_cnt = phy_read_paged(tp->phydev, 0x0c42, 0x13) & 0x3fff;
	if (rg_saw_cnt > 0) {
		u16 sw_cnt_1ms_ini;

		sw_cnt_1ms_ini = 16000000/rg_saw_cnt;
		sw_cnt_1ms_ini &= 0x0fff;
		r8168_mac_ocp_modify(tp, 0xd412, 0x0fff, sw_cnt_1ms_ini);
	}

	r8168_mac_ocp_modify(tp, 0xe056, 0x00f0, 0x0070);
	r8168_mac_ocp_modify(tp, 0xe052, 0x6000, 0x8008);
	r8168_mac_ocp_modify(tp, 0xe0d6, 0x01ff, 0x017f);
	r8168_mac_ocp_modify(tp, 0xd420, 0x0fff, 0x047f);

	r8168_mac_ocp_write(tp, 0xe63e, 0x0001);
	r8168_mac_ocp_write(tp, 0xe63e, 0x0000);
	r8168_mac_ocp_write(tp, 0xc094, 0x0000);
	r8168_mac_ocp_write(tp, 0xc09e, 0x0000);
}

static void rtl_hw_start_8168ep(struct rtl8169_private *tp)
{
	rtl8168ep_stop_cmac(tp);

	rtl_set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
	rtl8168g_set_pause_thresholds(tp, 0x2f, 0x5f);

	rtl_set_def_aspm_entry_latency(tp);

	rtl_reset_packet_filter(tp);

	rtl_eri_write(tp, 0x5f0, ERIAR_MASK_0011, 0x4f87);

	rtl_disable_rxdvgate(tp);

	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);

	rtl8168_config_eee_mac(tp);

	rtl_w0w1_eri(tp, 0x2fc, 0x01, 0x06);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~TX_10M_PS_EN);

	rtl_pcie_state_l2l3_disable(tp);
}

static void rtl_hw_start_8168ep_3(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8168ep_3[] = {
		{ 0x00, 0x0000,	0x0080 },
		{ 0x0d, 0x0100,	0x0200 },
		{ 0x19, 0x8021,	0x0000 },
		{ 0x1e, 0x0000,	0x2000 },
	};

	rtl_ephy_init(tp, e_info_8168ep_3);

	rtl_hw_start_8168ep(tp);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~PFM_EN);
	RTL_W8(tp, MISC_1, RTL_R8(tp, MISC_1) & ~PFM_D3COLD_EN);

	r8168_mac_ocp_modify(tp, 0xd3e2, 0x0fff, 0x0271);
	r8168_mac_ocp_modify(tp, 0xd3e4, 0x00ff, 0x0000);
	r8168_mac_ocp_modify(tp, 0xe860, 0x0000, 0x0080);
}

static void rtl_hw_start_8117(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8117[] = {
		{ 0x19, 0x0040,	0x1100 },
		{ 0x59, 0x0040,	0x1100 },
	};
	int rg_saw_cnt;

	rtl8168ep_stop_cmac(tp);
	rtl_ephy_init(tp, e_info_8117);

	rtl_set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
	rtl8168g_set_pause_thresholds(tp, 0x2f, 0x5f);

	rtl_set_def_aspm_entry_latency(tp);

	rtl_reset_packet_filter(tp);

	rtl_eri_set_bits(tp, 0xd4, 0x0010);

	rtl_eri_write(tp, 0x5f0, ERIAR_MASK_0011, 0x4f87);

	rtl_disable_rxdvgate(tp);

	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);

	rtl8168_config_eee_mac(tp);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~PFM_EN);
	RTL_W8(tp, MISC_1, RTL_R8(tp, MISC_1) & ~PFM_D3COLD_EN);

	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~TX_10M_PS_EN);

	rtl_eri_clear_bits(tp, 0x1b0, BIT(12));

	rtl_pcie_state_l2l3_disable(tp);

	rg_saw_cnt = phy_read_paged(tp->phydev, 0x0c42, 0x13) & 0x3fff;
	if (rg_saw_cnt > 0) {
		u16 sw_cnt_1ms_ini;

		sw_cnt_1ms_ini = (16000000 / rg_saw_cnt) & 0x0fff;
		r8168_mac_ocp_modify(tp, 0xd412, 0x0fff, sw_cnt_1ms_ini);
	}

	r8168_mac_ocp_modify(tp, 0xe056, 0x00f0, 0x0070);
	r8168_mac_ocp_write(tp, 0xea80, 0x0003);
	r8168_mac_ocp_modify(tp, 0xe052, 0x0000, 0x0009);
	r8168_mac_ocp_modify(tp, 0xd420, 0x0fff, 0x047f);

	r8168_mac_ocp_write(tp, 0xe63e, 0x0001);
	r8168_mac_ocp_write(tp, 0xe63e, 0x0000);
	r8168_mac_ocp_write(tp, 0xc094, 0x0000);
	r8168_mac_ocp_write(tp, 0xc09e, 0x0000);

	/* firmware is for MAC only */
	r8169_apply_firmware(tp);
}

static void rtl_hw_start_8102e_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8102e_1[] = {
		{ 0x01,	0, 0x6e65 },
		{ 0x02,	0, 0x091f },
		{ 0x03,	0, 0xc2f9 },
		{ 0x06,	0, 0xafb5 },
		{ 0x07,	0, 0x0e00 },
		{ 0x19,	0, 0xec80 },
		{ 0x01,	0, 0x2e65 },
		{ 0x01,	0, 0x6e65 }
	};
	u8 cfg1;

	rtl_set_def_aspm_entry_latency(tp);

	RTL_W8(tp, DBG_REG, FIX_NAK_1);

	RTL_W8(tp, Config1,
	       LEDS1 | LEDS0 | Speed_down | MEMMAP | IOMAP | VPD | PMEnable);
	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);

	cfg1 = RTL_R8(tp, Config1);
	if ((cfg1 & LEDS0) && (cfg1 & LEDS1))
		RTL_W8(tp, Config1, cfg1 & ~LEDS0);

	rtl_ephy_init(tp, e_info_8102e_1);
}

static void rtl_hw_start_8102e_2(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);

	RTL_W8(tp, Config1, MEMMAP | IOMAP | VPD | PMEnable);
	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);
}

static void rtl_hw_start_8102e_3(struct rtl8169_private *tp)
{
	rtl_hw_start_8102e_2(tp);

	rtl_ephy_write(tp, 0x03, 0xc2f9);
}

static void rtl_hw_start_8401(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8401[] = {
		{ 0x01,	0xffff, 0x6fe5 },
		{ 0x03,	0xffff, 0x0599 },
		{ 0x06,	0xffff, 0xaf25 },
		{ 0x07,	0xffff, 0x8e68 },
	};

	rtl_ephy_init(tp, e_info_8401);
	RTL_W8(tp, Config3, RTL_R8(tp, Config3) & ~Beacon_en);
}

static void rtl_hw_start_8105e_1(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8105e_1[] = {
		{ 0x07,	0, 0x4000 },
		{ 0x19,	0, 0x0200 },
		{ 0x19,	0, 0x0020 },
		{ 0x1e,	0, 0x2000 },
		{ 0x03,	0, 0x0001 },
		{ 0x19,	0, 0x0100 },
		{ 0x19,	0, 0x0004 },
		{ 0x0a,	0, 0x0020 }
	};

	/* Force LAN exit from ASPM if Rx/Tx are not idle */
	RTL_W32(tp, FuncEvent, RTL_R32(tp, FuncEvent) | 0x002800);

	/* Disable Early Tally Counter */
	RTL_W32(tp, FuncEvent, RTL_R32(tp, FuncEvent) & ~0x010000);

	RTL_W8(tp, MCU, RTL_R8(tp, MCU) | EN_NDP | EN_OOB_RESET);
	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) | PFM_EN);

	rtl_ephy_init(tp, e_info_8105e_1);

	rtl_pcie_state_l2l3_disable(tp);
}

static void rtl_hw_start_8105e_2(struct rtl8169_private *tp)
{
	rtl_hw_start_8105e_1(tp);
	rtl_ephy_write(tp, 0x1e, rtl_ephy_read(tp, 0x1e) | 0x8000);
}

static void rtl_hw_start_8402(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8402[] = {
		{ 0x19,	0xffff, 0xff64 },
		{ 0x1e,	0, 0x4000 }
	};

	rtl_set_def_aspm_entry_latency(tp);

	/* Force LAN exit from ASPM if Rx/Tx are not idle */
	RTL_W32(tp, FuncEvent, RTL_R32(tp, FuncEvent) | 0x002800);

	RTL_W8(tp, MCU, RTL_R8(tp, MCU) & ~NOW_IS_OOB);

	rtl_ephy_init(tp, e_info_8402);

	rtl_set_fifo_size(tp, 0x00, 0x00, 0x02, 0x06);
	rtl_reset_packet_filter(tp);
	rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
	rtl_eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);
	rtl_w0w1_eri(tp, 0x0d4, 0x0e00, 0xff00);

	/* disable EEE */
	rtl_eri_write(tp, 0x1b0, ERIAR_MASK_0011, 0x0000);

	rtl_pcie_state_l2l3_disable(tp);
}

static void rtl_hw_start_8106(struct rtl8169_private *tp)
{
	/* Force LAN exit from ASPM if Rx/Tx are not idle */
	RTL_W32(tp, FuncEvent, RTL_R32(tp, FuncEvent) | 0x002800);

	RTL_W32(tp, MISC, (RTL_R32(tp, MISC) | DISABLE_LAN_EN) & ~EARLY_TALLY_EN);
	RTL_W8(tp, MCU, RTL_R8(tp, MCU) | EN_NDP | EN_OOB_RESET);
	RTL_W8(tp, DLLPR, RTL_R8(tp, DLLPR) & ~PFM_EN);

	/* L0 7us, L1 32us - needed to avoid issues with link-up detection */
	rtl_set_aspm_entry_latency(tp, 0x2f);

	rtl_eri_write(tp, 0x1d0, ERIAR_MASK_0011, 0x0000);

	/* disable EEE */
	rtl_eri_write(tp, 0x1b0, ERIAR_MASK_0011, 0x0000);

	rtl_pcie_state_l2l3_disable(tp);
}

DECLARE_RTL_COND(rtl_mac_ocp_e00e_cond)
{
	return r8168_mac_ocp_read(tp, 0xe00e) & BIT(13);
}

static void rtl_hw_start_8125_common(struct rtl8169_private *tp)
{
	rtl_pcie_state_l2l3_disable(tp);

	RTL_W16(tp, 0x382, 0x221b);
	RTL_W32(tp, RSS_CTRL_8125, 0);
	RTL_W16(tp, Q_NUM_CTRL_8125, 0);

	/* disable UPS */
	r8168_mac_ocp_modify(tp, 0xd40a, 0x0010, 0x0000);

	RTL_W8(tp, Config1, RTL_R8(tp, Config1) & ~0x10);

	r8168_mac_ocp_write(tp, 0xc140, 0xffff);
	r8168_mac_ocp_write(tp, 0xc142, 0xffff);

	r8168_mac_ocp_modify(tp, 0xd3e2, 0x0fff, 0x03a9);
	r8168_mac_ocp_modify(tp, 0xd3e4, 0x00ff, 0x0000);
	r8168_mac_ocp_modify(tp, 0xe860, 0x0000, 0x0080);

	/* disable new tx descriptor format */
	r8168_mac_ocp_modify(tp, 0xeb58, 0x0001, 0x0000);

	if (tp->mac_version == RTL_GIGA_MAC_VER_70 ||
	    tp->mac_version == RTL_GIGA_MAC_VER_80)
		RTL_W8(tp, 0xD8, RTL_R8(tp, 0xD8) & ~0x02);

	if (tp->mac_version == RTL_GIGA_MAC_VER_80)
		r8168_mac_ocp_modify(tp, 0xe614, 0x0f00, 0x0f00);
	else if (tp->mac_version == RTL_GIGA_MAC_VER_70)
		r8168_mac_ocp_modify(tp, 0xe614, 0x0700, 0x0400);
	else if (tp->mac_version == RTL_GIGA_MAC_VER_63)
		r8168_mac_ocp_modify(tp, 0xe614, 0x0700, 0x0200);
	else
		r8168_mac_ocp_modify(tp, 0xe614, 0x0700, 0x0300);

	if (tp->mac_version == RTL_GIGA_MAC_VER_63)
		r8168_mac_ocp_modify(tp, 0xe63e, 0x0c30, 0x0000);
	else
		r8168_mac_ocp_modify(tp, 0xe63e, 0x0c30, 0x0020);

	r8168_mac_ocp_modify(tp, 0xc0b4, 0x0000, 0x000c);
	r8168_mac_ocp_modify(tp, 0xeb6a, 0x00ff, 0x0033);
	r8168_mac_ocp_modify(tp, 0xeb50, 0x03e0, 0x0040);
	r8168_mac_ocp_modify(tp, 0xe056, 0x00f0, 0x0030);
	r8168_mac_ocp_modify(tp, 0xe040, 0x1000, 0x0000);
	r8168_mac_ocp_modify(tp, 0xea1c, 0x0003, 0x0001);
	if (tp->mac_version == RTL_GIGA_MAC_VER_70 ||
	    tp->mac_version == RTL_GIGA_MAC_VER_80)
		r8168_mac_ocp_modify(tp, 0xea1c, 0x0300, 0x0000);
	else
		r8168_mac_ocp_modify(tp, 0xea1c, 0x0004, 0x0000);
	r8168_mac_ocp_modify(tp, 0xe0c0, 0x4f0f, 0x4403);
	r8168_mac_ocp_modify(tp, 0xe052, 0x0080, 0x0068);
	r8168_mac_ocp_modify(tp, 0xd430, 0x0fff, 0x047f);

	r8168_mac_ocp_modify(tp, 0xea1c, 0x0004, 0x0000);
	r8168_mac_ocp_modify(tp, 0xeb54, 0x0000, 0x0001);
	udelay(1);
	r8168_mac_ocp_modify(tp, 0xeb54, 0x0001, 0x0000);
	RTL_W16(tp, 0x1880, RTL_R16(tp, 0x1880) & ~0x0030);

	r8168_mac_ocp_write(tp, 0xe098, 0xc302);

	rtl_loop_wait_low(tp, &rtl_mac_ocp_e00e_cond, 1000, 10);

	if (tp->mac_version == RTL_GIGA_MAC_VER_61)
		rtl8125a_config_eee_mac(tp);
	else
		rtl8125b_config_eee_mac(tp);

	rtl_disable_rxdvgate(tp);
}

static void rtl_hw_start_8125a_2(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8125a_2[] = {
		{ 0x04, 0xffff, 0xd000 },
		{ 0x0a, 0xffff, 0x8653 },
		{ 0x23, 0xffff, 0xab66 },
		{ 0x20, 0xffff, 0x9455 },
		{ 0x21, 0xffff, 0x99ff },
		{ 0x29, 0xffff, 0xfe04 },

		{ 0x44, 0xffff, 0xd000 },
		{ 0x4a, 0xffff, 0x8653 },
		{ 0x63, 0xffff, 0xab66 },
		{ 0x60, 0xffff, 0x9455 },
		{ 0x61, 0xffff, 0x99ff },
		{ 0x69, 0xffff, 0xfe04 },
	};

	rtl_set_def_aspm_entry_latency(tp);
	rtl_ephy_init(tp, e_info_8125a_2);
	rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8125b(struct rtl8169_private *tp)
{
	static const struct ephy_info e_info_8125b[] = {
		{ 0x0b, 0xffff, 0xa908 },
		{ 0x1e, 0xffff, 0x20eb },
		{ 0x4b, 0xffff, 0xa908 },
		{ 0x5e, 0xffff, 0x20eb },
		{ 0x22, 0x0030, 0x0020 },
		{ 0x62, 0x0030, 0x0020 },
	};

	rtl_set_def_aspm_entry_latency(tp);
	rtl_ephy_init(tp, e_info_8125b);
	rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8125d(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);
	rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8126a(struct rtl8169_private *tp)
{
	rtl_disable_zrxdc_timeout(tp);
	rtl_set_def_aspm_entry_latency(tp);
	rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8127a(struct rtl8169_private *tp)
{
	rtl_set_def_aspm_entry_latency(tp);
	rtl_hw_start_8125_common(tp);
}

static void rtl_hw_config(struct rtl8169_private *tp)
{
	static const rtl_generic_fct hw_configs[] = {
		[RTL_GIGA_MAC_VER_07] = rtl_hw_start_8102e_1,
		[RTL_GIGA_MAC_VER_08] = rtl_hw_start_8102e_3,
		[RTL_GIGA_MAC_VER_09] = rtl_hw_start_8102e_2,
		[RTL_GIGA_MAC_VER_10] = NULL,
		[RTL_GIGA_MAC_VER_14] = rtl_hw_start_8401,
		[RTL_GIGA_MAC_VER_17] = rtl_hw_start_8168b,
		[RTL_GIGA_MAC_VER_18] = rtl_hw_start_8168cp_1,
		[RTL_GIGA_MAC_VER_19] = rtl_hw_start_8168c_1,
		[RTL_GIGA_MAC_VER_20] = rtl_hw_start_8168c_2,
		[RTL_GIGA_MAC_VER_21] = rtl_hw_start_8168c_2,
		[RTL_GIGA_MAC_VER_22] = rtl_hw_start_8168c_4,
		[RTL_GIGA_MAC_VER_23] = rtl_hw_start_8168cp_2,
		[RTL_GIGA_MAC_VER_24] = rtl_hw_start_8168cp_3,
		[RTL_GIGA_MAC_VER_25] = rtl_hw_start_8168d,
		[RTL_GIGA_MAC_VER_26] = rtl_hw_start_8168d,
		[RTL_GIGA_MAC_VER_28] = rtl_hw_start_8168d_4,
		[RTL_GIGA_MAC_VER_29] = rtl_hw_start_8105e_1,
		[RTL_GIGA_MAC_VER_30] = rtl_hw_start_8105e_2,
		[RTL_GIGA_MAC_VER_31] = rtl_hw_start_8168d,
		[RTL_GIGA_MAC_VER_32] = rtl_hw_start_8168e_1,
		[RTL_GIGA_MAC_VER_33] = rtl_hw_start_8168e_1,
		[RTL_GIGA_MAC_VER_34] = rtl_hw_start_8168e_2,
		[RTL_GIGA_MAC_VER_35] = rtl_hw_start_8168f_1,
		[RTL_GIGA_MAC_VER_36] = rtl_hw_start_8168f_1,
		[RTL_GIGA_MAC_VER_37] = rtl_hw_start_8402,
		[RTL_GIGA_MAC_VER_38] = rtl_hw_start_8411,
		[RTL_GIGA_MAC_VER_39] = rtl_hw_start_8106,
		[RTL_GIGA_MAC_VER_40] = rtl_hw_start_8168g_1,
		[RTL_GIGA_MAC_VER_42] = rtl_hw_start_8168g_2,
		[RTL_GIGA_MAC_VER_43] = rtl_hw_start_8168g_2,
		[RTL_GIGA_MAC_VER_44] = rtl_hw_start_8411_2,
		[RTL_GIGA_MAC_VER_46] = rtl_hw_start_8168h_1,
		[RTL_GIGA_MAC_VER_48] = rtl_hw_start_8168h_1,
		[RTL_GIGA_MAC_VER_51] = rtl_hw_start_8168ep_3,
		[RTL_GIGA_MAC_VER_52] = rtl_hw_start_8117,
		[RTL_GIGA_MAC_VER_61] = rtl_hw_start_8125a_2,
		[RTL_GIGA_MAC_VER_63] = rtl_hw_start_8125b,
		[RTL_GIGA_MAC_VER_64] = rtl_hw_start_8125d,
		[RTL_GIGA_MAC_VER_66] = rtl_hw_start_8125d,
		[RTL_GIGA_MAC_VER_70] = rtl_hw_start_8126a,
		[RTL_GIGA_MAC_VER_80] = rtl_hw_start_8127a,
	};

	if (hw_configs[tp->mac_version])
		hw_configs[tp->mac_version](tp);
}

static void rtl_hw_start_8125(struct rtl8169_private *tp)
{
	int i;

	RTL_W8(tp, INT_CFG0_8125, 0x00);

	/* disable interrupt coalescing */
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_61:
	case RTL_GIGA_MAC_VER_64:
	case RTL_GIGA_MAC_VER_66:
	case RTL_GIGA_MAC_VER_80:
		for (i = 0xa00; i < 0xb00; i += 4)
			RTL_W32(tp, i, 0);
		if (tp->mac_version == RTL_GIGA_MAC_VER_80)
			RTL_W16(tp, INT_CFG1_8125, 0x0000);
		break;
	case RTL_GIGA_MAC_VER_63:
	case RTL_GIGA_MAC_VER_70:
		for (i = 0xa00; i < 0xa80; i += 4)
			RTL_W32(tp, i, 0);
		RTL_W16(tp, INT_CFG1_8125, 0x0000);
		break;
	default:
		break;
	}

	/* enable extended tally counter */
	r8168_mac_ocp_modify(tp, 0xea84, 0, BIT(1) | BIT(0));

	rtl_hw_config(tp);
}

static void rtl_hw_start_8168(struct rtl8169_private *tp)
{
	if (rtl_is_8168evl_up(tp))
		RTL_W8(tp, MaxTxPacketSize, EarlySize);
	else
		RTL_W8(tp, MaxTxPacketSize, TxPacketMax);

	rtl_hw_config(tp);

	/* disable interrupt coalescing */
	RTL_W16(tp, IntrMitigate, 0x0000);
}

static void rtl_hw_start_8169(struct rtl8169_private *tp)
{
	RTL_W8(tp, EarlyTxThres, NoEarlyTx);

	tp->cp_cmd |= PCIMulRW;

	if (tp->mac_version == RTL_GIGA_MAC_VER_02 ||
	    tp->mac_version == RTL_GIGA_MAC_VER_03)
		tp->cp_cmd |= EnAnaPLL;

	RTL_W16(tp, CPlusCmd, tp->cp_cmd);

	rtl8169_set_magic_reg(tp);

	/* disable interrupt coalescing */
	RTL_W16(tp, IntrMitigate, 0x0000);
}

static void rtl_hw_start(struct  rtl8169_private *tp)
{
	rtl_unlock_config_regs(tp);
	/* disable aspm and clock request before ephy access */
	rtl_hw_aspm_clkreq_enable(tp, false);
	RTL_W16(tp, CPlusCmd, tp->cp_cmd);

	rtl_set_eee_txidle_timer(tp);

	if (tp->mac_version <= RTL_GIGA_MAC_VER_06)
		rtl_hw_start_8169(tp);
	else if (rtl_is_8125(tp))
		rtl_hw_start_8125(tp);
	else
		rtl_hw_start_8168(tp);

	rtl_enable_exit_l1(tp);
	rtl_hw_aspm_clkreq_enable(tp, true);
	rtl_set_rx_max_size(tp);
	rtl_set_rx_tx_desc_registers(tp);
	rtl_lock_config_regs(tp);

	rtl_jumbo_config(tp);

	/* Initially a 10 us delay. Turned it into a PCI commit. - FR */
	rtl_pci_commit(tp);

	RTL_W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);
	rtl_init_rxcfg(tp);
	rtl_set_tx_config_registers(tp);
	rtl_set_rx_config_features(tp, tp->dev->features);
	rtl_set_rx_mode(tp->dev);
	rtl_irq_enable(tp);
}

static int rtl8169_change_mtu(struct net_device *dev, int new_mtu)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	WRITE_ONCE(dev->mtu, new_mtu);
	netdev_update_features(dev);
	rtl_jumbo_config(tp);
	rtl_set_eee_txidle_timer(tp);

	return 0;
}

static void rtl8169_mark_to_asic(struct RxDesc *desc)
{
	u32 eor = le32_to_cpu(desc->opts1) & RingEnd;

	desc->opts2 = 0;
	/* Force memory writes to complete before releasing descriptor */
	dma_wmb();
	WRITE_ONCE(desc->opts1, cpu_to_le32(DescOwn | eor | R8169_RX_BUF_SIZE));
}

static struct page *rtl8169_alloc_rx_data(struct rtl8169_private *tp,
					  struct RxDesc *desc)
{
	struct device *d = tp_to_dev(tp);
	int node = dev_to_node(d);
	dma_addr_t mapping;
	struct page *data;

	data = alloc_pages_node(node, GFP_KERNEL, get_order(R8169_RX_BUF_SIZE));
	if (!data)
		return NULL;

	mapping = dma_map_page(d, data, 0, R8169_RX_BUF_SIZE, DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(d, mapping))) {
		netdev_err(tp->dev, "Failed to map RX DMA!\n");
		__free_pages(data, get_order(R8169_RX_BUF_SIZE));
		return NULL;
	}

	desc->addr = cpu_to_le64(mapping);
	rtl8169_mark_to_asic(desc);

	return data;
}

static void rtl8169_rx_clear(struct rtl8169_private *tp)
{
	int i;

	for (i = 0; i < NUM_RX_DESC && tp->Rx_databuff[i]; i++) {
		dma_unmap_page(tp_to_dev(tp),
			       le64_to_cpu(tp->RxDescArray[i].addr),
			       R8169_RX_BUF_SIZE, DMA_FROM_DEVICE);
		__free_pages(tp->Rx_databuff[i], get_order(R8169_RX_BUF_SIZE));
		tp->Rx_databuff[i] = NULL;
		tp->RxDescArray[i].addr = 0;
		tp->RxDescArray[i].opts1 = 0;
	}
}

static int rtl8169_rx_fill(struct rtl8169_private *tp)
{
	int i;

	for (i = 0; i < NUM_RX_DESC; i++) {
		struct page *data;

		data = rtl8169_alloc_rx_data(tp, tp->RxDescArray + i);
		if (!data) {
			rtl8169_rx_clear(tp);
			return -ENOMEM;
		}
		tp->Rx_databuff[i] = data;
	}

	/* mark as last descriptor in the ring */
	tp->RxDescArray[NUM_RX_DESC - 1].opts1 |= cpu_to_le32(RingEnd);

	return 0;
}

static int rtl8169_init_ring(struct rtl8169_private *tp)
{
	rtl8169_init_ring_indexes(tp);

	memset(tp->tx_skb, 0, sizeof(tp->tx_skb));
	memset(tp->Rx_databuff, 0, sizeof(tp->Rx_databuff));

	return rtl8169_rx_fill(tp);
}

static void rtl8169_unmap_tx_skb(struct rtl8169_private *tp, unsigned int entry)
{
	struct ring_info *tx_skb = tp->tx_skb + entry;
	struct TxDesc *desc = tp->TxDescArray + entry;

	dma_unmap_single(tp_to_dev(tp), le64_to_cpu(desc->addr), tx_skb->len,
			 DMA_TO_DEVICE);
	memset(desc, 0, sizeof(*desc));
	memset(tx_skb, 0, sizeof(*tx_skb));
}

static void rtl8169_tx_clear_range(struct rtl8169_private *tp, u32 start,
				   unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned int entry = (start + i) % NUM_TX_DESC;
		struct ring_info *tx_skb = tp->tx_skb + entry;
		unsigned int len = tx_skb->len;

		if (len) {
			struct sk_buff *skb = tx_skb->skb;

			rtl8169_unmap_tx_skb(tp, entry);
			if (skb)
				dev_consume_skb_any(skb);
		}
	}
}

static void rtl8169_tx_clear(struct rtl8169_private *tp)
{
	rtl8169_tx_clear_range(tp, tp->dirty_tx, NUM_TX_DESC);
	netdev_reset_queue(tp->dev);
}

static void rtl8169_cleanup(struct rtl8169_private *tp)
{
	napi_disable(&tp->napi);

	/* Give a racing hard_start_xmit a few cycles to complete. */
	synchronize_net();

	/* Disable interrupts */
	rtl8169_irq_mask_and_ack(tp);

	rtl_rx_close(tp);

	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_28:
	case RTL_GIGA_MAC_VER_31:
		rtl_loop_wait_low(tp, &rtl_npq_cond, 20, 2000);
		break;
	case RTL_GIGA_MAC_VER_34 ... RTL_GIGA_MAC_VER_38:
		RTL_W8(tp, ChipCmd, RTL_R8(tp, ChipCmd) | StopReq);
		rtl_loop_wait_high(tp, &rtl_txcfg_empty_cond, 100, 666);
		break;
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_LAST:
		rtl_enable_rxdvgate(tp);
		fsleep(2000);
		break;
	default:
		RTL_W8(tp, ChipCmd, RTL_R8(tp, ChipCmd) | StopReq);
		fsleep(100);
		break;
	}

	rtl_hw_reset(tp);

	rtl8169_tx_clear(tp);
	rtl8169_init_ring_indexes(tp);
}

static void rtl_reset_work(struct rtl8169_private *tp)
{
	int i;

	netif_stop_queue(tp->dev);

	rtl8169_cleanup(tp);

	for (i = 0; i < NUM_RX_DESC; i++)
		rtl8169_mark_to_asic(tp->RxDescArray + i);

	napi_enable(&tp->napi);
	rtl_hw_start(tp);
}

static void rtl8169_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	rtl_schedule_task(tp, RTL_FLAG_TASK_TX_TIMEOUT);
}

static int rtl8169_tx_map(struct rtl8169_private *tp, const u32 *opts, u32 len,
			  void *addr, unsigned int entry, bool desc_own)
{
	struct TxDesc *txd = tp->TxDescArray + entry;
	struct device *d = tp_to_dev(tp);
	dma_addr_t mapping;
	u32 opts1;
	int ret;

	mapping = dma_map_single(d, addr, len, DMA_TO_DEVICE);
	ret = dma_mapping_error(d, mapping);
	if (unlikely(ret)) {
		if (net_ratelimit())
			netdev_err(tp->dev, "Failed to map TX data!\n");
		return ret;
	}

	txd->addr = cpu_to_le64(mapping);
	txd->opts2 = cpu_to_le32(opts[1]);

	opts1 = opts[0] | len;
	if (entry == NUM_TX_DESC - 1)
		opts1 |= RingEnd;
	if (desc_own)
		opts1 |= DescOwn;
	txd->opts1 = cpu_to_le32(opts1);

	tp->tx_skb[entry].len = len;

	return 0;
}

static int rtl8169_xmit_frags(struct rtl8169_private *tp, struct sk_buff *skb,
			      const u32 *opts, unsigned int entry)
{
	struct skb_shared_info *info = skb_shinfo(skb);
	unsigned int cur_frag;

	for (cur_frag = 0; cur_frag < info->nr_frags; cur_frag++) {
		const skb_frag_t *frag = info->frags + cur_frag;
		void *addr = skb_frag_address(frag);
		u32 len = skb_frag_size(frag);

		entry = (entry + 1) % NUM_TX_DESC;

		if (unlikely(rtl8169_tx_map(tp, opts, len, addr, entry, true)))
			goto err_out;
	}

	return 0;

err_out:
	rtl8169_tx_clear_range(tp, tp->cur_tx + 1, cur_frag);
	return -EIO;
}

static bool rtl_skb_is_udp(struct sk_buff *skb)
{
	int no = skb_network_offset(skb);
	struct ipv6hdr *i6h, _i6h;
	struct iphdr *ih, _ih;

	switch (vlan_get_protocol(skb)) {
	case htons(ETH_P_IP):
		ih = skb_header_pointer(skb, no, sizeof(_ih), &_ih);
		return ih && ih->protocol == IPPROTO_UDP;
	case htons(ETH_P_IPV6):
		i6h = skb_header_pointer(skb, no, sizeof(_i6h), &_i6h);
		return i6h && i6h->nexthdr == IPPROTO_UDP;
	default:
		return false;
	}
}

#define RTL_MIN_PATCH_LEN	47

/* see rtl8125_get_patch_pad_len() in r8125 vendor driver */
static unsigned int rtl8125_quirk_udp_padto(struct rtl8169_private *tp,
					    struct sk_buff *skb)
{
	unsigned int padto = 0, len = skb->len;

	if (len < 128 + RTL_MIN_PATCH_LEN && rtl_skb_is_udp(skb) &&
	    skb_transport_header_was_set(skb)) {
		unsigned int trans_data_len = skb_tail_pointer(skb) -
					      skb_transport_header(skb);

		if (trans_data_len >= offsetof(struct udphdr, len) &&
		    trans_data_len < RTL_MIN_PATCH_LEN) {
			u16 dest = ntohs(udp_hdr(skb)->dest);

			/* dest is a standard PTP port */
			if (dest == 319 || dest == 320)
				padto = len + RTL_MIN_PATCH_LEN - trans_data_len;
		}

		if (trans_data_len < sizeof(struct udphdr))
			padto = max_t(unsigned int, padto,
				      len + sizeof(struct udphdr) - trans_data_len);
	}

	return padto;
}

static unsigned int rtl_quirk_packet_padto(struct rtl8169_private *tp,
					   struct sk_buff *skb)
{
	unsigned int padto = 0;

	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_63:
		padto = rtl8125_quirk_udp_padto(tp, skb);
		break;
	default:
		break;
	}

	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_34:
	case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_LAST:
		padto = max_t(unsigned int, padto, ETH_ZLEN);
		break;
	default:
		break;
	}

	return padto;
}

static void rtl8169_tso_csum_v1(struct sk_buff *skb, u32 *opts)
{
	u32 mss = skb_shinfo(skb)->gso_size;

	if (mss) {
		opts[0] |= TD_LSO;
		opts[0] |= mss << TD0_MSS_SHIFT;
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		const struct iphdr *ip = ip_hdr(skb);

		if (ip->protocol == IPPROTO_TCP)
			opts[0] |= TD0_IP_CS | TD0_TCP_CS;
		else if (ip->protocol == IPPROTO_UDP)
			opts[0] |= TD0_IP_CS | TD0_UDP_CS;
		else
			WARN_ON_ONCE(1);
	}
}

static bool rtl8169_tso_csum_v2(struct rtl8169_private *tp,
				struct sk_buff *skb, u32 *opts)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	u32 mss = shinfo->gso_size;

	if (mss) {
		if (shinfo->gso_type & SKB_GSO_TCPV4) {
			opts[0] |= TD1_GTSENV4;
		} else if (shinfo->gso_type & SKB_GSO_TCPV6) {
			if (skb_cow_head(skb, 0))
				return false;

			tcp_v6_gso_csum_prep(skb);
			opts[0] |= TD1_GTSENV6;
		} else {
			WARN_ON_ONCE(1);
		}

		opts[0] |= skb_transport_offset(skb) << GTTCPHO_SHIFT;
		opts[1] |= mss << TD1_MSS_SHIFT;
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		u8 ip_protocol;

		switch (vlan_get_protocol(skb)) {
		case htons(ETH_P_IP):
			opts[1] |= TD1_IPv4_CS;
			ip_protocol = ip_hdr(skb)->protocol;
			break;

		case htons(ETH_P_IPV6):
			opts[1] |= TD1_IPv6_CS;
			ip_protocol = ipv6_hdr(skb)->nexthdr;
			break;

		default:
			ip_protocol = IPPROTO_RAW;
			break;
		}

		if (ip_protocol == IPPROTO_TCP)
			opts[1] |= TD1_TCP_CS;
		else if (ip_protocol == IPPROTO_UDP)
			opts[1] |= TD1_UDP_CS;
		else
			WARN_ON_ONCE(1);

		opts[1] |= skb_transport_offset(skb) << TCPHO_SHIFT;
	} else {
		unsigned int padto = rtl_quirk_packet_padto(tp, skb);

		/* skb_padto would free the skb on error */
		return !__skb_put_padto(skb, padto, false);
	}

	return true;
}

static unsigned int rtl_tx_slots_avail(struct rtl8169_private *tp)
{
	return READ_ONCE(tp->dirty_tx) + NUM_TX_DESC - READ_ONCE(tp->cur_tx);
}

/* Versions RTL8102e and from RTL8168c onwards support csum_v2 */
static bool rtl_chip_supports_csum_v2(struct rtl8169_private *tp)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_02 ... RTL_GIGA_MAC_VER_06:
	case RTL_GIGA_MAC_VER_10 ... RTL_GIGA_MAC_VER_17:
		return false;
	default:
		return true;
	}
}

static void rtl8169_doorbell(struct rtl8169_private *tp)
{
	if (rtl_is_8125(tp))
		RTL_W16(tp, TxPoll_8125, BIT(0));
	else
		RTL_W8(tp, TxPoll, NPQ);
}

static netdev_tx_t rtl8169_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	unsigned int entry = tp->cur_tx % NUM_TX_DESC;
	struct TxDesc *txd_first, *txd_last;
	bool stop_queue, door_bell;
	unsigned int frags;
	u32 opts[2];

	if (unlikely(!rtl_tx_slots_avail(tp))) {
		if (net_ratelimit())
			netdev_err(dev, "BUG! Tx Ring full when queue awake!\n");
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	opts[1] = rtl8169_tx_vlan_tag(skb);
	opts[0] = 0;

	if (!rtl_chip_supports_csum_v2(tp))
		rtl8169_tso_csum_v1(skb, opts);
	else if (!rtl8169_tso_csum_v2(tp, skb, opts))
		goto err_dma_0;

	if (unlikely(rtl8169_tx_map(tp, opts, skb_headlen(skb), skb->data,
				    entry, false)))
		goto err_dma_0;

	txd_first = tp->TxDescArray + entry;

	frags = skb_shinfo(skb)->nr_frags;
	if (frags) {
		if (rtl8169_xmit_frags(tp, skb, opts, entry))
			goto err_dma_1;
		entry = (entry + frags) % NUM_TX_DESC;
	}

	txd_last = tp->TxDescArray + entry;
	txd_last->opts1 |= cpu_to_le32(LastFrag);
	tp->tx_skb[entry].skb = skb;

	skb_tx_timestamp(skb);

	/* Force memory writes to complete before releasing descriptor */
	dma_wmb();

	door_bell = __netdev_sent_queue(dev, skb->len, netdev_xmit_more());

	txd_first->opts1 |= cpu_to_le32(DescOwn | FirstFrag);

	/* rtl_tx needs to see descriptor changes before updated tp->cur_tx */
	smp_wmb();

	WRITE_ONCE(tp->cur_tx, tp->cur_tx + frags + 1);

	stop_queue = !netif_subqueue_maybe_stop(dev, 0, rtl_tx_slots_avail(tp),
						R8169_TX_STOP_THRS,
						R8169_TX_START_THRS);
	if (door_bell || stop_queue)
		rtl8169_doorbell(tp);

	return NETDEV_TX_OK;

err_dma_1:
	rtl8169_unmap_tx_skb(tp, entry);
err_dma_0:
	dev_kfree_skb_any(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static unsigned int rtl_last_frag_len(struct sk_buff *skb)
{
	struct skb_shared_info *info = skb_shinfo(skb);
	unsigned int nr_frags = info->nr_frags;

	if (!nr_frags)
		return UINT_MAX;

	return skb_frag_size(info->frags + nr_frags - 1);
}

/* Workaround for hw issues with TSO on RTL8168evl */
static netdev_features_t rtl8168evl_fix_tso(struct sk_buff *skb,
					    netdev_features_t features)
{
	/* IPv4 header has options field */
	if (vlan_get_protocol(skb) == htons(ETH_P_IP) &&
	    ip_hdrlen(skb) > sizeof(struct iphdr))
		features &= ~NETIF_F_ALL_TSO;

	/* IPv4 TCP header has options field */
	else if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV4 &&
		 tcp_hdrlen(skb) > sizeof(struct tcphdr))
		features &= ~NETIF_F_ALL_TSO;

	else if (rtl_last_frag_len(skb) <= 6)
		features &= ~NETIF_F_ALL_TSO;

	return features;
}

static netdev_features_t rtl8169_features_check(struct sk_buff *skb,
						struct net_device *dev,
						netdev_features_t features)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	if (skb_is_gso(skb)) {
		if (tp->mac_version == RTL_GIGA_MAC_VER_34)
			features = rtl8168evl_fix_tso(skb, features);

		if (skb_transport_offset(skb) > GTTCPHO_MAX &&
		    rtl_chip_supports_csum_v2(tp))
			features &= ~NETIF_F_ALL_TSO;
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		/* work around hw bug on some chip versions */
		if (skb->len < ETH_ZLEN)
			features &= ~NETIF_F_CSUM_MASK;

		if (rtl_quirk_packet_padto(tp, skb))
			features &= ~NETIF_F_CSUM_MASK;

		if (skb_transport_offset(skb) > TCPHO_MAX &&
		    rtl_chip_supports_csum_v2(tp))
			features &= ~NETIF_F_CSUM_MASK;
	}

	return vlan_features_check(skb, features);
}

static void rtl8169_pcierr_interrupt(struct net_device *dev)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	struct pci_dev *pdev = tp->pci_dev;
	int pci_status_errs;
	u16 pci_cmd;

	pci_read_config_word(pdev, PCI_COMMAND, &pci_cmd);

	pci_status_errs = pci_status_get_and_clear_errors(pdev);

	if (net_ratelimit())
		netdev_err(dev, "PCI error (cmd = 0x%04x, status_errs = 0x%04x)\n",
			   pci_cmd, pci_status_errs);

	rtl_schedule_task(tp, RTL_FLAG_TASK_RESET_PENDING);
}

static void rtl_tx(struct net_device *dev, struct rtl8169_private *tp,
		   int budget)
{
	unsigned int dirty_tx, bytes_compl = 0, pkts_compl = 0;
	struct sk_buff *skb;

	dirty_tx = tp->dirty_tx;

	while (READ_ONCE(tp->cur_tx) != dirty_tx) {
		unsigned int entry = dirty_tx % NUM_TX_DESC;
		u32 status;

		status = le32_to_cpu(READ_ONCE(tp->TxDescArray[entry].opts1));
		if (status & DescOwn)
			break;

		skb = tp->tx_skb[entry].skb;
		rtl8169_unmap_tx_skb(tp, entry);

		if (skb) {
			pkts_compl++;
			bytes_compl += skb->len;
			napi_consume_skb(skb, budget);
		}
		dirty_tx++;
	}

	if (tp->dirty_tx != dirty_tx) {
		dev_sw_netstats_tx_add(dev, pkts_compl, bytes_compl);
		WRITE_ONCE(tp->dirty_tx, dirty_tx);

		netif_subqueue_completed_wake(dev, 0, pkts_compl, bytes_compl,
					      rtl_tx_slots_avail(tp),
					      R8169_TX_START_THRS);
		/*
		 * 8168 hack: TxPoll requests are lost when the Tx packets are
		 * too close. Let's kick an extra TxPoll request when a burst
		 * of start_xmit activity is detected (if it is not detected,
		 * it is slow enough). -- FR
		 * If skb is NULL then we come here again once a tx irq is
		 * triggered after the last fragment is marked transmitted.
		 */
		if (READ_ONCE(tp->cur_tx) != dirty_tx && skb)
			rtl8169_doorbell(tp);
	}
}

static inline int rtl8169_fragmented_frame(u32 status)
{
	return (status & (FirstFrag | LastFrag)) != (FirstFrag | LastFrag);
}

static inline void rtl8169_rx_csum(struct sk_buff *skb, u32 opts1)
{
	u32 status = opts1 & (RxProtoMask | RxCSFailMask);

	if (status == RxProtoTCP || status == RxProtoUDP)
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	else
		skb_checksum_none_assert(skb);
}

static int rtl_rx(struct net_device *dev, struct rtl8169_private *tp, int budget)
{
	struct device *d = tp_to_dev(tp);
	int count;

	for (count = 0; count < budget; count++, tp->cur_rx++) {
		unsigned int pkt_size, entry = tp->cur_rx % NUM_RX_DESC;
		struct RxDesc *desc = tp->RxDescArray + entry;
		struct sk_buff *skb;
		const void *rx_buf;
		dma_addr_t addr;
		u32 status;

		status = le32_to_cpu(READ_ONCE(desc->opts1));
		if (status & DescOwn)
			break;

		/* This barrier is needed to keep us from reading
		 * any other fields out of the Rx descriptor until
		 * we know the status of DescOwn
		 */
		dma_rmb();

		if (unlikely(status & RxRES)) {
			if (net_ratelimit())
				netdev_warn(dev, "Rx ERROR. status = %08x\n",
					    status);
			dev->stats.rx_errors++;
			if (status & (RxRWT | RxRUNT))
				dev->stats.rx_length_errors++;
			if (status & RxCRC)
				dev->stats.rx_crc_errors++;

			if (!(dev->features & NETIF_F_RXALL))
				goto release_descriptor;
			else if (status & RxRWT || !(status & (RxRUNT | RxCRC)))
				goto release_descriptor;
		}

		pkt_size = status & GENMASK(13, 0);
		if (likely(!(dev->features & NETIF_F_RXFCS)))
			pkt_size -= ETH_FCS_LEN;

		/* The driver does not support incoming fragmented frames.
		 * They are seen as a symptom of over-mtu sized frames.
		 */
		if (unlikely(rtl8169_fragmented_frame(status))) {
			dev->stats.rx_dropped++;
			dev->stats.rx_length_errors++;
			goto release_descriptor;
		}

		skb = napi_alloc_skb(&tp->napi, pkt_size);
		if (unlikely(!skb)) {
			dev->stats.rx_dropped++;
			goto release_descriptor;
		}

		addr = le64_to_cpu(desc->addr);
		rx_buf = page_address(tp->Rx_databuff[entry]);

		dma_sync_single_for_cpu(d, addr, pkt_size, DMA_FROM_DEVICE);
		prefetch(rx_buf);
		skb_copy_to_linear_data(skb, rx_buf, pkt_size);
		skb->tail += pkt_size;
		skb->len = pkt_size;
		dma_sync_single_for_device(d, addr, pkt_size, DMA_FROM_DEVICE);

		rtl8169_rx_csum(skb, status);
		skb->protocol = eth_type_trans(skb, dev);

		rtl8169_rx_vlan_tag(desc, skb);

		if (skb->pkt_type == PACKET_MULTICAST)
			dev->stats.multicast++;

		napi_gro_receive(&tp->napi, skb);

		dev_sw_netstats_rx_add(dev, pkt_size);
release_descriptor:
		rtl8169_mark_to_asic(desc);
	}

	return count;
}

static irqreturn_t rtl8169_interrupt(int irq, void *dev_instance)
{
	struct rtl8169_private *tp = dev_instance;
	u32 status = rtl_get_events(tp);

	if ((status & 0xffff) == 0xffff || !(status & tp->irq_mask))
		return IRQ_NONE;

	/* At least RTL8168fp may unexpectedly set the SYSErr bit */
	if (unlikely(status & SYSErr &&
	    tp->mac_version <= RTL_GIGA_MAC_VER_06)) {
		rtl8169_pcierr_interrupt(tp->dev);
		goto out;
	}

	if (status & LinkChg)
		phy_mac_interrupt(tp->phydev);

	rtl_irq_disable(tp);
	napi_schedule(&tp->napi);
out:
	rtl_ack_events(tp, status);

	return IRQ_HANDLED;
}

static void rtl_task(struct work_struct *work)
{
	struct rtl8169_private *tp =
		container_of(work, struct rtl8169_private, wk.work);
	int ret;

	if (test_and_clear_bit(RTL_FLAG_TASK_TX_TIMEOUT, tp->wk.flags)) {
		/* if chip isn't accessible, reset bus to revive it */
		if (RTL_R32(tp, TxConfig) == ~0) {
			ret = pci_reset_bus(tp->pci_dev);
			if (ret < 0) {
				netdev_err(tp->dev, "Can't reset secondary PCI bus, detach NIC\n");
				netif_device_detach(tp->dev);
				return;
			}
		}

		/* ASPM compatibility issues are a typical reason for tx timeouts */
		ret = pci_disable_link_state(tp->pci_dev, PCIE_LINK_STATE_L1 |
							  PCIE_LINK_STATE_L0S);
		if (!ret)
			netdev_warn_once(tp->dev, "ASPM disabled on Tx timeout\n");
		goto reset;
	}

	if (test_and_clear_bit(RTL_FLAG_TASK_RESET_PENDING, tp->wk.flags)) {
reset:
		rtl_reset_work(tp);
		netif_wake_queue(tp->dev);
	}
}

static int rtl8169_poll(struct napi_struct *napi, int budget)
{
	struct rtl8169_private *tp = container_of(napi, struct rtl8169_private, napi);
	struct net_device *dev = tp->dev;
	int work_done;

	rtl_tx(dev, tp, budget);

	work_done = rtl_rx(dev, tp, budget);

	if (work_done < budget && napi_complete_done(napi, work_done))
		rtl_irq_enable(tp);

	return work_done;
}

static void r8169_phylink_handler(struct net_device *ndev)
{
	struct rtl8169_private *tp = netdev_priv(ndev);
	struct device *d = tp_to_dev(tp);

	if (netif_carrier_ok(ndev)) {
		rtl_link_chg_patch(tp);
		pm_request_resume(d);
	} else {
		pm_runtime_idle(d);
	}

	phy_print_status(tp->phydev);
}

static int r8169_phy_connect(struct rtl8169_private *tp)
{
	struct phy_device *phydev = tp->phydev;
	phy_interface_t phy_mode;
	int ret;

	phy_mode = tp->supports_gmii ? PHY_INTERFACE_MODE_GMII :
		   PHY_INTERFACE_MODE_MII;

	ret = phy_connect_direct(tp->dev, phydev, r8169_phylink_handler,
				 phy_mode);
	if (ret)
		return ret;

	if (!tp->supports_gmii)
		phy_set_max_speed(phydev, SPEED_100);

	phy_attached_info(phydev);

	return 0;
}

static void rtl8169_down(struct rtl8169_private *tp)
{
	disable_work_sync(&tp->wk.work);
	/* Clear all task flags */
	bitmap_zero(tp->wk.flags, RTL_FLAG_MAX);

	phy_stop(tp->phydev);

	rtl8169_update_counters(tp);

	pci_clear_master(tp->pci_dev);
	rtl_pci_commit(tp);

	rtl8169_cleanup(tp);
	rtl_disable_exit_l1(tp);
	rtl_prepare_power_down(tp);

	if (tp->dash_type != RTL_DASH_NONE)
		rtl8168_driver_stop(tp);
}

static void rtl8169_up(struct rtl8169_private *tp)
{
	if (tp->dash_type != RTL_DASH_NONE)
		rtl8168_driver_start(tp);

	pci_set_master(tp->pci_dev);
	phy_init_hw(tp->phydev);
	phy_resume(tp->phydev);
	rtl8169_init_phy(tp);
	napi_enable(&tp->napi);
	enable_work(&tp->wk.work);
	rtl_reset_work(tp);

	phy_start(tp->phydev);
}

static int rtl8169_close(struct net_device *dev)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	struct pci_dev *pdev = tp->pci_dev;

	pm_runtime_get_sync(&pdev->dev);

	netif_stop_queue(dev);
	rtl8169_down(tp);
	rtl8169_rx_clear(tp);

	free_irq(tp->irq, tp);

	phy_disconnect(tp->phydev);

	dma_free_coherent(&pdev->dev, R8169_RX_RING_BYTES, tp->RxDescArray,
			  tp->RxPhyAddr);
	dma_free_coherent(&pdev->dev, R8169_TX_RING_BYTES, tp->TxDescArray,
			  tp->TxPhyAddr);
	tp->TxDescArray = NULL;
	tp->RxDescArray = NULL;

	pm_runtime_put_sync(&pdev->dev);

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void rtl8169_netpoll(struct net_device *dev)
{
	struct rtl8169_private *tp = netdev_priv(dev);

	rtl8169_interrupt(tp->irq, tp);
}
#endif

static int rtl_open(struct net_device *dev)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	struct pci_dev *pdev = tp->pci_dev;
	unsigned long irqflags;
	int retval = -ENOMEM;

	pm_runtime_get_sync(&pdev->dev);

	/*
	 * Rx and Tx descriptors needs 256 bytes alignment.
	 * dma_alloc_coherent provides more.
	 */
	tp->TxDescArray = dma_alloc_coherent(&pdev->dev, R8169_TX_RING_BYTES,
					     &tp->TxPhyAddr, GFP_KERNEL);
	if (!tp->TxDescArray)
		goto out;

	tp->RxDescArray = dma_alloc_coherent(&pdev->dev, R8169_RX_RING_BYTES,
					     &tp->RxPhyAddr, GFP_KERNEL);
	if (!tp->RxDescArray)
		goto err_free_tx_0;

	retval = rtl8169_init_ring(tp);
	if (retval < 0)
		goto err_free_rx_1;

	rtl_request_firmware(tp);

	irqflags = pci_dev_msi_enabled(pdev) ? IRQF_NO_THREAD : IRQF_SHARED;
	retval = request_irq(tp->irq, rtl8169_interrupt, irqflags, dev->name, tp);
	if (retval < 0)
		goto err_release_fw_2;

	retval = r8169_phy_connect(tp);
	if (retval)
		goto err_free_irq;

	rtl8169_up(tp);
	rtl8169_init_counter_offsets(tp);
	netif_start_queue(dev);
out:
	pm_runtime_put_sync(&pdev->dev);

	return retval;

err_free_irq:
	free_irq(tp->irq, tp);
err_release_fw_2:
	rtl_release_firmware(tp);
	rtl8169_rx_clear(tp);
err_free_rx_1:
	dma_free_coherent(&pdev->dev, R8169_RX_RING_BYTES, tp->RxDescArray,
			  tp->RxPhyAddr);
	tp->RxDescArray = NULL;
err_free_tx_0:
	dma_free_coherent(&pdev->dev, R8169_TX_RING_BYTES, tp->TxDescArray,
			  tp->TxPhyAddr);
	tp->TxDescArray = NULL;
	goto out;
}

static void
rtl8169_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct rtl8169_private *tp = netdev_priv(dev);
	struct pci_dev *pdev = tp->pci_dev;
	struct rtl8169_counters *counters = tp->counters;

	pm_runtime_get_noresume(&pdev->dev);

	netdev_stats_to_stats64(stats, &dev->stats);
	dev_fetch_sw_netstats(stats, dev->tstats);

	/*
	 * Fetch additional counter values missing in stats collected by driver
	 * from tally counters.
	 */
	if (pm_runtime_active(&pdev->dev))
		rtl8169_update_counters(tp);

	/*
	 * Subtract values fetched during initalization.
	 * See rtl8169_init_counter_offsets for a description why we do that.
	 */
	stats->tx_errors = le64_to_cpu(counters->tx_errors) -
		le64_to_cpu(tp->tc_offset.tx_errors);
	stats->collisions = le32_to_cpu(counters->tx_multi_collision) -
		le32_to_cpu(tp->tc_offset.tx_multi_collision);
	stats->tx_aborted_errors = le16_to_cpu(counters->tx_aborted) -
		le16_to_cpu(tp->tc_offset.tx_aborted);
	stats->rx_missed_errors = le16_to_cpu(counters->rx_missed) -
		le16_to_cpu(tp->tc_offset.rx_missed);

	pm_runtime_put_noidle(&pdev->dev);
}

static void rtl8169_net_suspend(struct rtl8169_private *tp)
{
	netif_device_detach(tp->dev);

	if (netif_running(tp->dev))
		rtl8169_down(tp);
}

static int rtl8169_runtime_resume(struct device *dev)
{
	struct rtl8169_private *tp = dev_get_drvdata(dev);

	rtl_rar_set(tp, tp->dev->dev_addr);
	__rtl8169_set_wol(tp, tp->saved_wolopts);

	if (tp->TxDescArray)
		rtl8169_up(tp);

	netif_device_attach(tp->dev);

	return 0;
}

static int rtl8169_suspend(struct device *device)
{
	struct rtl8169_private *tp = dev_get_drvdata(device);

	rtnl_lock();
	rtl8169_net_suspend(tp);
	if (!device_may_wakeup(tp_to_dev(tp)))
		clk_disable_unprepare(tp->clk);
	rtnl_unlock();

	return 0;
}

static int rtl8169_resume(struct device *device)
{
	struct rtl8169_private *tp = dev_get_drvdata(device);

	if (!device_may_wakeup(tp_to_dev(tp)))
		clk_prepare_enable(tp->clk);

	/* Reportedly at least Asus X453MA truncates packets otherwise */
	if (tp->mac_version == RTL_GIGA_MAC_VER_37)
		rtl_init_rxcfg(tp);

	return rtl8169_runtime_resume(device);
}

static int rtl8169_runtime_suspend(struct device *device)
{
	struct rtl8169_private *tp = dev_get_drvdata(device);

	if (!tp->TxDescArray) {
		netif_device_detach(tp->dev);
		return 0;
	}

	rtnl_lock();
	__rtl8169_set_wol(tp, WAKE_PHY);
	rtl8169_net_suspend(tp);
	rtnl_unlock();

	return 0;
}

static int rtl8169_runtime_idle(struct device *device)
{
	struct rtl8169_private *tp = dev_get_drvdata(device);

	if (tp->dash_enabled)
		return -EBUSY;

	if (!netif_running(tp->dev) || !netif_carrier_ok(tp->dev))
		pm_schedule_suspend(device, 10000);

	return -EBUSY;
}

static const struct dev_pm_ops rtl8169_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(rtl8169_suspend, rtl8169_resume)
	RUNTIME_PM_OPS(rtl8169_runtime_suspend, rtl8169_runtime_resume,
		       rtl8169_runtime_idle)
};

static void rtl_shutdown(struct pci_dev *pdev)
{
	struct rtl8169_private *tp = pci_get_drvdata(pdev);

	rtnl_lock();
	rtl8169_net_suspend(tp);
	rtnl_unlock();

	/* Restore original MAC address */
	rtl_rar_set(tp, tp->dev->perm_addr);

	if (system_state == SYSTEM_POWER_OFF && !tp->dash_enabled)
		pci_prepare_to_sleep(pdev);
}

static void rtl_remove_one(struct pci_dev *pdev)
{
	struct rtl8169_private *tp = pci_get_drvdata(pdev);

	if (pci_dev_run_wake(pdev))
		pm_runtime_get_noresume(&pdev->dev);

	disable_work_sync(&tp->wk.work);

	if (IS_ENABLED(CONFIG_R8169_LEDS))
		r8169_remove_leds(tp->leds);

	unregister_netdev(tp->dev);

	if (tp->dash_type != RTL_DASH_NONE)
		rtl8168_driver_stop(tp);

	rtl_release_firmware(tp);

	/* restore original MAC address */
	rtl_rar_set(tp, tp->dev->perm_addr);
}

static const struct net_device_ops rtl_netdev_ops = {
	.ndo_open		= rtl_open,
	.ndo_stop		= rtl8169_close,
	.ndo_get_stats64	= rtl8169_get_stats64,
	.ndo_start_xmit		= rtl8169_start_xmit,
	.ndo_features_check	= rtl8169_features_check,
	.ndo_tx_timeout		= rtl8169_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_change_mtu		= rtl8169_change_mtu,
	.ndo_fix_features	= rtl8169_fix_features,
	.ndo_set_features	= rtl8169_set_features,
	.ndo_set_mac_address	= rtl_set_mac_address,
	.ndo_eth_ioctl		= phy_do_ioctl_running,
	.ndo_set_rx_mode	= rtl_set_rx_mode,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= rtl8169_netpoll,
#endif

};

static void rtl_set_irq_mask(struct rtl8169_private *tp)
{
	tp->irq_mask = RxOK | RxErr | TxOK | TxErr | LinkChg;

	if (tp->mac_version <= RTL_GIGA_MAC_VER_06)
		tp->irq_mask |= SYSErr | RxFIFOOver;
}

static int rtl_alloc_irq(struct rtl8169_private *tp)
{
	unsigned int flags;

	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_02 ... RTL_GIGA_MAC_VER_06:
		rtl_unlock_config_regs(tp);
		RTL_W8(tp, Config2, RTL_R8(tp, Config2) & ~MSIEnable);
		rtl_lock_config_regs(tp);
		fallthrough;
	case RTL_GIGA_MAC_VER_07 ... RTL_GIGA_MAC_VER_17:
		flags = PCI_IRQ_INTX;
		break;
	default:
		flags = PCI_IRQ_ALL_TYPES;
		break;
	}

	return pci_alloc_irq_vectors(tp->pci_dev, 1, 1, flags);
}

static void rtl_read_mac_address(struct rtl8169_private *tp,
				 u8 mac_addr[ETH_ALEN])
{
	/* Get MAC address */
	if (rtl_is_8168evl_up(tp) && tp->mac_version != RTL_GIGA_MAC_VER_34) {
		u32 value;

		value = rtl_eri_read(tp, 0xe0);
		put_unaligned_le32(value, mac_addr);
		value = rtl_eri_read(tp, 0xe4);
		put_unaligned_le16(value, mac_addr + 4);
	} else if (rtl_is_8125(tp)) {
		rtl_read_mac_from_reg(tp, mac_addr, MAC0_BKP);
	}
}

DECLARE_RTL_COND(rtl_link_list_ready_cond)
{
	return RTL_R8(tp, MCU) & LINK_LIST_RDY;
}

static void r8168g_wait_ll_share_fifo_ready(struct rtl8169_private *tp)
{
	rtl_loop_wait_high(tp, &rtl_link_list_ready_cond, 100, 42);
}

static int r8169_mdio_read_reg(struct mii_bus *mii_bus, int phyaddr, int phyreg)
{
	struct rtl8169_private *tp = mii_bus->priv;

	if (phyaddr > 0)
		return -ENODEV;

	return rtl_readphy(tp, phyreg);
}

static int r8169_mdio_write_reg(struct mii_bus *mii_bus, int phyaddr,
				int phyreg, u16 val)
{
	struct rtl8169_private *tp = mii_bus->priv;

	if (phyaddr > 0)
		return -ENODEV;

	rtl_writephy(tp, phyreg, val);

	return 0;
}

static int r8169_mdio_read_reg_c45(struct mii_bus *mii_bus, int addr,
				   int devnum, int regnum)
{
	struct rtl8169_private *tp = mii_bus->priv;

	if (addr > 0)
		return -ENODEV;

	if (devnum == MDIO_MMD_VEND2 && regnum > MDIO_STAT2)
		return r8168_phy_ocp_read(tp, regnum);

	return 0;
}

static int r8169_mdio_write_reg_c45(struct mii_bus *mii_bus, int addr,
				    int devnum, int regnum, u16 val)
{
	struct rtl8169_private *tp = mii_bus->priv;

	if (addr > 0 || devnum != MDIO_MMD_VEND2 || regnum <= MDIO_STAT2)
		return -ENODEV;

	r8168_phy_ocp_write(tp, regnum, val);

	return 0;
}

static int r8169_mdio_register(struct rtl8169_private *tp)
{
	struct pci_dev *pdev = tp->pci_dev;
	struct mii_bus *new_bus;
	int ret;

	/* On some boards with this chip version the BIOS is buggy and misses
	 * to reset the PHY page selector. This results in the PHY ID read
	 * accessing registers on a different page, returning a more or
	 * less random value. Fix this by resetting the page selector first.
	 */
	if (tp->mac_version == RTL_GIGA_MAC_VER_25 ||
	    tp->mac_version == RTL_GIGA_MAC_VER_26)
		r8169_mdio_write(tp, 0x1f, 0);

	new_bus = devm_mdiobus_alloc(&pdev->dev);
	if (!new_bus)
		return -ENOMEM;

	new_bus->name = "r8169";
	new_bus->priv = tp;
	new_bus->parent = &pdev->dev;
	new_bus->irq[0] = PHY_MAC_INTERRUPT;
	new_bus->phy_mask = GENMASK(31, 1);
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "r8169-%x-%x",
		 pci_domain_nr(pdev->bus), pci_dev_id(pdev));

	new_bus->read = r8169_mdio_read_reg;
	new_bus->write = r8169_mdio_write_reg;

	if (tp->mac_version >= RTL_GIGA_MAC_VER_40) {
		new_bus->read_c45 = r8169_mdio_read_reg_c45;
		new_bus->write_c45 = r8169_mdio_write_reg_c45;
	}

	ret = devm_mdiobus_register(&pdev->dev, new_bus);
	if (ret)
		return ret;

	tp->phydev = mdiobus_get_phy(new_bus, 0);
	if (!tp->phydev) {
		return -ENODEV;
	} else if (!tp->phydev->drv) {
		/* Most chip versions fail with the genphy driver.
		 * Therefore ensure that the dedicated PHY driver is loaded.
		 */
		dev_err(&pdev->dev, "no dedicated PHY driver found for PHY ID 0x%08x, maybe realtek.ko needs to be added to initramfs?\n",
			tp->phydev->phy_id);
		return -EUNATCH;
	}

	tp->phydev->mac_managed_pm = true;
	if (rtl_supports_eee(tp))
		phy_support_eee(tp->phydev);
	phy_support_asym_pause(tp->phydev);

	/* mimic behavior of r8125/r8126 vendor drivers */
	if (tp->mac_version == RTL_GIGA_MAC_VER_61)
		phy_disable_eee_mode(tp->phydev,
				     ETHTOOL_LINK_MODE_2500baseT_Full_BIT);

	/* PHY will be woken up in rtl_open() */
	phy_suspend(tp->phydev);

	return 0;
}

static void rtl_hw_init_8168g(struct rtl8169_private *tp)
{
	rtl_enable_rxdvgate(tp);

	RTL_W8(tp, ChipCmd, RTL_R8(tp, ChipCmd) & ~(CmdTxEnb | CmdRxEnb));
	msleep(1);
	RTL_W8(tp, MCU, RTL_R8(tp, MCU) & ~NOW_IS_OOB);

	r8168_mac_ocp_modify(tp, 0xe8de, BIT(14), 0);
	r8168g_wait_ll_share_fifo_ready(tp);

	r8168_mac_ocp_modify(tp, 0xe8de, 0, BIT(15));
	r8168g_wait_ll_share_fifo_ready(tp);
}

static void rtl_hw_init_8125(struct rtl8169_private *tp)
{
	rtl_enable_rxdvgate(tp);

	RTL_W8(tp, ChipCmd, RTL_R8(tp, ChipCmd) & ~(CmdTxEnb | CmdRxEnb));
	msleep(1);
	RTL_W8(tp, MCU, RTL_R8(tp, MCU) & ~NOW_IS_OOB);

	r8168_mac_ocp_modify(tp, 0xe8de, BIT(14), 0);
	r8168g_wait_ll_share_fifo_ready(tp);

	r8168_mac_ocp_write(tp, 0xc0aa, 0x07d0);
	r8168_mac_ocp_write(tp, 0xc0a6, 0x0150);
	r8168_mac_ocp_write(tp, 0xc01e, 0x5555);
	r8168g_wait_ll_share_fifo_ready(tp);
}

static void rtl_hw_initialize(struct rtl8169_private *tp)
{
	switch (tp->mac_version) {
	case RTL_GIGA_MAC_VER_51 ... RTL_GIGA_MAC_VER_52:
		rtl8168ep_stop_cmac(tp);
		fallthrough;
	case RTL_GIGA_MAC_VER_40 ... RTL_GIGA_MAC_VER_48:
		rtl_hw_init_8168g(tp);
		break;
	case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_LAST:
		rtl_hw_init_8125(tp);
		break;
	default:
		break;
	}
}

static int rtl_jumbo_max(struct rtl8169_private *tp)
{
	/* Non-GBit versions don't support jumbo frames */
	if (!tp->supports_gmii)
		return 0;

	switch (tp->mac_version) {
	/* RTL8169 */
	case RTL_GIGA_MAC_VER_02 ... RTL_GIGA_MAC_VER_06:
		return JUMBO_7K;
	/* RTL8168b */
	case RTL_GIGA_MAC_VER_17:
		return JUMBO_4K;
	/* RTL8168c */
	case RTL_GIGA_MAC_VER_18 ... RTL_GIGA_MAC_VER_24:
		return JUMBO_6K;
	/* RTL8125/8126 */
	case RTL_GIGA_MAC_VER_61 ... RTL_GIGA_MAC_VER_LAST:
		return JUMBO_16K;
	default:
		return JUMBO_9K;
	}
}

static void rtl_init_mac_address(struct rtl8169_private *tp)
{
	u8 mac_addr[ETH_ALEN] __aligned(2) = {};
	struct net_device *dev = tp->dev;
	int rc;

	rc = eth_platform_get_mac_address(tp_to_dev(tp), mac_addr);
	if (!rc)
		goto done;

	rtl_read_mac_address(tp, mac_addr);
	if (is_valid_ether_addr(mac_addr))
		goto done;

	rtl_read_mac_from_reg(tp, mac_addr, MAC0);
	if (is_valid_ether_addr(mac_addr))
		goto done;

	eth_random_addr(mac_addr);
	dev->addr_assign_type = NET_ADDR_RANDOM;
	dev_warn(tp_to_dev(tp), "can't read MAC address, setting random one\n");
done:
	eth_hw_addr_set(dev, mac_addr);
	rtl_rar_set(tp, mac_addr);
}

/* register is set if system vendor successfully tested ASPM 1.2 */
static bool rtl_aspm_is_safe(struct rtl8169_private *tp)
{
	if (tp->mac_version >= RTL_GIGA_MAC_VER_46 &&
	    r8168_mac_ocp_read(tp, 0xc0b2) & 0xf)
		return true;

	return false;
}

static int rtl_init_one(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	const struct rtl_chip_info *chip;
	struct rtl8169_private *tp;
	int jumbo_max, region, rc;
	struct net_device *dev;
	u32 txconfig;
	u16 xid;

	dev = devm_alloc_etherdev(&pdev->dev, sizeof (*tp));
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, &pdev->dev);
	dev->netdev_ops = &rtl_netdev_ops;
	tp = netdev_priv(dev);
	tp->dev = dev;
	tp->pci_dev = pdev;
	tp->supports_gmii = ent->driver_data == RTL_CFG_NO_GBIT ? 0 : 1;
	tp->ocp_base = OCP_STD_PHY_BASE;

	raw_spin_lock_init(&tp->mac_ocp_lock);
	mutex_init(&tp->led_lock);

	/* Get the *optional* external "ether_clk" used on some boards */
	tp->clk = devm_clk_get_optional_enabled(&pdev->dev, "ether_clk");
	if (IS_ERR(tp->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(tp->clk), "failed to get ether_clk\n");

	/* enable device (incl. PCI PM wakeup and hotplug setup) */
	rc = pcim_enable_device(pdev);
	if (rc < 0)
		return dev_err_probe(&pdev->dev, rc, "enable failure\n");

	if (pcim_set_mwi(pdev) < 0)
		dev_info(&pdev->dev, "Mem-Wr-Inval unavailable\n");

	/* use first MMIO region */
	region = ffs(pci_select_bars(pdev, IORESOURCE_MEM)) - 1;
	if (region < 0)
		return dev_err_probe(&pdev->dev, -ENODEV, "no MMIO resource found\n");

	tp->mmio_addr = pcim_iomap_region(pdev, region, KBUILD_MODNAME);
	if (IS_ERR(tp->mmio_addr))
		return dev_err_probe(&pdev->dev, PTR_ERR(tp->mmio_addr),
				     "cannot remap MMIO, aborting\n");

	txconfig = RTL_R32(tp, TxConfig);
	if (txconfig == ~0U)
		return dev_err_probe(&pdev->dev, -EIO, "PCI read failed\n");

	xid = (txconfig >> 20) & 0xfcf;

	/* Identify chip attached to board */
	chip = rtl8169_get_chip_version(xid, tp->supports_gmii);
	if (chip->mac_version == RTL_GIGA_MAC_NONE)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unknown chip XID %03x, contact r8169 maintainers (see MAINTAINERS file)\n",
				     xid);
	tp->mac_version = chip->mac_version;
	tp->fw_name = chip->fw_name;

	/* Disable ASPM L1 as that cause random device stop working
	 * problems as well as full system hangs for some PCIe devices users.
	 */
	if (rtl_aspm_is_safe(tp))
		rc = 0;
	else
		rc = pci_disable_link_state(pdev, PCIE_LINK_STATE_L1);
	tp->aspm_manageable = !rc;

	tp->dash_type = rtl_get_dash_type(tp);
	tp->dash_enabled = rtl_dash_is_enabled(tp);

	tp->cp_cmd = RTL_R16(tp, CPlusCmd) & CPCMD_MASK;

	if (sizeof(dma_addr_t) > 4 && tp->mac_version >= RTL_GIGA_MAC_VER_18 &&
	    !dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64)))
		dev->features |= NETIF_F_HIGHDMA;

	rtl_init_rxcfg(tp);

	rtl8169_irq_mask_and_ack(tp);

	rtl_hw_initialize(tp);

	rtl_hw_reset(tp);

	rc = rtl_alloc_irq(tp);
	if (rc < 0)
		return dev_err_probe(&pdev->dev, rc, "Can't allocate interrupt\n");

	tp->irq = pci_irq_vector(pdev, 0);

	INIT_WORK(&tp->wk.work, rtl_task);
	disable_work(&tp->wk.work);

	rtl_init_mac_address(tp);

	dev->ethtool_ops = &rtl8169_ethtool_ops;

	netif_napi_add(dev, &tp->napi, rtl8169_poll);

	dev->hw_features = NETIF_F_IP_CSUM | NETIF_F_RXCSUM |
			   NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX;
	dev->vlan_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_TSO;
	dev->priv_flags |= IFF_LIVE_ADDR_CHANGE;

	/*
	 * Pretend we are using VLANs; This bypasses a nasty bug where
	 * Interrupts stop flowing on high load on 8110SCd controllers.
	 */
	if (tp->mac_version == RTL_GIGA_MAC_VER_05)
		/* Disallow toggling */
		dev->hw_features &= ~NETIF_F_HW_VLAN_CTAG_RX;

	if (rtl_chip_supports_csum_v2(tp))
		dev->hw_features |= NETIF_F_IPV6_CSUM;

	dev->features |= dev->hw_features;

	if (rtl_chip_supports_csum_v2(tp)) {
		dev->hw_features |= NETIF_F_SG | NETIF_F_TSO | NETIF_F_TSO6;
		netif_set_tso_max_size(dev, RTL_GSO_MAX_SIZE_V2);
		netif_set_tso_max_segs(dev, RTL_GSO_MAX_SEGS_V2);
	} else {
		dev->hw_features |= NETIF_F_SG | NETIF_F_TSO;
		netif_set_tso_max_size(dev, RTL_GSO_MAX_SIZE_V1);
		netif_set_tso_max_segs(dev, RTL_GSO_MAX_SEGS_V1);
	}

	/* There has been a number of reports that using SG/TSO results in
	 * tx timeouts. However for a lot of people SG/TSO works fine.
	 * It's not fully clear which chip versions are affected. Vendor
	 * drivers enable SG/TSO for certain chip versions per default,
	 * let's mimic this here. On other chip versions users can
	 * use ethtool to enable SG/TSO, use at own risk!
	 */
	if (tp->mac_version >= RTL_GIGA_MAC_VER_46 &&
	    tp->mac_version != RTL_GIGA_MAC_VER_61)
		dev->features |= dev->hw_features;

	dev->hw_features |= NETIF_F_RXALL;
	dev->hw_features |= NETIF_F_RXFCS;

	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;

	netdev_sw_irq_coalesce_default_on(dev);

	/* configure chip for default features */
	rtl8169_set_features(dev, dev->features);

	if (!tp->dash_enabled) {
		rtl_set_d3_pll_down(tp, true);
	} else {
		rtl_set_d3_pll_down(tp, false);
		dev->ethtool->wol_enabled = 1;
	}

	jumbo_max = rtl_jumbo_max(tp);
	if (jumbo_max)
		dev->max_mtu = jumbo_max;

	rtl_set_irq_mask(tp);

	tp->counters = dmam_alloc_coherent (&pdev->dev, sizeof(*tp->counters),
					    &tp->counters_phys_addr,
					    GFP_KERNEL);
	if (!tp->counters)
		return -ENOMEM;

	pci_set_drvdata(pdev, tp);

	rc = r8169_mdio_register(tp);
	if (rc)
		return rc;

	rc = register_netdev(dev);
	if (rc)
		return rc;

	if (IS_ENABLED(CONFIG_R8169_LEDS)) {
		if (rtl_is_8125(tp))
			tp->leds = rtl8125_init_leds(dev);
		else if (tp->mac_version > RTL_GIGA_MAC_VER_06)
			tp->leds = rtl8168_init_leds(dev);
	}

	netdev_info(dev, "%s, %pM, XID %03x, IRQ %d\n",
		    chip->name, dev->dev_addr, xid, tp->irq);

	if (jumbo_max)
		netdev_info(dev, "jumbo features [frames: %d bytes, tx checksumming: %s]\n",
			    jumbo_max, tp->mac_version <= RTL_GIGA_MAC_VER_06 ?
			    "ok" : "ko");

	if (tp->dash_type != RTL_DASH_NONE) {
		netdev_info(dev, "DASH %s\n",
			    tp->dash_enabled ? "enabled" : "disabled");
		rtl8168_driver_start(tp);
	}

	if (pci_dev_run_wake(pdev))
		pm_runtime_put_sync(&pdev->dev);

	return 0;
}

static struct pci_driver rtl8169_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= rtl8169_pci_tbl,
	.probe		= rtl_init_one,
	.remove		= rtl_remove_one,
	.shutdown	= rtl_shutdown,
	.driver.pm	= pm_ptr(&rtl8169_pm_ops),
};

module_pci_driver(rtl8169_pci_driver);
