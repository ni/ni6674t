/*
 * ni6674t.c: Driver for NI-PXIe 6674T signal-based routing board
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
#define DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>

#include "ni6674t.h"
#include "ni6674t_registers.h"

struct ni6674t {
	struct kset *terminal_set;

	struct mutex devlock;

	struct pxi_trig_route_terminal *pxi_trig[8];
	struct route_terminal *pfi[6];
	struct route_terminal *pxi_star[17];
	struct route_terminal *srca;
	struct route_terminal *srcb;
	struct route_terminal *srca_div_sel;
	struct route_terminal *srcb_div_sel;
	struct route_terminal *pxie_dstara[17];
	struct route_terminal *bank[4];
	struct mite __iomem *mite;
	struct ni_sync __iomem *sync;
};

static const char *terminal_polarity_strs[] = {
	[POLARITY_NORMAL]	= "normal",
	[POLARITY_INVERTED]	= "inverted",
};

static const struct route_terminal_desc rt_floating = {
	.name		= "floating",
};

static const struct route_terminal_desc rt_logic_high = {
	.name		= "logic_high",
};

static const struct route_terminal_desc rt_logic_low = {
	.name		= "logic_low",
};

static void triggerctrl_flush_terminal_attrs(struct route_terminal *rt)
{
	const struct route_terminal_desc *dst = rt->rt_desc;
	const struct route_terminal_input *src = rt->input;
	struct ni6674t *dev = rt->owner;
	u32 trigctrl;

	trigctrl = TRIG_CTRL_DEST(dst->dest_data) | TRIG_CTRL_SRC(src->data);

	/* If a user wants the source of this terminal to be
	 *   'floating' we also assume they want output disabled. */
	if (src->desc != &rt_floating)
		trigctrl |= TRIG_CTRL_ENABLED;

	trigctrl |= TRIG_CTRL_ASYNCHRONOUS;

	if (rt->polarity == POLARITY_INVERTED)
		trigctrl |= TRIG_CTRL_INVERTED;

	iowrite32(trigctrl, &dev->sync->triggerctrl);
}

static void triggerctrl_set_input(struct route_terminal *rt,
				  const struct route_terminal_input *input)
{
	triggerctrl_flush_terminal_attrs(rt);
}

static ssize_t route_terminal_current_input_show(struct route_terminal *rt,
					         char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", rt->input->desc->name);
}

static void set_input_and_update_state(struct route_terminal *rt,
				       const struct route_terminal_input *input)
{
	const struct route_terminal_desc *desc = rt->rt_desc;

	/* Update state regardless of whether there's a set_input function or
	 * not.  This is to handle the case of terminals w/ hard-wired inputs
	 * (terminal has an input, but nothing to program). */
	rt->input = input;

	if (desc->set_input)
		desc->set_input(rt, input);
}

static ssize_t route_terminal_current_input_store(struct route_terminal *rt,
						  const char *buf, size_t count)
{
	const struct route_terminal_input *in = rt->rt_desc->available_inputs;
	const char *name;
	size_t len;

	len = strlen(buf);
	if (buf[len - 1] == '\n')
		--len;

	while(in->desc) {
		name = in->desc->name;
		if(!strncmp(buf, name, len)) {
			set_input_and_update_state(rt, in);
			return count;
		}
		in++;
	}

	return -EINVAL;
}

static ssize_t route_terminal_polarity_show(struct route_terminal *rt,
					    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n",
			terminal_polarity_strs[rt->polarity]);
}

static ssize_t route_terminal_polarity_store(struct route_terminal *rt,
					     const char *buf, size_t count)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(terminal_polarity_strs); i++) {
		const char *name = terminal_polarity_strs[i];
		if (!strncmp(name, buf, strlen(name))) {
			rt->polarity = i;
			triggerctrl_flush_terminal_attrs(rt);
			return count;
		}
	}

	return -EINVAL;
}

static ssize_t route_terminal_available_inputs_show(struct route_terminal *rt,
						  char *buf)
{
	size_t total = 0;
	int written = 0;
	const struct route_terminal_input *in = rt->rt_desc->available_inputs;

	while(in->desc) {
		written = snprintf(buf, PAGE_SIZE - total, "%s ",
			   in->desc->name);
		buf += written;
		total += written;
		in++;
	}

	*(buf - 1) = '\n';

	return total;
}

static ssize_t route_terminal_line_state_show(struct route_terminal *rt,
					      char *buf)
{
	struct ni6674t *dev = rt->owner;
	const struct route_terminal_desc* rt_desc = rt->rt_desc;
	u32 trigread;
	unsigned int lsb;
	unsigned long line_state;

	lsb = rt_desc->line_state_bit;
	trigread = ioread32(&dev->sync->trigread[lsb / 32]);
	line_state = !!(trigread & (1 << (lsb % 32)));

	return snprintf(buf, PAGE_SIZE, "%lu\n", line_state);
}

