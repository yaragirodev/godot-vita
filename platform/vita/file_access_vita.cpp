/**************************************************************************/
/*  file_access_vita.cpp                                                   */
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

#include "file_access_vita.h"

#ifdef VITA_ENABLED

#include "core/os/os.h"
#include "core/print_string.h"

#include <psp2/kernel/clib.h>
#include <psp2/rtc.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

// Godot always stores little endian data; `endian_swap` asks for the value to
// be interpreted as big endian instead (mirrors FileAccess::get_16/32/64).
static _FORCE_INLINE_ uint16_t _vita_decode_16(const uint8_t *p, bool p_swap) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	uint16_t v;
	memcpy(&v, p, sizeof(v));
#else
	uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
#endif
	return p_swap ? BSWAP16(v) : v;
}

static _FORCE_INLINE_ uint32_t _vita_decode_32(const uint8_t *p, bool p_swap) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	uint32_t v;
	memcpy(&v, p, sizeof(v));
#else
	uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
#endif
	return p_swap ? BSWAP32(v) : v;
}

static _FORCE_INLINE_ uint64_t _vita_decode_64(const uint8_t *p, bool p_swap) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	uint64_t v;
	memcpy(&v, p, sizeof(v));
#else
	uint64_t v = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
			((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
#endif
	return p_swap ? BSWAP64(v) : v;
}

////////////////////////////////////////////////////////////////////////////////
// internal helpers
////////////////////////////////////////////////////////////////////////////////

uint64_t FileAccessVita::_tell() const {
	return buffer_start + buffer_pos;
}

void FileAccessVita::_drop_buffer() const {
	buffer_start = _tell();
	buffer_len = 0;
	buffer_pos = 0;
}

void FileAccessVita::_seek_hw(uint64_t p_pos) const {
	if (hw_pos == (SceOff)p_pos) {
		return;
	}
	sceIoLseek(fd, (SceOff)p_pos, SCE_SEEK_SET);
	hw_pos = (SceOff)p_pos;
}

int FileAccessVita::_fill() const {
	if (buffer == nullptr) {
		return 0;
	}

	// The buffer always starts at the current logical position.
	const uint64_t start = buffer_start + buffer_pos;
	_seek_hw(start);

	const int r = sceIoRead(fd, buffer, (SceSize)buffer_size);
	if (r <= 0) {
		buffer_start = start;
		buffer_len = 0;
		buffer_pos = 0;
		if (r < 0) {
			last_error = ERR_FILE_CANT_READ;
		}
		return 0;
	}

	hw_pos = (SceOff)(start + r);
	buffer_start = start;
	buffer_len = (uint32_t)r;
	buffer_pos = 0;

	return r;
}

bool FileAccessVita::_ensure(uint32_t p_bytes) const {
	if (buffer_pos + p_bytes <= buffer_len) {
		return true;
	}

	// Refill. If the request does not even fit in a full buffer the caller
	// (get_16/get_32/get_64) will fall back to get_buffer(), which reads
	// directly into the destination.
	if (buffer_pos >= buffer_len) {
		if (_fill() <= 0) {
			eof = true;
			last_error = ERR_FILE_EOF;
			return false;
		}
		return (uint32_t)(buffer_len - buffer_pos) >= p_bytes;
	}

	// Partially buffered value sitting at the end of the buffer: shift the
	// leftovers down and top the buffer up. This is rare (only for values
	// straddling a buffer boundary).
	const uint32_t left = buffer_len - buffer_pos;
	memmove(buffer, buffer + buffer_pos, left);
	buffer_start += buffer_pos;
	buffer_pos = 0;
	buffer_len = left;

	const uint64_t start = buffer_start + left;
	_seek_hw(start);
	const int r = sceIoRead(fd, buffer + left, (SceSize)(buffer_size - left));
	if (r <= 0) {
		if (r < 0) {
			last_error = ERR_FILE_CANT_READ;
		}
		if (left == 0) {
			eof = true;
			last_error = ERR_FILE_EOF;
			return false;
		}
		return false;
	}

	hw_pos = (SceOff)(start + r);
	buffer_len = left + (uint32_t)r;
	return (uint32_t)(buffer_len - buffer_pos) >= p_bytes;
}

////////////////////////////////////////////////////////////////////////////////
// open / close
////////////////////////////////////////////////////////////////////////////////
FileAccessVita::CloseNotificationFunc FileAccessVita::close_notification_func = nullptr;


Error FileAccessVita::_open(const String &p_path, int p_mode_flags) {
	if (fd >= 0) {
		sceIoClose(fd);
		fd = -1;
	}

	path_src = p_path;
	path = fix_path(p_path);

	int sce_flags = 0;
	if (p_mode_flags == READ) {
		sce_flags = SCE_O_RDONLY;
	} else if (p_mode_flags == WRITE) {
		sce_flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;
	} else if (p_mode_flags == READ_WRITE) {
		sce_flags = SCE_O_RDWR | SCE_O_CREAT;
	} else if (p_mode_flags == WRITE_READ) {
		sce_flags = SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC;
	} else {
		return ERR_INVALID_PARAMETER;
	}

	if (is_backup_save_enabled() && (p_mode_flags & WRITE) && !(p_mode_flags & READ)) {
		save_path = path;
		path = path + ".tmp";
	}

	const CharString cpath = path.utf8();
	fd = sceIoOpen(cpath.get_data(), sce_flags, 0666);

	if (fd < 0) {
		fd = -1;
		// There is no errno on SceIofilemgr, so report the generic error.
		last_error = ERR_FILE_CANT_OPEN;
		return last_error;
	}

	last_error = OK;
	flags = p_mode_flags;
	eof = false;
	file_size = 0;
	file_size_cached = false;
	hw_pos = 0;
	buffer_start = 0;
	buffer_len = 0;
	buffer_pos = 0;

	// Only readers need the read-ahead buffer, and only allocate it once the
	// file is really open.
	if (p_mode_flags & READ) {
		if (buffer == nullptr) {
			buffer_size = (uint32_t)VITA_FILE_BUFFER_KB * 1024;
			buffer = (uint8_t *)memalloc(buffer_size);
		}
	} else if (buffer != nullptr) {
		// Write-only files never read, so don't waste RAM on a read-ahead buffer.
		memfree(buffer);
		buffer = nullptr;
		buffer_size = 0;
	}

	return OK;
}

void FileAccessVita::close() {
	if (fd < 0) {
		return;
	}

	sceIoClose(fd);
	fd = -1;

	if (buffer != nullptr) {
		memfree(buffer);
		buffer = nullptr;
		buffer_size = 0;
	}
	buffer_start = 0;
	buffer_len = 0;
	buffer_pos = 0;

	if (close_notification_func) {
		close_notification_func(path, flags);
	}

	if (save_path != "") {
		const int rename_error = ::rename((save_path + ".tmp").utf8().get_data(), save_path.utf8().get_data());
		if (rename_error && close_fail_notify) {
			close_fail_notify(save_path);
		}
		save_path = "";
		ERR_FAIL_COND(rename_error != 0);
	}
}

bool FileAccessVita::is_open() const {
	return fd >= 0;
}

String FileAccessVita::get_path() const {
	return path_src;
}

String FileAccessVita::get_path_absolute() const {
	return path;
}

////////////////////////////////////////////////////////////////////////////////
// positioning
////////////////////////////////////////////////////////////////////////////////

void FileAccessVita::seek(uint64_t p_position) {
	ERR_FAIL_COND_MSG(fd < 0, "File must be opened before use.");

	last_error = OK;
	eof = false;

	if (buffer != nullptr && p_position >= buffer_start && p_position <= buffer_start + buffer_len) {
		// Still inside the read-ahead buffer: no syscall needed.
		buffer_pos = (uint32_t)(p_position - buffer_start);
		return;
	}

	buffer_start = p_position;
	buffer_len = 0;
	buffer_pos = 0;
}

void FileAccessVita::seek_end(int64_t p_position) {
	ERR_FAIL_COND_MSG(fd < 0, "File must be opened before use.");

	const uint64_t len = get_len();
	seek((uint64_t)((int64_t)len + p_position));
}

uint64_t FileAccessVita::get_position() const {
	ERR_FAIL_COND_V_MSG(fd < 0, 0, "File must be opened before use.");

	return _tell();
}

uint64_t FileAccessVita::get_len() const {
	ERR_FAIL_COND_V_MSG(fd < 0, 0, "File must be opened before use.");

	if (file_size_cached) {
		return file_size;
	}

	// Cache it: FileAccessUnix would do up to 3 seek syscalls every single
	// time this is called.
	const SceOff cur = sceIoLseek(fd, 0, SCE_SEEK_CUR);
	const SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (cur >= 0) {
		sceIoLseek(fd, cur, SCE_SEEK_SET);
		hw_pos = cur;
	}

	// "cur" was the hardware position, which is ahead of the logical one
	// because of read-ahead, so drop the buffer.
	const uint64_t logical = _tell();
	buffer_start = logical;
	buffer_len = 0;
	buffer_pos = 0;

	if (end >= 0) {
		file_size = (uint64_t)end;
		file_size_cached = true;
	}

	return file_size;
}

bool FileAccessVita::eof_reached() const {
	return eof;
}

////////////////////////////////////////////////////////////////////////////////
// reading
////////////////////////////////////////////////////////////////////////////////

uint8_t FileAccessVita::get_8() const {
	ERR_FAIL_COND_V_MSG(fd < 0, 0, "File must be opened before use.");

	if (buffer_pos >= buffer_len) {
		if (_fill() <= 0) {
			eof = true;
			last_error = ERR_FILE_EOF;
			return 0;
		}
	}

	return buffer[buffer_pos++];
}

uint16_t FileAccessVita::get_16() const {
	ERR_FAIL_COND_V_MSG(fd < 0, 0, "File must be opened before use.");

	if (_ensure(2)) {
		const uint16_t v = _vita_decode_16(buffer + buffer_pos, endian_swap);
		buffer_pos += 2;
		return v;
	}

	uint8_t b[2] = { 0, 0 };
	get_buffer(b, 2);
	return _vita_decode_16(b, endian_swap);
}

uint32_t FileAccessVita::get_32() const {
	ERR_FAIL_COND_V_MSG(fd < 0, 0, "File must be opened before use.");

	if (_ensure(4)) {
		const uint32_t v = _vita_decode_32(buffer + buffer_pos, endian_swap);
		buffer_pos += 4;
		return v;
	}

	uint8_t b[4] = { 0, 0, 0, 0 };
	get_buffer(b, 4);
	return _vita_decode_32(b, endian_swap);
}

uint64_t FileAccessVita::get_64() const {
	ERR_FAIL_COND_V_MSG(fd < 0, 0, "File must be opened before use.");

	if (_ensure(8)) {
		const uint64_t v = _vita_decode_64(buffer + buffer_pos, endian_swap);
		buffer_pos += 8;
		return v;
	}

	uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	get_buffer(b, 8);
	return _vita_decode_64(b, endian_swap);
}

uint64_t FileAccessVita::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V(!p_dst && p_length > 0, -1);
	ERR_FAIL_COND_V_MSG(fd < 0, -1, "File must be opened before use.");

	if (p_length == 0) {
		return 0;
	}

	const uint64_t start = _tell();

	// Big reads bypass the read-ahead buffer entirely so that we never copy
	// twice (this is the common case for textures, audio, .scn blobs, ...).
	if (buffer == nullptr || p_length >= buffer_size) {
		_seek_hw(start);

		uint64_t done = 0;
		while (done < p_length) {
			uint64_t chunk = p_length - done;
			if (chunk > 0x10000000ULL) { // keep it inside SceSize range
				chunk = 0x10000000ULL;
			}
			const int r = sceIoRead(fd, p_dst + done, (SceSize)chunk);
			if (r <= 0) {
				if (r < 0) {
					last_error = ERR_FILE_CANT_READ;
				}
				if (done == 0) {
					eof = true;
					last_error = ERR_FILE_EOF;
				}
				break;
			}
			done += (uint64_t)r;
			hw_pos = (SceOff)(start + done);
		}

		buffer_start = start + done;
		buffer_len = 0;
		buffer_pos = 0;
		return done;
	}

	uint64_t done = 0;
	while (done < p_length) {
		if (buffer_pos >= buffer_len) {
			if (_fill() <= 0) {
				eof = true;
				last_error = ERR_FILE_EOF;
				break;
			}
		}

		uint64_t avail = (uint64_t)(buffer_len - buffer_pos);
		uint64_t to_copy = p_length - done;
		if (to_copy > avail) {
			to_copy = avail;
		}

		memcpy(p_dst + done, buffer + buffer_pos, (size_t)to_copy);
		buffer_pos += (uint32_t)to_copy;
		done += to_copy;
	}

	return done;
}

