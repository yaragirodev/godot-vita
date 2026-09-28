#!/usr/bin/env python3
"""Generate a Godot .pck full of many small files, used to benchmark the
pack read path (works with any Godot 3.x build, not just the Vita port).

    python3 make_bench_pck.py bench.pck [num_files] [file_size]
    ./godot --main-pack bench.pck              # regular path
    ./godot --main-pack bench.pck --preload-pack   # whole pack in RAM

The generated project opens every file, reads it, then performs ~64 tiny
get_8/get_16/get_32/get_64 reads per file - which is exactly what the
resource loader does. Both timings are printed as a BENCH line.
"""
import hashlib
import os
import struct
import sys

MAGIC = 0x43504447
VERSION = 1
MAJOR, MINOR, PATCH = 3, 7, 0

out = sys.argv[1]
NFILES = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
FSIZE = int(sys.argv[3]) if len(sys.argv) > 3 else 3072
align = 4

files = {}
files["res://project.godot"] = b"""; Engine configuration file.
config_version=4

[application]

config/name="vitabench"
run/main_scene="res://main.tscn"

[rendering]

environment/default_clear_color=Color( 0, 0, 0, 1 )
"""
files["res://main.tscn"] = b"""[gd_scene load_steps=3 format=2]

[ext_resource path="res://main.gd" type="Script" id=1]

[node name="Node" type="Node"]
script = ExtResource( 1 )
"""
files["res://main.gd"] = ("""extends Node

const N := %d
const SZ := %d

func _ready() -> void:
	bench()
	get_tree().quit()

func bench() -> void:
	# warm up so both variants start with a warm page cache
	scan(false)
	var t0 := OS.get_ticks_msec()
	scan(true)
	var seq_ms := OS.get_ticks_msec() - t0

	var t1 := OS.get_ticks_msec()
	random_reads()
	var rnd_ms := OS.get_ticks_msec() - t1

	print("BENCH files=", N, " size=", SZ, " sequential_ms=", seq_ms, " random_reads_ms=", rnd_ms)

func scan(p_count: bool) -> void:
	var total := 0
	var files_read := 0
	for i in range(N):
		var p := "res://many/f%%05d.bin" %% i
		var f := File.new()
		if f.open(p, File.READ) != OK:
			print("FAIL open ", p)
			return
		if p_count:
			total += int(f.get_len())
			var b := f.get_buffer(SZ)
			total += b.size()
			files_read += 1
		f.close()
	if p_count:
		print("scan: files=", files_read, " bytes=", total)

func random_reads() -> void:
	# Simulate what the resource loader does: open a file, then do many tiny
	# reads (get_8 / get_16 / get_32) with a few seeks.
	var total := 0
	for i in range(N / 4):
		var p := "res://many/f%%05d.bin" %% (i * 3)
		var f := File.new()
		f.open(p, File.READ)
		for j in range(64):
			f.seek(j * 8)
			total += int(f.get_8())
			total += int(f.get_16())
			total += int(f.get_32())
			total += int(f.get_64())
		f.close()
	print("random_reads: checksum=", total)
""" % (NFILES, FSIZE)).encode("ascii")

blob = os.urandom(FSIZE)
for i in range(NFILES):
    files["res://many/f%05d.bin" % i] = blob


def pad(n, a):
    return (a - (n % a)) % a if a else 0


buf = bytearray()
buf += struct.pack("<IIIII", MAGIC, VERSION, MAJOR, MINOR, PATCH)
buf += struct.pack("<16I", *([0] * 16))
buf += struct.pack("<I", len(files))

index_size = 0
for path in files:
    pb = path.encode("utf-8")
    index_size += 4 + len(pb) + 8 + 8 + 16
raw_end = 20 + 64 + 4 + index_size
data_start = raw_end + pad(raw_end, align)

offsets = {}
pos = data_start
for path in sorted(files):
    pb = path.encode("utf-8")
    buf += struct.pack("<I", len(pb)) + pb
    buf += struct.pack("<QQ", pos, len(files[path]))
    buf += hashlib.md5(files[path]).digest()
    offsets[path] = pos
    pos += len(files[path])
    pos += pad(pos, align)

buf += b"\0" * (data_start - raw_end)
for path in sorted(files):
    assert len(buf) == offsets[path]
    buf += files[path]
    buf += b"\0" * pad(len(buf), align)

os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
open(out, "wb").write(buf)
print("wrote %s (%d bytes, %d files)" % (out, len(buf), len(files)))