static ROUTE_TERMINAL_ATTR(current_input, 0600);
static ROUTE_TERMINAL_ATTR(polarity, 0600);
static ROUTE_TERMINAL_ATTR_RO(available_inputs, 0600);
static ROUTE_TERMINAL_ATTR_RO(line_state, 0600);

static const struct route_terminal_desc pfi_rt_desc[];

#define PXI_TRIG_RT_DESC_MEMBERS(n)					\
	.name		= "PXI_Trig" #n,				\
	.dest_data	= TRIG_CTRL_DEST_PXITRIG(n),			\
	.line_state_bit	= TRIG_READ_PXI_TRIG_LINE_STATE_BIT(n),		\
	.set_input	= &triggerctrl_set_input

#define PFI_SE_INPUT(n) { &pfi_rt_desc[n], TRIG_CTRL_SRC_PFI_SE(n) }
#define ALL_PFI_SE_INPUTS	\
	PFI_SE_INPUT(0),	\
	PFI_SE_INPUT(1),	\
	PFI_SE_INPUT(2),	\
	PFI_SE_INPUT(3),	\
	PFI_SE_INPUT(4),	\
	PFI_SE_INPUT(5)

static const struct route_terminal_desc pxi_trig_rt_desc[] = {
	{
		PXI_TRIG_RT_DESC_MEMBERS(0),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 },
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(1),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 }
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(2),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 }
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(3),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 }
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(4),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 }
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(5),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 }
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(6),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[7], TRIG_CTRL_SRC_PXITRIG(7) },
				{ NULL, 0 }
			},
	},
	{
		PXI_TRIG_RT_DESC_MEMBERS(7),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				ALL_PFI_SE_INPUTS,
				{ &pxi_trig_rt_desc[0], TRIG_CTRL_SRC_PXITRIG(0) },
				{ &pxi_trig_rt_desc[1], TRIG_CTRL_SRC_PXITRIG(1) },
				{ &pxi_trig_rt_desc[2], TRIG_CTRL_SRC_PXITRIG(2) },
				{ &pxi_trig_rt_desc[3], TRIG_CTRL_SRC_PXITRIG(3) },
				{ &pxi_trig_rt_desc[4], TRIG_CTRL_SRC_PXITRIG(4) },
				{ &pxi_trig_rt_desc[5], TRIG_CTRL_SRC_PXITRIG(5) },
				{ &pxi_trig_rt_desc[6], TRIG_CTRL_SRC_PXITRIG(6) },
				{ NULL, 0 }
			},
	},

};

static const struct route_terminal_desc pxi_star_rt_desc[];

#define PFI_RT_DESC_MEMBERS(n) \
	.name		= "PFI" #n,					\
	.dest_data	= TRIG_CTRL_DEST_PFI_SE(n),			\
	.line_state_bit	= TRIG_READ_PFI_LINE_STATE_BIT(n),		\
	.set_input	= &triggerctrl_set_input

#define PXI_STAR_INPUT(n) { &pxi_star_rt_desc[n], TRIG_CTRL_SRC_PXISTAR(n) }
#define ALL_PXI_STAR_INPUTS	\
	PXI_STAR_INPUT(0),	\
	PXI_STAR_INPUT(1),	\
	PXI_STAR_INPUT(2),	\
	PXI_STAR_INPUT(3),	\
	PXI_STAR_INPUT(4),	\
	PXI_STAR_INPUT(5),	\
	PXI_STAR_INPUT(6),	\
	PXI_STAR_INPUT(7),	\
	PXI_STAR_INPUT(8),	\
	PXI_STAR_INPUT(9),	\
	PXI_STAR_INPUT(10),	\
	PXI_STAR_INPUT(11),	\
	PXI_STAR_INPUT(12),	\
	PXI_STAR_INPUT(13),	\
	PXI_STAR_INPUT(14),	\
	PXI_STAR_INPUT(15),	\
	PXI_STAR_INPUT(16)

#define PXI_TRIG_INPUT(n) { &pxi_trig_rt_desc[n], TRIG_CTRL_SRC_PXITRIG(n) }
#define ALL_PXI_TRIG_INPUTS	\
	PXI_TRIG_INPUT(0),	\
	PXI_TRIG_INPUT(1),	\
	PXI_TRIG_INPUT(2),	\
	PXI_TRIG_INPUT(3),	\
	PXI_TRIG_INPUT(4),	\
	PXI_TRIG_INPUT(5),	\
	PXI_TRIG_INPUT(6),	\
	PXI_TRIG_INPUT(7)

