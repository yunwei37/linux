/* SPDX-License-Identifier: GPL-2.0 */
#include "request.h"
#include "l1-request-layout.h"
static unsigned kexec_request_mode;
static const struct modelos_kho_request *kexec_request_record;
static const u8 *kexec_request_state;
static int __init kexec_request_arg(char *s)
{
 if(!strcmp(s,"restore"))kexec_request_mode=1;
 else if(!strcmp(s,"replay"))kexec_request_mode=2;
 else return 0;
 return 1;
}
__setup("modelos_request=",kexec_request_arg);
static int kexec_request_reject(const char *reason)
{pr_err("KEXEC_REQUEST_REJECT reason=%s\n",reason);return -EINVAL;}
static int kexec_request_validate(void)
{
 phys_addr_t start,bytes,model_start,model_bytes;const struct modelos_kho_request *r;
 const struct modelos_kho_payload *m;const u8 *p;
 if(!kexec_request_mode||kexec_request_record)return 0;
 if(!is_kho_boot())return kexec_request_reject("not-kho");
 if(!reserve_mem_find_by_name(MODELOS_REQUEST_NAME,&start,&bytes)||bytes!=MODELOS_REQUEST_REGION)return kexec_request_reject("region");
 p=phys_to_virt(start);r=(const void *)p;
 if(memcmp(r->magic,MODELOS_REQUEST_MAGIC,8)||r->version!=1||r->header_bytes!=MODELOS_REQUEST_HEADER)return kexec_request_reject("header");
 if(r->arena_bytes!=L1_REQUEST_ARENA_BYTES||r->arena_bytes>bytes-MODELOS_REQUEST_HEADER||r->layout_tag!=L1_REQUEST_LAYOUT_TAG||r->capacity!=L1_REQUEST_CAPACITY)return kexec_request_reject("layout");
 if(r->header_fnv!=modelos_request_header_fnv(p))return kexec_request_reject("header-fnv");
 if(r->state_fnv!=modelos_request_fnv(p+MODELOS_REQUEST_HEADER,r->arena_bytes))return kexec_request_reject("state-fnv");
 if(*(const u32 *)(p+MODELOS_REQUEST_HEADER)!=r->capacity)return kexec_request_reject("capacity");
 if(r->case_id||!r->emitted||r->emitted>64-MODELOS_REQUEST_CONTINUE||!r->position||r->position>r->capacity-MODELOS_REQUEST_CONTINUE||r->history_count!=r->position||!r->instance_generation||!r->request_generation)return kexec_request_reject("cursor");
 for(unsigned i=0;i<r->history_count;i++)if(r->history[i]>=151936)return kexec_request_reject("history-token");
 for(unsigned i=0;i<r->emitted;i++)if(r->emitted_ids[i]>=151936)return kexec_request_reject("emitted-token");
 if(r->next_token>=151936)return kexec_request_reject("next-token");
 if(!reserve_mem_find_by_name(KHO_MODEL_NAME,&model_start,&model_bytes)||model_bytes<4096)return kexec_request_reject("model-region");
 m=phys_to_virt(model_start+model_bytes-4096);
 if(r->model_fnv!=m->weights_fnv||r->cubin_fnv!=m->cubin_fnv)return kexec_request_reject("model-identity");
 kexec_request_record=r;kexec_request_state=p+MODELOS_REQUEST_HEADER;
 pr_info("KEXEC_REQUEST_VALIDATED kho_boot=1 phys=%pa arena=%llu emitted=%u position=%u next=%u generation=%llu state_fnv=%08x\n",&start,r->arena_bytes,r->emitted,r->position,r->next_token,r->request_generation,r->state_fnv);
 return 0;
}