String FileAccessVita::get_line() const {
	ERR_FAIL_COND_V_MSG(fd < 0, String(), "File must be opened before use.");

	if (buffer_pos >= buffer_len) {
		if (_fill() <= 0) {
			eof = true;
			last_error = ERR_FILE_EOF;
			return String();
		}
	}

	const uint8_t *start = buffer + buffer_pos;
	const uint64_t avail = (uint64_t)(buffer_len - buffer_pos);

	// FileAccess::get_line() terminates on either '\n' or a nul byte.
	uint64_t term = avail;
	const uint8_t *nl = (const uint8_t *)memchr(start, '\n', avail);
	if (nl != nullptr) {
		term = (uint64_t)(nl - start);
	}
	const uint8_t *nul = (const uint8_t *)memchr(start, 0, term);
	if (nul != nullptr) {
		term = (uint64_t)(nul - start);
	}

	if (term == avail) {
		// The line spans a buffer boundary: fall back to the generic
		// (get_8() based) implementation. Rare, and still correct.
		return FileAccess::get_line();
	}

	if (memchr(start, '\r', term) == nullptr) {
		buffer_pos += (uint32_t)(term + 1);
		return String::utf8((const char *)start, (int)term);
	}

	// Rare path: FileAccess::get_line() silently drops every '\r'.
	Vector<char> buf;
	buf.resize((int)term + 1);
	char *w = buf.ptrw();
	int j = 0;
	for (uint64_t i = 0; i < term; i++) {
		if (start[i] != '\r') {
			w[j++] = (char)start[i];
		}
	}
	w[j] = 0;

	buffer_pos += (uint32_t)(term + 1);

	return String::utf8(w, j);
}