static const struct route_terminal_desc pfi_rt_desc[] = {
	{
		PFI_RT_DESC_MEMBERS(0),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				PFI_SE_INPUT(1),
				PFI_SE_INPUT(2),
				PFI_SE_INPUT(3),
				PFI_SE_INPUT(4),
				PFI_SE_INPUT(5),
				ALL_PXI_TRIG_INPUTS,
				ALL_PXI_STAR_INPUTS,
				{ NULL, 0 }
			},
	},
	{
		PFI_RT_DESC_MEMBERS(1),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				PFI_SE_INPUT(0),
				PFI_SE_INPUT(2),
				PFI_SE_INPUT(3),
				PFI_SE_INPUT(4),
				PFI_SE_INPUT(5),
				ALL_PXI_TRIG_INPUTS,
				ALL_PXI_STAR_INPUTS,
				{ NULL, 0 }
			},
	},
	{
		PFI_RT_DESC_MEMBERS(2),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				PFI_SE_INPUT(0),
				PFI_SE_INPUT(1),
				PFI_SE_INPUT(3),
				PFI_SE_INPUT(4),
				PFI_SE_INPUT(5),
				ALL_PXI_TRIG_INPUTS,
				ALL_PXI_STAR_INPUTS,
				{ NULL, 0 }
			},
	},
	{
		PFI_RT_DESC_MEMBERS(3),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				PFI_SE_INPUT(0),
				PFI_SE_INPUT(1),
				PFI_SE_INPUT(2),
				PFI_SE_INPUT(4),
				PFI_SE_INPUT(5),
				ALL_PXI_TRIG_INPUTS,
				ALL_PXI_STAR_INPUTS,
				{ NULL, 0 }
			},
	},
	{
		PFI_RT_DESC_MEMBERS(4),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				PFI_SE_INPUT(0),
				PFI_SE_INPUT(1),
				PFI_SE_INPUT(2),
				PFI_SE_INPUT(3),
				PFI_SE_INPUT(5),
				ALL_PXI_TRIG_INPUTS,
				ALL_PXI_STAR_INPUTS,
				{ NULL, 0 }
			},
	},
	{
		PFI_RT_DESC_MEMBERS(5),
		.available_inputs = (const struct route_terminal_input[])
			{
				{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },
				{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },
				{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },
				PFI_SE_INPUT(0),
				PFI_SE_INPUT(1),
				PFI_SE_INPUT(2),
				PFI_SE_INPUT(3),
				PFI_SE_INPUT(4),
				ALL_PXI_TRIG_INPUTS,
				ALL_PXI_STAR_INPUTS,
				{ NULL, 0 }
			},
	},
};

#define PXI_STAR_RT_DESC_MEMBERS(n)						\
	.name			= "PXI_Star" #n,				\
	.dest_data		= TRIG_CTRL_DEST_PXISTAR(n),			\
	.line_state_bit		= TRIG_READ_PXI_STAR_LINE_STATE_BIT(n),		\
	.available_inputs	= (const struct route_terminal_input[])		\
		{								\
			{ &rt_floating,		TRIG_CTRL_SRC_FLOATING },	\
			{ &rt_logic_high,	TRIG_CTRL_SRC_LOGIC_HIGH },	\
			{ &rt_logic_low,	TRIG_CTRL_SRC_LOGIC_LOW },	\
			ALL_PFI_SE_INPUTS,					\
			{ NULL, 0 }						\
		},								\
	.set_input		= &triggerctrl_set_input

static const struct route_terminal_desc pxi_star_rt_desc[] = {
	{
		PXI_STAR_RT_DESC_MEMBERS(0)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(1)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(2)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(3)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(4)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(5)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(6)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(7)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(8)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(9)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(10)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(11)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(12)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(13)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(14)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(15)
	},
	{
		PXI_STAR_RT_DESC_MEMBERS(16)
	},
};

static const struct route_terminal_desc clkin_rt_desc = {
	.name		= "ClkIn"
};

static void enable_clkin(struct ni6674t *dev)
{
	iowrite32(CLKIN_CTRL_ENABLE(1), &dev->sync->clkinctrl);
}

static void src_a_b_set_input(struct route_terminal *rt,
			      const struct route_terminal_input *input)
{
	struct ni6674t *dev = rt->owner;
	u32 regval;

	mutex_lock(&dev->devlock);
	regval = ioread32(&dev->sync->dstaractrl1);

	/* dest_data contains the field mask in this case */
	regval &= ~rt->rt_desc->dest_data;
	regval |= input->data;

	iowrite32(regval, &dev->sync->dstaractrl1);
	mutex_unlock(&dev->devlock);
}

static const struct route_terminal_desc srca_rt_desc = {
	.name			= "SourceA",
	.set_input		= &src_a_b_set_input,
	.dest_data		= DSTARA_SRCA_MUX2_MASK,
	.available_inputs	= (const struct route_terminal_input[])
		{
			{ &clkin_rt_desc, DSTARA_SRCA_MUX2(DSTARA_SRC_CLKIN) },
			{ NULL, 0 }
		}
};

static const struct route_terminal_desc srcb_rt_desc = {
	.name			= "SourceB",
	.set_input	        = &src_a_b_set_input,
	.dest_data		= DSTARA_SRCB_MUX2_MASK,
	.available_inputs	= (const struct route_terminal_input[])
		{
			{ &clkin_rt_desc, DSTARA_SRCB_MUX2(DSTARA_SRC_CLKIN) },
			{ NULL, 0 }
		}
};

