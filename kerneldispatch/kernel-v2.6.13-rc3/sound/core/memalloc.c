/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *                   Takashi Iwai <tiwai@suse.de>
 * 
 *  Generic memory allocators
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <asm/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <asm/semaphore.h>
#include <sound/memalloc.h>
#ifdef CONFIG_SBUS
#include <asm/sbus.h>
#endif


MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>, Jaroslav Kysela <perex@suse.cz>");
MODULE_DESCRIPTION("Memory allocator for ALSA system.");
MODULE_LICENSE("GPL");


#ifndef SNDRV_CARDS
#define SNDRV_CARDS	8
#endif

/*
 */

void *snd_malloc_sgbuf_pages(struct device *device,
                             size_t size, struct snd_dma_buffer *dmab,
			     size_t *res_size);
int snd_free_sgbuf_pages(struct snd_dma_buffer *dmab);

/*
 */

static DECLARE_MUTEX(list_mutex);
static LIST_HEAD(mem_list_head);

/* buffer preservation list */
struct snd_mem_list {
	struct snd_dma_buffer buffer;
	unsigned int id;
	struct list_head list;
};

/* id for pre-allocated buffers */
#define SNDRV_DMA_DEVICE_UNUSED (unsigned int)-1

#ifdef CONFIG_SND_DEBUG
#define __ASTRING__(x) #x
#define snd_assert(expr, args...) do {\
	if (!(expr)) {\
		printk(KERN_ERR "snd-malloc: BUG? (%s) (called from %p)\n", __ASTRING__(expr), __builtin_return_address(0));\
		args;\
	}\
} while (0)
#else
#define snd_assert(expr, args...) /**/
#endif

/*
 *  Hacks
 */

#if defined(__i386__) || defined(__ppc__) || defined(__x86_64__)
/*
 * A hack to allocate large buffers via dma_alloc_coherent()
 *
 * since dma_alloc_coherent always tries GFP_DMA when the requested
 * pci memory region is below 32bit, it happens quite often that even
 * 2 order of pages cannot be allocated.
 *
 * so in the following, we allocate at first without dma_mask, so that
 * allocation will be done without GFP_DMA.  if the area doesn't match
 * with the requested region, then realloate with the original dma_mask
 * again.
 *
 * Really, we want to move this type of thing into dma_alloc_coherent()
 * so dma_mask doesn't have to be messed with.
 */

static void *snd_dma_hack_alloc_coherent(struct device *dev, size_t size,
					 dma_addr_t *dma_handle, int flags)
{
	void *ret;
	u64 dma_mask, coherent_dma_mask;

	if (dev == NULL || !dev->dma_mask)
		return dma_alloc_coherent(dev, size, dma_handle, flags);
	dma_mask = *dev->dma_mask;
	coherent_dma_mask = dev->coherent_dma_mask;
	*dev->dma_mask = 0xffffffff; 	/* do without masking */
	dev->coherent_dma_mask = 0xffffffff; 	/* do without masking */
	ret = dma_alloc_coherent(dev, size, dma_handle, flags);
	*dev->dma_mask = dma_mask;	/* restore */
	dev->coherent_dma_mask = coherent_dma_mask;	/* restore */
	if (ret) {
		/* obtained address is out of range? */
		if (((unsigned long)*dma_handle + size - 1) & ~dma_mask) {
			/* reallocate with the proper mask */
			dma_free_coherent(dev, size, ret, *dma_handle);
			ret = dma_alloc_coherent(dev, size, dma_handle, flags);
		}
	} else {
		/* wish to success now with the proper mask... */
		if (dma_mask != 0xffffffffUL) {
			/* allocation with GFP_ATOMIC to avoid the long stall */
			flags &= ~GFP_KERNEL;
			flags |= GFP_ATOMIC;
			ret = dma_alloc_coherent(dev, size, dma_handle, flags);
		}
	}
	return ret;
}

/* redefine dma_alloc_coherent for some architectures */
#undef dma_alloc_coherent
#define dma_alloc_coherent snd_dma_hack_alloc_coherent

#endif /* arch */

#if ! defined(__arm__)
#define NEED_RESERVE_PAGES
#endif

/*
 *
 *  Generic memory allocators
 *
 */

static long snd_allocated_pages; /* holding the number of allocated pages */

