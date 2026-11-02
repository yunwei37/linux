// SPDX-License-Identifier: GPL-2.0
/* Opt-in allocation/overwrite workload for retained-memory experiments. */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/vmstat.h>
#include <linux/memblock.h>
#include <linux/kexec_free_snapshot.h>
#define ARENA_ORDER 9
#define ARENA_BLOCKS 4096
static bool enabled;
static struct page **arena;
static unsigned allocated;
static unsigned long before;
static int __init arena_setup(char *unused){enabled=true;return 1;}
__setup("modelos_buddy_arena",arena_setup);
void __init modelos_buddy_arena_admit(void);
int __init modelos_buddy_arena_release(void);
void __init modelos_buddy_arena_admit(void)
{
 u64 begin=ktime_get_ns();unsigned long bytes=(unsigned long)ARENA_BLOCKS*PAGE_SIZE*(1UL<<ARENA_ORDER);
 if(!enabled)return;
 {
  phys_addr_t start,size;
  if(reserve_mem_find_by_name(KEXEC_FREE_NAME,&start,&size)&&size>=sizeof(struct kexec_free_snapshot)){
   const struct kexec_free_snapshot *h=phys_to_virt(start);
   pr_info("KEXEC_FREE_CAPTURE_DATA phys=%pa status=%u pages=%llu max_pfn=%llu bitmap_bytes=%u flags=%u\n",&start,h->status,h->free_pages,h->max_pfn,h->bitmap_bytes,h->capture_flags);
  }
 }

 before=global_zone_page_state(NR_FREE_PAGES);
 if(before<(bytes+(128UL<<20))/PAGE_SIZE){pr_info("KEXEC_ARENA_DENIED need=%lu free=%lu\n",bytes,before<<PAGE_SHIFT);return;}
 arena=kcalloc(ARENA_BLOCKS,sizeof(*arena),GFP_KERNEL);
 if(!arena){pr_err("KEXEC_ARENA_ALLOC_FAIL metadata=1\n");return;}
 for(unsigned i=0;i<ARENA_BLOCKS;i++){
  arena[i]=alloc_pages(GFP_NOWAIT|__GFP_NOWARN,ARENA_ORDER);
  if(!arena[i]){pr_err("KEXEC_ARENA_ALLOC_FAIL blocks=%u\n",allocated);return;}
  allocated++;memset(page_address(arena[i]),(i*17+3)&255,PAGE_SIZE<<ARENA_ORDER);
 }
 pr_info("KEXEC_ARENA_ADMITTED bytes=%lu blocks=%u fill_ns=%llu free_before=%lu free_after=%lu\n",bytes,allocated,ktime_get_ns()-begin,before<<PAGE_SHIFT,global_zone_page_state(NR_FREE_PAGES)<<PAGE_SHIFT);
}
int __init modelos_buddy_arena_release(void)
{
 u64 begin=ktime_get_ns();unsigned bad=0,count=allocated;
 if(!arena)return 0;
 for(unsigned i=0;i<allocated;i++){
  bad+=memchr_inv(page_address(arena[i]),(i*17+3)&255,PAGE_SIZE<<ARENA_ORDER)!=NULL;
  __free_pages(arena[i],ARENA_ORDER);
 }
 kfree(arena);arena=NULL;allocated=0;
 pr_info("KEXEC_ARENA_RELEASE bytes=%lu bad_blocks=%u check_free_ns=%llu free_before=%lu free_after=%lu live_blocks=0\n",(unsigned long)count*PAGE_SIZE*(1UL<<ARENA_ORDER),bad,ktime_get_ns()-begin,before<<PAGE_SHIFT,global_zone_page_state(NR_FREE_PAGES)<<PAGE_SHIFT);
 return bad||count!=ARENA_BLOCKS?-EINVAL:0;
}