static void src_a_b_div_sel_set_input(struct route_terminal *rt,
				      const struct route_terminal_input *input)
{
	struct ni6674t *dev = rt->owner;
	u32 regval;

	mutex_lock(&dev->devlock);
	regval = ioread32(&dev->sync->dstaractrl2);

	/* dest_data contains the field mask in this case */
	regval &= ~rt->rt_desc->dest_data;
	regval |= input->data;

	iowrite32(regval, &dev->sync->dstaractrl2);
	mutex_unlock(&dev->devlock);
}

static const struct route_terminal_desc srca_div_sel_rt_desc = {
	.name			= "SourceADividerSelect",
	.set_input		= &src_a_b_div_sel_set_input,
	.dest_data		= DSTARA_SRCA_USE_DIVIDER(1),
	.available_inputs	= (const struct route_terminal_input[])
		{
			{ &srca_rt_desc, DSTARA_SRCA_USE_DIVIDER(0) },
			{ NULL, 0 }
		}
};

static const struct route_terminal_desc srcb_div_sel_rt_desc = {
	.name			= "SourceBDividerSelect",
	.set_input		= &src_a_b_div_sel_set_input,
	.dest_data		= DSTARA_SRCB_USE_DIVIDER(1),
	.available_inputs	= (const struct route_terminal_input[])
		{
			{ &srcb_rt_desc, DSTARA_SRCB_USE_DIVIDER(0) },
			{ NULL, 0 }
		}
};

static const struct route_terminal_desc bank_rt_desc[];

static void bank_set_input(struct route_terminal *rt,
			   const struct route_terminal_input *input)
{
	struct ni6674t *dev = rt->owner;
	u32 regval;

	mutex_lock(&dev->devlock);
	regval = ioread32(&dev->sync->dstaractrl1);

	/* dest_data contains the field mask in this case */
	regval &= ~rt->rt_desc->dest_data;
	regval |= input->data;

	iowrite32(regval, &dev->sync->dstaractrl1);
	mutex_unlock(&dev->devlock);
}

#define BANK_RT_DESC_MEMBERS(n)										\
	.name			= "Bank" #n,								\
	.set_input		= &bank_set_input,							\
	.dest_data		= DSTARA_BANK_N_MASK(n),						\
	.available_inputs	= (const struct route_terminal_input[])					\
		{											\
			{ &rt_floating,			DSTARA_BANK_N(n, DSTARA_SRC_FLOATING) },	\
			{ &srca_div_sel_rt_desc,	DSTARA_BANK_N(n, DSTARA_SRC_SRCA) },		\
			{ &srcb_div_sel_rt_desc,	DSTARA_BANK_N(n, DSTARA_SRC_SRCB) },		\
			{ NULL, 0 }									\
		}

static const struct route_terminal_desc bank_rt_desc[] = {
	{
		BANK_RT_DESC_MEMBERS(0)
	},
	{
		BANK_RT_DESC_MEMBERS(1)
	},
	{
		BANK_RT_DESC_MEMBERS(2)
	},
	{
		BANK_RT_DESC_MEMBERS(3)
	}
};

#define PXIE_DSTARA_RT_DESC_MEMBERS(n, banknum)				\
	.name			= "PXIe_DStarA" #n,			\
	.available_inputs	= (const struct route_terminal_input[])	\
		{							\
			{ &bank_rt_desc[banknum], 0 },			\
			{ NULL, 0 }					\
		}

static const struct route_terminal_desc dstara_rt_desc[] = {
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(0, 0)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(1, 0)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(2, 0)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(3, 0)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(4, 1)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(5, 1)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(6, 1)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(7, 1)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(8, 2)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(9, 2)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(10, 2)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(11, 2)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(12, 3)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(13, 3)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(14, 3)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(15, 3)
	},
	{
		PXIE_DSTARA_RT_DESC_MEMBERS(16, 3)
	}
};

static ssize_t route_terminal_show(struct kobject *kobj, struct attribute *attr,
				   char *buf)
{
	struct route_terminal_attr *rt_attr;
	struct route_terminal *rt;

	rt_attr = container_of(attr, struct route_terminal_attr, attr);
	rt = container_of(kobj, struct route_terminal, kobj);

	return rt_attr->show ? rt_attr->show(rt, buf) : -EINVAL;
}

static ssize_t route_terminal_store(struct kobject *kobj,
				    struct attribute *attr,
				    const char *buf, size_t count)
{
	struct route_terminal_attr *rt_attr;
	struct route_terminal *rt;

	rt_attr = container_of(attr, struct route_terminal_attr, attr);
	rt = container_of(kobj, struct route_terminal, kobj);

	return rt_attr->store ? rt_attr->store(rt, buf, count) : -EINVAL;
}

