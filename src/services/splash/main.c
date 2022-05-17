
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

	// make screen a solid colour for testing

	for (size_t i = 0; i < x_res * y_res * 4; i += 4) {
		fb[i + 0] = 0xFF;
		fb[i + 1] = 0x00;
		fb[i + 2] = 0xFF;
		fb[i + 3] = 0x00;
	}

	return 0;
}

size_t get_deps_len(void) {
	return 0;
}

char* get_dep_names(void) {
	return NULL;
}