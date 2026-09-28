/**************************************************************************/
/*  file_access_pack.cpp                                                  */
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

#include "file_access_pack.h"

#include "core/io/file_access_memory.h"
#include "core/map.h"
#include "core/os/os.h"
#include "core/version.h"

#include <stdio.h>
#include <string.h>

Error PackedData::add_pack(const String &p_path, bool p_replace_files, uint64_t p_offset) {
	for (int i = 0; i < sources.size(); i++) {
		if (sources[i]->try_open_pack(p_path, p_replace_files, p_offset)) {
			return OK;
		};
	};

	return ERR_FILE_UNRECOGNIZED;
};

void PackedData::add_path(const String &p_pkg_path, const String &p_path, uint64_t p_ofs, uint64_t p_size, const uint8_t *p_md5, PackSource *p_src, bool p_replace_files) {
	PathMD5 pmd5(p_path.md5_buffer());

	bool exists = files.has(pmd5);

	PackedFile pf;
	pf.pack = p_pkg_path;
	pf.offset = p_ofs;
	pf.size = p_size;
	for (int i = 0; i < 16; i++) {
		pf.md5[i] = p_md5[i];
	}
	pf.src = p_src;

	if (!exists || p_replace_files) {
		files[pmd5] = pf;
	}

	if (!exists) {
		//search for dir
		String p = p_path.replace_first("res://", "");
		PackedDir *cd = root;

		if (p.find("/") != -1) { //in a subdir

			Vector<String> ds = p.get_base_dir().split("/");

			for (int j = 0; j < ds.size(); j++) {
				if (!cd->subdirs.has(ds[j])) {
					PackedDir *pd = memnew(PackedDir);
					pd->name = ds[j];
					pd->parent = cd;
					cd->subdirs[pd->name] = pd;
					cd = pd;
				} else {
					cd = cd->subdirs[ds[j]];
				}
			}
		}
		String filename = p_path.get_file();
		// Don't add as a file if the path points to a directory
		if (!filename.empty()) {
			cd->files.insert(filename);
		}
	}
}

void PackedData::add_pack_source(PackSource *p_source) {
	if (p_source != nullptr) {
		sources.push_back(p_source);
	}
};

PackedData *PackedData::singleton = nullptr;

PackedData::PackedData() {
	singleton = this;
	root = memnew(PackedDir);
	root->parent = nullptr;
	disabled = false;

	add_pack_source(memnew(PackedSourcePCK));
}

void PackedData::_free_packed_dirs(PackedDir *p_dir) {
	for (Map<String, PackedDir *>::Element *E = p_dir->subdirs.front(); E; E = E->next()) {
		_free_packed_dirs(E->get());
	}
	memdelete(p_dir);
}

PackedData::~PackedData() {
	clear_memory_packs();
	for (int i = 0; i < sources.size(); i++) {
		memdelete(sources[i]);
	}
	_free_packed_dirs(root);
}

//////////////////////////////////////////////////////////////////
// MEMORY BACKED PACKS
//
// Some consoles (notably the PS Vita) have storage that is fast for big
// sequential reads but painfully slow for the thousands of tiny seeks and
// 1-byte reads Godot performs while parsing scenes and resources. Slurping the
// whole .pck into RAM once and serving every resource out of that buffer
// removes virtually all I/O latency from the loading path.
//////////////////////////////////////////////////////////////////

PackedData::MemoryPackPreloadFunc PackedData::memory_pack_preload_func = nullptr;
uint64_t PackedData::memory_pack_preload_limit = 0;

static Map<String, MemoryPack> memory_packs;

bool PackedData::try_preload_memory_pack(const String &p_path) {
	if (memory_packs.has(p_path)) {
		return true;
	}
	if (!memory_pack_preload_func) {
		return false;
	}

	MemoryPack mp;
	if (!memory_pack_preload_func(p_path, &mp)) {
		return false;
	}
	if (mp.data == nullptr || mp.size == 0) {
		if (mp.data != nullptr && mp.free_func != nullptr) {
			mp.free_func((void *)mp.data);
		}
		return false;
	}

	memory_packs[p_path] = mp;
#ifdef DEBUG_ENABLED
	print_verbose("Pack preloaded into RAM: " + p_path + " (" + String::num_int64(mp.size / 1024) + " KiB)");
#endif
	return true;
}

