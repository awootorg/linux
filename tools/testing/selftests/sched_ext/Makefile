# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
include ../../../build/Build.include
include ../../../scripts/Makefile.arch
include ../../../scripts/Makefile.include

TEST_GEN_PROGS := runner

# override lib.mk's default rules
OVERRIDE_TARGETS := 1
include ../lib.mk

CURDIR := $(abspath .)
REPOROOT := $(abspath ../../../..)
TOOLSDIR := $(REPOROOT)/tools
LIBDIR := $(TOOLSDIR)/lib
BPFDIR := $(LIBDIR)/bpf
TOOLSINCDIR := $(TOOLSDIR)/include
BPFTOOLDIR := $(TOOLSDIR)/bpf/bpftool
APIDIR := $(TOOLSINCDIR)/uapi
GENDIR := $(REPOROOT)/include/generated
GENHDR := $(GENDIR)/autoconf.h
SCXTOOLSDIR := $(TOOLSDIR)/sched_ext
SCXTOOLSINCDIR := $(TOOLSDIR)/sched_ext/include

OUTPUT_DIR := $(OUTPUT)/build
OBJ_DIR := $(OUTPUT_DIR)/obj
INCLUDE_DIR := $(OUTPUT_DIR)/include
BPFOBJ_DIR := $(OBJ_DIR)/libbpf
SCXOBJ_DIR := $(OBJ_DIR)/sched_ext
BPFOBJ := $(BPFOBJ_DIR)/libbpf.a
LIBBPF_OUTPUT := $(OBJ_DIR)/libbpf/libbpf.a

DEFAULT_BPFTOOL := $(OUTPUT_DIR)/host/sbin/bpftool
HOST_OBJ_DIR := $(OBJ_DIR)/host/bpftool
HOST_LIBBPF_OUTPUT := $(OBJ_DIR)/host/libbpf/
HOST_LIBBPF_DESTDIR := $(OUTPUT_DIR)/host/
HOST_DESTDIR := $(OUTPUT_DIR)/host/

VMLINUX_BTF_PATHS ?= $(if $(O),$(O)/vmlinux)					\
		     $(if $(KBUILD_OUTPUT),$(KBUILD_OUTPUT)/vmlinux)		\
		     ../../../../vmlinux					\
		     /sys/kernel/btf/vmlinux					\
		     /boot/vmlinux-$(shell uname -r)
VMLINUX_BTF ?= $(abspath $(firstword $(wildcard $(VMLINUX_BTF_PATHS))))
ifeq ($(VMLINUX_BTF),)
$(error Cannot find a vmlinux for VMLINUX_BTF at any of "$(VMLINUX_BTF_PATHS)")
endif

BPFTOOL ?= $(DEFAULT_BPFTOOL)

ifneq ($(wildcard $(GENHDR)),)
  GENFLAGS := -DHAVE_GENHDR
endif

CFLAGS += -g -O2 -rdynamic -pthread -Wall -Werror $(GENFLAGS)			\
	  -I$(INCLUDE_DIR) -I$(GENDIR) -I$(LIBDIR)				\
	  -I$(TOOLSINCDIR) -I$(APIDIR) -I$(CURDIR)/include -I$(SCXTOOLSINCDIR)

# Silence some warnings when compiled with clang
ifneq ($(LLVM),)
CFLAGS += -Wno-unused-command-line-argument
endif

LDFLAGS = -lelf -lz -lpthread -lzstd

IS_LITTLE_ENDIAN = $(shell $(CC) -dM -E - </dev/null |				\
			grep 'define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__')

# Get Clang's default includes on this system, as opposed to those seen by
# '-target bpf'. This fixes "missing" files on some architectures/distros,
# such as asm/byteorder.h, asm/socket.h, asm/sockios.h, sys/cdefs.h etc.
#
# Use '-idirafter': Don't interfere with include mechanics except where the
# build would have failed anyways.
define get_sys_includes
$(shell $(1) $(2) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }') \
$(shell $(1) $(2) -dM -E - </dev/null | grep '__riscv_xlen ' | awk '{printf("-D__riscv_xlen=%d -D__BITS_PER_LONG=%d", $$3, $$3)}')
endef

ifneq ($(CROSS_COMPILE),)
CLANG_TARGET_ARCH = --target=$(notdir $(CROSS_COMPILE:%-=%))
endif

CLANG_SYS_INCLUDES = $(call get_sys_includes,$(CLANG),$(CLANG_TARGET_ARCH))

BPF_CFLAGS = -g -D__TARGET_ARCH_$(SRCARCH)					\
	     $(if $(IS_LITTLE_ENDIAN),-mlittle-endian,-mbig-endian)		\
	     -I$(CURDIR)/include -I$(CURDIR)/include/bpf-compat			\
	     -I$(INCLUDE_DIR) -I$(APIDIR) -I$(SCXTOOLSINCDIR)			\
	     -I$(REPOROOT)/include						\
	     $(CLANG_SYS_INCLUDES) 						\
	     -Wall -Wno-compare-distinct-pointer-types				\
	     -Wno-incompatible-function-pointer-types				\
	     -O2 -mcpu=v3

# sort removes libbpf duplicates when not cross-building
MAKE_DIRS := $(sort $(OBJ_DIR)/libbpf $(OBJ_DIR)/libbpf				\
	       $(OBJ_DIR)/bpftool $(OBJ_DIR)/resolve_btfids			\
	       $(HOST_OBJ_DIR) $(INCLUDE_DIR) $(SCXOBJ_DIR))

$(MAKE_DIRS):
	$(call msg,MKDIR,,$@)
	$(Q)mkdir -p $@

