#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/sched.h>
#include <linux/console.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include "os.h"
#include "../../../drivers/video/console/fbcon.h"

#include "irq_kern.h"
#include "irq_user.h"
#include "shmfb_user.h"
#include "shmfb_kern.h"

/*
 * These are all constant after early boot
 */
static int shmfb_enable;
static int shmfb_width;
static int shmfb_height;
static int shmfb_depth;
static char *shmfb_client;

struct shmfb_kerndata {
	/* common stuff */
	struct shmfb_window       *win;

	/* framebuffer driver */
	struct fb_fix_screeninfo  *fix;
	struct fb_var_screeninfo  *var;
	struct fb_info            *info;
	int                       x1, x2, y1, y2;

	/* fb mapping */
	struct semaphore          mm_lock;
	struct vm_area_struct     *vma;
	atomic_t                  map_refs;
	int                       faults;
	int                       nr_pages;
	struct page               **pages;
	int                       *mapped;

	/* input drivers */
	struct input_dev          *kbd;
	struct input_dev          *mouse;

	/* i/o socket */
	int                       sock;
};

static int do_unlink_socket(struct notifier_block *notifier,
				unsigned long what, void *data)
{
	return shmfb_unlink_socket();
}


static struct notifier_block reboot_notifier = {
	.notifier_call		= do_unlink_socket,
	.priority		= 0,
};

void shmfb_mmap_update(struct shmfb_kerndata *kd)
{
	int i, off, len;
	char *src;

	zap_page_range(kd->vma, kd->vma->vm_start,
		       kd->vma->vm_end - kd->vma->vm_start, NULL);
	kd->faults = 0;
	for (i = 0; i < kd->nr_pages; i++) {
		if (NULL == kd->pages[i])
			continue;
		if (0 == kd->mapped[i])
			continue;
		kd->mapped[i] = 0;
		off = i << PAGE_SHIFT;
		len = PAGE_SIZE;
		if (len > kd->fix->smem_len - off)
			len = kd->fix->smem_len - off;
		src = kmap(kd->pages[i]);
		memcpy(kd->info->screen_base + off, src, len);
		kunmap(kd->pages[i]);
	}
}

/* ------------------------------------------------------------------------- */
/* input driver                                                              */

void shmfb_kbd_input(struct shmfb_kerndata *kd, int key, int down)
{
	if (key >= KEY_MAX) {
		if (down)
			printk(KERN_INFO "%s: unknown key pressed [%d]\n",
			       __func__, key-KEY_MAX);
		return;
	}
	input_report_key(kd->kbd, key, down);
	input_sync(kd->kbd);
}

void shmfb_mouse_input(struct shmfb_kerndata *kd, int key, int down,
		     int x, int y)
{
	if (key != KEY_RESERVED)
		input_report_key(kd->mouse, key, down);
	input_report_abs(kd->mouse, ABS_X, x);
	input_report_abs(kd->mouse, ABS_Y, y);
	input_sync(kd->mouse);
}

/* ------------------------------------------------------------------------- */
/* framebuffer driver                                                        */

static int shmfb_setcolreg(unsigned regno, unsigned red, unsigned green,
			 unsigned blue, unsigned transp,
			 struct fb_info *info)
{
	if (regno >= info->cmap.len)
		return 1;

	switch (info->var.bits_per_pixel) {
	case 16:
		if (info->var.red.offset == 10) {
			/* 1:5:5:5 */
			((u32 *) (info->pseudo_palette))[regno] =
				((red   & 0xf800) >> 1) |
				((green & 0xf800) >> 6) |
				((blue  & 0xf800) >> 11);
		} else {
			/* 0:5:6:5 */
			((u32 *) (info->pseudo_palette))[regno] =
				((red   & 0xf800)) |
				((green & 0xfc00) >> 5) |
				((blue  & 0xf800) >> 11);
		}
		break;
	case 24:
		red   >>= 8;
		green >>= 8;
		blue  >>= 8;
		((u32 *)(info->pseudo_palette))[regno] =
			(red   << info->var.red.offset)   |
			(green << info->var.green.offset) |
			(blue  << info->var.blue.offset);
		break;
	case 32:
		red   >>= 8;
		green >>= 8;
		blue  >>= 8;
		((u32 *)(info->pseudo_palette))[regno] =
			(red   << info->var.red.offset)   |
			(green << info->var.green.offset) |
			(blue  << info->var.blue.offset);
		break;
	}
	return 0;
}

