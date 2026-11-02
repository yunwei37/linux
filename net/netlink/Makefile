# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the netlink driver.
#

obj-y				:= af_netlink.o

ifndef CONFIG_MODELOS_KERNEL_ONLY
obj-y				+= genetlink.o policy.o
endif

obj-$(CONFIG_NETLINK_DIAG)	+= netlink_diag.o
netlink_diag-y			:= diag.o