static struct sysfs_ops route_terminal_sysfs_ops = {
	.show	= route_terminal_show,
	.store	= route_terminal_store,
};

static struct attribute *basic_route_terminal_default_attrs[] = {
	&route_terminal_attr_current_input.attr,
	&route_terminal_attr_available_inputs.attr,
	NULL,
};

static struct attribute *route_terminal_default_attrs[] = {
	&route_terminal_attr_current_input.attr,
	&route_terminal_attr_polarity.attr,
	&route_terminal_attr_available_inputs.attr,
	&route_terminal_attr_line_state.attr,
	NULL,
};

static struct attribute *pxi_trig_route_terminal_default_attrs[] = {
	/* the first few items are the same as route_terminal_default_attrs */
	&route_terminal_attr_current_input.attr,
	&route_terminal_attr_polarity.attr,
	&route_terminal_attr_available_inputs.attr,
	&route_terminal_attr_line_state.attr,
	NULL,
};


static void route_terminal_release(struct kobject *kobj)
{
	struct route_terminal *rt = container_of(kobj, struct route_terminal, kobj);
	kfree(rt);
}

static void pxi_trig_route_terminal_release(struct kobject *kobj)
{
	struct pxi_trig_route_terminal *prt = container_of(kobj, struct pxi_trig_route_terminal, rt.kobj);
	kfree(prt);
}

static struct kobj_type basic_route_terminal_ktype = {
	.release	= route_terminal_release,
	.sysfs_ops	= &route_terminal_sysfs_ops,
	.default_attrs	= basic_route_terminal_default_attrs,
};

static struct kobj_type route_terminal_ktype = {
	.release	= route_terminal_release,
	.sysfs_ops	= &route_terminal_sysfs_ops,
	.default_attrs	= route_terminal_default_attrs,
};

static struct kobj_type pxi_trig_route_terminal_ktype = {
	.release	= pxi_trig_route_terminal_release,
	.sysfs_ops	= &route_terminal_sysfs_ops,
	.default_attrs	= pxi_trig_route_terminal_default_attrs,
};

static void release_route_terminal(struct route_terminal *rt)
{
	kobject_put(&rt->kobj);
}

static int init_and_add_route_terminal(struct ni6674t *dev,
				       struct route_terminal *rt,
				       struct kobj_type *ktype,
				       const struct route_terminal_desc *desc)
{
	int err;

	rt->owner = dev;
	rt->rt_desc = desc;
	rt->kobj.kset = dev->terminal_set;

	err = kobject_init_and_add(&rt->kobj, ktype, NULL, desc->name);
	if (!err) {
		/* Put the terminal into a known route state */
		set_input_and_update_state(rt, &desc->available_inputs[0]);
	}
	return err;
}

static int __devinit init_pxi_trig_terminals(struct ni6674t *dev)
{
	int i, err;
	for (i = 0; i < ARRAY_SIZE(dev->pxi_trig); ++i) {
		dev->pxi_trig[i] = kzalloc(sizeof(*dev->pxi_trig[i]),
					   GFP_KERNEL);
		if (!dev->pxi_trig[i]) {
			err = -ENOMEM;
			goto fail_allocation;
		}

		err = init_and_add_route_terminal(dev, &dev->pxi_trig[i]->rt,
						  &pxi_trig_route_terminal_ktype,
						  &pxi_trig_rt_desc[i]);
		if (err)
			goto fail_registration;
	}
	return 0;

fail_registration:
	kfree(dev->pxi_trig[i]);
fail_allocation:
	while (--i >= 0)
		release_route_terminal(&dev->pxi_trig[i]->rt);
	return err;
}

static void release_pxi_trig_terminals(struct ni6674t *dev)
{
	int i;
	for (i = ARRAY_SIZE(dev->pxi_trig) - 1; i >= 0; --i)
		release_route_terminal(&dev->pxi_trig[i]->rt);
}

int init_route_terminals(struct ni6674t *dev,
			 struct route_terminal *rt[],
			 const size_t rt_size,
			 const struct route_terminal_desc rtt[])
{
	int i, err;
	for (i = 0; i < rt_size; ++i) {
		rt[i] = kzalloc(sizeof(*rt[i]), GFP_KERNEL);
		if (!rt[i]) {
			err = -ENOMEM;
			goto fail_allocation;
		}

		err = init_and_add_route_terminal(dev, rt[i],
						  &route_terminal_ktype,
						  &rtt[i]);
		if (err)
			goto fail_registration;
	}
	return 0;

fail_registration:
	kfree(rt[i]);
fail_allocation:
	while (--i >= 0)
		release_route_terminal(rt[i]);
	return err;
}

static int __devinit init_pfi_terminals(struct ni6674t *dev)
{
	return init_route_terminals(dev, dev->pfi, ARRAY_SIZE(dev->pfi),
				    pfi_rt_desc);
}

