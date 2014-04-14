/*
 * ni6674t.h: Driver for NI-PXIe 6674T signal-based routing board
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
#ifndef _NI6674T_H_
#define _NI6674T_H_

struct route_terminal;
struct route_terminal_input;

/**
 * struct route_terminal_desc - Structure used to describe routing hierarchy.
 *
 * @name:			The name of the terminal.
 * @available_inputs		Terminals that can act as an input to this one.
 *				This list is terminated with a NULL value.  The
 *				first value in this array is the default input
 *				for this terminal.
 * @set_input:			Function to set the current input of a terminal
 * @dest_data			Data that can be used when programming this
 *				terminal's source
 * @line_state_bit		Offset of this terminal's line state within the
 *				3 trigread registers
 */
struct route_terminal_desc {
	const char *name;
	const struct route_terminal_input *available_inputs;
	void (*set_input)(struct route_terminal *, const struct route_terminal_input *);
	unsigned int dest_data;
	unsigned int line_state_bit;
};

/**
 * struct route_terminal_input - Structure used to describe an input to a terminal
 *
 * @desc:			The route_terminal_desc that describes this input
 * @data:			Arbitrary data related to this input.  This can
 *				be used to store the register/field value used to
 *				program this input.
 */
struct route_terminal_input {
	const struct route_terminal_desc *desc;
	const unsigned long data;
};

enum terminal_polarity {
	POLARITY_NORMAL,
	POLARITY_INVERTED,
};

struct ni6674t;

/**
 * struct route_terminal - Run-time data about route terminal.
 *
 * @kobj:	Embedded struct kobject.
 * @rt_desc:	Pointer to the descriptor of this terminal.
 * @input:	Pointer to terminal currently driving this one.
 * @owner:	Pointer to device object which owns this terminal.
 * @polarity:	Whether or not the terminal is inverting the polarity of the signal.
 */
struct route_terminal {
	struct kobject kobj;
	const struct route_terminal_desc *rt_desc;
	const struct route_terminal_input *input;
	struct ni6674t *owner;
	enum terminal_polarity polarity;
};

/**
 * struct pxi_trig_route_terminal
 *
 * @rt:	Embedded struct route_terminal object.
 */
struct pxi_trig_route_terminal {
	struct route_terminal rt;
};
#define to_pxi_trig_rt(rt)					\
	container_of(rt, struct pxi_trig_route_terminal, rt)

/**
 * struct route_terminal_attr
 *
 * @attr:	Embedded struct attribute object.
 * @show:	Called when usermode reads this attribute.
 * @store:	Called when usermode writes to this attribute.
 */
struct route_terminal_attr {
	struct attribute attr;
	ssize_t (*show)(struct route_terminal *, char *);
	ssize_t (*store)(struct route_terminal *, const char *, size_t);
};

#define __ROUTE_TERMINAL_ATTR(_name, _mode, _show, _store)	\
struct route_terminal_attr route_terminal_attr_##_name = {	\
	.attr	= {						\
		.name = #_name,					\
		.mode = _mode,					\
	},							\
	.show	= _show,					\
	.store	= _store,					\
}

#define ROUTE_TERMINAL_ATTR(_name, _mode)			\
	__ROUTE_TERMINAL_ATTR(_name, _mode,			\
			      route_terminal_##_name##_show,	\
			      route_terminal_##_name##_store)

#define ROUTE_TERMINAL_ATTR_RO(_name, _mode)				\
	__ROUTE_TERMINAL_ATTR(_name, _mode,				\
			      route_terminal_##_name##_show, NULL)	\

#endif
