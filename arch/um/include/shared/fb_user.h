/* shmfb_user */

#ifndef __KERNEL__
#include <stdint.h>
#define u32 uint32_t
#endif

#include "sysdep/ptrace.h"

struct shmfb_window;
struct shmfb_kerndata;

struct shmfb_window *shmfb_open(int width, int height, int depth);
void *shmfb_get_fbmem(struct shmfb_window *win);

struct fb_fix_screeninfo *shmfb_get_fix(struct shmfb_window *win);
struct fb_var_screeninfo *shmfb_get_var(struct shmfb_window *win);

enum shmfb_event_code { SHMFB_ERROR_EVENT, SHMFB_BUTTON_EVENT,
	SHMFB_KEY_EVENT, SHMFB_MOTION_EVENT, SHMFB_UPDATE_EVENT };

struct shmfb_io_request {
	u32 type;
	u32 key;
	u32 down;
	u32 x;
	u32 y;
};

struct shmfb_io_reply {
	u32 type;
	u32 x1;
	u32 y1;
	u32 x2;
	u32 y2;
};

struct shmfb_request {
	int len;

	int originating_fd;
	unsigned int originlen;
	unsigned char origin[128];		/* sockaddr_un */

	struct shmfb_kerndata *kd;

	struct shmfb_io_request request;
};

int shmfb_unlink_socket(void);
int shmfb_get_request(int fd, struct shmfb_request *req);
int shmfb_reply(struct shmfb_request *req, enum shmfb_event_code type,
		int x1, int y1, int x2, int y2);
void shmfb_client_open(char **argv);