static void shmfb_fb_refresh(struct shmfb_kerndata *kd,
			   int x1, int y1, int w, int h)
{
	int x2, y2;

	x2 = x1 + w;
	y2 = y1 + h;
	if (0 == kd->x2 || 0 == kd->y2) {
		kd->x1 = x1;
		kd->x2 = x2;
		kd->y1 = y1;
		kd->y2 = y2;
	}
	if (kd->x1 > x1)
		kd->x1 = x1;
	if (kd->x2 < x2)
		kd->x2 = x2;
	if (kd->y1 > y1)
		kd->y1 = y1;
	if (kd->y2 < y2)
		kd->y2 = y2;
}

void shmfb_fillrect(struct fb_info *p, const struct fb_fillrect *rect)
{
	struct shmfb_kerndata *kd = p->par;

	cfb_fillrect(p, rect);
	shmfb_fb_refresh(kd, rect->dx, rect->dy, rect->width, rect->height);
}

void shmfb_imageblit(struct fb_info *p, const struct fb_image *image)
{
	struct shmfb_kerndata *kd = p->par;

	cfb_imageblit(p, image);
	shmfb_fb_refresh(kd, image->dx, image->dy, image->width, image->height);
}

void shmfb_copyarea(struct fb_info *p, const struct fb_copyarea *area)
{
	struct shmfb_kerndata *kd = p->par;

	cfb_copyarea(p, area);
	shmfb_fb_refresh(kd, area->dx, area->dy, area->width, area->height);
}

static int shmfb_blank(int blank, struct fb_info *p)
{
	return 0;
}

/* ------------------------------------------------------------------------- */

static void
shmfb_fb_vm_open(struct vm_area_struct *vma)
{
	struct shmfb_kerndata *kd = vma->vm_private_data;

	atomic_inc(&kd->map_refs);
}

static void
shmfb_fb_vm_close(struct vm_area_struct *vma)
{
	struct shmfb_kerndata *kd = vma->vm_private_data;
	int i;

	if (!atomic_dec_and_test(&kd->map_refs))
		return;
	down(&kd->mm_lock);
	for (i = 0; i < kd->nr_pages; i++) {
		if (NULL == kd->pages[i])
			continue;
		put_page(kd->pages[i]);
	}
	kfree(kd->pages);
	kfree(kd->mapped);
	kd->pages    = NULL;
	kd->mapped   = NULL;
	kd->vma      = NULL;
	kd->nr_pages = 0;
	kd->faults   = 0;
	up(&kd->mm_lock);
}

static int shmfb_fb_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct shmfb_kerndata *kd = vma->vm_private_data;
	int pgnr = vmf->pgoff;
	int y1, y2;

	if (pgnr >= kd->nr_pages)
		return VM_FAULT_SIGBUS;

	down(&kd->mm_lock);
	if (NULL == kd->pages[pgnr]) {
		unsigned long vaddr = vma->vm_start + (vmf->pgoff<<PAGE_SHIFT);
		struct page *page;
		page = alloc_page_vma(GFP_HIGHUSER, vma, vaddr);
		if (!page)
			return VM_FAULT_SIGBUS;
		clear_user_highpage(page, vaddr);
		kd->pages[pgnr] = page;
	}
	get_page(kd->pages[pgnr]);
	kd->mapped[pgnr] = 1;
	kd->faults++;
	up(&kd->mm_lock);

	y1 = pgnr * PAGE_SIZE / kd->fix->line_length;
	y2 = (pgnr * PAGE_SIZE + PAGE_SIZE-1) / kd->fix->line_length;
	if (y2 > kd->var->yres)
		y2 = kd->var->yres;
	shmfb_fb_refresh(kd, 0, y1, kd->var->xres, y2 - y1);

	vmf->page = kd->pages[pgnr];
	return 0;
}

static struct vm_operations_struct shmfb_fb_vm_ops = {
	.open     = shmfb_fb_vm_open,
	.close    = shmfb_fb_vm_close,
	.fault    = shmfb_fb_vm_fault,
};

