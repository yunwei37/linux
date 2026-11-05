// SPDX-License-Identifier: GPL-2.0
/* ModelOS polled modern virtio-net. PCI/DMA bring-up only uses Linux; the
 * owner owns both split vrings. No virtqueue framework, netdev, NAPI, skb,
 * IRQ handler or network softirq. No feature for checksum/segmentation offload
 * or merged receive buffers is negotiated. All packet memory is fixed at boot.
 * Ring structs/layout come from the existing BSD-licensed virtio_ring.h.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/modelos_net.h>
#include <linux/pci.h>
#include <linux/virtio_config.h>
#include <uapi/linux/virtio_net.h>
#include <uapi/linux/virtio_pci.h>
#include <uapi/linux/virtio_ring.h>
#define QUEUE_MAX 256
#define BUFFER_BYTES 2048
#define FRAME_BYTES 1536
#define HDR_BYTES sizeof(struct virtio_net_hdr_v1)
struct poll_queue {
 struct vring ring;
 void *memory, *buffers;
 dma_addr_t dma, buffers_dma;
 size_t ring_bytes;
 void __iomem *notify;
 u16 avail, published, used, free_count;
 int reserved;
 u16 free[QUEUE_MAX];
 bool pending[QUEUE_MAX];
};
static struct pci_dev *nic;
static struct virtio_pci_common_cfg __iomem *common;
static void __iomem *notify_base, *device_cfg;
static u32 notify_multiplier, notify_length;
static struct poll_queue queues[2];
static bool ready;
static struct mos_nic_stats counters;
void mos_nic_stats(struct mos_nic_stats *out,int reset)
{if(out)*out=counters;if(reset)memset(&counters,0,sizeof(counters));}
static void status(u8 value){writeb(value,&common->device_status);}
static void put64(void __iomem *where,u64 value)
{writel(lower_32_bits(value),where);writel(upper_32_bits(value),where+4);}
static void kick(unsigned index)
{ /* Publish ring descriptors before MMIO notification. */
 u64 start=ktime_get_ns();
 dma_wmb();writew(index,queues[index].notify);
 if(index){counters.tx_kicks++;counters.tx_kick_ns+=ktime_get_ns()-start;}
 else{counters.rx_kicks++;counters.rx_kick_ns+=ktime_get_ns()-start;}
}
static int map_caps(void)
{
 int cap;
 for(cap=pci_find_capability(nic,PCI_CAP_ID_VNDR);cap;cap=pci_find_next_capability(nic,cap,PCI_CAP_ID_VNDR)){
  u8 type,bar,len;u32 offset,bytes;void __iomem *mapped;
  pci_read_config_byte(nic,cap+offsetof(struct virtio_pci_cap,cap_len),&len);
  pci_read_config_byte(nic,cap+offsetof(struct virtio_pci_cap,cfg_type),&type);
  if(type!=VIRTIO_PCI_CAP_COMMON_CFG&&type!=VIRTIO_PCI_CAP_NOTIFY_CFG&&type!=VIRTIO_PCI_CAP_DEVICE_CFG)continue;
  if(len<sizeof(struct virtio_pci_cap))return -EINVAL;
  pci_read_config_byte(nic,cap+offsetof(struct virtio_pci_cap,bar),&bar);
  pci_read_config_dword(nic,cap+offsetof(struct virtio_pci_cap,offset),&offset);
  pci_read_config_dword(nic,cap+offsetof(struct virtio_pci_cap,length),&bytes);
  if(bar>=PCI_STD_NUM_BARS||!(pci_resource_flags(nic,bar)&IORESOURCE_MEM)||!bytes||
     (u64)offset+bytes>pci_resource_len(nic,bar))return -EINVAL;
  if((type==VIRTIO_PCI_CAP_COMMON_CFG&&bytes<sizeof(*common))||
     (type==VIRTIO_PCI_CAP_DEVICE_CFG&&bytes<6))return -EINVAL;
  mapped=pci_iomap_range(nic,bar,offset,bytes);if(!mapped)return -ENOMEM;
  if(type==VIRTIO_PCI_CAP_COMMON_CFG)common=mapped;
  else if(type==VIRTIO_PCI_CAP_DEVICE_CFG)device_cfg=mapped;
  else{
   if(len<sizeof(struct virtio_pci_notify_cap)){pci_iounmap(nic,mapped);return -EINVAL;}
   notify_base=mapped;notify_length=bytes;
   pci_read_config_dword(nic,cap+offsetof(struct virtio_pci_notify_cap,notify_off_multiplier),&notify_multiplier);
  }
 }
 return common&&notify_base&&device_cfg?0:-ENODEV;
}
/* Publish a whole owner batch, then obey the device's split-ring kick
 * suppression flag. The full barrier pairs the avail update with vhost's
 * enable-notification/recheck sequence: neither side may miss the wakeup. */
