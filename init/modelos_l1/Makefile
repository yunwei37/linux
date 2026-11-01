# ModelOS L1 consumer of the separately owned NVIDIA RM adapter.
obj-y += probe.o
nvidia := $(realpath $(srctree)/drivers/modelos-nvidia)
ccflags-y += -I$(objtree)/drivers/modelos-nvidia -I$(nvidia)/common/inc
ccflags-y += -I$(nvidia)/../src/common/sdk/nvidia/inc
ccflags-y += -DNVRM -DNV_KERNEL_INTERFACE_LAYER -DNV_FILESYSTEM_ACCESS_AVAILABLE=0
ccflags-y += -DNV_VERSION_STRING=\"615.78.08\" -DNV_SPECTRE_V2=0
ccflags-y += -Wno-unused-function
