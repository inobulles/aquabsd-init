
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/fbio.h>
#include <sys/kbio.h>
#include <sys/consio.h>
#include <sys/param.h>
#include <sys/linker.h>

#include "logo.h"

#define flag int

flag on_start;

int start(void) {
	// make sure the VESA kernel module is loaded

	if (kldload("vesa") < 0) {
		if (errno == EEXIST) {
			warnx("can't load vesa: module already loaded or in kernel");
		}

		else if (errno == ENOEXEC) {
			warnx("an error occurred while loading vesa kernel module; please check dmesg(8) for details");
		}

		else {
			warnx("can't load vesa kernel module");
		}
	}

	// find highest resolution appropriate mode (cf. 'usr.sbin/vidcontrol/vidcontrol.c:show_mode_info')

	int mode = -1;

	uint32_t x_res = 0;
	uint32_t y_res = 0;

	for (size_t i = 0; i <= M_VESA_MODE_MAX; i++) {
		video_info_t info = { .vi_mode = i };

		if (ioctl(0, CONS_MODEINFO, &info) || \
			info.vi_mode != i || \
			!info.vi_width || !info.vi_height || !info.vi_cwidth || !info.vi_cheight // TODO this comparison is incorrect in the aquabsd_vga backend in the aquabsd.alps.vga device
		) {
			continue;
		}

		// found video mode, check if it's the one we want

		if (!(info.vi_flags & V_INFO_GRAPHICS)) {
			continue;
		}

		if (info.vi_depth != 32) {
			continue;
		}

		if (info.vi_height < y_res) {
			continue;
		}

		if (info.vi_height == y_res && info.vi_width < x_res) {
			continue;
		}

		mode = i;

		x_res = info.vi_width;
		y_res = info.vi_height;
	}

	if (mode < -1) {
		warnx("can't find appropriate mode");
		return -1;
	}

	// switch to that video mode

	video_info_t info = { .vi_mode = mode };
	ioctl(0, CONS_MODEINFO, &info);

	int prev_mode;
	ioctl(0, CONS_GET, &prev_mode);

	ioctl(0, KDENABIO, 0);
	ioctl(0, VT_WAITACTIVE, 0);

	ioctl(0, KDSETMODE, KD_GRAPHICS);
	ioctl(0, _IO('V', info.vi_mode - M_VESA_BASE), 0);

	video_adapter_info_t adapter_info = { 0 };
	ioctl(0, CONS_ADPINFO, &adapter_info);

	ioctl(0, CONS_SETWINORG, 0);

	uint8_t* fb = mmap(NULL, adapter_info.va_window_size, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, 0, 0);

	if (fb == MAP_FAILED) {
		warnx("framebuffer memory map failed: %s\n", strerror(errno));
		return -1;
	}

	// clear screen

	memset(fb, 0, x_res * y_res * 4);

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
