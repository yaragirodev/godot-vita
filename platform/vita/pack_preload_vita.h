/**************************************************************************/
/*  pack_preload_vita.h                                                    */
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

#ifndef PACK_PRELOAD_VITA_H
#define PACK_PRELOAD_VITA_H

#include "core/io/file_access_pack.h"

// Whole-pack preloading for the PS Vita.
//
// Reading a .pck from `app0:` means one sceIoOpen() + one sceIoLseek() + a
// handful of tiny sceIoRead()s *per resource*. A mid sized Godot game easily
// has a few thousand resources, and every one of those syscalls costs roughly
// a millisecond on the Vita's memory card. That - not the CPU - is what makes
// games take minutes to boot.
//
// The fix is to slurp the entire .pck into RAM in one sequential pass (which
// the Vita does fast, it is optimised for large linear reads) and then serve
// every resource out of that buffer with memcpy(). Sycall count during loading
// drops to zero.
namespace VitaPackPreload {

// Tries to load `p_path` completely into RAM. Returns false (and leaves
// `r_pack` untouched) when preloading is disabled, when the file is too big
// for the configured limit, or when there is not enough memory - the engine
// then transparently falls back to buffered file access.
bool preload(const String &p_path, MemoryPack *r_pack);

// Hard cap in bytes. 0 disables preloading entirely.
void set_limit(uint64_t p_limit);
uint64_t get_limit();

// Bytes currently held by preloaded packs (useful for diagnostics).
uint64_t get_loaded_bytes();

} // namespace VitaPackPreload

#endif // PACK_PRELOAD_VITA_H