static void _default_memory_pack_free(void *p_ptr) {
	memfree(p_ptr);
}

bool PackedData::default_memory_pack_preload(const String &p_path, MemoryPack *r_pack) {
	FileAccess *f = FileAccess::open(p_path, FileAccess::READ);
	if (!f) {
		return false;
	}

	const uint64_t size = f->get_len();
	if (size == 0 || (memory_pack_preload_limit != 0 && size > memory_pack_preload_limit)) {
		memdelete(f);
		return false;
	}

	void *buf = memalloc((size_t)size);
	if (buf == nullptr) {
		memdelete(f);
		return false;
	}

	const uint64_t read = f->get_buffer((uint8_t *)buf, size);
	memdelete(f);

	if (read != size) {
		memfree(buf);
		return false;
	}

	r_pack->data = (const uint8_t *)buf;
	r_pack->size = size;
	r_pack->free_func = _default_memory_pack_free;

	return true;
}

bool PackedData::has_memory_pack(const String &p_path) {
	return memory_packs.has(p_path);
}

bool PackedData::get_memory_pack(const String &p_path, const uint8_t **r_data, uint64_t *r_size) {
	Map<String, MemoryPack>::Element *E = memory_packs.find(p_path);
	if (!E) {
		return false;
	}
	*r_data = E->get().data;
	*r_size = E->get().size;
	return true;
}

void PackedData::clear_memory_packs() {
	for (Map<String, MemoryPack>::Element *E = memory_packs.front(); E; E = E->next()) {
		if (E->get().data != nullptr && E->get().free_func != nullptr) {
			E->get().free_func((void *)E->get().data);
		}
	}
	memory_packs.clear();
}

//////////////////////////////////////////////////////////////////
// FileAccessPackMem
//////////////////////////////////////////////////////////////////

// Decode helpers. Godot pack data is always little endian, `endian_swap` asks
// for the value to be interpreted as big endian instead (this mirrors
// FileAccess::get_16/get_32/get_64).
static _FORCE_INLINE_ uint16_t _decode_16(const uint8_t *p, bool p_swap) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	uint16_t v;
	memcpy(&v, p, sizeof(v));
#else
	uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
#endif
	return p_swap ? BSWAP16(v) : v;
}

static _FORCE_INLINE_ uint32_t _decode_32(const uint8_t *p, bool p_swap) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	uint32_t v;
	memcpy(&v, p, sizeof(v));
#else
	uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
#endif
	return p_swap ? BSWAP32(v) : v;
}

static _FORCE_INLINE_ uint64_t _decode_64(const uint8_t *p, bool p_swap) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	uint64_t v;
	memcpy(&v, p, sizeof(v));
#else
	uint64_t v = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
			((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
#endif
	return p_swap ? BSWAP64(v) : v;
}

FileAccessPackMem::FileAccessPackMem() {
	base = nullptr;
	offset = 0;
	length = 0;
	pos = 0;
	eof = false;
}

void FileAccessPackMem::open_custom(const uint8_t *p_base, uint64_t p_offset, uint64_t p_size) {
	base = p_base;
	offset = p_offset;
	length = p_size;
	pos = 0;
	eof = false;
}

Error FileAccessPackMem::_open(const String &p_path, int p_mode_flags) {
	return ERR_UNAVAILABLE;
}

void FileAccessPackMem::close() {
	base = nullptr;
}

bool FileAccessPackMem::is_open() const {
	return base != nullptr;
}

void FileAccessPackMem::seek(uint64_t p_position) {
	if (p_position > length) {
		eof = true;
	} else {
		eof = false;
	}
	pos = p_position;
}

void FileAccessPackMem::seek_end(int64_t p_position) {
	seek(length + p_position);
}

uint64_t FileAccessPackMem::get_position() const {
	return pos;
}

uint64_t FileAccessPackMem::get_len() const {
	return length;
}

bool FileAccessPackMem::eof_reached() const {
	return eof;
}

uint8_t FileAccessPackMem::get_8() const {
	if (pos >= length) {
		eof = true;
		return 0;
	}
	return base[offset + pos++];
}

uint16_t FileAccessPackMem::get_16() const {
	if (pos + 2 > length) {
		eof = true;
		return 0;
	}
	uint16_t v = _decode_16(base + offset + pos, endian_swap);
	pos += 2;
	return v;
}

uint32_t FileAccessPackMem::get_32() const {
	if (pos + 4 > length) {
		eof = true;
		return 0;
	}
	uint32_t v = _decode_32(base + offset + pos, endian_swap);
	pos += 4;
	return v;
}

uint64_t FileAccessPackMem::get_64() const {
	if (pos + 8 > length) {
		eof = true;
		return 0;
	}
	uint64_t v = _decode_64(base + offset + pos, endian_swap);
	pos += 8;
	return v;
}

uint64_t FileAccessPackMem::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V(!p_dst && p_length > 0, -1);

	if (eof) {
		return 0;
	}

	uint64_t left = (pos < length) ? (length - pos) : 0;
	if (p_length > left) {
		eof = true;
		p_length = left;
	}

	memcpy(p_dst, base + offset + pos, p_length);
	pos += p_length;

	return p_length;
}