int shmfb_mmap(struct fb_info *p, struct vm_area_struct *vma)
{
	struct shmfb_kerndata *kd = p->par;
	int retval;
	int fb_pages;
	int map_pages;

	down(&kd->mm_lock);

	retval = -EBUSY;
	if (kd->vma) {
		printk(KERN_ERR "%s: busy, mapping exists\n", __func__);
		goto out;
	}

	retval = -EINVAL;
	if (!(vma->vm_flags & VM_WRITE)) {
		printk(KERN_ERR "%s: need writable mapping\n", __func__);
		goto out;
	}
	if (!(vma->vm_flags & VM_SHARED)) {
		printk(KERN_ERR "%s: need shared mapping\n", __func__);
		goto out;
	}
	if (vma->vm_pgoff != 0) {
		printk(KERN_ERR
			"%s: need offset 0 (vm_pgoff=%ld)\n", __func__,
		       vma->vm_pgoff);
		goto out;
	}

	fb_pages  = (p->fix.smem_len             + PAGE_SIZE-1) >> PAGE_SHIFT;
	map_pages = (vma->vm_end - vma->vm_start + PAGE_SIZE-1) >> PAGE_SHIFT;
	if (map_pages > fb_pages) {
		printk(KERN_ERR "%s: mapping too big (%ld>%d)\n", __func__,
		       vma->vm_end - vma->vm_start, p->fix.smem_len);
		goto out;
	}

	retval = -ENOMEM;
	kd->pages = kcalloc(map_pages, sizeof(struct page *), GFP_KERNEL);
	if (NULL == kd->pages)
		goto out;
	kd->mapped = kcalloc(map_pages, sizeof(int), GFP_KERNEL);
	if (NULL == kd->mapped) {
		kfree(kd->pages);
		goto out;
	}
	kd->vma = vma;
	kd->nr_pages = map_pages;
	atomic_set(&kd->map_refs, 1);
	kd->faults = 0;

	vma->vm_ops   = &shmfb_fb_vm_ops;
	vma->vm_flags |= VM_DONTEXPAND | VM_RESERVED;
	vma->vm_flags &= ~VM_IO; /* using shared anonymous pages */
	vma->vm_private_data = kd;
	retval = 0;

out:
	up(&kd->mm_lock);
	return retval;
}

/* ------------------------------------------------------------------------- */

static struct fb_ops shmfb_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= shmfb_setcolreg,
	.fb_fillrect	= shmfb_fillrect,
	.fb_copyarea	= shmfb_copyarea,
	.fb_imageblit	= shmfb_imageblit,
	.fb_blank       = shmfb_blank,
	.fb_cursor	= soft_cursor,
	.fb_mmap        = shmfb_mmap,
};

/* ------------------------------------------------------------------------- */

void shmfb_process_request(struct shmfb_request *req)
{
	if (req->request.type == SHMFB_MOTION_EVENT) {
		shmfb_mouse_input(req->kd, KEY_RESERVED, 0, req->request.x,
			req->request.y);
	} else if (req->request.type == SHMFB_BUTTON_EVENT) {
		shmfb_mouse_input(req->kd, req->request.key, req->request.down,
				req->request.x, req->request.y);
	} else if (req->request.type == SHMFB_KEY_EVENT) {
		shmfb_kbd_input(req->kd, req->request.key, req->request.down);
	}

	down(&req->kd->mm_lock);
	if (req->kd->faults > 0)
		shmfb_mmap_update(req->kd);
	up(&req->kd->mm_lock);

	shmfb_reply(req, SHMFB_UPDATE_EVENT, req->kd->x1,
			req->kd->y1, req->kd->x2, req->kd->y2);

	req->kd->x1 = req->kd->x2 = req->kd->y1 = req->kd->y2 = 0;
}

static LIST_HEAD(shmfb_requests);

static void shmfb_work_proc(struct work_struct *unused)
{
	struct shmfb_entry *req;
	unsigned long flags;

	while (!list_empty(&shmfb_requests)) {
		local_irq_save(flags);
		req = list_entry(shmfb_requests.next, struct shmfb_entry, list);
		list_del(&req->list);
		local_irq_restore(flags);
		shmfb_process_request(&req->request);
		kfree(req);
	}
}

static DECLARE_WORK(shmfb_work, shmfb_work_proc);