static inline void inc_snd_pages(int order)
{
	snd_allocated_pages += 1 << order;
}

static inline void dec_snd_pages(int order)
{
	snd_allocated_pages -= 1 << order;
}

static void mark_pages(struct page *page, int order)
{
	struct page *last_page = page + (1 << order);
	while (page < last_page)
		SetPageReserved(page++);
}

static void unmark_pages(struct page *page, int order)
{
	struct page *last_page = page + (1 << order);
	while (page < last_page)
		ClearPageReserved(page++);
}

/**
 * snd_malloc_pages - allocate pages with the given size
 * @size: the size to allocate in bytes
 * @gfp_flags: the allocation conditions, GFP_XXX
 *
 * Allocates the physically contiguous pages with the given size.
 *
 * Returns the pointer of the buffer, or NULL if no enoguh memory.
 */
void *snd_malloc_pages(size_t size, unsigned int gfp_flags)
{
	int pg;
	void *res;

	snd_assert(size > 0, return NULL);
	snd_assert(gfp_flags != 0, return NULL);
	pg = get_order(size);
	if ((res = (void *) __get_free_pages(gfp_flags, pg)) != NULL) {
		mark_pages(virt_to_page(res), pg);
		inc_snd_pages(pg);
	}
	return res;
}

/**
 * snd_free_pages - release the pages
 * @ptr: the buffer pointer to release
 * @size: the allocated buffer size
 *
 * Releases the buffer allocated via snd_malloc_pages().
 */
void snd_free_pages(void *ptr, size_t size)
{
	int pg;

	if (ptr == NULL)
		return;
	pg = get_order(size);
	dec_snd_pages(pg);
	unmark_pages(virt_to_page(ptr), pg);
	free_pages((unsigned long) ptr, pg);
}

/*
 *
 *  Bus-specific memory allocators
 *
 */

/* allocate the coherent DMA pages */
static void *snd_malloc_dev_pages(struct device *dev, size_t size, dma_addr_t *dma)
{
	int pg;
	void *res;
	unsigned int gfp_flags;

	snd_assert(size > 0, return NULL);
	snd_assert(dma != NULL, return NULL);
	pg = get_order(size);
	gfp_flags = GFP_KERNEL
		| __GFP_NORETRY /* don't trigger OOM-killer */
		| __GFP_NOWARN; /* no stack trace print - this call is non-critical */
	res = dma_alloc_coherent(dev, PAGE_SIZE << pg, dma, gfp_flags);
	if (res != NULL) {
#ifdef NEED_RESERVE_PAGES
		mark_pages(virt_to_page(res), pg); /* should be dma_to_page() */
#endif
		inc_snd_pages(pg);
	}

	return res;
}

/* free the coherent DMA pages */
static void snd_free_dev_pages(struct device *dev, size_t size, void *ptr,
			       dma_addr_t dma)
{
	int pg;

	if (ptr == NULL)
		return;
	pg = get_order(size);
	dec_snd_pages(pg);
#ifdef NEED_RESERVE_PAGES
	unmark_pages(virt_to_page(ptr), pg); /* should be dma_to_page() */
#endif
	dma_free_coherent(dev, PAGE_SIZE << pg, ptr, dma);
}

#ifdef CONFIG_SBUS

static void *snd_malloc_sbus_pages(struct device *dev, size_t size,
				   dma_addr_t *dma_addr)
{
	struct sbus_dev *sdev = (struct sbus_dev *)dev;
	int pg;
	void *res;

	snd_assert(size > 0, return NULL);
	snd_assert(dma_addr != NULL, return NULL);
	pg = get_order(size);
	res = sbus_alloc_consistent(sdev, PAGE_SIZE * (1 << pg), dma_addr);
	if (res != NULL)
		inc_snd_pages(pg);
	return res;
}

static void snd_free_sbus_pages(struct device *dev, size_t size,
				void *ptr, dma_addr_t dma_addr)
{
	struct sbus_dev *sdev = (struct sbus_dev *)dev;
	int pg;

	if (ptr == NULL)
		return;
	pg = get_order(size);
	dec_snd_pages(pg);
	sbus_free_consistent(sdev, PAGE_SIZE * (1 << pg), ptr, dma_addr);
}

#endif /* CONFIG_SBUS */

/*
 *
 *  ALSA generic memory management
 *
 */