String FileAccessPackMem::get_line() const {
	if (base == nullptr || pos >= length) {
		eof = true;
		return String();
	}

	const uint8_t *start = base + offset + pos;
	uint64_t left = length - pos;

	// FileAccess::get_line() ends the line on '\n' or on a nul byte.
	uint64_t term = left;
	const uint8_t *nl = (const uint8_t *)memchr(start, '\n', left);
	if (nl != nullptr) {
		term = (uint64_t)(nl - start);
	}
	const uint8_t *nul = (const uint8_t *)memchr(start, 0, term);
	if (nul != nullptr) {
		term = (uint64_t)(nul - start);
	}

	String ret;
	if (memchr(start, '\r', term) != nullptr) {
		// Rare path: FileAccess::get_line() silently drops every '\r'.
		Vector<char> buf;
		buf.resize(term + 1);
		char *w = buf.ptrw();
		int j = 0;
		for (uint64_t i = 0; i < term; i++) {
			if (start[i] != '\r') {
				w[j++] = (char)start[i];
			}
		}
		w[j] = 0;
		ret = String::utf8(w, j);
	} else {
		ret = String::utf8((const char *)start, (int)term);
	}

	pos += (term < left) ? (term + 1) : term;
	if (term == left) {
		eof = true;
	}

	return ret;
}

String FileAccessPackMem::get_as_utf8_string(bool p_skip_cr) const {
	if (base == nullptr || pos >= length) {
		eof = true;
		return String();
	}

	uint64_t left = length - pos;
	String ret;
	if (ret.parse_utf8((const char *)(base + offset + pos), (int)left, p_skip_cr)) {
		pos = length;
		eof = true;
		return String();
	}

	pos += left;
	eof = true;

	return ret;
}

Error FileAccessPackMem::get_error() const {
	return eof ? ERR_FILE_EOF : OK;
}

void FileAccessPackMem::flush() {
}

void FileAccessPackMem::store_8(uint8_t p_dest) {
	ERR_FAIL();
}

void FileAccessPackMem::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL();
}

bool FileAccessPackMem::file_exists(const String &p_name) {
	return false;
}

//////////////////////////////////////////////////////////////////