static irqreturn_t shmfb_interrupt(int irq, void *data)
{
	struct shmfb_kerndata *kd = (struct shmfb_kerndata *)data;
	struct shmfb_entry *new;
	struct shmfb_request req;

	while (shmfb_get_request(kd->sock, &req)) {
		req.kd = kd;
		new = kmalloc(sizeof(*new), GFP_NOWAIT);
		if (new == NULL)
			shmfb_reply(&req, SHMFB_ERROR_EVENT, 0, 0, 0, 0);
		else {
			new->request = req;
			list_add(&new->list, &shmfb_requests);
		}
	}
	if (!list_empty(&shmfb_requests))
		schedule_work(&shmfb_work);
	reactivate_fd(kd->sock, SHMFB_IRQ);
	return IRQ_HANDLED;
}

static int __init shmfb_probe(struct shmfb_kerndata **data)
{
	struct shmfb_kerndata *kd;
	int i, err;

	*data = NULL;
	if (!shmfb_enable)
		return -ENODEV;

	kd = kzalloc(sizeof(*kd), GFP_KERNEL);
	if (NULL == kd)
		return -ENOMEM;

	/* keyboard setup */
	kd->kbd = input_allocate_device();
	set_bit(EV_KEY, kd->kbd->evbit);
	for (i = 0; i < KEY_MAX; i++)
		set_bit(i, kd->kbd->keybit);
	kd->kbd->id.bustype = BUS_HOST;
	kd->kbd->name = "virtual keyboard";
	kd->kbd->phys = "shmfb/input0";
	err = input_register_device(kd->kbd);
	if (err) {
		input_free_device(kd->kbd);
		goto fail_free;
	}

	/* mouse setup */
	kd->mouse = input_allocate_device();
	set_bit(EV_ABS,     kd->mouse->evbit);
	set_bit(EV_KEY,     kd->mouse->evbit);
	set_bit(BTN_TOUCH,  kd->mouse->keybit);
	set_bit(BTN_LEFT,   kd->mouse->keybit);
	set_bit(BTN_MIDDLE, kd->mouse->keybit);
	set_bit(BTN_RIGHT,  kd->mouse->keybit);
	set_bit(ABS_X,      kd->mouse->absbit);
	set_bit(ABS_Y,      kd->mouse->absbit);
	input_set_abs_params(kd->mouse, ABS_X, 0, shmfb_width, 0, 0);
	input_set_abs_params(kd->mouse, ABS_Y, 0, shmfb_height, 0, 0);
	kd->mouse->id.bustype = BUS_HOST;
	kd->mouse->name = "virtual mouse";
	kd->mouse->phys = "shmfb/input1";
	err = input_register_device(kd->mouse);
	if (err) {
		input_free_device(kd->mouse);
		goto fail_unregister_kbd;
	}

	kd->win = shmfb_open(shmfb_width, shmfb_height, shmfb_depth);
	if (NULL == kd->win) {
		printk(KERN_ERR "fb: can't open shared memory framebuffer\n");
		goto fail_unregister_all;
	}
	kd->fix = shmfb_get_fix(kd->win);
	kd->var = shmfb_get_var(kd->win);

	/* framebuffer setup */
	kd->info = framebuffer_alloc(sizeof(u32) * 256, NULL);
	kd->info->pseudo_palette = kd->info->par;
	kd->info->par = kd;
	kd->info->screen_base = shmfb_get_fbmem(kd->win);

	kd->info->fbops = &shmfb_fb_ops;
	kd->info->var = *kd->var;
	kd->info->fix = *kd->fix;
	kd->info->flags = FBINFO_FLAG_DEFAULT;

	fb_alloc_cmap(&kd->info->cmap, 256, 0);
	err = register_framebuffer(kd->info);
	if (err)
		goto fail_unregister_all;

	printk(KERN_INFO "fb%d: %s frame buffer device, "
		"%dx%d, %d bpp (%d:%d:%d)\n",
		kd->info->node, kd->info->fix.id,
		kd->var->xres, kd->var->yres,
		kd->var->bits_per_pixel, kd->var->red.length,
		kd->var->green.length, kd->var->blue.length);

	/* misc common kernel stuff */
	init_MUTEX(&kd->mm_lock);

	*data = kd;

	return 0;

fail_unregister_all:
	input_unregister_device(kd->mouse);
fail_unregister_kbd:
	input_unregister_device(kd->kbd);
fail_free:
	kfree(kd);
	return -ENODEV;
}

