# Ускорение загрузки игр на PS Vita

Этот документ описывает, почему Godot-игры на Vita грузились минутами, и что
сделано, чтобы это исправить.

---

## 1. В чём была проблема

Загрузка Godot-игры — это **тысячи мелких чтений**, а не одно большое:

* движок открывает `.pck` **заново для каждого ресурса** (`FileAccessPack`
  создаёт новый `FileAccess` на каждый файл);
* внутри ресурса он читает значения по 1–8 байт: `get_8()`, `get_16()`,
  `get_32()`, `get_64()`. В базовой реализации `get_32()` — это **4 вызова
  `get_8()`**, а `get_64()` — 8;
* каждый `get_8()` шёл через `fread(&b, 1, 1, f)` в newlib stdio;
* каждое открытие файла стоило `stat()` + `fopen()` + `fcntl(F_GETFD)` +
  `fcntl(F_SETFD)` + `fseeko()` + `fclose()`.

На десктопе это незаметно: системные вызовы дешёвые (~1 мкс), плюс кэш
страниц. На Vita:

* `app0:` — это карта памяти (а иногда и картридж), `sceIoOpen`/`sceIoRead`
  стоят **на порядок дороже**;
* **кэша страниц нет** — каждое чтение реально идёт на носитель;
* CPU — Cortex-A9, всё упирается в задержку, а не в пропускную способность.

Итог: игра с 3000 ресурсами делала **сотни тысяч системных вызовов**, каждый
из которых стоил доли миллисекунды — отсюда и «10 минут на загрузку».

Замер (40 003 файла в одном `.pck`, Linux, тёплый кэш — т.е. **худший
возможный случай для демонстрации выигрыша**):

| | обычный путь | весь `.pck` в RAM |
|---|---|---|
| перехваченных syscall'ов (только `stat`+`fcntl`) | **270 034** | **25** |
| последовательное чтение 40 003 файлов | ~1060 мс | **~668 мс** (−37 %) |
| 40 960 мелких чтений (`get_8/16/32/64`) | ~2100 мс | **~1770 мс** (−16 %) |

На Vita, где один syscall стоит в 10–100 раз дороже, а кэша нет вообще,
выигрыш на практике кратно больше.

---

## 2. Что сделано

### 2.1. Весь `.pck` загружается в RAM (`core/io/file_access_pack.*`)

Новый механизм **memory pack**: платформа может «всосать» весь файл пака в
память одним последовательным чтением, после чего все ресурсы читаются
оттуда через `memcpy()`.

* `PackedData::memory_pack_preload_func` — хук, который ставит платформа.
* `FileAccessPackMem` — новый `FileAccess`, читающий прямо из буфера.
  Переопределены `get_8/16/32/64`, `get_buffer`, `get_line`,
  `get_as_utf8_string`, `seek`, `get_len` — **ни одного syscall'а**.
* Каталог пака тоже парсится из памяти (через `FileAccessMemory`).
* Если прeлоад невозможен (слишком большой пак / нет памяти), движок
  **прозрачно** падает обратно на обычное чтение с диска.
* Портируемая реализация (`PackedData::default_memory_pack_preload`)
  доступна **всем** платформам и включается флагом `--preload-pack[=MiB]`.

### 2.2. `FileAccessVita` — буферизованный ввод-вывод напрямую через `sceIo`
(`platform/vita/file_access_vita.*`)

Заменяет `FileAccessUnix` на Vita:

* открытие = один `sceIoOpen()` вместо `stat` + `fopen` + 2×`fcntl`;
* буфер предчтения 64 КиБ (настраивается), большие чтения идут в обход
  буфера прямо в целевой буфер — без двойного копирования;
* `get_8/16/32/64`, `get_line` читаются прямо из буфера;
* `get_len()` кэшируется (вместо 3-х `lseek` на каждый вызов);
* запись осталась корректной (небуферизованная), включая «безопасное
  сохранение» через `.tmp` + rename.

### 2.3. `FileAccessPack`: чтение многобайтовых значений одним запросом

Даже без прeлоада `get_16/32/64` теперь делают **один** вызов `get_buffer()`
вместо 2/4/8 вызовов `get_8()`.

### 2.4. Дешёвый `get_ticks_usec()`

`clock_gettime()` — это syscall через `libc.suprx`. Заменён на
`sceKernelGetProcessTimeWide()` — прямой и гораздо более дешёвый вызов ядра.
Эта функция вызывается движком постоянно.

### 2.5. Настраиваемый разгон

Частота CPU теперь задаётся опцией сборки (и может быть переопределена в
`vita_opt.txt`). 444 МГц — безопасно на всех ретейл-консолях, 500 МГц в
основном работает на PSTV.

---

## 3. Опции сборки

```bash
scons platform=vita target=release \
      vita_preload_pack=yes       # грузить весь .pck в RAM (по умолчанию: yes)
      vita_preload_limit_mb=160   # максимум, МБ (0 = выключить)
      vita_file_buffer_kb=64      # буфер предчтения на каждый открытый файл
      vita_buffered_io=yes        # FileAccessVita вместо newlib stdio (yes)
      vita_oc=444                 # 333 / 444 / 500 МГц
```

