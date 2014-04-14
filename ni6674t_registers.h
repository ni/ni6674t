/*
 * ni6674t_registers.h: Register set(s) used with the NI PXIe-6674T
 *
 * (C) Copyright 2011 National Instruments Corp.
 * Authors: Josh Cartwright <josh.cartwright@ni.com>,
 *          Rick Ratzel <rick.ratzel@ni.com>,
 *          Tyler Krehbiel <tyler.krehbiel@ni.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef _NI6674T_REGISTERS_H_
#define _NI6674T_REGISTERS_H_

#define _NI6674_PASTE(x,y) x ## y
#define NI6674_PASTE(x,y) _NI6674_PASTE(x,y)
#define NI6674_RESERVE_BYTES(bytes) u8 NI6674_PASTE(__reserved, __LINE__)[bytes]

/* Register set for PCI bus interface chip (MITE) */
struct mite {
/*00*/	NI6674_RESERVE_BYTES(0xc0);
/*C0*/	u32 iodwbsr;
#define MITE_IODWBSR_WENAB	(1<<7)
/*C4*/	u32 iowbsr1;
#define MITE_IOWBSR1_WENAB	(1<<7)
#define MITE_IOWBSR1_WSIZE4	(1<<4)
} __attribute__((__packed__));

/* MITE ConfigEngine register set */
#define CE_REGBLOCK_OFFSET	(0x1000)
struct ce {
/*00*/	u32 command;
#define CE_COMMAND_RESET_FIFO	(1<<24)
#define CE_COMMAND_START_FPGA	(1<<2)
/*04*/	u32 flash_info;
/*08*/	u32 prog_pulse_config;
#define CE_PROG_PULSE_START_READY_IMMEDIATE	(1<<18)
#define CE_PROG_PULSE_START_DRIVE_UNASSERT	(1<<17)
#define CE_PROG_PULSE_START_LEN(n)		(n)
/*0C*/	u32 data_config;
#define CE_DATA_DATA_CLKS(n)		((n)<<8)
#define CE_DATA_ORDER_MSB2LSB		(1<<3)
#define CE_DATA_ISPARALLEL		(1<<1)
/*10*/	u32 start_config;
#define CE_START_CLKRDY_DELAY(n)	(n)
/*14*/	u32 stop_config;
#define CE_STOP_POSTCLKS(n)		((n)<<24)
#define CE_STOP_DONEHIGHTRUE		(1<<18)
#define CE_STOP_NOERRHIGHTRUE		(1<<17)
#define CE_STOP_DONERDY_IMMEDIATE	(1<<16)
/*18*/	u32 flash_addr;
/*1C*/	u32 status;
#define CE_STATUS_IN_RESET	(1<<31)
#define CE_STATUS_IN_WAIT_START	(1<<24)
#define CE_STATUS_IN_GEN_DATA	(1<<12)
#define CE_STATUS_CONFIG_DONE	(1<<6)
#define CE_STATUS_CONFIG_ERROR	(1<<4)
#define CE_STATUS_STOP_DOWNLOAD (CE_STATUS_CONFIG_ERROR|CE_STATUS_CONFIG_DONE)
/*20*/	u32 fifo;
/*24*/	u32 self_config;
} __attribute__((__packed__));

/* Register set controlling sync hardware */
struct ni_sync {
/*00*/	NI6674_RESERVE_BYTES(0x24);
/*24*/  u32 dacctrl;
#define DAC_CTRL_SERIAL_PORT_BUSY       (1<<31)
/*28*/  u32 clkinctrl;
#define CLKIN_CTRL_ENABLE(x)		((x)<<1)
/*2c*/  NI6674_RESERVE_BYTES(0x08);
/*34*/  u32 dstaractrl1;
#define DSTARA_SRCA_MUX2(x)             ((x)<<4)
#define DSTARA_SRCA_MUX2_MASK		(DSTARA_SRCA_MUX2(7))
#define DSTARA_SRCB_MUX2(x)             ((x)<<12)
#define DSTARA_SRCB_MUX2_MASK		(DSTARA_SRCB_MUX2(7))
#define DSTARA_BANK_N(n, x)		((x)<<(16+(n*4)))
#define DSTARA_BANK_N_MASK(n)		(DSTARA_BANK_N(n, 3))
#define DSTARA_SRC_CLKIN                (7)
#define DSTARA_SRC_FLOATING		(0)
#define DSTARA_SRC_SRCA			(2)
#define DSTARA_SRC_SRCB			(3)
/*38*/  u32 dstaractrl2;
#define DSTARA_SRCA_USE_DIVIDER(x)	((x)<<20)
#define DSTARA_SRCB_USE_DIVIDER(x)	((x)<<21)
/*3c*/  NI6674_RESERVE_BYTES(0x14);
/*50*/	u32 triggerctrl;
#define TRIG_CTRL_DEST(x)		((x)<<24)
#define TRIG_CTRL_DEST_PXITRIG(n)	((n)+1)
#define TRIG_CTRL_DEST_PXISTAR(n)	((n)+9)
#define TRIG_CTRL_DEST_PXIeDSTARB(n)	((n)+26)
#define TRIG_CTRL_DEST_PFI_SE(n)	((n)+43)
#define TRIG_CTRL_DEST_LVDS(n)		((n)+49)
#define TRIG_CTRL_DEST_STAR_PERIPH	(52)
#define TRIG_CTRL_DEST_DSTARC_PERIPH	(53)
#define TRIG_CTRL_SRC(x)		((x)<<16)
#define TRIG_CTRL_SRC_FLOATING		(0)
#define TRIG_CTRL_SRC_PXITRIG(n)	((n)+1)
#define TRIG_CTRL_SRC_PXISTAR(n)	((n)+9)
#define TRIG_CTRL_SRC_PXIeDSTARC(n)	((n)+26)
#define TRIG_CTRL_SRC_PFI_SE(n)		((n)+43)
#define TRIG_CTRL_SRC_LVDS(n)		((n)+49)
#define TRIG_CTRL_SRC_STAR_PERIPH	(52)
#define TRIG_CTRL_SRC_DSTARB_PERIPH	(53)
#define TRIG_CTRL_SRC_GLOBAL_SW		(54)
#define TRIG_CTRL_SRC_LOCAL_SW		(55)
#define TRIG_CTRL_SRC_SYNC_CLK		(56)
#define TRIG_CTRL_SRC_LOGIC_HIGH	(57)
#define TRIG_CTRL_SRC_LOGIC_LOW		(58)
#define TRIG_CTRL_SYNC_CLOCK(x)		((x)<<14)
#define TRIG_CTRL_SYNC_CLOCK_FULL	(0)
#define TRIG_CTRL_SYNC_CLOCK_DIV1	(2)
#define TRIG_CTRL_SYNC_CLOCK_DIV2	(3)
#define TRIG_CTRL_EDGE_FALLING		(1<<13)
#define TRIG_CTRL_ASYNCHRONOUS		(1<<12)
#define TRIG_CTRL_INVERTED		(1<<11)
#define TRIG_CTRL_ENABLED		(1<<10)
#define TRIG_CTRL_PXI_DELAY(x)		((x)<<7)
/*54*/  NI6674_RESERVE_BYTES(0x0c);
/*60*/  u32 trigread[3];
#define TRIG_READ_PXI_STAR_LINE_STATE_BIT(n)	(n)
#define TRIG_READ_PXI_TRIG_LINE_STATE_BIT(n)	((n)+18)
#define TRIG_READ_PFI_LINE_STATE_BIT(n)		((n)+26)
} __attribute__((__packed__));

#endif