static void release_pfi_terminals(struct ni6674t *dev)
{
	int i;
	for (i = ARRAY_SIZE(dev->pfi) - 1; i >= 0; --i)
		release_route_terminal(dev->pfi[i]);
}

static int __devinit init_pxi_star_terminals(struct ni6674t *dev)
{
	return init_route_terminals(dev, dev->pxi_star, ARRAY_SIZE(dev->pxi_star),
				    pxi_star_rt_desc);
}

static void release_pxi_star_terminals(struct ni6674t *dev)
{
	int i;
	for (i = ARRAY_SIZE(dev->pxi_star) - 1; i >= 0; --i)
		release_route_terminal(dev->pxi_star[i]);
}

static int __devinit init_basic_terminal(struct ni6674t *dev, struct route_terminal** rt,
					 const struct route_terminal_desc* desc)
{
	int err;

	*rt = kzalloc(sizeof(**rt), GFP_KERNEL);
	if (!*rt) return -ENOMEM;

	err = init_and_add_route_terminal(dev, *rt, &basic_route_terminal_ktype, desc);
	if (err) kfree(*rt);

	return err;
}

static int __devinit init_other_terminals(struct ni6674t *dev)
{
	int i, err;

	err = init_basic_terminal(dev, &dev->srca, &srca_rt_desc);
	if (err)
		goto fail_srca;

	err = init_basic_terminal(dev, &dev->srcb, &srcb_rt_desc);
	if (err)
		goto fail_srcb;

	err = init_basic_terminal(dev, &dev->srca_div_sel, &srca_div_sel_rt_desc);
	if (err)
		goto fail_srca_div_sel;

	err = init_basic_terminal(dev, &dev->srcb_div_sel, &srcb_div_sel_rt_desc);
	if (err)
		goto fail_srcb_div_sel;

	for (i = 0; i < ARRAY_SIZE(dev->bank); ++i) {
		err = init_basic_terminal(dev, &dev->bank[i], &bank_rt_desc[i]);
		if (err)
			goto fail_bank;
	}

	for (i = 0; i < ARRAY_SIZE(dev->pxie_dstara); ++i) {
		err = init_basic_terminal(dev, &dev->pxie_dstara[i], &dstara_rt_desc[i]);
		if (err)
			goto fail_dstara;
	}

	/* FIXME: We're just enabling ClkIn by default here.  In the future,
	 * we'll probably want to lazily enable ClkIn the first time it's used.
	 * We'll probably also want to ref count it to make sure it's not
	 * prematurely disabled. */
	enable_clkin(dev);

	return 0;

fail_dstara:
	while (--i >= 0)
		release_route_terminal(dev->pxie_dstara[i]);
	i = ARRAY_SIZE(dev->bank);
fail_bank:
	while (--i >= 0)
		release_route_terminal(dev->bank[i]);
	release_route_terminal(dev->srcb_div_sel);
fail_srcb_div_sel:
	release_route_terminal(dev->srca_div_sel);
fail_srca_div_sel:
	release_route_terminal(dev->srcb);
fail_srcb:
	release_route_terminal(dev->srca);
fail_srca:
	return err;
}

static void release_other_terminals(struct ni6674t *dev)
{
	int i;

	for (i = ARRAY_SIZE(dev->pxie_dstara) - 1; i >= 0; --i)
		release_route_terminal(dev->pxie_dstara[i]);

	for (i = ARRAY_SIZE(dev->bank) - 1; i >= 0; --i)
		release_route_terminal(dev->bank[i]);

	release_route_terminal(dev->srcb_div_sel);
	release_route_terminal(dev->srca_div_sel);
	release_route_terminal(dev->srcb);
	release_route_terminal(dev->srca);
}

static int __devinit ni6674t_dac_write(struct ni6674t *dev,
				       struct pci_dev *pdev,
				       u32 val)
{
	u32 timeout = 100;
	while ((ioread32(&dev->sync->dacctrl) & DAC_CTRL_SERIAL_PORT_BUSY) && --timeout)
		udelay(10);
	if (!timeout) {
		dev_err(&pdev->dev, "DAC serial timeout.\n");
		return -EIO;
	}

	iowrite32(val, &dev->sync->dacctrl);
	return 0;
}

static int __devinit ni6674t_init_dac(struct ni6674t *dev,
				      struct pci_dev *pdev)
{
	int pfinum, err;
	/* FIXME: This code is mostly borrowed from the Windows driver
	 * (w/ accompanying documentation).  Remember to document the meaning of
    * these magic numbers. */

	/* Set DAC_ChipSelect to PFI Threshold DAC */
	/* Sets gain of output amplifier and reference selection options */
	err = ni6674t_dac_write(dev, pdev, 0x800c);
	if (err) return err;

	/* LDAC options */
	err = ni6674t_dac_write(dev, pdev, 0xa000);
	if (err) return err;

	/* Power-down options */
	err = ni6674t_dac_write(dev, pdev, 0xc000);
	if (err) return err;