Лимит по умолчанию = `min(160 МБ, 60 % кучи newlib)`, так что на
стандартной сборке это ~132 МБ. Если пак больше — движок молча перейдёт на
обычное (но всё равно буферизованное) чтение.

> **Внимание:** предзагрузка занимает оперативку навсегда. Если игра
> вылетает с OOM — снизьте `vita_preload_limit_mb`.

---

## 4. Настройка без пересборки: `vita_opt.txt`

Положите текстовый файл рядом с игрой — `app0:/game_data/vita_opt.txt`:

```ini
# максимальный размер .pck для загрузки в RAM, МБ (0 = отключить)
preload_limit_mb=128

# частота CPU: 333, 444 или 500
arm_clock=444
```

Все ключи необязательны, неизвестные игнорируются. Если файла нет —
используются значения, зашитые при сборке.

---

## 5. Как проверить, что это работает

Соберите с `target=release_debug` (или включите verbose) — в логе будет:

```
Vita: preloaded 'app0:/game_data/game.pck' (43 521 KiB) into RAM in 1834 ms.
```

или, если пак слишком большой:

```
Vita: not preloading '...' (184 320 KiB, limit 135 168 KiB), using buffered file access.
```

В `platform/vita/tools/make_bench_pck.py` есть генератор нагрузочного `.pck`
(много мелких файлов) — с его помощью удобно замерять разницу на устройстве.

---

## 6. Что ещё можно сделать со стороны проекта (очень важно!)

Оптимизации движка убирают накладные расходы на ввод-вывод. Второй, не менее
важный, фактор — **сколько данных вообще нужно прочитать и распаковать**.
Проверьте настройки экспорта проекта:

1. **Экспортируйте сцены в бинарном виде.** Текстовые `.tscn` парсятся
   `VariantParser`-ом в цикле `get_line()`/`get_token()` — на 444 МГц это
   очень медленно. В настройках экспорта включите конвертацию текстовых
   ресурсов в бинарные (`.scn`/`.res`), либо конвертируйте сцены в редакторе.
2. **Используйте VRAM-сжатие текстур (PVRTC).** Vita (SGX543) поддерживает
   PVRTC аппаратно. Несжатый RGBA8 — это в 4–8 раз больше данных, которые
   нужно прочитать, распаковать и залить в GPU.
3. **Уменьшите размеры текстур** до реально необходимых (960×544 экран).
4. **Выключите mipmaps** там, где они не нужны.
5. **Аудио:** используйте Vorbis/Opus с умеренным битрейтом, а не WAV.
6. **Не грузите всё сразу.** Большие сцены разбивайте и подгружайте
   через `ResourceLoader.load_interactive()` / фоновую загрузку — так
   игрок увидит меню, пока остальное догружается.
7. **Выключите `sceMotion`/гироскоп**, если игра их не использует — это
   лишние прерывания и разряженная батарея.

---

## 7. Совместимость

* Изменения в `core/io/file_access_pack.*` **не меняют поведение** ни на
  одной платформе, если хук прелоада не установлен (по умолчанию он
  установлен только на Vita и только при запуске с `--preload-pack`).
* Прeлоад включается только для самостоятельных `.pck` (`p_offset == 0`);
  self-contained исполняемые файлы и ZIP-паки работают как раньше.
* Память освобождается в `~PackedData()`.

---

# Loading speed on PS Vita (English summary)

Godot reads resources with thousands of tiny reads; on the Vita every
`sceIoOpen`/`sceIoRead` is expensive and there is no page cache, so booting a
game could take many minutes.

What changed:

1. **Whole-`.pck` preloading** (`PackedData::memory_pack_preload_func` +
   `FileAccessPackMem`): the pack is read into RAM once, every resource read
   becomes a `memcpy()`. Falls back transparently to file access when the pack
   is too large or memory is tight. Available on **all** platforms through the
   new `--preload-pack[=MiB]` command line option.
2. **`FileAccessVita`**: buffered (64 KiB read-ahead), direct `sceIo` I/O,
   with fast `get_8/16/32/64`, `get_line` and a cached `get_len()` — replaces
   the slow newlib stdio path.
3. **`FileAccessPack::get_16/32/64`** now issue a single `get_buffer()` call
   instead of 2/4/8 `get_8()` calls.
4. **`get_ticks_usec()`** uses `sceKernelGetProcessTimeWide()` instead of
   `clock_gettime()`.
5. **Configurable overclock** (`vita_oc`) and runtime tuning via
   `app0:/game_data/vita_opt.txt`.

Measured on Linux with a warm page cache (i.e. the *worst* case for showing a
win), 40 003 files in one pack:

| | regular | `--preload-pack` |
|---|---|---|
| syscalls (`stat`+`fcntl`) | 270 034 | 25 |
| sequential scan | ~1060 ms | ~668 ms |
| 40 960 tiny reads | ~2100 ms | ~1770 ms |

On Vita (no page cache, ~10–100× more expensive syscalls) the gain is far
bigger.

SCons options: `vita_preload_pack`, `vita_preload_limit_mb`,
`vita_file_buffer_kb`, `vita_buffered_io`, `vita_oc`.