/**
 * snd_dma_alloc_pages - allocate the buffer area according to the given type
 * @type: the DMA buffer type
 * @device: the device pointer
 * @size: the buffer size to allocate
 * @dmab: buffer allocation record to store the allocated data
 *
 * Calls the memory-allocator function for the corresponding
 * buffer type.
 * 
 * Returns zero if the buffer with the given size is allocated successfuly,
 * other a negative value at error.
 */
int snd_dma_alloc_pages(int type, struct device *device, size_t size,
			struct snd_dma_buffer *dmab)
{
	snd_assert(size > 0, return -ENXIO);
	snd_assert(dmab != NULL, return -ENXIO);

	dmab->dev.type = type;
	dmab->dev.dev = device;
	dmab->bytes = 0;
	switch (type) {
	case SNDRV_DMA_TYPE_CONTINUOUS:
		dmab->area = snd_malloc_pages(size, (unsigned long)device);
		dmab->addr = 0;
		break;
#ifdef CONFIG_SBUS
	case SNDRV_DMA_TYPE_SBUS:
		dmab->area = snd_malloc_sbus_pages(device, size, &dmab->addr);
		break;
#endif
	case SNDRV_DMA_TYPE_DEV:
		dmab->area = snd_malloc_dev_pages(device, size, &dmab->addr);
		break;
	case SNDRV_DMA_TYPE_DEV_SG:
		snd_malloc_sgbuf_pages(device, size, dmab, NULL);
		break;
	default:
		printk(KERN_ERR "snd-malloc: invalid device type %d\n", type);
		dmab->area = NULL;
		dmab->addr = 0;
		return -ENXIO;
	}
	if (! dmab->area)
		return -ENOMEM;
	dmab->bytes = size;
	return 0;
}

/**
 * snd_dma_alloc_pages_fallback - allocate the buffer area according to the given type with fallback
 * @type: the DMA buffer type
 * @device: the device pointer
 * @size: the buffer size to allocate
 * @dmab: buffer allocation record to store the allocated data
 *
 * Calls the memory-allocator function for the corresponding
 * buffer type.  When no space is left, this function reduces the size and
 * tries to allocate again.  The size actually allocated is stored in
 * res_size argument.
 * 
 * Returns zero if the buffer with the given size is allocated successfuly,
 * other a negative value at error.
 */
int snd_dma_alloc_pages_fallback(int type, struct device *device, size_t size,
				 struct snd_dma_buffer *dmab)
{
	int err;

	snd_assert(size > 0, return -ENXIO);
	snd_assert(dmab != NULL, return -ENXIO);

	while ((err = snd_dma_alloc_pages(type, device, size, dmab)) < 0) {
		if (err != -ENOMEM)
			return err;
		size >>= 1;
		if (size <= PAGE_SIZE)
			return -ENOMEM;
	}
	if (! dmab->area)
		return -ENOMEM;
	return 0;
}


/**
 * snd_dma_free_pages - release the allocated buffer
 * @dmab: the buffer allocation record to release
 *
 * Releases the allocated buffer via snd_dma_alloc_pages().
 */
void snd_dma_free_pages(struct snd_dma_buffer *dmab)
{
	switch (dmab->dev.type) {
	case SNDRV_DMA_TYPE_CONTINUOUS:
		snd_free_pages(dmab->area, dmab->bytes);
		break;
#ifdef CONFIG_SBUS
	case SNDRV_DMA_TYPE_SBUS:
		snd_free_sbus_pages(dmab->dev.dev, dmab->bytes, dmab->area, dmab->addr);
		break;
#endif
	case SNDRV_DMA_TYPE_DEV:
		snd_free_dev_pages(dmab->dev.dev, dmab->bytes, dmab->area, dmab->addr);
		break;
	case SNDRV_DMA_TYPE_DEV_SG:
		snd_free_sgbuf_pages(dmab);
		break;
	default:
		printk(KERN_ERR "snd-malloc: invalid device type %d\n", dmab->dev.type);
	}
}


/**
 * snd_dma_get_reserved - get the reserved buffer for the given device
 * @dmab: the buffer allocation record to store
 * @id: the buffer id
 *
 * Looks for the reserved-buffer list and re-uses if the same buffer
 * is found in the list.  When the buffer is found, it's removed from the free list.
 *
 * Returns the size of buffer if the buffer is found, or zero if not found.
 */