bool PackedSourcePCK::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset) {
	bool memory_mode = false;
	FileAccessMemory mem_fa;
	FileAccess *f = nullptr;

	// If the platform can slurp the whole pack into RAM, parse it from there.
	if (p_offset == 0 && PackedData::try_preload_memory_pack(p_path)) {
		const uint8_t *mem_data = nullptr;
		uint64_t mem_size = 0;
		if (PackedData::get_memory_pack(p_path, &mem_data, &mem_size)) {
			mem_fa.open_custom(mem_data, mem_size);
			f = &mem_fa;
			memory_mode = true;
		}
	}

	if (f == nullptr) {
		f = FileAccess::open(p_path, FileAccess::READ);
	}
	if (!f) {
		return false;
	}

	bool pck_header_found = false;

	// Search for the header at the start offset - standalone PCK file.
	f->seek(p_offset);

	uint32_t magic = f->get_32();
	if (magic == PACK_HEADER_MAGIC) {
		pck_header_found = true;
	}

	// Search for the header in the executable "pck" section - self contained executable.
	if (!pck_header_found && !memory_mode) {
		// Loading with offset feature not supported for self contained exe files.
		if (p_offset != 0) {
			f->close();
			if (!memory_mode) memdelete(f);
			ERR_FAIL_V_MSG(false, "Loading self-contained executable with offset not supported.");
		}

		int64_t pck_off = OS::get_singleton()->get_embedded_pck_offset();
		if (pck_off != 0) {
			// Search for the header, in case PCK start and section have different alignment.
			for (int i = 0; i < 8; i++) {
				f->seek(pck_off);
				magic = f->get_32();
				if (magic == PACK_HEADER_MAGIC) {
#ifdef DEBUG_ENABLED
					print_verbose("PCK header found in executable pck section, loading from offset 0x" + String::num_int64(pck_off - 4, 16));
#endif
					pck_header_found = true;
					break;
				}
				pck_off++;
			}
		}
	}

	// Search for the header at the end of file - self contained executable.
	if (!pck_header_found && !memory_mode) {
		// Loading with offset feature not supported for self contained exe files.
		if (p_offset != 0) {
			f->close();
			if (!memory_mode) memdelete(f);
			ERR_FAIL_V_MSG(false, "Loading self-contained executable with offset not supported.");
		}

		f->seek_end();
		f->seek(f->get_position() - 4);

		magic = f->get_32();
		if (magic == PACK_HEADER_MAGIC) {
			f->seek(f->get_position() - 12);
			uint64_t ds = f->get_64();
			f->seek(f->get_position() - ds - 8);
			magic = f->get_32();
			if (magic == PACK_HEADER_MAGIC) {
#ifdef DEBUG_ENABLED
				print_verbose("PCK header found at the end of executable, loading from offset 0x" + String::num_int64(f->get_position() - 4, 16));
#endif
				pck_header_found = true;
			}
		}
	}

	if (!pck_header_found) {
		f->close();
		if (!memory_mode) memdelete(f);
		return false;
	}

	uint32_t version = f->get_32();
	uint32_t ver_major = f->get_32();
	uint32_t ver_minor = f->get_32();
	f->get_32(); // patch number, not used for validation.

	if (version != PACK_FORMAT_VERSION) {
		f->close();
		if (!memory_mode) memdelete(f);
		ERR_FAIL_V_MSG(false, "Pack version unsupported: " + itos(version) + ".");
	}
	if (ver_major > VERSION_MAJOR || (ver_major == VERSION_MAJOR && ver_minor > VERSION_MINOR)) {
		f->close();
		if (!memory_mode) memdelete(f);
		ERR_FAIL_V_MSG(false, "Pack created with a newer version of the engine: " + itos(ver_major) + "." + itos(ver_minor) + ".");
	}

	for (int i = 0; i < 16; i++) {
		//reserved
		f->get_32();
	}

	int file_count = f->get_32();

	for (int i = 0; i < file_count; i++) {
		uint32_t sl = f->get_32();
		CharString cs;
		cs.resize(sl + 1);
		f->get_buffer((uint8_t *)cs.ptr(), sl);
		cs[sl] = 0;

		String path;
		path.parse_utf8(cs.ptr());

		uint64_t ofs = f->get_64();
		uint64_t size = f->get_64();
		uint8_t md5[16];
		f->get_buffer(md5, 16);
		PackedData::get_singleton()->add_path(p_path, path, ofs + p_offset, size, md5, this, p_replace_files);
	};

	f->close();
	if (!memory_mode) memdelete(f);
	return true;
};

