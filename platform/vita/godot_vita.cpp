/**************************************************************************/
/*  godot_vita.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "main/main.h"
#include "os_vita.h"
#include "pack_preload_vita.h"

#include <psp2/io/fcntl.h>
#include <taihen.h>

#ifdef VITAGL
int _newlib_heap_size_user = 256 * 1024 * 1024;
#else
#ifndef MEMORY_GRAPHICS_MB
#define MEMORY_GRAPHICS_MB 256 // Default Split, 256 Graphics/221 Main
#endif
#define MEMORY_NEWLIB_MB (477 - MEMORY_GRAPHICS_MB)
#define MEMORY_SCELIBC_MB 10

//#define DEVKIT_ENABLED 1

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;
unsigned int sceLibcHeapSize = MEMORY_SCELIBC_MB * 1024 * 1024;
#endif

#ifndef VITA_OC_MHZ
#define VITA_OC_MHZ 444
#endif

// Optional plain-text tuning file that ships next to the game data. It lets
// the loading knobs be tweaked without rebuilding the eboot:
//
//   # app0:/game_data/vita_opt.txt
//   preload_limit_mb=128   # max .pck size to slurp into RAM (0 = disable)
//   arm_clock=444          # 333, 444 or 500 MHz
//
// Every key is optional and unknown keys are ignored.
#define VITA_BOOT_CONFIG "app0:/game_data/vita_opt.txt"

static int vita_arm_clock = VITA_OC_MHZ;

// Trims trailing whitespace in place. Leading whitespace is skipped by
// _skip_spaces().
static void _trim(char *p_str) {
	if (p_str == NULL) {
		return;
	}

	int len = (int)strlen(p_str);
	while (len > 0 && (p_str[len - 1] == '\r' || p_str[len - 1] == '\n' || p_str[len - 1] == ' ' || p_str[len - 1] == '\t')) {
		p_str[--len] = 0;
	}
}

static char *_skip_spaces(char *p_str) {
	while (p_str[0] == ' ' || p_str[0] == '\t') {
		p_str++;
	}
	return p_str;
}

static void _vita_apply_boot_config() {
	const SceUID fd = sceIoOpen(VITA_BOOT_CONFIG, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return; // No tuning file, keep the compiled-in defaults.
	}

	char buf[1024];
	const int r = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);

	if (r <= 0) {
		return;
	}
	buf[r] = 0;

	char *p = buf;
	while (p != NULL && p[0] != 0) {
		char *eol = strchr(p, '\n');
		if (eol != NULL) {
			eol[0] = 0;
		}

		char *line = _skip_spaces(p);
		if (line[0] != '#' && line[0] != 0) {
			char *eq = strchr(line, '=');
			if (eq != NULL) {
				eq[0] = 0;
				char *key = line;
				char *value = _skip_spaces(eq + 1);
				_trim(key);
				_trim(value);

				if (strcmp(key, "preload_limit_mb") == 0) {
#ifdef VITA_PRELOAD_PACK
					const int mb = atoi(value);
					VitaPackPreload::set_limit((uint64_t)(mb > 0 ? mb : 0) * 1024 * 1024);
#endif
				} else if (strcmp(key, "arm_clock") == 0) {
					const int mhz = atoi(value);
					if (mhz == 333 || mhz == 444 || mhz == 500) {
						vita_arm_clock = mhz;
					}
				}
			}
		}

		if (eol == NULL) {
			break;
		}
		p = eol + 1;
	}
}

int main(int argc, char *argv[]) {
	OS_Vita os;
#ifndef VITAGL
	char title_id[0xA];
	char app_dir_path[0x100];
	char app_kernel_module_path[0x100];
	SceUID pid = -1;
	sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, NULL, 0, NULL, NULL);
	sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, NULL, 0, NULL, NULL);

	pid = sceKernelGetProcessId();
	sceAppMgrAppParamGetString(pid, 12, title_id, sizeof(title_id));
	snprintf(app_dir_path, sizeof(app_dir_path), "ux0:app/%s", title_id);
	snprintf(app_kernel_module_path, sizeof(app_kernel_module_path), "%s/module/libgpu_es4_kernel_ext.skprx", app_dir_path);

	SceUID res = taiLoadStartKernelModule(app_kernel_module_path, 0, NULL, 0);
	if (res < 0) {
		sceClibPrintf("Failed to load kernel module: %08x\n", res);
	}
#else
	sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
#endif

	_vita_apply_boot_config();

	// Overclock: the Vita boots at 333 MHz and the engine is completely CPU
	// bound while parsing scenes, inflating textures and compiling shaders.
	// 444 MHz is safe on every retail unit, 500 MHz mostly works on PSTV.
	scePowerSetArmClockFrequency(vita_arm_clock);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	char *args[] = { "--path", "app0:/game_data", "--main-pack", "app0:/game_data/game.pck" };

	const Error err = Main::setup("", sizeof(args) / sizeof(args[0]), args);
	if (err != OK) {
		return 255;
	}

	if (Main::start()) {
		os.run(); // it is actually the OS that decides how to run
	}
	Main::cleanup();
	return 0;
}