size_t snd_dma_get_reserved_buf(struct snd_dma_buffer *dmab, unsigned int id)
{
	struct list_head *p;
	struct snd_mem_list *mem;

	snd_assert(dmab, return 0);

	down(&list_mutex);
	list_for_each(p, &mem_list_head) {
		mem = list_entry(p, struct snd_mem_list, list);
		if (mem->id == id &&
		    (mem->buffer.dev.dev == NULL || dmab->dev.dev == NULL ||
		     ! memcmp(&mem->buffer.dev, &dmab->dev, sizeof(dmab->dev)))) {
			struct device *dev = dmab->dev.dev;
			list_del(p);
			*dmab = mem->buffer;
			if (dmab->dev.dev == NULL)
				dmab->dev.dev = dev;
			kfree(mem);
			up(&list_mutex);
			return dmab->bytes;
		}
	}
	up(&list_mutex);
	return 0;
}

/**
 * snd_dma_reserve_buf - reserve the buffer
 * @dmab: the buffer to reserve
 * @id: the buffer id
 *
 * Reserves the given buffer as a reserved buffer.
 * 
 * Returns zero if successful, or a negative code at error.
 */
int snd_dma_reserve_buf(struct snd_dma_buffer *dmab, unsigned int id)
{
	struct snd_mem_list *mem;

	snd_assert(dmab, return -EINVAL);
	mem = kmalloc(sizeof(*mem), GFP_KERNEL);
	if (! mem)
		return -ENOMEM;
	down(&list_mutex);
	mem->buffer = *dmab;
	mem->id = id;
	list_add_tail(&mem->list, &mem_list_head);
	up(&list_mutex);
	return 0;
}

/*
 * purge all reserved buffers
 */
static void free_all_reserved_pages(void)
{
	struct list_head *p;
	struct snd_mem_list *mem;

	down(&list_mutex);
	while (! list_empty(&mem_list_head)) {
		p = mem_list_head.next;
		mem = list_entry(p, struct snd_mem_list, list);
		list_del(p);
		snd_dma_free_pages(&mem->buffer);
		kfree(mem);
	}
	up(&list_mutex);
}


#ifdef CONFIG_PROC_FS
/*
 * proc file interface
 */
#define SND_MEM_PROC_FILE	"driver/snd-page-alloc"
struct proc_dir_entry *snd_mem_proc;

static int snd_mem_proc_read(char *page, char **start, off_t off,
			     int count, int *eof, void *data)
{
	int len = 0;
	long pages = snd_allocated_pages >> (PAGE_SHIFT-12);
	struct list_head *p;
	struct snd_mem_list *mem;
	int devno;
	static char *types[] = { "UNKNOWN", "CONT", "DEV", "DEV-SG", "SBUS" };

	down(&list_mutex);
	len += snprintf(page + len, count - len,
			"pages  : %li bytes (%li pages per %likB)\n",
			pages * PAGE_SIZE, pages, PAGE_SIZE / 1024);
	devno = 0;
	list_for_each(p, &mem_list_head) {
		mem = list_entry(p, struct snd_mem_list, list);
		devno++;
		len += snprintf(page + len, count - len,
				"buffer %d : ID %08x : type %s\n",
				devno, mem->id, types[mem->buffer.dev.type]);
		len += snprintf(page + len, count - len,
				"  addr = 0x%lx, size = %d bytes\n",
				(unsigned long)mem->buffer.addr, (int)mem->buffer.bytes);
	}
	up(&list_mutex);
	return len;
}

/* FIXME: for pci only - other bus? */
#ifdef CONFIG_PCI
#define gettoken(bufp) strsep(bufp, " \t\n")

