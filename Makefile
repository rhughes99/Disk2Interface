#
# TARGET defined as file to be compiled without .c
# PRUN defined as PRU number (0 or 1) to compile for

TARGET0 = Disk2Pru0
TARGET1 = Disk2Pru1
PRUN0 = 0
PRUN1 = 1

# PRU_CGT environment variable points to the TI PRU compiler directory.
# PRU_SUPPORT points to pru-software-support-package.
# GEN_DIR points to where to put the generated files.

PRU_CGT := /usr/share/ti/cgt-pru
PRU_SUPPORT := /usr/lib/ti/pru-software-support-package

GEN_DIR0 := /tmp/pru$(PRUN0)-gen
GEN_DIR1 := /tmp/pru$(PRUN1)-gen

LINKER_COMMAND_FILE = AM335x_PRU.cmd
LIBS = --library=$(PRU_SUPPORT)/lib/rpmsg_lib.lib
INCLUDE = --include_path=$(PRU_SUPPORT)/include --include_path=$(PRU_SUPPORT)/include/am335x --include_path=../../common

STACK_SIZE = 0x100
HEAP_SIZE  = 0x100

CFLAGS0 = -v3 -O2 --printf_support=minimal --display_error_number --endian=little --hardware_mac=on --obj_directory=$(GEN_DIR0) --pp_directory=$(GEN_DIR0) --asm_directory=$(GEN_DIR0) -ppd -ppa --asm_listing --c_src_interlist -DAI=$(AI) # --absolute_listing
CFLAGS1 = -v3 -O2 --printf_support=minimal --display_error_number --endian=little --hardware_mac=on --obj_directory=$(GEN_DIR1) --pp_directory=$(GEN_DIR1) --asm_directory=$(GEN_DIR1) -ppd -ppa --asm_listing --c_src_interlist -DAI=$(AI) # --absolute_listing

LFLAGS0 = --reread_libs --warn_sections --stack_size=$(STACK_SIZE) --heap_size=$(HEAP_SIZE) -m $(GEN_DIR0)/$(TARGET0).map
LFLAGS1 = --reread_libs --warn_sections --stack_size=$(STACK_SIZE) --heap_size=$(HEAP_SIZE) -m $(GEN_DIR1)/$(TARGET1).map

# Model-dependent
AI=0
CHIP=am335x
PRU_DIR0 = /sys/class/remoteproc/remoteproc1
PRU_DIR1 = /sys/class/remoteproc/remoteproc2

$(warning CHIP= $(CHIP), PRU_DIR0= $(PRU_DIR0), PRU_DIR1= $(PRU_DIR1))

all: stop install0 install1 start
	@echo "CHIP     = $(CHIP)"
	@echo "PRUN0    = $(PRUN0)"
	@echo "PRUN1    = $(PRUN1)"
	@echo "PRU_DIR0 = $(PRU_DIR0)"
	@echo "PRU_DIR1 = $(PRU_DIR1)"

stop:
	@echo "-    Stopping PRUs"
	@echo stop | tee $(PRU_DIR0)/state || echo Cannot stop $(PRUN0)
	@echo stop | tee $(PRU_DIR1)/state || echo Cannot stop $(PRUN1)

start:
	@echo "-    Starting PRUs"
	@echo start | tee $(PRU_DIR0)/state
	@echo start | tee $(PRU_DIR1)/state
	@echo write_init_pins.sh
	gcc Disk2Controller.c -o Controller

install0: $(GEN_DIR0)/$(TARGET0).out
	@echo '-	copying firmware file $(GEN_DIR0)/$(TARGET0).out to /lib/firmware/$(CHIP)-pru$(PRUN0)-fw'
	@cp $(GEN_DIR0)/$(TARGET0).out /lib/firmware/$(CHIP)-pru$(PRUN0)-fw

install1: $(GEN_DIR1)/$(TARGET1).out
	@echo '-	copying firmware file $(GEN_DIR1)/$(TARGET1).out to /lib/firmware/$(CHIP)-pru$(PRUN1)-fw'
	@cp $(GEN_DIR1)/$(TARGET1).out /lib/firmware/$(CHIP)-pru$(PRUN1)-fw

$(GEN_DIR0)/$(TARGET0).out: $(GEN_DIR0)/$(TARGET0).obj
	@echo 'LD	$^' 
	@lnkpru -i$(PRU_CGT)/lib -i$(PRU_CGT)/include $(LFLAGS0) -o $@ $^ $(LINKER_COMMAND_FILE) --library=libc.a $(LIBS) $^

$(GEN_DIR1)/$(TARGET1).out: $(GEN_DIR1)/$(TARGET1).obj
	@echo 'LD	$^' 
	@lnkpru -i$(PRU_CGT)/lib -i$(PRU_CGT)/include $(LFLAGS1) -o $@ $^ $(LINKER_COMMAND_FILE) --library=libc.a $(LIBS) $^

$(GEN_DIR0)/$(TARGET0).obj: $(TARGET0).c
	@mkdir -p $(GEN_DIR0)
	@echo 'CC	$<'
	@clpru --include_path=$(PRU_CGT)/include $(INCLUDE) $(CFLAGS0) -D=PRUN0=$(PRUN0) -fe $@ $<

$(GEN_DIR1)/$(TARGET1).obj: $(TARGET1).c
	@mkdir -p $(GEN_DIR1)
	@echo 'CC	$<'
	@clpru --include_path=$(PRU_CGT)/include $(INCLUDE) $(CFLAGS1) -D=PRUN1=$(PRUN1) -fe $@ $<

clean:
	@echo 'CLEAN	.    PRUs'
	@rm -rf $(GEN_DIR0)
	@rm -rf $(GEN_DIR1)