$(BPFOBJ): $(wildcard $(BPFDIR)/*.[ch] $(BPFDIR)/Makefile)			\
	   $(APIDIR)/linux/bpf.h						\
	   | $(OBJ_DIR)/libbpf
	$(Q)$(MAKE) $(submake_extras) -C $(BPFDIR) OUTPUT=$(OBJ_DIR)/libbpf/	\
		    ARCH=$(ARCH) CC="$(CC)" CROSS_COMPILE=$(CROSS_COMPILE)	\
		    EXTRA_CFLAGS='-g -O0 -fPIC'					\
		    DESTDIR=$(OUTPUT_DIR) prefix= all install_headers

$(DEFAULT_BPFTOOL): $(wildcard $(BPFTOOLDIR)/*.[ch] $(BPFTOOLDIR)/Makefile)	\
		    $(LIBBPF_OUTPUT) | $(HOST_OBJ_DIR)
	$(Q)$(MAKE) $(submake_extras)  -C $(BPFTOOLDIR)				\
		    ARCH= CROSS_COMPILE= CC=$(HOSTCC) LD=$(HOSTLD)		\
		    EXTRA_CFLAGS='-g -O0'					\
		    OUTPUT=$(HOST_OBJ_DIR)/					\
		    LIBBPF_OUTPUT=$(HOST_LIBBPF_OUTPUT)				\
		    LIBBPF_DESTDIR=$(HOST_LIBBPF_DESTDIR)			\
		    prefix= DESTDIR=$(HOST_DESTDIR) install-bin

$(INCLUDE_DIR)/vmlinux.h: $(VMLINUX_BTF) $(BPFTOOL) | $(INCLUDE_DIR)
ifeq ($(VMLINUX_H),)
	$(call msg,GEN,,$@)
	$(Q)$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@
else
	$(call msg,CP,,$@)
	$(Q)cp "$(VMLINUX_H)" $@
endif

$(SCXOBJ_DIR)/%.bpf.o: %.bpf.c $(INCLUDE_DIR)/vmlinux.h	| $(BPFOBJ) $(SCXOBJ_DIR)
	$(call msg,CLNG-BPF,,$(notdir $@))
	$(Q)$(CLANG) $(BPF_CFLAGS) -target bpf -c $< -o $@

$(INCLUDE_DIR)/%.bpf.skel.h: $(SCXOBJ_DIR)/%.bpf.o $(INCLUDE_DIR)/vmlinux.h $(BPFTOOL) | $(INCLUDE_DIR)
	$(eval sched=$(notdir $@))
	$(call msg,GEN-SKEL,,$(sched))
	$(Q)$(BPFTOOL) gen object $(<:.o=.linked1.o) $<
	$(Q)$(BPFTOOL) gen object $(<:.o=.linked2.o) $(<:.o=.linked1.o)
	$(Q)$(BPFTOOL) gen object $(<:.o=.linked3.o) $(<:.o=.linked2.o)
	$(Q)diff $(<:.o=.linked2.o) $(<:.o=.linked3.o)
	$(Q)$(BPFTOOL) gen skeleton $(<:.o=.linked3.o) name $(subst .bpf.skel.h,,$(sched)) > $@
	$(Q)$(BPFTOOL) gen subskeleton $(<:.o=.linked3.o) name $(subst .bpf.skel.h,,$(sched)) > $(@:.skel.h=.subskel.h)

################
# C schedulers #
################

override define CLEAN
	rm -rf $(OUTPUT_DIR)
	rm -f $(TEST_GEN_PROGS)
endef

# Every testcase takes all of the BPF progs are dependencies by default. This
# allows testcases to load any BPF scheduler, which is useful for testcases
# that don't need their own prog to run their test.
all_test_bpfprogs := $(foreach prog,$(wildcard *.bpf.c),$(INCLUDE_DIR)/$(patsubst %.c,%.skel.h,$(prog)))

auto-test-targets :=			\
	create_dsq			\
	enq_last_no_enq_fails		\
	ddsp_bogus_dsq_fail		\
	ddsp_vtimelocal_fail		\
	dsp_local_on			\
	enq_select_cpu			\
	exit				\
	hotplug				\
	init_enable_count		\
	maximal				\
	maybe_null			\
	minimal				\
	numa				\
	allowed_cpus			\
	prog_run			\
	reload_loop			\
	select_cpu_dfl			\
	select_cpu_dfl_nodispatch	\
	select_cpu_dispatch		\
	select_cpu_dispatch_bad_dsq	\
	select_cpu_dispatch_dbl_dsp	\
	select_cpu_vtime		\
	test_example			\

testcase-targets := $(addsuffix .o,$(addprefix $(SCXOBJ_DIR)/,$(auto-test-targets)))

$(SCXOBJ_DIR)/runner.o: runner.c | $(SCXOBJ_DIR) $(BPFOBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# Create all of the test targets object files, whose testcase objects will be
# registered into the runner in ELF constructors.
#
# Note that we must do double expansion here in order to support conditionally
# compiling BPF object files only if one is present, as the wildcard Make
# function doesn't support using implicit rules otherwise.
$(testcase-targets): $(SCXOBJ_DIR)/%.o: %.c $(SCXOBJ_DIR)/runner.o $(all_test_bpfprogs) | $(SCXOBJ_DIR)
	$(eval test=$(patsubst %.o,%.c,$(notdir $@)))
	$(CC) $(CFLAGS) -c $< -o $@

$(SCXOBJ_DIR)/util.o: util.c | $(SCXOBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT)/runner: $(SCXOBJ_DIR)/runner.o $(SCXOBJ_DIR)/util.o $(BPFOBJ) $(testcase-targets)
	@echo "$(testcase-targets)"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

.DEFAULT_GOAL := all

.DELETE_ON_ERROR:

.SECONDARY:
