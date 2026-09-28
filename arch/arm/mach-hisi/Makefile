# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Hisilicon processors family
#

CFLAGS_platmcpm.o	:= -march=armv7-a

obj-y	+= hisilicon.o
obj-$(CONFIG_MCPM)		+= platmcpm.o
obj-$(CONFIG_SMP)		+= platsmp.o hotplug.o

obj-$(CONFIG_ARCH_HI3520DV400) += hi3520dv400.o
