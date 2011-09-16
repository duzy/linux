/* shmfb_kern */

struct shmfb_window;
struct shmfb_kerndata;

extern char shmfb_socket_name[];

void shmfb_kbd_input(struct shmfb_kerndata *kd, int key, int down);
void shmfb_mouse_input(struct shmfb_kerndata *kd, int key, int down,
			int x, int y);

struct shmfb_entry {
	struct list_head list;
	struct shmfb_request request;
};