	/* DAC Register Write (for each pfi line) */
	for (pfinum=0; pfinum < 6; ++pfinum) {
		err = ni6674t_dac_write(dev, pdev, (pfinum << 12) | (60 << 4));
		if (err) return err;
	}
	return 0;
}

static int __devinit ni6674t_init_sysfs(struct ni6674t *dev,
					struct pci_dev *pdev)
{
	int err = 0;

	dev->terminal_set = kset_create_and_add("terminals", NULL,
						&pdev->dev.kobj);
	if (!dev->terminal_set) {
		err = -ENOMEM;
		goto fail_alloc_term_kset;
	}

	err = init_pxi_trig_terminals(dev);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize PXI Trig terminals.\n");
		goto fail_pxi_trig_init;
	}

	err = init_pfi_terminals(dev);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize PFI terminals.\n");
		goto fail_pfi_init;
	}

	err = init_pxi_star_terminals(dev);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize PXI Star terminals.\n");
		goto fail_pxi_star_init;
	}

	err = init_other_terminals(dev);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize other terminals.\n");
		goto fail_other_init;
	}

	return 0;

fail_other_init:
	release_pxi_star_terminals(dev);
fail_pxi_star_init:
	/* PXI Star initialization failed and cleaned up after itself */
	/* Need to clean up PFI terminals */
	release_pfi_terminals(dev);
fail_pfi_init:
	/* PFI initialization failed and cleaned up after itself */
	release_pxi_trig_terminals(dev);
fail_pxi_trig_init:
	kset_put(dev->terminal_set);
fail_alloc_term_kset:
	return err;
}

static int __devinit ni6674t_load_fpga(struct ni6674t *dev,
				       struct pci_dev *pdev, const char *fw_str)
{
	int i, timeout, err, rem_bytes;
	u32 status, *fwdata, tmp = 0;
	const struct firmware *fw;
	struct ce *ce;

	err = request_firmware(&fw, fw_str, &pdev->dev);
	if (err) {
		dev_err(&pdev->dev,
			"Unable to find firmware \"%s\".\n", fw_str);
		return err;
	}

	/* CE registers only exist to bootstrap firmware */
	ce = ioremap(pci_resource_start(pdev, 1) + CE_REGBLOCK_OFFSET,
		     sizeof(*ce));
	if (!ce) {
		dev_err(&pdev->dev, "Failed to map CE register.\n");
		err = -EIO;
		goto fail_ce_map;
	}

	iowrite32((u32)pci_resource_start(pdev, 1) | MITE_IODWBSR_WENAB,
		  &dev->mite->iodwbsr);

	status = ioread32(&ce->status);

	if ((status & (CE_STATUS_IN_RESET | CE_STATUS_IN_WAIT_START))
		!= CE_STATUS_IN_WAIT_START) {
		dev_err(&pdev->dev, "Device in invalid state.\n");
		err = -EIO;
		goto fail_ce_state;
	}

	iowrite32(CE_COMMAND_RESET_FIFO, &ce->command);
	mmiowb();

	iowrite32(0, &ce->flash_info);
	iowrite32(CE_PROG_PULSE_START_READY_IMMEDIATE |
		  CE_PROG_PULSE_START_DRIVE_UNASSERT  |
		  CE_PROG_PULSE_START_LEN(0x13), &ce->prog_pulse_config);
	iowrite32(CE_DATA_DATA_CLKS(1)  |
		  CE_DATA_ORDER_MSB2LSB |
		  CE_DATA_ISPARALLEL, &ce->data_config);
	iowrite32(CE_START_CLKRDY_DELAY(1), &ce->start_config);
	iowrite32(CE_STOP_POSTCLKS(0x64) |
		  CE_STOP_DONEHIGHTRUE   |
		  CE_STOP_NOERRHIGHTRUE  |
		  CE_STOP_DONERDY_IMMEDIATE, &ce->stop_config);
	iowrite32(0, &ce->flash_addr);
	mmiowb();

	iowrite32(CE_COMMAND_START_FPGA, &ce->command);

	timeout = 100;
	while (!(ioread32(&ce->status) & CE_STATUS_IN_GEN_DATA) && --timeout)
		mdelay(10);

	if (!timeout) {
		dev_err(&pdev->dev, "FPGA config engine timeout.\n");
		err = -EIO;
		goto fail_ce_fpga_start;
	}

	fwdata = (u32*) fw->data;
	for (i = 0; i < fw->size / 4; i++) {
		iowrite32(cpu_to_be32(*fwdata++), &ce->fifo);
		tmp = ioread32(&ce->status);
		if (tmp & CE_STATUS_STOP_DOWNLOAD)
			break;
	}

	/* zero pad last word */
	if (!(tmp & CE_STATUS_CONFIG_DONE) && (rem_bytes = fw->size & 3)) {
		u32 mask = (1 << rem_bytes * 8) - 1;
		iowrite32(cpu_to_be32(*fwdata & mask), &ce->fifo);
		tmp = ioread32(&ce->status);
	}

