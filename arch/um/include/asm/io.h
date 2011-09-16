#ifndef __UM_IO_H
#define __UM_IO_H

#include "asm/page.h"
#include "linux/string.h"

#define IO_SPACE_LIMIT 0xdeadbeef /* Sure hope nothing uses this */

static inline int inb(unsigned long i) { return(0); }
static inline void outb(char c, unsigned long i) { }
static inline void outw(char c, unsigned long i) { }

#ifndef inb_p
# define inb_p		inb
#endif
/*
#ifndef inw_p
# define inw_p		inw
#endif
#ifndef inl_p
# define inl_p		inl
#endif
*/

#ifndef outb_p
# define outb_p		outb
#endif
/*
#ifndef outw_p
# define outw_p		outw
#endif
#ifndef outl_p
# define outl_p		outl
#endif
*/

/*
 * Change virtual addresses to physical addresses and vv.
 * These are pretty trivial
 */
static inline unsigned long virt_to_phys(volatile void * address)
{
	return __pa((void *) address);
}

static inline void * phys_to_virt(unsigned long address)
{
	return __va(address);
}

static inline void
memcpy_fromio(void *dst, const volatile void __iomem *src, size_t count)
{
	memcpy(dst, (const void __force *)src, count);
}

static inline void
memcpy_toio(volatile void __iomem *dst, const void *src, size_t count)
{
	memcpy((void __force *)dst, src, count);
}

/*
 * Convert a physical pointer to a virtual kernel pointer for /dev/mem
 * access
 */
#define xlate_dev_mem_ptr(p)	__va(p)

/*
 * Convert a virtual cached pointer to an uncached pointer
 */
#define xlate_dev_kmem_ptr(p)	p

static inline __u8 __readb(const volatile void __iomem *addr)
{
	return *(__force volatile __u8 *)addr;
}
static inline __u16 __readw(const volatile void __iomem *addr)
{
	return *(__force volatile __u16 *)addr;
}
static __always_inline __u32 __readl(const volatile void __iomem *addr)
{
	return *(__force volatile __u32 *)addr;
}
static inline __u64 __readq(const volatile void __iomem *addr)
{
	return *(__force volatile __u64 *)addr;
}
#define readb(x) __readb(x)
#define readw(x) __readw(x)
#define readl(x) __readl(x)
#define readq(x) __readq(x)

static inline void writeb(unsigned char b, volatile void __iomem *addr)
{
	*(volatile unsigned char __force *) addr = b;
}
static inline void writew(unsigned short b, volatile void __iomem *addr)
{
	*(volatile unsigned short __force *) addr = b;
}
static inline void writel(unsigned int b, volatile void __iomem *addr)
{
	*(volatile unsigned int __force *) addr = b;
}
static inline void writeq(unsigned int b, volatile void __iomem *addr)
{
	*(volatile unsigned long long __force *) addr = b;
}
#define __raw_writeb writeb
#define __raw_writew writew
#define __raw_writel writel
#define __raw_writeq writeq

#endif
