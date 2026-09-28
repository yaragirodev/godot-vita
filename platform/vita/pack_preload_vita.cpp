/**************************************************************************/
/*  pack_preload_vita.cpp                                                  */
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

#include "pack_preload_vita.h"

#ifdef VITA_ENABLED

#include "core/os/os.h"
#include "core/print_string.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>

#include <stdlib.h>

// Size of one sequential read while slurping the pack. The Vita is much faster
// at a few large reads than at many small ones.
#ifndef VITA_PRELOAD_CHUNK_KB
#define VITA_PRELOAD_CHUNK_KB 512
#endif

// Default cap, in MiB, when nothing else is configured.
#ifndef VITA_PRELOAD_LIMIT_MB
#define VITA_PRELOAD_LIMIT_MB 160
#endif

// Set by godot_vita.cpp; the size of the newlib heap the engine runs in.
extern int _newlib_heap_size_user;

namespace VitaPackPreload {

static uint64_t limit = 0;
static bool limit_configured = false;
static uint64_t loaded_bytes = 0;

static uint64_t _default_limit() {
	// Never take more than 60% of the heap the engine has to live in.
	uint64_t heap = (_newlib_heap_size_user > 0) ? (uint64_t)_newlib_heap_size_user : (uint64_t)192 * 1024 * 1024;
	uint64_t by_heap = heap * 3 / 5;
	uint64_t by_option = (uint64_t)VITA_PRELOAD_LIMIT_MB * 1024 * 1024;
	return MIN(by_heap, by_option);
}

uint64_t get_limit() {
	if (!limit_configured) {
		limit = _default_limit();
		limit_configured = true;
	}
	return limit;
}

void set_limit(uint64_t p_limit) {
	limit = p_limit;
	limit_configured = true;
}

uint64_t get_loaded_bytes() {
	return loaded_bytes;
}

static void _vita_free(void *p_ptr) {
	::free(p_ptr);
}

bool preload(const String &p_path, MemoryPack *r_pack) {
	const uint64_t max_bytes = get_limit();
	if (max_bytes == 0) {
		return false;
	}

	const CharString cpath = p_path.utf8();

	SceIoStat st;
	sceClibMemset(&st, 0, sizeof(st));
	if (sceIoGetstat(cpath.get_data(), &st) < 0) {
		return false;
	}

	const uint64_t size = (uint64_t)st.st_size;
	if (size == 0 || size > max_bytes) {
		print_verbose("Vita: not preloading '" + p_path + "' (" + String::num_int64(size / 1024) +
				" KiB, limit " + String::num_int64(max_bytes / 1024) + " KiB), using buffered file access.");
		return false;
	}

	uint8_t *buf = (uint8_t *)::malloc((size_t)size);
	if (buf == nullptr) {
		print_verbose("Vita: not enough memory to preload '" + p_path + "', using buffered file access.");
		return false;
	}

	const SceUID fd = sceIoOpen(cpath.get_data(), SCE_O_RDONLY, 0);
	if (fd < 0) {
		::free(buf);
		return false;
	}

	const uint64_t started = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;

	const size_t chunk = (size_t)VITA_PRELOAD_CHUNK_KB * 1024;
	uint64_t total = 0;
	while (total < size) {
		size_t to_read = (size_t)(size - total);
		if (to_read > chunk) {
			to_read = chunk;
		}
		const int r = sceIoRead(fd, buf + total, (SceSize)to_read);
		if (r <= 0) {
			break;
		}
		total += (uint64_t)r;
	}

	sceIoClose(fd);

	if (total != size) {
		::free(buf);
		print_verbose("Vita: short read while preloading '" + p_path + "', using buffered file access.");
		return false;
	}

	const uint64_t elapsed = OS::get_singleton() ? (OS::get_singleton()->get_ticks_usec() - started) : 0;

	r_pack->data = buf;
	r_pack->size = size;
	r_pack->free_func = _vita_free;
	loaded_bytes += size;

	if (OS::get_singleton()) {
		print_verbose("Vita: preloaded '" + p_path + "' (" + String::num_int64(size / 1024) + " KiB) into RAM in " +
				String::num_int64(elapsed / 1000) + " ms.");
	}

	return true;
}

} // namespace VitaPackPreload

#endif // VITA_ENABLED