////////////////////////////////////////////////////////////////////////////////
// writing
////////////////////////////////////////////////////////////////////////////////

void FileAccessVita::store_8(uint8_t p_dest) {
	ERR_FAIL_COND_MSG(fd < 0, "File must be opened before use.");

	const uint64_t at = _tell();
	_seek_hw(at);

	if (sceIoWrite(fd, &p_dest, 1) != 1) {
		last_error = ERR_FILE_CANT_WRITE;
		return;
	}

	hw_pos = (SceOff)(at + 1);
	buffer_start = at + 1;
	buffer_len = 0;
	buffer_pos = 0;
	file_size_cached = false;
}

void FileAccessVita::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_COND_MSG(fd < 0, "File must be opened before use.");
	ERR_FAIL_COND(!p_src && p_length > 0);

	if (p_length == 0) {
		return;
	}

	const uint64_t at = _tell();
	_seek_hw(at);

	uint64_t done = 0;
	while (done < p_length) {
		uint64_t chunk = p_length - done;
		if (chunk > 0x10000000ULL) {
			chunk = 0x10000000ULL;
		}
		const int r = sceIoWrite(fd, p_src + done, (SceSize)chunk);
		if (r <= 0) {
			last_error = ERR_FILE_CANT_WRITE;
			break;
		}
		done += (uint64_t)r;
		hw_pos = (SceOff)(at + done);
	}

	buffer_start = at + done;
	buffer_len = 0;
	buffer_pos = 0;
	file_size_cached = false;
}