static int snd_mem_proc_write(struct file *file, const char __user *buffer,
			      unsigned long count, void *data)
{
	char buf[128];
	char *token, *p;

	if (count > ARRAY_SIZE(buf) - 1)
		count = ARRAY_SIZE(buf) - 1;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[ARRAY_SIZE(buf) - 1] = '\0';

	p = buf;
	token = gettoken(&p);
	if (! token || *token == '#')
		return (int)count;
	if (strcmp(token, "add") == 0) {
		char *endp;
		int vendor, device, size, buffers;
		long mask;
		int i, alloced;
		struct pci_dev *pci;

		if ((token = gettoken(&p)) == NULL ||
		    (vendor = simple_strtol(token, NULL, 0)) <= 0 ||
		    (token = gettoken(&p)) == NULL ||
		    (device = simple_strtol(token, NULL, 0)) <= 0 ||
		    (token = gettoken(&p)) == NULL ||
		    (mask = simple_strtol(token, NULL, 0)) < 0 ||
		    (token = gettoken(&p)) == NULL ||
		    (size = memparse(token, &endp)) < 64*1024 ||
		    size > 16*1024*1024 /* too big */ ||
		    (token = gettoken(&p)) == NULL ||
		    (buffers = simple_strtol(token, NULL, 0)) <= 0 ||
		    buffers > 4) {
			printk(KERN_ERR "snd-page-alloc: invalid proc write format\n");
			return (int)count;
		}
		vendor &= 0xffff;
		device &= 0xffff;

		alloced = 0;
		pci = NULL;
		while ((pci = pci_find_device(vendor, device, pci)) != NULL) {
			if (mask > 0 && mask < 0xffffffff) {
				if (pci_set_dma_mask(pci, mask) < 0 ||
				    pci_set_consistent_dma_mask(pci, mask) < 0) {
					printk(KERN_ERR "snd-page-alloc: cannot set DMA mask %lx for pci %04x:%04x\n", mask, vendor, device);
					return (int)count;
				}
			}
			for (i = 0; i < buffers; i++) {
				struct snd_dma_buffer dmab;
				memset(&dmab, 0, sizeof(dmab));
				if (snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, snd_dma_pci_data(pci),
							size, &dmab) < 0) {
					printk(KERN_ERR "snd-page-alloc: cannot allocate buffer pages (size = %d)\n", size);
					return (int)count;
				}
				snd_dma_reserve_buf(&dmab, snd_dma_pci_buf_id(pci));
			}
			alloced++;
		}
		if (! alloced) {
			for (i = 0; i < buffers; i++) {
				struct snd_dma_buffer dmab;
				memset(&dmab, 0, sizeof(dmab));
				/* FIXME: We can allocate only in ZONE_DMA
				 * without a device pointer!
				 */
				if (snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, NULL,
							size, &dmab) < 0) {
					printk(KERN_ERR "snd-page-alloc: cannot allocate buffer pages (size = %d)\n", size);
					break;
				}
				snd_dma_reserve_buf(&dmab, (unsigned int)((vendor << 16) | device));
			}
		}
	} else if (strcmp(token, "erase") == 0)
		/* FIXME: need for releasing each buffer chunk? */
		free_all_reserved_pages();
	else
		printk(KERN_ERR "snd-page-alloc: invalid proc cmd\n");
	return (int)count;
}
#endif /* CONFIG_PCI */
#endif /* CONFIG_PROC_FS */

/*
 * module entry
 */

static int __init snd_mem_init(void)
{
#ifdef CONFIG_PROC_FS
	snd_mem_proc = create_proc_entry(SND_MEM_PROC_FILE, 0644, NULL);
	if (snd_mem_proc) {
		snd_mem_proc->read_proc = snd_mem_proc_read;
#ifdef CONFIG_PCI
		snd_mem_proc->write_proc = snd_mem_proc_write;
#endif
	}
#endif
	return 0;
}

static void __exit snd_mem_exit(void)
{
	if (snd_mem_proc)
		remove_proc_entry(SND_MEM_PROC_FILE, NULL);
	free_all_reserved_pages();
	if (snd_allocated_pages > 0)
		printk(KERN_ERR "snd-malloc: Memory leak?  pages not freed = %li\n", snd_allocated_pages);
}


module_init(snd_mem_init)
module_exit(snd_mem_exit)


/*
 * exports
 */
EXPORT_SYMBOL(snd_dma_alloc_pages);
EXPORT_SYMBOL(snd_dma_alloc_pages_fallback);
EXPORT_SYMBOL(snd_dma_free_pages);

EXPORT_SYMBOL(snd_dma_get_reserved_buf);
EXPORT_SYMBOL(snd_dma_reserve_buf);

EXPORT_SYMBOL(snd_malloc_pages);
EXPORT_SYMBOL(snd_free_pages);