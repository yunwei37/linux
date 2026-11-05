// SPDX-License-Identifier: GPL-2.0
/* No allocation: called after machine_shutdown(), before load_segments(). */
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memblock.h>
#include <linux/kexec_handover.h>
#include <linux/kexec_free_snapshot.h>
#include <linux/bitmap.h>
#include <linux/irqflags.h>
static phys_addr_t snapshot_start,snapshot_bytes;
static int __init snapshot_bind(void)
{
 if(kho_is_enabled())
  reserve_mem_find_by_name(KEXEC_FREE_NAME,&snapshot_start,&snapshot_bytes);
 return 0;
}
late_initcall(snapshot_bind);
static bool corrupt;
static int __init corrupt_setup(char *unused){corrupt=true;return 1;}
__setup("modelos_free_corrupt",corrupt_setup);
void modelos_kexec_snapshot_free(void)
{
 phys_addr_t start,bytes;struct kexec_free_snapshot *h;struct zone *zone;u64 protect;unsigned long count=0;
 if(!snapshot_bytes)return;
 start=snapshot_start;bytes=snapshot_bytes;
 h=phys_to_virt(start);protect=h->protected_phys;
 memset(h,0,bytes);memcpy(h->magic,KEXEC_FREE_MAGIC,8);h->version=1;h->header_bytes=sizeof(*h);
 h->max_pfn=max_pfn;h->protected_phys=protect;h->bitmap_bytes=BITS_TO_LONGS(max_pfn)*sizeof(unsigned long);h->status=2;h->capture_flags=irqs_disabled()?1:0;
 if(h->capture_flags!=1||sizeof(*h)+h->bitmap_bytes>bytes)goto out;
 for_each_zone(zone){
  if(!populated_zone(zone))continue;
  if(!spin_trylock(&zone->lock))goto out;
  for(unsigned order=0;order<NR_PAGE_ORDERS;order++)for(unsigned type=0;type<MIGRATE_TYPES;type++){
   struct page *page;
   list_for_each_entry(page,&zone->free_area[order].free_list[type],buddy_list){
    unsigned long pfn=page_to_pfn(page),nr=1UL<<order;
    if(!PageBuddy(page)||pfn+nr>max_pfn){spin_unlock(&zone->lock);goto out;}
    bitmap_set(h->bitmap,pfn,nr);count+=nr;
   }
  }
  spin_unlock(&zone->lock);
 }
 h->free_pages=count;
 if(protect&&((protect&~PAGE_MASK)||protect>>PAGE_SHIFT>=max_pfn||test_bit(protect>>PAGE_SHIFT,h->bitmap)))goto out;
 h->status=1;
out:
 if(sizeof(*h)+h->bitmap_bytes<=bytes){h->checksum=kexec_free_checksum(h);if(corrupt)((u8 *)h->bitmap)[0]^=1;}
}
