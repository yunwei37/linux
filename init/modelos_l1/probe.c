// SPDX-License-Identifier: MIT
/* L1 consumer: call the trim lane's direct RM bridge, never an ioctl. */
#include "nv-linux.h"
#include "nvos.h"
#ifdef CONFIG_MODELOS_KEXEC_TEST
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include "payload.h"
#endif
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"

extern NV_STATUS modelos_rm_alloc(nvidia_stack_t *,NvHandle,NvHandle,NvHandle *,NvU32,void *,NvU32);
extern NV_STATUS modelos_rm_control(nvidia_stack_t *,NvHandle,NvHandle,NvU32,void *,NvU32);
extern NV_STATUS modelos_rm_free(nvidia_stack_t *,NvHandle,NvHandle);

#ifdef CONFIG_MODELOS_KEXEC_TEST
extern int modelos_kexec_payload_verify(void);
#endif

extern NV_STATUS modelos_l1_resources(nvidia_stack_t *,NvHandle,NvHandle,NvHandle);

static int __init modelos_l1_probe(void)
{
    NvU32 ids[32],count=32;
    struct pci_dev *pci=NULL;
    bool bound=false;
    NvHandle client=0,device=0,subdevice=0;
    NV_STATUS status=NV_ERR_INVALID_DEVICE;
    nvidia_stack_t *stack=NULL;
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS info={0};
    NV0080_ALLOC_PARAMETERS dev={0};
    NV2080_ALLOC_PARAMETERS sub={0};
    NV0080_CTRL_GPU_GET_CLASSLIST_PARAMS classes={0};
    bool held=false;
    unsigned short cs;
    asm volatile("mov %%cs,%0":"=r"(cs));
    pr_info("L1_RM_CONTEXT cpl=%u mm=%s\n",cs&3,current->mm?"present":"none");
#ifdef CONFIG_MODELOS_KEXEC_TEST
    if(is_kho_boot() && modelos_kexec_payload_verify()) {pr_err("KEXEC_PAYLOAD_REJECTED\n");return 0;}
#endif
    while((pci=pci_get_device(0x10de,PCI_ANY_ID,pci))) {
        if((pci->class>>16)==PCI_BASE_CLASS_DISPLAY && pci->driver &&
           !strcmp(pci->driver->name,"nvidia")) { bound=true;pci_dev_put(pci);break; }
    }
    if(!bound) { pr_err("L1_RM_PCI_BINDING_FAIL\n");return 0; }
    if(!nvidia_get_gpuid_list(ids,&count)||count!=1) {
        pr_err("L1_RM_GPU_ENUM_FAIL count=%u\n",count);return 0;
    }
    if(nv_kmem_cache_alloc_stack(&stack))return 0;
    if(nvidia_dev_get(ids[0],stack,NV_FALSE))goto done;
    held=true;
#define CALL(label,expr) do { status=(expr);pr_info("L1_RM_CALL %s status=0x%08x\n",label,status);if(status!=NV_OK)goto done; } while(0)
#ifdef CONFIG_MODELOS_KEXEC_TEST
    if(is_kho_boot()) {
        phys_addr_t pa,len;
        if(reserve_mem_find_by_name(KHO_MODEL_NAME,&pa,&len) && len>=4096) {
            const struct modelos_kho_payload *old=(const void *)((const char *)phys_to_virt(pa)+len-4096);
            if(!memcmp(old->magic,"Q3KHO001",8) && old->mode==2) {
                NV0080_CTRL_GPU_GET_CLASSLIST_PARAMS q={0};
                NV_STATUS adopted=modelos_rm_control(stack,old->old_client,old->old_device,NV0080_CTRL_CMD_GPU_GET_CLASSLIST,&q,sizeof(q));
                pr_info("KEXEC_ADOPT_CONTROL old_client=%x old_device=%x old_sub=%x status=0x%08x classes=%u\n",old->old_client,old->old_device,old->old_subdevice,adopted,q.numClasses);
            }
        }
    }
#endif
    CALL("root",modelos_rm_alloc(stack,0,0,&client,NV01_ROOT,&client,sizeof(client)));
    info.gpuId=ids[0];
    CALL("gpu-id",modelos_rm_control(stack,client,client,NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2,&info,sizeof(info)));
    dev.deviceId=info.deviceInstance;dev.hClientShare=client;
    dev.vaMode=NV_DEVICE_ALLOCATION_VAMODE_OPTIONAL_MULTIPLE_VASPACES;
    CALL("device",modelos_rm_alloc(stack,client,client,&device,NV01_DEVICE_0,&dev,sizeof(dev)));
    sub.subDeviceId=info.subDeviceInstance;
    CALL("subdevice",modelos_rm_alloc(stack,client,device,&subdevice,NV20_SUBDEVICE_0,&sub,sizeof(sub)));
    CALL("classes",modelos_rm_control(stack,client,device,NV0080_CTRL_CMD_GPU_GET_CLASSLIST,&classes,sizeof(classes)));
    if(!classes.numClasses){status=NV_ERR_INVALID_STATE;goto done;}
    pr_info("L1_RM_PROBE_PASS gpu=0x%08x classes=%u no_userspace=1\n",ids[0],classes.numClasses);
    CALL("replay-resources",modelos_l1_resources(stack,client,device,subdevice));
    status=NV_OK;
#undef CALL
done:
    if(status!=NV_OK)pr_err("L1_RM_PROBE_FAIL status=0x%08x held=%u\n",status,held);
    if(status==NV_ERR_TIMEOUT) { pr_err("L1_REPLAY_RETAIN active channel: retain RM resources until VM teardown\n");return 0; }
    if(client){NV_STATUS released=modelos_rm_free(stack,client,client);pr_info("L1_RM_FREE status=0x%08x\n",released);}
    if(held)nvidia_dev_put(ids[0],stack,NV_FALSE);
    nv_kmem_cache_free_stack(stack);
    return 0;
}
late_initcall(modelos_l1_probe);
