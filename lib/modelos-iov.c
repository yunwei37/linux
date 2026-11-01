// SPDX-License-Identifier: GPL-2.0-only
/* ModelOS kernel-buffer iterators. No user iovec, GUP or file-page-cache path. */
#include <linux/uio.h>
#include <linux/bvec.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <net/checksum.h>
void iov_iter_kvec(struct iov_iter *i,unsigned int direction,const struct kvec *v,unsigned long n,size_t count)
{ *i=(struct iov_iter){.iter_type=ITER_KVEC,.data_source=direction,.kvec=v,.nr_segs=n,.count=count}; }
void iov_iter_bvec(struct iov_iter *i,unsigned int direction,const struct bio_vec *v,unsigned long n,size_t count)
{ *i=(struct iov_iter){.iter_type=ITER_BVEC,.data_source=direction,.bvec=v,.nr_segs=n,.count=count}; }
void iov_iter_advance(struct iov_iter *i,size_t n)
{
 n=min(n,i->count);i->count-=n;
 while(n&&i->nr_segs){size_t len=iov_iter_is_kvec(i)?i->kvec->iov_len:i->bvec->bv_len;
  size_t take=min(n,len-i->iov_offset);i->iov_offset+=take;n-=take;
  if(i->iov_offset==len){if(iov_iter_is_kvec(i))i->kvec++;else i->bvec++;i->nr_segs--;i->iov_offset=0;}}
}
void iov_iter_revert(struct iov_iter *i,size_t n)
{
 i->count+=n;
 while(n>i->iov_offset){n-=i->iov_offset;if(iov_iter_is_kvec(i)){--i->kvec;i->iov_offset=i->kvec->iov_len;}else{--i->bvec;i->iov_offset=i->bvec->bv_len;}++i->nr_segs;}
 i->iov_offset-=n;
}
void iov_iter_restore(struct iov_iter *i,struct iov_iter_state *s)
{
 if(iov_iter_is_kvec(i))i->kvec-=s->nr_segs-i->nr_segs;else if(iov_iter_is_bvec(i))i->bvec-=s->nr_segs-i->nr_segs;
 i->iov_offset=s->iov_offset;i->count=s->count;i->nr_segs=s->nr_segs;
}
static size_t transfer(void *buffer,size_t bytes,struct iov_iter *i,bool to_iter)
{
 size_t done=0;bytes=min(bytes,i->count);
 while(done<bytes&&i->nr_segs){void *ptr,*mapped=NULL;size_t n;
  if(iov_iter_is_kvec(i)){n=i->kvec->iov_len-i->iov_offset;ptr=(char *)i->kvec->iov_base+i->iov_offset;}
  else if(iov_iter_is_bvec(i)){size_t off=i->bvec->bv_offset+i->iov_offset;
   mapped=kmap_local_page(pfn_to_page(page_to_pfn(i->bvec->bv_page)+off/PAGE_SIZE));ptr=(char *)mapped+off%PAGE_SIZE;
   n=min_t(size_t,i->bvec->bv_len-i->iov_offset,PAGE_SIZE-off%PAGE_SIZE);}
  else break;
  n=min(n,bytes-done);
  if(!n){if(iov_iter_is_kvec(i))i->kvec++;else i->bvec++;--i->nr_segs;i->iov_offset=0;if(mapped)kunmap_local(mapped);continue;}
  if(to_iter)memcpy(ptr,(char *)buffer+done,n);else memcpy((char *)buffer+done,ptr,n);
  if(mapped)kunmap_local(mapped);iov_iter_advance(i,n);done+=n;
 }
 return done;
}
size_t _copy_to_iter(const void *p,size_t n,struct iov_iter *i){return transfer((void *)p,n,i,true);}
size_t _copy_from_iter(void *p,size_t n,struct iov_iter *i){return transfer(p,n,i,false);}
size_t _copy_from_iter_nocache(void *p,size_t n,struct iov_iter *i){return _copy_from_iter(p,n,i);}
static size_t page_transfer(struct page *page,size_t off,size_t n,struct iov_iter *i,bool to_iter)
{
 size_t done=0;
 while(done<n){void *p=kmap_local_page(pfn_to_page(page_to_pfn(page)+off/PAGE_SIZE));size_t part=min_t(size_t,n-done,PAGE_SIZE-off%PAGE_SIZE);
  size_t copied=transfer((char *)p+off%PAGE_SIZE,part,i,to_iter);kunmap_local(p);done+=copied;off+=copied;if(copied!=part)break;}
 return done;
}
size_t copy_page_to_iter(struct page *p,size_t off,size_t n,struct iov_iter *i){return page_transfer(p,off,n,i,true);}
size_t copy_page_from_iter(struct page *p,size_t off,size_t n,struct iov_iter *i){return page_transfer(p,off,n,i,false);}
unsigned long iov_iter_alignment(const struct iov_iter *i)
{
 unsigned long a=i->iov_offset;size_t count=i->count,off=i->iov_offset;
 if(iov_iter_is_kvec(i))for(unsigned long j=0;j<i->nr_segs&&count;j++){size_t n=min(count,i->kvec[j].iov_len-off);a|=(unsigned long)i->kvec[j].iov_base+off;a|=n;count-=n;off=0;}
 return a;
}
/* ModelOS token sockets use copied kernel vectors, never page-splice requests. */
ssize_t iov_iter_get_pages2(struct iov_iter *i,struct page **p,size_t n,unsigned max,size_t *start){return -EOPNOTSUPP;}
ssize_t iov_iter_extract_pages(struct iov_iter *i,struct page ***p,size_t n,unsigned max,iov_iter_extraction_t flags,size_t *start){return -EOPNOTSUPP;}
__wsum csum_partial_copy_nocheck(const void *src,void *dst,int len){memcpy(dst,src,len);return csum_partial(dst,len,0);}
/* Kernel-only pseudo-header checksum, from csum-wrappers_64.c. */
__sum16 csum_ipv6_magic(const struct in6_addr *saddr,
			const struct in6_addr *daddr,
			__u32 len, __u8 proto, __wsum sum)
{
	__u64 rest, sum64;

	rest = (__force __u64)htonl(len) + (__force __u64)htons(proto) +
		(__force __u64)sum;

	asm("	addq (%[saddr]),%[sum]\n"
	    "	adcq 8(%[saddr]),%[sum]\n"
	    "	adcq (%[daddr]),%[sum]\n"
	    "	adcq 8(%[daddr]),%[sum]\n"
	    "	adcq $0,%[sum]\n"

	    : [sum] "=r" (sum64)
	    : "[sum]" (rest), [saddr] "r" (saddr), [daddr] "r" (daddr));

	return csum_fold(
	       (__force __wsum)add32_with_carry(sum64 & 0xffffffff, sum64>>32));
}