FileAccess *PackedSourcePCK::get_file(const String &p_path, PackedData::PackedFile *p_file) {
	// NOTE: `p_path` is the *resource* path here; the pack file itself is
	// `p_file->pack` (that is also what FileAccessPack opens).
	if (PackedData::has_memory_pack(p_file->pack)) {
		const uint8_t *mem_data = nullptr;
		uint64_t mem_size = 0;
		if (PackedData::get_memory_pack(p_file->pack, &mem_data, &mem_size)) {
			// The file offsets stored in the pack are absolute offsets inside the
			// pack file, which is exactly our in-RAM blob for memory packs.
			if (p_file->offset + p_file->size <= mem_size) {
				FileAccessPackMem *fam = memnew(FileAccessPackMem);
				fam->open_custom(mem_data, p_file->offset, p_file->size);
				return fam;
			}
			// Out of bounds (corrupt pack): fall through to the regular reader.
		}
	}
	return memnew(FileAccessPack(p_path, *p_file));
};

//////////////////////////////////////////////////////////////////

Error FileAccessPack::_open(const String &p_path, int p_mode_flags) {
	ERR_FAIL_V(ERR_UNAVAILABLE);
	return ERR_UNAVAILABLE;
}

void FileAccessPack::close() {
	f->close();
}

bool FileAccessPack::is_open() const {
	return f->is_open();
}

void FileAccessPack::seek(uint64_t p_position) {
	if (p_position > pf.size) {
		eof = true;
	} else {
		eof = false;
	}

	f->seek(pf.offset + p_position);
	pos = p_position;
}

void FileAccessPack::seek_end(int64_t p_position) {
	seek(pf.size + p_position);
}

uint64_t FileAccessPack::get_position() const {
	return pos;
}

uint64_t FileAccessPack::get_len() const {
	return pf.size;
}

bool FileAccessPack::eof_reached() const {
	return eof;
}

uint8_t FileAccessPack::get_8() const {
	if (pos >= pf.size) {
		eof = true;
		return 0;
	}

	pos++;
	return f->get_8();
}

// Reading multi byte values through get_8() costs one virtual call (and, on
// some platforms, one read syscall) per byte. Read them in one shot instead.
uint16_t FileAccessPack::get_16() const {
	if (pos + 2 > pf.size) {
		eof = true;
		return 0;
	}

	uint8_t b[2] = { 0, 0 };
	f->get_buffer(b, 2);
	pos += 2;

	return _decode_16(b, endian_swap);
}

uint32_t FileAccessPack::get_32() const {
	if (pos + 4 > pf.size) {
		eof = true;
		return 0;
	}

	uint8_t b[4] = { 0, 0, 0, 0 };
	f->get_buffer(b, 4);
	pos += 4;

	return _decode_32(b, endian_swap);
}

uint64_t FileAccessPack::get_64() const {
	if (pos + 8 > pf.size) {
		eof = true;
		return 0;
	}

	uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	f->get_buffer(b, 8);
	pos += 8;

	return _decode_64(b, endian_swap);
}

uint64_t FileAccessPack::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V(!p_dst && p_length > 0, -1);

	if (eof) {
		return 0;
	}

	int64_t to_read = p_length;
	if (to_read + pos > pf.size) {
		eof = true;
		to_read = (int64_t)pf.size - (int64_t)pos;
	}

	pos += p_length;

	if (to_read <= 0) {
		return 0;
	}
	f->get_buffer(p_dst, to_read);

	return to_read;
}

void FileAccessPack::set_endian_swap(bool p_swap) {
	FileAccess::set_endian_swap(p_swap);
	f->set_endian_swap(p_swap);
}

Error FileAccessPack::get_error() const {
	if (eof) {
		return ERR_FILE_EOF;
	}
	return OK;
}

void FileAccessPack::flush() {
	ERR_FAIL();
}

void FileAccessPack::store_8(uint8_t p_dest) {
	ERR_FAIL();
}

void FileAccessPack::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL();
}

bool FileAccessPack::file_exists(const String &p_name) {
	return false;
}

FileAccessPack::FileAccessPack(const String &p_path, const PackedData::PackedFile &p_file) :
		pf(p_file),
		f(FileAccess::open(pf.pack, FileAccess::READ)) {
	ERR_FAIL_COND_MSG(!f, "Can't open pack-referenced file '" + String(pf.pack) + "'.");

	f->seek(pf.offset);
	pos = 0;
	eof = false;
}