static int __init shmfb_init(void)
{
	struct shmfb_kerndata *kd;
	int sock;
	int err;
	char file[256];

	err = shmfb_probe(&kd);
	if (err)
		return err;

	if (umid_file_name("shmfb", file, sizeof(file)))
		return -1;
	snprintf(shmfb_socket_name, sizeof(file), "%s", file);

	sock = os_create_unix_socket(file, sizeof(file), 1);
	if (sock < 0) {
		printk(KERN_ERR "Failed to initialize shmfb socket\n");
		return 1;
	}
	if (os_set_fd_block(sock, 0))
		goto out;

	register_reboot_notifier(&reboot_notifier);

	kd->sock = sock;
	err = um_request_irq(SHMFB_IRQ, sock, IRQ_READ, shmfb_interrupt,
			     IRQF_DISABLED | IRQF_SHARED | IRQF_SAMPLE_RANDOM,
			     "shmfb", (void *)kd);
	if (err) {
		printk(KERN_ERR "Failed to get IRQ for shmfb io\n");
		goto out;
	}

	printk(KERN_INFO "shmfb io initialized on %s\n", shmfb_socket_name);

	if (shmfb_client) {
		int argc;
		char **argv, **argv2;

		argv = argv_split(GFP_KERNEL, shmfb_client, &argc);
		if (argv == NULL)
			goto done;

		argc += 2;
		argv2 = krealloc(argv, sizeof(argv[0])*argc, GFP_KERNEL);
		if (argv2 == NULL)
			goto fail;

		argv = argv2;
		argv[argc-2] = kstrdup("--umid", GFP_KERNEL);
		argv[argc-1] = kstrdup(get_umid(), GFP_KERNEL);
		argv[argc] = NULL;

		shmfb_client_open(argv);
fail:
		argv_free(argv);
	}

done:
	return 0;

out:
	os_close_file(sock);
	return 1;
}

static void __exit shmfb_fini(void)
{
	/* FIXME */
}

module_init(shmfb_init);
module_exit(shmfb_fini);

static int shmfb_setup(char *str)
{
	if (3 == sscanf(str, "%dx%dx%d", &shmfb_width, &shmfb_height,
				&shmfb_depth)) {
		shmfb_enable = 1;
		return 0;
	}
	return -1;
}
__setup("shmfb=", shmfb_setup);

static int shmfb_client_setup(char *str)
{
	shmfb_client = str;
	return 0;
}
__setup("shmfb_client=", shmfb_client_setup);
diff -up /dev/null linux-2.6.29/arch/um/drivers/shmfb_user.c
--- /dev/null	2009-03-28 13:14:03.106015999 +0000
++ linux-2.6.29/arch/um/drivers/shmfb_user.c	2009-03-28 13:26:09.000000000 +0000
@@ -0,0 +1,207 @@
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <linux/fb.h>
#include <linux/input.h>

#include "init.h"
#include "kern_constants.h"
#include "os.h"
#include "user.h"
#include "shmfb_user.h"

/* ------------------------------------------------------------------------- */

struct shmfb_window {
	/* shared memory */
	char *buffer;

	/* framebuffer -- kernel */
	struct fb_fix_screeninfo  fix;
	struct fb_var_screeninfo  var;
};

char shmfb_socket_name[256];

int shmfb_unlink_socket(void)
{
	unlink(shmfb_socket_name);
	return 0;
}

/* ------------------------------------------------------------------------- */

static void __init create_shmid_file(int width, int height, int depth,
						key_t shmid)
{
	char file[256];
	char buffer[50];
	int fd, n;

	if (umid_file_name("shmid", file, sizeof(file)))
		return;

	fd = open(file, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		printk(UM_KERN_ERR "Open of shmid file \"%s\" failed: %s\n",
			file, strerror(errno));
		return;
	}

	snprintf(buffer, sizeof(buffer), "%d %d %d %d\n", shmid, width,
			height, depth);
	n = write(fd, buffer, strlen(buffer));
	if (n != strlen(buffer))
		printk(UM_KERN_ERR "Write of shmid file failed: %s\n",
			strerror(errno));

	close(fd);
}

