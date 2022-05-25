
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/fbio.h>
#include <sys/kbio.h>
#include <sys/consio.h>
#include <sys/param.h>
#include <sys/linker.h>

#include "logo.h"

#define flag int

flag on_start;

#define VT_DEV_PREFIX "/dev/ttyv"

int start(void) {
	// find an appropriate virtual terminal
	// usually, you'd want to use VT_OPENQRY to find an available virtual terminal to switch to, but that seems to be acting a little weird (see tests/vt-test)

	int default_fd = open(VT_DEV_PREFIX"0", O_RDWR | O_NDELAY, 0);

	if (default_fd < 0) {
		warnx("open: %s", strerror(errno));
		return -1;
	}

	int vt;

	if (ioctl(default_fd, VT_GETACTIVE, &vt) < 0) {
		warnx("VT_GETACTIVE: %s", strerror(errno));
		return -1;
	}

	close(default_fd);

	// found virtual terminal, activate it

	char* vt_path;
	asprintf(&vt_path, VT_DEV_PREFIX"%d", vt);

	int fd = open(vt_path, O_RDWR | O_NDELAY, 0);
	free(vt_path);

	if (fd < 0) {
		warnx("open: %s", strerror(errno));
		return -1;
	}

	if (ioctl(fd, VT_ACTIVATE, vt) < 0) {
		warnx("VT_ACTIVATE: %s", strerror(errno));
		return -1;
	}

	if (ioctl(fd, VT_WAITACTIVE, vt) < 0) {
		warnx("VT_WAITACTIVE: %s", strerror(errno));
		return -1;
	}

	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0) {
		warnx("KDSETMODE: %s", strerror(errno));
		return -1;
	}

	// get window size

	uint32_t x_res = 1280;
	uint32_t y_res = 1024;

	size_t win_size = x_res * y_res * 4;

	// map framebuffer memory

	void* fb_start;

	if (ioctl(fd, FBIO_GETDISPSTART, &fb_start) < 0) {
		warnx("FBIO_GETDISPSTART: %s", strerror(errno));
		return -1;
	}

	uint8_t* fb = mmap(fb_start, win_size, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, 0, 0);

	if (fb == MAP_FAILED) {
		warnx("framebuffer memory map failed: %s\n", strerror(errno));
		return -1;
	}

	// clear screen

	memset(fb, 0, win_size);

	// draw logo to screen

	uint32_t dst_pitch = x_res * 4;
	uint32_t src_pitch = gimp_image.width * 4;

	uint32_t start_x = x_res / 2 - gimp_image.width  / 2;
	uint32_t start_y = y_res / 2 - gimp_image.height / 2;

	for (size_t i = 0; i < gimp_image.height; i++) {
		uint8_t* dst_line = fb + dst_pitch * (i + start_y);
		uint8_t* src_line = (uint8_t*) gimp_image.pixel_data + src_pitch * i;

		memcpy(dst_line + start_x * 4, src_line, src_pitch);
	}

	return 0;
}

size_t get_deps_len(void) {
	return 0;
}

char* get_dep_names(void) {
	return NULL;
}