void mos_nic_flush(void)
{
 if(!ready)return;
 for(unsigned index=0;index<2;index++){
  struct poll_queue *q=&queues[index];
  if(q->published==q->avail)continue;
  dma_wmb();WRITE_ONCE(q->ring.avail->idx,cpu_to_le16(q->avail));
  q->published=q->avail;mb();
  if(!(le16_to_cpu(READ_ONCE(q->ring.used->flags))&VRING_USED_F_NO_NOTIFY))kick(index);
 }
}
static int queue_init(unsigned index)
{
 struct poll_queue *q=&queues[index];unsigned n;u64 offset;
 writew(index,&common->queue_select);
 n=readw(&common->queue_size);if(!n)return -ENODEV;
 n=min(n,QUEUE_MAX);if(!is_power_of_2(n))return -EINVAL;
 writew(n,&common->queue_size);writew(VIRTIO_MSI_NO_VECTOR,&common->queue_msix_vector);
 q->reserved=-1;q->ring.num=n;q->ring_bytes=vring_size(n,PAGE_SIZE);
 q->memory=dma_alloc_coherent(&nic->dev,q->ring_bytes,&q->dma,GFP_KERNEL);
 q->buffers=dma_alloc_coherent(&nic->dev,n*BUFFER_BYTES,&q->buffers_dma,GFP_KERNEL);
 if(!q->memory||!q->buffers)return -ENOMEM;
 memset(q->memory,0,q->ring_bytes);memset(q->buffers,0,n*BUFFER_BYTES);
 vring_init(&q->ring,n,q->memory,PAGE_SIZE);
 q->ring.avail->flags=cpu_to_le16(VRING_AVAIL_F_NO_INTERRUPT);
 for(unsigned i=0;i<n;i++){
  q->ring.desc[i].addr=cpu_to_le64(q->buffers_dma+i*BUFFER_BYTES);
  if(!index){
   q->ring.desc[i].len=cpu_to_le32(BUFFER_BYTES);
   q->ring.desc[i].flags=cpu_to_le16(VRING_DESC_F_WRITE);
   q->ring.avail->ring[q->avail++]=cpu_to_le16(i);
  }else q->free[q->free_count++]=i;
 }
 dma_wmb();WRITE_ONCE(q->ring.avail->idx,cpu_to_le16(q->avail));q->published=q->avail;
 offset=(u64)readw(&common->queue_notify_off)*notify_multiplier;
 if(offset+2>notify_length)return -EINVAL;
 q->notify=notify_base+offset;
 put64(&common->queue_desc_lo,q->dma);
 put64(&common->queue_avail_lo,q->dma+((char *)q->ring.avail-(char *)q->memory));
 put64(&common->queue_used_lo,q->dma+((char *)q->ring.used-(char *)q->memory));
 writew(1,&common->queue_enable);return 0;
}
static void cleanup(void)
{
 if(common){status(0);readb(&common->device_status);}
 for(unsigned i=0;i<2;i++){
  struct poll_queue *q=&queues[i];
  if(q->buffers)dma_free_coherent(&nic->dev,q->ring.num*BUFFER_BYTES,q->buffers,q->buffers_dma);
  if(q->memory)dma_free_coherent(&nic->dev,q->ring_bytes,q->memory,q->dma);
  memset(q,0,sizeof(*q));
 }
 if(common)pci_iounmap(nic,common);
 if(notify_base)pci_iounmap(nic,notify_base);
 if(device_cfg)pci_iounmap(nic,device_cfg);
 common=NULL;notify_base=NULL;device_cfg=NULL;
}
int mos_nic_init(u8 mac[6])
{
 int error;u32 low,high;unsigned retry;
 if(ready||nic)return -EBUSY;
 nic=pci_get_device(0x1af4,0x1041,NULL);
 if(!nic)nic=pci_get_device(0x1af4,0x1000,NULL);
 if(!nic)return -ENODEV;
 error=pci_enable_device(nic);if(error)goto put;
 error=pci_request_regions(nic,"modelos-poll-net");if(error)goto disable;
 error=dma_set_mask_and_coherent(&nic->dev,DMA_BIT_MASK(64));if(error)goto regions;
 pci_set_master(nic);pci_intx(nic,0);
 error=map_caps();if(error)goto failed;
 status(0);
 for(retry=0;readb(&common->device_status)&&retry<1000;retry++)udelay(10);
 if(retry==1000){error=-ETIMEDOUT;goto failed;}
 status(VIRTIO_CONFIG_S_ACKNOWLEDGE|VIRTIO_CONFIG_S_DRIVER);
 writel(0,&common->device_feature_select);low=readl(&common->device_feature);
 writel(1,&common->device_feature_select);high=readl(&common->device_feature);
 if(!(low&BIT(VIRTIO_NET_F_MAC))||!(high&BIT(VIRTIO_F_VERSION_1-32))){error=-ENOTSUPP;goto failed;}
 writel(0,&common->guest_feature_select);writel(BIT(VIRTIO_NET_F_MAC),&common->guest_feature);
 writel(1,&common->guest_feature_select);writel(BIT(VIRTIO_F_VERSION_1-32),&common->guest_feature);
 status(VIRTIO_CONFIG_S_ACKNOWLEDGE|VIRTIO_CONFIG_S_DRIVER|VIRTIO_CONFIG_S_FEATURES_OK);
 if(!(readb(&common->device_status)&VIRTIO_CONFIG_S_FEATURES_OK)){error=-ENOTSUPP;goto failed;}
 writew(VIRTIO_MSI_NO_VECTOR,&common->msix_config);
 if(readw(&common->num_queues)<2){error=-ENODEV;goto failed;}
 for(unsigned i=0;i<6;i++)mac[i]=readb(device_cfg+i);
 error=queue_init(0);if(error)goto failed;
 error=queue_init(1);if(error)goto failed;
 dma_wmb();status(VIRTIO_CONFIG_S_ACKNOWLEDGE|VIRTIO_CONFIG_S_DRIVER|VIRTIO_CONFIG_S_FEATURES_OK|VIRTIO_CONFIG_S_DRIVER_OK);
 ready=true;kick(0);
 pr_info("MODELOS_POLL_NET ready=1 mac=%pM rx=%u tx=%u irq=0 netdev=0\n",mac,queues[0].ring.num,queues[1].ring.num);return 0;
failed:cleanup();pci_clear_master(nic);
regions:pci_release_regions(nic);
disable:pci_disable_device(nic);
put:pci_dev_put(nic);nic=NULL;return error;
}
void mos_nic_poll(void)
{
 struct poll_queue *q=&queues[1];unsigned budget=QUEUE_MAX;
 if(!ready)return;
 while(budget--&&q->used!=le16_to_cpu(READ_ONCE(q->ring.used->idx))){
  u32 id;dma_rmb();id=le32_to_cpu(q->ring.used->ring[q->used++%q->ring.num].id);
  if(id>=q->ring.num||!q->pending[id])panic("modelos-net: invalid TX completion");
  q->pending[id]=false;q->free[q->free_count++]=id;
 }
}
int mos_nic_tx_ready(void)
{
 if(!ready)return 0;
 if(!queues[1].free_count)mos_nic_poll();
 return queues[1].free_count&&queues[1].reserved<0;
}
void *mos_nic_tx_acquire(size_t bytes)
{
 struct poll_queue *q=&queues[1];unsigned id;
 if(bytes<14||bytes>FRAME_BYTES||!mos_nic_tx_ready())return NULL;
 id=q->free[--q->free_count];q->reserved=id;
 /* The non-offloaded virtio header was zeroed at boot and stays zero. */
 return q->buffers+id*BUFFER_BYTES+HDR_BYTES;
}
int mos_nic_tx_commit(size_t bytes)
{
 struct poll_queue *q=&queues[1];int id=q->reserved;
 if(id<0||bytes<14||bytes>FRAME_BYTES)return -EINVAL;
 q->reserved=-1;q->ring.desc[id].len=cpu_to_le32(HDR_BYTES+bytes);q->pending[id]=true;
 q->ring.avail->ring[q->avail++%q->ring.num]=cpu_to_le16(id);
 counters.tx_frames++;counters.tx_bytes+=bytes;return 0;
}
int mos_nic_send(const void *frame,size_t bytes)
{
 void *buffer=mos_nic_tx_acquire(bytes);
 if(!buffer)return bytes<14||bytes>FRAME_BYTES?-EMSGSIZE:-EAGAIN;
 memcpy(buffer,frame,bytes);counters.copy_bytes+=bytes;
 /* Preserve the original copy-send ABI for already linked owners. The new
  * smoltcp adapter uses acquire/commit and flushes only at Step boundaries. */
 int error=mos_nic_tx_commit(bytes);mos_nic_flush();return error;
}
int mos_nic_receive(void *out,size_t capacity)
{
 struct poll_queue *q=&queues[0];unsigned budget=QUEUE_MAX;
 if(!ready)return -ENODEV;
 while(budget--&&q->used!=le16_to_cpu(READ_ONCE(q->ring.used->idx))){
  u32 id,len;int copied=0;struct virtio_net_hdr_v1 *hdr;
  dma_rmb();id=le32_to_cpu(q->ring.used->ring[q->used%q->ring.num].id);
  len=le32_to_cpu(q->ring.used->ring[q->used++%q->ring.num].len);
  if(id>=q->ring.num)panic("modelos-net: invalid RX completion");
  hdr=q->buffers+id*BUFFER_BYTES;
  if(len>=HDR_BYTES+14&&len<=min_t(size_t,BUFFER_BYTES,HDR_BYTES+capacity)&&
     !hdr->flags&&!hdr->gso_type){
   copied=len-HDR_BYTES;counters.rx_frames++;memcpy(out,(void *)hdr+HDR_BYTES,copied);
  }
  q->ring.avail->ring[q->avail%q->ring.num]=cpu_to_le16(id);
  q->avail++;
  if(copied)return copied;
 }
 return 0;
}