FileAccessPack::~FileAccessPack() {
	if (f) {
		memdelete(f);
	}
}

//////////////////////////////////////////////////////////////////////////////////
// DIR ACCESS
//////////////////////////////////////////////////////////////////////////////////

Error DirAccessPack::list_dir_begin() {
	list_dirs.clear();
	list_files.clear();

	for (Map<String, PackedData::PackedDir *>::Element *E = current->subdirs.front(); E; E = E->next()) {
		list_dirs.push_back(E->key());
	}

	for (Set<String>::Element *E = current->files.front(); E; E = E->next()) {
		list_files.push_back(E->get());
	}

	return OK;
}

String DirAccessPack::get_next() {
	if (list_dirs.size()) {
		cdir = true;
		String d = list_dirs.front()->get();
		list_dirs.pop_front();
		return d;
	} else if (list_files.size()) {
		cdir = false;
		String f = list_files.front()->get();
		list_files.pop_front();
		return f;
	} else {
		return String();
	}
}
bool DirAccessPack::current_is_dir() const {
	return cdir;
}
bool DirAccessPack::current_is_hidden() const {
	return false;
}
void DirAccessPack::list_dir_end() {
	list_dirs.clear();
	list_files.clear();
}

int DirAccessPack::get_drive_count() {
	return 0;
}
String DirAccessPack::get_drive(int p_drive) {
	return "";
}

PackedData::PackedDir *DirAccessPack::_find_dir(String p_dir) {
	String nd = p_dir.replace("\\", "/");

	// Special handling since simplify_path() will forbid it
	if (p_dir == "..") {
		return current->parent;
	}

	bool absolute = false;
	if (nd.begins_with("res://")) {
		nd = nd.replace_first("res://", "");
		absolute = true;
	}

	nd = nd.simplify_path();

	if (nd == "") {
		nd = ".";
	}

	if (nd.begins_with("/")) {
		nd = nd.replace_first("/", "");
		absolute = true;
	}

	Vector<String> paths = nd.split("/");

	PackedData::PackedDir *pd;

	if (absolute) {
		pd = PackedData::get_singleton()->root;
	} else {
		pd = current;
	}

	for (int i = 0; i < paths.size(); i++) {
		String p = paths[i];
		if (p == ".") {
			continue;
		} else if (p == "..") {
			if (pd->parent) {
				pd = pd->parent;
			}
		} else if (pd->subdirs.has(p)) {
			pd = pd->subdirs[p];

		} else {
			return nullptr;
		}
	}

	return pd;
}

Error DirAccessPack::change_dir(String p_dir) {
	PackedData::PackedDir *pd = _find_dir(p_dir);
	if (pd) {
		current = pd;
		return OK;
	} else {
		return ERR_INVALID_PARAMETER;
	}
}

String DirAccessPack::get_current_dir() {
	PackedData::PackedDir *pd = current;
	String p = current->name;

	while (pd->parent) {
		pd = pd->parent;
		p = pd->name.plus_file(p);
	}

	return "res://" + p;
}

bool DirAccessPack::file_exists(String p_file) {
	p_file = fix_path(p_file);

	PackedData::PackedDir *pd = _find_dir(p_file.get_base_dir());
	if (!pd) {
		return false;
	}
	return pd->files.has(p_file.get_file());
}

bool DirAccessPack::dir_exists(String p_dir) {
	p_dir = fix_path(p_dir);

	return _find_dir(p_dir) != nullptr;
}

Error DirAccessPack::make_dir(String p_dir) {
	return ERR_UNAVAILABLE;
}

Error DirAccessPack::rename(String p_from, String p_to) {
	return ERR_UNAVAILABLE;
}
Error DirAccessPack::remove(String p_name) {
	return ERR_UNAVAILABLE;
}

uint64_t DirAccessPack::get_space_left() {
	return 0;
}

String DirAccessPack::get_filesystem_type() const {
	return "PCK";
}

DirAccessPack::DirAccessPack() {
	current = PackedData::get_singleton()->root;
	cdir = false;
}

DirAccessPack::~DirAccessPack() {
}