	if (!(tmp & CE_STATUS_CONFIG_DONE)) {
		/* dummy writes until signaled or timeout.  number of cycles
		 * has to be at least 100, but we want to give it plenty of
		 * slop. 1100 should be enough. */
		timeout = 1100;
		while (--timeout) {
			iowrite32(0xFFFFFFFF, &ce->fifo);
			tmp = ioread32(&ce->status);
			if (tmp & CE_STATUS_STOP_DOWNLOAD)
				break;
		}
	}

	if (!timeout || (tmp & CE_STATUS_CONFIG_ERROR)) {
		dev_err(&pdev->dev, "FPGA image download failed.\n");
		err = -EIO;
		goto fail_fpga_download;
	}

	iounmap(ce);
	release_firmware(fw);

	tmp = ioread32(&dev->mite->iodwbsr) & ~MITE_IODWBSR_WENAB;
	iowrite32(tmp, &dev->mite->iodwbsr);

	tmp = pci_resource_start(pdev, 1);
	tmp |= MITE_IOWBSR1_WENAB | MITE_IOWBSR1_WSIZE4;
	iowrite32(tmp, &dev->mite->iowbsr1);

	return 0;

fail_fpga_download:
fail_ce_fpga_start:
fail_ce_state:
	iounmap(ce);
fail_ce_map:
	release_firmware(fw);
	return err;
}

static int __devinit ni6674t_probe(struct pci_dev *pdev,
				   const struct pci_device_id *id)
{
	const char *fw_str = (const char *) id->driver_data;
	struct ni6674t *dev;
	int err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(&pdev->dev, "Unable to allocate ni6674t object.\n");
		return -ENOMEM;
	}

	pci_set_drvdata(pdev, dev);

	err = pci_request_regions(pdev, "ni6674t");
	if (err) {
		dev_err(&pdev->dev, "Requesting device regions failed.\n");
		goto fail_request_regions;
	}

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Unable to enable device.\n");
		goto fail_enable;
	}

	dev->mite = ioremap(pci_resource_start(pdev, 0),
			    pci_resource_len(pdev, 0));
	if (!dev->mite) {
		dev_err(&pdev->dev, "Could not map BAR0 (MITE space).\n");
		err = -EIO;
		goto fail_mite_map;
	}

	err = ni6674t_load_fpga(dev, pdev, fw_str);
	if (err) {
		dev_err(&pdev->dev, "Could not load FPGA image.\n");
		goto fail_load_fpga;
	}

	dev->sync = ioremap(pci_resource_start(pdev, 1),
			    pci_resource_len(pdev, 1));
	if (!dev->sync) {
		dev_err(&pdev->dev, "Could not map sync registers.\n");
		goto fail_sync_map;
	}

	mutex_init(&dev->devlock);

	err = ni6674t_init_dac(dev, pdev);
	if (err) {
		dev_err(&pdev->dev, "Could not init DAC.\n");
		goto fail_init_dac;
	}

	err = ni6674t_init_sysfs(dev, pdev);
	if (err) {
		dev_err(&pdev->dev, "Could not create sysfs entries.\n");
		goto fail_init_sysfs;
	}

	return 0;

fail_init_sysfs:
fail_init_dac:
	iounmap(dev->sync);
fail_sync_map:
fail_load_fpga:
	iounmap(dev->mite);
fail_mite_map:
	pci_disable_device(pdev);
fail_enable:
	pci_release_regions(pdev);
fail_request_regions:
	pci_set_drvdata(pdev, NULL);
	kfree(dev);
	return err;
}

static void __devexit ni6674t_remove(struct pci_dev *pdev)
{
	struct ni6674t *dev = pci_get_drvdata(pdev);

	release_other_terminals(dev);
	release_pxi_star_terminals(dev);
	release_pfi_terminals(dev);
	release_pxi_trig_terminals(dev);

	kset_put(dev->terminal_set);
	iounmap(dev->sync);
	iounmap(dev->mite);
	pci_disable_device(pdev);
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(dev);
}

static struct pci_device_id ni6674t_pciids[] __devinitconst = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_NI, 0x7405),
		.driver_data	= (kernel_ulong_t) "ni_pxie6674t.bin",
	},
	{ },
};

static struct pci_driver ni6674t_pci_driver = {
	.name		= "ni6674t",
	.id_table	= ni6674t_pciids,
	.probe		= ni6674t_probe,
	.remove		= ni6674t_remove,
};

static int __init ni6674t_init(void)
{
	pr_devel("driver loaded.\n");
	return pci_register_driver(&ni6674t_pci_driver);
}

static void __exit ni6674t_exit(void)
{
	pci_unregister_driver(&ni6674t_pci_driver);
}

module_init(ni6674t_init);
module_exit(ni6674t_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Josh Cartwright <josh.cartwright@ni.com>");
