/**************************************************************************/
/*  file_access_vita.h                                                     */
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

#ifndef FILE_ACCESS_VITA_H
#define FILE_ACCESS_VITA_H

#include "core/os/file_access.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/types.h>

// Buffered FileAccess for the PS Vita.
//
// The stock FileAccessUnix implementation goes through newlib's stdio, so
// every single get_8() (and therefore get_16()/get_32()/get_64(), which are
// built on top of it) ends up inside _fread_r() doing lock handling and
// bookkeeping before it ever reaches sceIoRead(). Godot reads resource files
// byte by byte, so on a 444 MHz Cortex-A9 that overhead completely dominates
// loading times.
//
// FileAccessVita talks to SceIofilemgr directly and keeps a large read buffer
// around, which means:
//   * one sceIoOpen()/sceIoClose() per file instead of stat() + fopen() +
//     fcntl(F_GETFD) + fcntl(F_SETFD) + fclose(),
//   * one sceIoRead() every N KiB instead of one fread() per value,
//   * get_8()/get_16()/get_32()/get_64()/get_line() served straight out of RAM.
// Read-ahead buffer size in KiB. Tune with the `vita_file_buffer_kb` SCons
// option.
#ifndef VITA_FILE_BUFFER_KB
#define VITA_FILE_BUFFER_KB 64
#endif

class FileAccessVita : public FileAccess {
	SceUID fd;
	int flags;
	mutable Error last_error;
	mutable bool eof;

	String path;
	String path_src;
	String save_path;

	mutable uint8_t *buffer; // read-ahead buffer (nullptr when write only)
	mutable uint32_t buffer_size; // allocated capacity of `buffer`
	mutable uint64_t buffer_start; // file offset of buffer[0]
	mutable uint32_t buffer_len; // valid bytes inside `buffer`
	mutable uint32_t buffer_pos; // read cursor inside `buffer`

	mutable SceOff hw_pos; // where the SceIofilemgr cursor currently is

	mutable uint64_t file_size; // cached result of get_len()
	mutable bool file_size_cached;

	int _fill() const; // refill the buffer at the current logical position
	bool _ensure(uint32_t p_bytes) const; // make sure `p_bytes` are buffered
	void _drop_buffer() const;
	uint64_t _tell() const; // logical position
	void _seek_hw(uint64_t p_pos) const;

public:
	// Not used by the Vita port (only the JavaScript port installs one) but kept
	// for API compatibility with FileAccessUnix.
	typedef void (*CloseNotificationFunc)(const String &p_file, int p_flags);
	static CloseNotificationFunc close_notification_func;

	virtual Error _open(const String &p_path, int p_mode_flags);
	virtual void close();
	virtual bool is_open() const;

	virtual String get_path() const;
	virtual String get_path_absolute() const;

	virtual void seek(uint64_t p_position);
	virtual void seek_end(int64_t p_position = 0);
	virtual uint64_t get_position() const;
	virtual uint64_t get_len() const;
	virtual bool eof_reached() const;

	virtual uint8_t get_8() const;
	virtual uint16_t get_16() const;
	virtual uint32_t get_32() const;
	virtual uint64_t get_64() const;
	virtual uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const;
	virtual String get_line() const;

	virtual Error get_error() const;
	virtual void flush();

	virtual void store_8(uint8_t p_dest);
	virtual void store_buffer(const uint8_t *p_src, uint64_t p_length);

	virtual bool file_exists(const String &p_path);

	virtual uint64_t _get_modified_time(const String &p_file);
	virtual uint32_t _get_unix_permissions(const String &p_file);
	virtual Error _set_unix_permissions(const String &p_file, uint32_t p_permissions);

	FileAccessVita();
	~FileAccessVita();
};

#endif // FILE_ACCESS_VITA_H