struct shmfb_window * __init shmfb_open(int width, int height, int depth)
{
	struct shmfb_window *win;
	key_t shmid;

	win = malloc(sizeof(*win));
	if (NULL == win)
		goto fail;

	shmid = shmget(IPC_PRIVATE, width*height*depth, IPC_CREAT|0777);
	if (-1 == shmid)
		goto fail_free;

	win->buffer = (char *)shmat(shmid, NULL, 0);
	shmctl(shmid, IPC_RMID, 0);
	if (NULL == win->buffer)
		goto fail_free;

	/* fill structs */
	win->var.xres           = width;
	win->var.xres_virtual   = width;
	win->var.yres           = height;
	win->var.yres_virtual   = height;
	win->var.xoffset        = 0;
	win->var.yoffset        = 0;
	win->var.grayscale      = 0;
	win->var.nonstd         = 0;
	win->var.bits_per_pixel = depth * 8;
	win->var.pixclock       = 10000000 / win->var.xres *
					1000 / win->var.yres;
	win->var.left_margin    = (win->var.xres / 8) & 0xf8;
	win->var.hsync_len      = (win->var.xres / 8) & 0xf8;

	if (depth == 4) {
		win->var.red.offset = 16;
		win->var.red.length = 8;
		win->var.green.offset = 8;
		win->var.green.length = 8;
		win->var.blue.offset = 0;
		win->var.blue.length = 8;
		win->var.transp.offset = 0;
		win->var.transp.length = 0;
	} else {
		win->var.red.offset = 10;
		win->var.red.length = 5;
		win->var.green.offset = 5;
		win->var.green.length = 5;
		win->var.blue.offset = 0;
		win->var.blue.length = 5;
		win->var.transp.offset = 0;
		win->var.transp.length = 0;
	}

	win->var.activate       = FB_ACTIVATE_NOW;
	win->var.height         = -1;
	win->var.width          = -1;
	win->var.right_margin   = 32;
	win->var.upper_margin   = 16;
	win->var.lower_margin   = 4;
	win->var.vsync_len      = 4;
	win->var.vmode          = FB_VMODE_NONINTERLACED;
	win->var.rotate         = 0;

	win->fix.visual         = FB_VISUAL_TRUECOLOR;
	win->fix.line_length    = width * depth;
	win->fix.smem_start     = 0;
	win->fix.smem_len       = win->fix.line_length * win->var.yres;
	win->fix.xpanstep       = 0;
	win->fix.ypanstep       = 0;
	win->fix.ywrapstep      = 0;

	strcpy(win->fix.id, "shmfb");
	win->fix.type		= FB_TYPE_PACKED_PIXELS;
	win->fix.accel		= FB_ACCEL_NONE;

	create_shmid_file(width, height, depth, shmid);

	return win;

fail_free:
	free(win);
fail:
	return NULL;
}

struct fb_fix_screeninfo *shmfb_get_fix(struct shmfb_window *win)
{
	return &win->fix;
}

struct fb_var_screeninfo *shmfb_get_var(struct shmfb_window *win)
{
	return &win->var;
}

void *shmfb_get_fbmem(struct shmfb_window *win)
{
	return win->buffer;
}

int shmfb_get_request(int fd, struct shmfb_request *req)
{
	req->originlen = sizeof(req->origin);
	req->len = recvfrom(fd, &req->request, sizeof(req->request), 0,
			    (struct sockaddr *) req->origin, &req->originlen);
	if (req->len < 0)
		return 0;

	req->originating_fd = fd;

	return 1;
}

int shmfb_reply(struct shmfb_request *req, enum shmfb_event_code type,
		int x1, int y1, int x2, int y2)
{
	struct shmfb_io_reply reply;
	int n;

	reply.type = type;
	reply.x1 = x1;
	reply.y1 = y1;
	reply.x2 = x2;
	reply.y2 = y2;

	n = sendto(req->originating_fd, &reply, sizeof(reply), 0,
			  (struct sockaddr *) req->origin, req->originlen);
	if (n < 0)
		return -errno;

	return 0;
}

void shmfb_client_open(char **argv)
{
	run_helper(NULL, NULL, argv);
}