void FileAccessVita::flush() {
	// Writes are never buffered, so there is nothing to flush.
}

////////////////////////////////////////////////////////////////////////////////
// misc
////////////////////////////////////////////////////////////////////////////////

Error FileAccessVita::get_error() const {
	return last_error;
}

bool FileAccessVita::file_exists(const String &p_path) {
	const CharString cpath = fix_path(p_path).utf8();

	SceIoStat st;
	sceClibMemset(&st, 0, sizeof(st));
	if (sceIoGetstat(cpath.get_data(), &st) < 0) {
		return false;
	}

#if defined(SCE_S_IFREG) && defined(SCE_S_IFMT)
	if ((st.st_mode & SCE_S_IFMT) == SCE_S_IFREG) {
		return true;
	}
	if ((st.st_mode & SCE_S_IFMT) != 0) {
		return false; // directory, symlink, ...
	}
#endif

	return true;
}

uint64_t FileAccessVita::_get_modified_time(const String &p_file) {
	const CharString cpath = fix_path(p_file).utf8();

	SceIoStat st;
	sceClibMemset(&st, 0, sizeof(st));
	if (sceIoGetstat(cpath.get_data(), &st) < 0) {
		print_verbose("Failed to get modified time for: " + p_file);
		return 0;
	}

	uint64_t time = 0;
	if (sceRtcConvertDateTimeToTime64_t(&st.st_mtime, (time_t *)&time) < 0) {
		return 0;
	}

	return time;
}

uint32_t FileAccessVita::_get_unix_permissions(const String &p_file) {
	const CharString cpath = fix_path(p_file).utf8();

	SceIoStat st;
	sceClibMemset(&st, 0, sizeof(st));
	if (sceIoGetstat(cpath.get_data(), &st) < 0) {
		ERR_FAIL_V_MSG(0, "Failed to get unix permissions for: " + p_file + ".");
	}

	return st.st_mode & 0x7FF;
}

Error FileAccessVita::_set_unix_permissions(const String &p_file, uint32_t p_permissions) {
	// Not supported by SceIofilemgr.
	return FAILED;
}

FileAccessVita::FileAccessVita() {
	fd = -1;
	flags = 0;
	last_error = OK;
	eof = false;
	buffer = nullptr;
	buffer_size = 0;
	buffer_start = 0;
	buffer_len = 0;
	buffer_pos = 0;
	hw_pos = 0;
	file_size = 0;
	file_size_cached = false;
}

FileAccessVita::~FileAccessVita() {
	close();
}

#endif // VITA_ENABLED
