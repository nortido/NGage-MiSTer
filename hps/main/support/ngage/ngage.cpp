// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "../../file_io.h"
#include "../../hardware.h"
#include "../../lib/miniz/miniz.h"
#include "../../menu.h"
#include "../../spi.h"
#include "../../user_io.h"
#include "../arcade/mra_loader.h"
#include "ngage.h"

// the shared C headers check their layout with the C11 keyword
#define _Static_assert static_assert
extern "C" {
#include "common/hyb_regs.h"
#include "lib/ngl_cdrv.h"
#include "lib/ngl_fat.h"
#include "lib/ngl_fork.h"
#include "lib/ngl_game.h"
#include "lib/ngl_hw.h"
#include "lib/ngl_lcd.h"
#include "lib/ngl_libs.h"
#include "lib/ngl_patch.h"
#include "lib/ngl_romdir.h"
#include "lib/ngl_sav.h"
#include "lib/ngl_src.h"
#include "lib/ngl_svcrom.h"
#include "lib/ngl_xcap.h"
}
#undef _Static_assert

#define NGAGE_POLL_MS  50u
#define NGAGE_LOG_DIR  "/tmp/ngl"
#define NGAGE_LOG      NGAGE_LOG_DIR "/fork.log"
#define NGAGE_LOG_MAX  (1 << 20)
// never created: user_io_file_mount marks a missing file as type 2 and announces pre_size
#define NGAGE_RAM_IMG  NGAGE_LOG_DIR "/ngage_ram.img"
#define NGAGE_MON      "ngage_mon.bin"
#define NGAGE_PATCH    "ngage_patch.txt"
#define NGAGE_BASE_ROM "base.rom"
#define NGAGE_TEXT_MAX (1 << 20)
#define NGAGE_DEV_DIR  NGAGE_LOG_DIR "/dev"
#define NGAGE_DEV_SEL  NGAGE_DEV_DIR "/select"
#define NGAGE_ROM_MAX  (PL_RAM16_BASE - PL_ROM_BASE)
#define NGAGE_CDRV_TRIES 3u
#define NGAGE_CDRV_STEP 0x4000u
#define NGAGE_CDRV_WSTEP 0x10000u
// Info lines are at most 30 characters (osd.cpp INFO_MAXW 32 with the frame)
#define NGAGE_CID_MSG  "Game not supported\nwithout card CID."
#define NGAGE_CID_SHOW_MS 12000
#define NGAGE_CID_AGAIN_MS 10000u

static struct ngl_hw hw;
static struct ngl_io io;
static int io_state;
static struct ngl_fork fk;
static struct ngl_sink_mem img;
static int own;
static char *mon_buf;
static size_t mon_len;
static char *patch_buf;
static size_t patch_len;
static unsigned long poll_timer;
static unsigned long cid_timer;
static struct ngl_sav sav;
static uint8_t sav_bm[NGL_SAV_BITMAP_BYTES];
static int sav_on;
static int sav_err;
static uint32_t sav_id;
static uint32_t sav_writes;
static uint32_t sav_bytes;
static uint32_t sav_drops;
static char sav_core[256];
static char sav_dir[1024];
static char sav_path[1024];
static struct ngl_cdrv_sched cdrv_sch;
static struct ngl_cdrv_action cdrv_act;
static int cdrv_on;
static int cdrv_run;
static int cdrv_nonblank;
static int cdrv_guarded;
static int cdrv_err;
static uint32_t cdrv_rom;
static uint32_t cdrv_fid;
static char cdrv_core[256];
static char cdrv_dir[1024];
static char cdrv_path[1024];
enum { CF_IDLE = 0, CF_COPY, CF_WRITE, CF_SYNC, CF_RENAME, CF_DIRSYNC, CF_BG };
static struct
{
	int phase;
	int scheduled;
	int io;
	const char *why;
	int fd;
	uint32_t off;
	uint32_t gen;
	unsigned long crc;
	uint32_t row;
	uint32_t now;
	unsigned tries;
	unsigned long t0;
	unsigned long wt;
	unsigned long slow;
} cf;
static struct ngl_romdir rom_libs = { 0, 0, {0}, {0}, {0}, "none" };
static uint8_t *cdrv_buf;
struct bgjob
{
	pthread_t th;
	int on;
	int done;
	int stop;
	int rc;
	int err;
};
static struct bgjob cdrv_job;
static struct
{
	struct bgjob j;
	uint8_t *buf;
	uint32_t len;
	uint32_t now;
	uint32_t recs;
	uint32_t writes;
	uint32_t bytes;
	uint32_t last_write;
	int scheduled;
	int err;
	int dir_bad;
	int dir_err;
	const char *why;
	unsigned long t0;
} sw;

static void flog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void flog(const char *fmt, ...)
{
	char line[512], up[32] = "-";
	struct stat st;
	va_list ap;
	FILE *f;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("ngage: %s\n", line);

	f = fopen("/proc/uptime", "r");
	if (f)
	{
		if (fscanf(f, "%31s", up) != 1) strcpy(up, "-");
		fclose(f);
	}
	mkdir(NGAGE_LOG_DIR, 0755);
	if (!stat(NGAGE_LOG, &st) && st.st_size >= NGAGE_LOG_MAX) return;
	f = fopen(NGAGE_LOG, "a");
	if (!f) return;
	fprintf(f, "%s %d %s\n", up, (int)getpid(), line);
	fclose(f);
}

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void info(const char *fmt, ...)
{
	char msg[160], line[176];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(line, sizeof(line), "N-Gage: %s", msg);
	flog("%s", line);
	Info(line, 4000);
}

// Info shows nothing while the OSD menu is open, so ngage_poll shows it again until a select or the unload
static void cid_show()
{
	Info(NGAGE_CID_MSG, NGAGE_CID_SHOW_MS);
	cid_timer = GetTimer(NGAGE_CID_AGAIN_MS);
}

static void cid_stay(int kind)
{
	char m[sizeof(NGAGE_CID_MSG)], *s, *sp = NULL;

	// a pre-frame exit keeps the core with no OSD text
	if (kind == 2)
	{
		flog("exit: card identity (pre-frame exit), core stays");
		return;
	}
	flog("exit: card identity, core stays");
	snprintf(m, sizeof(m), "%s", NGAGE_CID_MSG);
	for (s = strtok_r(m, "\n", &sp); s; s = strtok_r(NULL, "\n", &sp)) flog("exit: message: %s", s);
	cid_show();
}

static void io_log(void *ctx, const char *msg)
{
	(void)ctx;
	flog("lib: %s", msg);
}

static const struct ngl_io *io_get()
{
	if (!io_state)
	{
		memset(&io, 0, sizeof(io));
		io_state = ngl_hw_open(&hw, &io) ? -1 : 1;
		if (io_state > 0) io.log = io_log;
		else flog("no /dev/mem");
	}
	return io_state > 0 ? &io : NULL;
}

static char *read_file(const char *path, size_t max, size_t *len, int root_only)
{
	struct stat st;
	char *b;
	FILE *f;

	f = fopen(path, "rb");
	if (!f) return NULL;
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > max ||
		(root_only && st.st_uid))
	{
		fclose(f);
		return NULL;
	}
	b = (char *)malloc(st.st_size + 1);
	if (b && fread(b, 1, st.st_size, f) != (size_t)st.st_size)
	{
		free(b);
		b = NULL;
	}
	fclose(f);
	if (!b) return NULL;
	b[st.st_size] = 0;
	*len = st.st_size;
	return b;
}

static char *load(const char *name, size_t max, size_t *len)
{
	char path[1024];
	char *b;

	snprintf(path, sizeof(path), "%s/%s", NGAGE_DEV_DIR, name);
	if (!access(path, F_OK))
	{
		b = read_file(path, max, len, 1);
		flog("dev %s %s", path, b ? "used" : "refused");
		if (b) return b;
	}
	snprintf(path, sizeof(path), "%s/%s", HomeDir(), name);
	return read_file(getFullPath(path), max, len, 0);
}

// the card list carries no firmware words; on any failure the stripped text stays so the start refuses it
static int fill_patch()
{
	char rom[1024];
	unsigned long t0 = GetTimer(0);
	size_t rl = 0, nw = 0;
	uint32_t *w = (uint32_t *)malloc(PL_PATCH_SIZE);
	char *out = (char *)malloc(NGAGE_TEXT_MAX);
	char *r;
	long n;

	if (!w || !out)
	{
		flog("patch: out of memory");
		free(w);
		free(out);
		return 0;
	}
	if (ngl_patch_parse(patch_buf, patch_len, w, PL_PATCH_SIZE / 4, &nw) != NGL_P_ROM)
	{
		free(w);
		free(out);
		return 0;
	}
	snprintf(rom, sizeof(rom), "%s/%s", HomeDir(), NGAGE_BASE_ROM);
	r = read_file(getFullPath(rom), NGAGE_ROM_MAX, &rl, 0);
	if (!r)
	{
		flog("patch: cannot read %s", rom);
		free(w);
		free(out);
		return 0;
	}
	n = ngl_patch_expand(patch_buf, patch_len, (const uint8_t *)r, rl, w, PL_PATCH_SIZE / 4, out, NGAGE_TEXT_MAX);
	free(r);
	free(w);
	if (n < 0)
	{
		flog("patch: fill failed, code %ld", n);
		free(out);
		if (n == NGL_P_ROM) return 0;
		info("bad patch list (%ld)", n);
		return 1;
	}
	free(patch_buf);
	patch_buf = out;
	patch_len = (size_t)n;
	flog("patch: filled from %s in %lu ms", NGAGE_BASE_ROM, GetTimer(0) - t0);
	return 0;
}

static int load_start_files()
{
	free(mon_buf);
	free(patch_buf);
	mon_buf = load(NGAGE_MON, PL_MON_SIZE, &mon_len);
	patch_buf = load(NGAGE_PATCH, NGAGE_TEXT_MAX, &patch_len);
	if (!mon_buf || !patch_buf)
	{
		info("no %s in %s", !mon_buf ? NGAGE_MON : NGAGE_PATCH, HomeDir());
		return -1;
	}
	if (fill_patch()) return -1;
	return 0;
}

static void rom_names(const uint8_t *rom, size_t len)
{
	// no ROM name set means no lib is added ("libs: skipped (rom: none)")
	if (!rom)
	{
		rom_libs.n = 0;
		snprintf(rom_libs.err, sizeof(rom_libs.err), "none");
		return;
	}
	ngl_romdir_list(rom, len, NGL_ROMDIR_LIBS, &rom_libs);
}

static int has_ext(const char *path, const char *ext)
{
	size_t l = strlen(path), e = strlen(ext);
	return l > e && !strcasecmp(path + l - e, ext);
}

static int build(const char *full, char *starter)
{
	static struct ngl_sel sel;
	static struct ngl_alias al;
	static struct ngl_sorted sv;
	static struct ngl_libs lb;
	struct ngl_game_src gs, ls;
	struct ngl_report rep;
	struct ngl_src *s, *lsrc = NULL;
	struct stat st;
	const char *ld;
	char err[160], lp[1024], line[NGL_LIBS_LINE];
	int rc;

	s = ngl_src_open(full, err, sizeof(err));
	if (!s)
	{
		info("%s", err);
		return -1;
	}
	gs.ops = &ngl_src_ops;
	gs.ctx = s;
	if (ngl_game_select(&gs, &sel))
	{
		info("%s", sel.err);
		ngl_src_close(s);
		return -1;
	}
	// the card's E:\System\Libs; with no such folder the game source goes on as it is
	snprintf(lp, sizeof(lp), "%s/System/Libs", HomeDir());
	ld = getFullPath(lp);
	line[0] = 0;
	if (!stat(ld, &st) && S_ISDIR(st.st_mode) && !(lsrc = ngl_src_open_top(ld, err, sizeof(err))))
		snprintf(line, sizeof(line), "libs: skipped (%s)", err);
	ls.ops = &ngl_src_ops;
	ls.ctx = lsrc;
	// a name the ROM holds is resolved from Z:, so a working game's image and save stay as they are
	ngl_libs_wrap(&gs, lsrc ? &ls : NULL, &rom_libs, &sel, &lb);
	if (!line[0]) ngl_libs_line(&lb, line, sizeof(line));
	flog("%s", line);
	flog("libs %s", lb.rom_why);
	if (ngl_alias_wrap(&lb.src, &sel, &al))
	{
		info("no alias files for %s", sel.dir);
		ngl_src_close(lsrc);
		ngl_src_close(s);
		return -1;
	}
	// one zeroed 64 MiB buffer per game; calloc keeps untouched clusters out of RAM
	ngl_sink_mem_free(&img);
	if (ngl_sorted_view(&al.src, &sv) || ngl_sink_mem_init(&img))
	{
		info("out of memory");
		ngl_sorted_free(&sv);
		ngl_src_close(lsrc);
		ngl_src_close(s);
		return -1;
	}
	ProgressMessage("N-Gage", "Building the card image", 0, 1);
	memset(&rep, 0, sizeof(rep));
	rc = ngl_fat_build(sv.src.ops, sv.src.ctx, &img.sink, &rep);
	ProgressMessage("N-Gage", "Building the card image", 1, 1);
	ngl_sorted_free(&sv);
	ngl_src_close(lsrc);
	ngl_src_close(s);
	if (rc)
	{
		ngl_sink_mem_free(&img);
		info("image: %s", rep.err);
		return -1;
	}
	strcpy(starter, sel.path);
	flog("image %s: app %s/%s path %s alias %s files %u dirs %u clusters %u", full, sel.dir, sel.app, sel.path,
		sel.alias[0] ? sel.alias : "-", rep.files, rep.dirs, rep.clusters);
	return 0;
}

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

static int sav_exists(void *, const char *path)
{
	struct stat st;
	return !lstat(path, &st) || errno != ENOENT;
}

static int sav_aside(uint32_t id, int st)
{
	char aside[1100];

	if (ngl_sav_aside_name(sav_path, id, sav_exists, NULL, aside, sizeof(aside)))
	{
		flog("sav: no free aside name for %s", sav_path);
		info("cannot set %s aside, saves off", base_name(sav_path));
		return -1;
	}
	if (rename(sav_path, aside))
	{
		flog("sav: aside %s -> %s failed: %s", sav_path, aside, strerror(errno));
		info("cannot set %s aside, saves off", base_name(sav_path));
		return -1;
	}
	flog("sav: %s %s set aside as %s", ngl_sav_str(st), sav_path, aside);
	if (st == NGL_SAV_MISMATCH) info("game changed, old save kept as %s", base_name(aside));
	else info("save damaged, kept as %s", base_name(aside));
	return 0;
}

static void sav_open(const char *full)
{
	struct ngl_sav_info si;
	struct stat st;
	unsigned long t0 = GetTimer(0);
	uint8_t hdr[NGL_SAV_HDR_BYTES];
	uint32_t applied = 0;
	size_t len = 0;
	char *b = NULL;
	int rc;

	ngl_sav_init(&sav, sav_bm);
	sav_on = 0;
	sav_err = 0;
	sav_writes = 0;
	sav_bytes = 0;
	sav_id = ngl_sav_image_id(img.buf);
	flog("sav: id 0x%08x in %lu ms", (unsigned)sav_id, GetTimer(0) - t0);
	snprintf(sav_core, sizeof(sav_core), "%s", CoreName2);
	snprintf(sav_dir, sizeof(sav_dir), "%s/%s", SAVE_DIR, sav_core);
	snprintf(sav_dir, sizeof(sav_dir), "%s", getFullPath(sav_dir));
	if (ngl_sav_path(sav_dir, full, sav_path, sizeof(sav_path)))
	{
		info("save name too long, saves off");
		return;
	}
	memset(&si, 0, sizeof(si));
	if (lstat(sav_path, &st))
	{
		if (errno != ENOENT)
		{
			flog("sav: %s: %s", sav_path, strerror(errno));
			info("cannot read %s, saves off", base_name(sav_path));
			return;
		}
		rc = NGL_SAV_ABSENT;
	}
	else if (!S_ISREG(st.st_mode)) rc = NGL_SAV_FOREIGN;
	else if (!st.st_size) rc = NGL_SAV_ABSENT;
	else if (st.st_size > (off_t)NGL_SAV_MAX_FILE)
	{
		// only the header: the id names the aside file, a foreign file stays untouched
		FILE *f = fopen(sav_path, "rb");
		rc = f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) ? ngl_sav_parse(hdr, sizeof(hdr), sav_id, &si) : -1;
		if (f) fclose(f);
		if (rc >= 0 && rc != NGL_SAV_FOREIGN) rc = NGL_SAV_CORRUPT;
	}
	else
	{
		b = read_file(sav_path, NGL_SAV_MAX_FILE, &len, 0);
		rc = -1;
		if (b)
		{
			ngl_sav_parse((const uint8_t *)b, len, sav_id, &si);
			rc = ngl_sav_apply(&sav, img.buf, (const uint8_t *)b, len, sav_id, &applied);
		}
	}
	free(b);
	switch (rc)
	{
	case NGL_SAV_OK:
		sav_on = 1;
		flog("sav: applied %u from %s (%u bytes)", (unsigned)applied, sav_path, (unsigned)len);
		break;
	case NGL_SAV_ABSENT:
		sav_on = 1;
		flog("sav: %s absent, empty overlay", sav_path);
		break;
	case NGL_SAV_MISMATCH:
	case NGL_SAV_CORRUPT:
		flog("sav: %s %s, file id 0x%08x records %u", sav_path, ngl_sav_str(rc), (unsigned)si.id, (unsigned)si.count);
		sav_on = !sav_aside(si.id, rc);
		break;
	case -1:
		flog("sav: cannot read %s", sav_path);
		info("cannot read %s, saves off", base_name(sav_path));
		break;
	default:
		flog("sav: %s %s, saves off", sav_path, ngl_sav_str(rc));
		info("%s is not an N-Gage save, saves off", base_name(sav_path));
		break;
	}
}

static int fd_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
	int fd = *(int *)ctx;
	const uint8_t *p = (const uint8_t *)buf;

	while (len)
	{
		ssize_t n = pwrite(fd, p, len, (off_t)off);
		if (n <= 0) return -1;
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int bg_start(struct bgjob *j, void *(*fn)(void *))
{
	int rc;

	j->done = 0;
	j->stop = 0;
	rc = pthread_create(&j->th, NULL, fn, NULL);
	j->on = !rc;
	errno = rc;
	return rc ? -1 : 0;
}

static void bg_end(struct bgjob *j, int rc)
{
	j->rc = rc;
	j->err = errno;
	__atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
}

static void bg_join(struct bgjob *j)
{
	if (j->on) pthread_join(j->th, NULL);
	j->on = 0;
}

static int bg_running(struct bgjob *j)
{
	if (!j->on) return 0;
	if (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) return 1;
	bg_join(j);
	return 0;
}

static int sav_mem_write(void *, uint64_t off, const void *buf, uint32_t len)
{
	if (off > sw.len || len > sw.len - off) return -1;
	memcpy(sw.buf + off, buf, len);
	return 0;
}

static int sav_io()
{
	char tmp[1100];
	int fd, dfd, rc;

	snprintf(tmp, sizeof(tmp), "%s.tmp", sav_path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	rc = fd < 0 || fd_write(&fd, 0, sw.buf, sw.len) || fsync(fd);
	if (fd >= 0 && close(fd)) rc = 1;
	if (!rc && rename(tmp, sav_path)) rc = 1;
	if (rc)
	{
		sw.err = errno;
		return -1;
	}
	// vfat keeps the new dir entry in cache until writeback: a power-off could lose the rename
	dfd = open(sav_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	sw.dir_bad = dfd < 0 || fsync(dfd);
	sw.dir_err = errno;
	if (dfd >= 0) close(dfd);
	return 0;
}

static void *sav_worker(void *)
{
	bg_end(&sw.j, sav_io());
	return NULL;
}

static int sav_end(int rc)
{
	free(sw.buf);
	sw.buf = NULL;
	if (rc)
	{
		ngl_sav_flush_failed(&sav, sw.now, sw.scheduled);
		errno = sw.err;
		flog("sav: flush %s failed: %s", sw.why, strerror(errno));
		if (!sav_err++) info("cannot write %s", base_name(sav_path));
		return -1;
	}
	if (sw.dir_bad)
	{
		errno = sw.dir_err;
		flog("sav: flush %s dir sync failed: %s", sw.why, strerror(errno));
	}
	ngl_sav_flushed(&sav, sw.now, sw.scheduled);
	flog("sav: flush %s records %u bytes %u writes %u (%u bytes) in %lu ms, scheduled %u", sw.why, (unsigned)sw.recs,
		(unsigned)sw.len, (unsigned)sw.writes, (unsigned)sw.bytes, GetTimer(0) - sw.t0, (unsigned)sav.sched_flushes);
	sav_writes -= sw.writes;
	sav_bytes -= sw.bytes;
	// guest writes stored while the worker wrote the file are not in it
	if (sav.last_write_ms != sw.last_write) sav.pending = 1;
	return 0;
}

static int sav_flush(const char *why, int scheduled)
{
	struct ngl_sink sk;
	uint32_t now = (uint32_t)GetTimer(0);
	int rc;

	if (sw.j.on)
	{
		if (scheduled && bg_running(&sw.j)) return 0;
		bg_join(&sw.j);
		rc = sav_end(sw.j.rc);
		if (scheduled) return rc;
	}
	if (!own || !img.buf || !sav_on || !ngl_sav_due(&sav, now, !scheduled)) return 0;
	sw.why = why;
	sw.scheduled = scheduled;
	sw.now = now;
	sw.t0 = GetTimer(0);
	sw.recs = sav.dirty;
	sw.len = ngl_sav_size(&sav);
	sw.writes = sav_writes;
	sw.bytes = sav_bytes;
	sw.last_write = sav.last_write_ms;
	sw.dir_bad = 0;
	create_path(SAVE_DIR, sav_core);
	sw.buf = (uint8_t *)malloc(sw.len);
	sk.write = sav_mem_write;
	sk.ctx = NULL;
	if (!sw.buf || ngl_sav_serialize(&sav, img.buf, sav_id, &sk))
	{
		sw.err = errno;
		return sav_end(-1);
	}
	if (!scheduled) return sav_end(sav_io());
	// the guest MMC driver times out after 60 ms and this thread serves its reads: the card IO runs on a worker
	if (bg_start(&sw.j, sav_worker))
	{
		sw.err = errno;
		return sav_end(-1);
	}
	return 0;
}

static void sav_drops_log()
{
	if (sav_drops) flog("sav: dropped %u writes with no own image", (unsigned)sav_drops);
	sav_drops = 0;
}

static int fd_read(int fd, uint8_t *buf, size_t len)
{
	while (len)
	{
		ssize_t n = read(fd, buf, len);
		if (n <= 0) return -1;
		buf += n;
		len -= n;
	}
	return 0;
}

static void cdrv_dir_sync(const char *why)
{
	int dfd = open(cdrv_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (dfd < 0 || fsync(dfd)) flog("cdrv: %s dir sync failed: %s", why, strerror(errno));
	if (dfd >= 0) close(dfd);
}

static int cdrv_copy(const uint8_t *b, const char *aside)
{
	int fd = open(aside, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	int rc = fd < 0 || fd_write(&fd, 0, b, NGL_CDRV_FILE_BYTES) || fsync(fd);

	if (fd >= 0 && close(fd)) rc = 1;
	if (rc) return -1;
	cdrv_dir_sync("aside");
	return 0;
}

static uint32_t cdrv_row(const struct ngl_io *p)
{
	uint32_t key = 'M' << 24 | 'K' << 16 | '0' << 8 | ('0' + fk.cdrv_slot);

	if (!cdrv_run || fk.cdrv_slot < 0) return 0;
	for (uint32_t i = 0; i < NGST_NPROBE; i++)
		if (p->rd32(p->ctx, NGL_ST(probe) + 8u * i) == key) return p->rd32(p->ctx, NGL_ST(probe) + 8u * i + 4u);
	// a monitor without the row cannot prove the C: was mounted: keep the loaded C: aside
	return 1;
}


static int cdrv_guard(const struct ngl_io *p, const char *why, uint32_t *row)
{
	char aside[1100];

	*row = cdrv_row(p);
	if (cdrv_guarded || ngl_cdrv_guard(cdrv_act, cdrv_nonblank, *row) != NGL_CDRV_ASIDE_RENAME) return 0;
	cdrv_guarded = 1;
	if (ngl_sav_aside_name(cdrv_path, cdrv_fid, sav_exists, NULL, aside, sizeof(aside)) || rename(cdrv_path, aside))
	{
		cdrv_on = 0;
		flog("cdrv: guard %s slot %d row %u: aside of %s failed: %s, persistence off", why, fk.cdrv_slot, (unsigned)*row,
			cdrv_path, strerror(errno));
		info("cannot set %s aside, C: saves off", NGL_CDRV_NAME);
		return -1;
	}
	flog("cdrv: guard %s slot %d row %u: %s set aside as %s", why, fk.cdrv_slot, (unsigned)*row, cdrv_path, aside);
	info("C: was reformatted, old C: kept as %s", base_name(aside));
	return 0;
}

static uint32_t cdrv_read_gen(const struct ngl_io *p)
{
	// before the first start of this core session the status page may hold another session's word
	return cdrv_run ? p->rd32(p->ctx, NGL_ST(cdrv_gen)) : 0;
}

static void cdrv_begin(const char *why, int scheduled, int phase, uint32_t row, uint32_t now)
{
	cf.phase = phase;
	cf.scheduled = scheduled;
	cf.why = why;
	cf.fd = -1;
	cf.off = 0;
	cf.gen = 0;
	cf.crc = 0;
	cf.row = row;
	cf.now = now;
	cf.tries = 0;
	cf.t0 = GetTimer(0);
	cf.slow = 0;
	cf.wt = 0;
}

static int cdrv_fail(const char *what)
{
	if (cf.fd >= 0) close(cf.fd);
	cf.fd = -1;
	cf.phase = CF_IDLE;
	ngl_cdrv_flush_failed(&cdrv_sch, cf.now, cf.scheduled);
	flog("cdrv: flush %s %s: gen %u tries %u", cf.why, what, (unsigned)cf.gen, cf.tries);
	if (!cf.scheduled && !cdrv_err++) info("cannot write %s", NGL_CDRV_NAME);
	return -1;
}

static int cdrv_end(int rc, int err)
{
	if (rc > 0) return 1;
	if (rc < 0)
	{
		errno = err;
		flog("cdrv: %s write failed: %s", cf.why, strerror(errno));
		return cdrv_fail("failed");
	}
	cdrv_fid = cdrv_rom;
	cf.phase = CF_IDLE;
	ngl_cdrv_flushed(&cdrv_sch, cf.gen, cf.now, cf.scheduled);
	flog("cdrv: flush %s guard row %u gen %u tries %u bytes %u in %lu ms, max step %lu ms, scheduled %u", cf.why,
		(unsigned)cf.row, (unsigned)cf.gen, cf.tries, (unsigned)NGL_CDRV_FILE_BYTES, GetTimer(0) - cf.t0, cf.slow,
		(unsigned)cdrv_sch.sched_flushes);
	return 0;
}

static void cdrv_abort(const char *why)
{
	if (cdrv_job.on)
	{
		__atomic_store_n(&cdrv_job.stop, 1, __ATOMIC_RELAXED);
		bg_join(&cdrv_job);
		// the worker stops only before the rename: a renamed file is a finished flush
		if (cdrv_job.rc <= 0) cdrv_end(cdrv_job.rc, cdrv_job.err);
	}
	if (cf.phase == CF_IDLE) return;
	if (cf.fd >= 0) close(cf.fd);
	cf.fd = -1;
	flog("cdrv: flush %s aborted by %s", cf.why, why);
	cf.phase = CF_IDLE;
}

static void cdrv_pace()
{
	unsigned long d = GetTimer(0) - cf.wt;

	if (d < NGAGE_POLL_MS) usleep((NGAGE_POLL_MS - d) * 1000);
}

static int cdrv_io(int *phase)
{
	char tmp[1100];
	uint32_t n;

	snprintf(tmp, sizeof(tmp), "%s.tmp", cdrv_path);
	switch (*phase)
	{
	case CF_WRITE:
		if (cf.scheduled) cdrv_pace();
		if (!cf.off)
		{
			cf.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
			if (cf.fd < 0) return -1;
		}
		n = NGL_CDRV_FILE_BYTES - cf.off < NGAGE_CDRV_WSTEP ? NGL_CDRV_FILE_BYTES - cf.off : NGAGE_CDRV_WSTEP;
		if (fd_write(&cf.fd, cf.off, cdrv_buf + cf.off, n)) return -1;
		// starts the card write of this chunk; one chunk per NGAGE_POLL_MS leaves the final fsync only the metadata
		if (cf.scheduled) posix_fadvise(cf.fd, cf.off, n, POSIX_FADV_DONTNEED);
		cf.wt = GetTimer(0);
		cf.off += n;
		if (cf.off == NGL_CDRV_FILE_BYTES) *phase = CF_SYNC;
		return 1;
	case CF_SYNC:
		if (cf.scheduled) cdrv_pace();
		if (fsync(cf.fd))
		{
			close(cf.fd);
			cf.fd = -1;
			return -1;
		}
		if (close(cf.fd))
		{
			cf.fd = -1;
			return -1;
		}
		cf.fd = -1;
		*phase = CF_RENAME;
		return 1;
	case CF_RENAME:
		if (rename(tmp, cdrv_path)) return -1;
		*phase = CF_DIRSYNC;
		return 1;
	}
	cdrv_dir_sync(cf.why);
	return 0;
}

static void *cdrv_worker(void *)
{
	int rc;

	while ((rc = cdrv_io(&cf.io)) > 0 && (cf.io == CF_DIRSYNC || !__atomic_load_n(&cdrv_job.stop, __ATOMIC_RELAXED))) {}
	bg_end(&cdrv_job, rc);
	return NULL;
}

static int cdrv_step(const struct ngl_io *p)
{
	int rc;

	switch (cf.phase)
	{
	case CF_IDLE:
		return 0;
	case CF_COPY:
		if (!cf.off)
		{
			if (cf.tries >= NGAGE_CDRV_TRIES) return cdrv_fail("deferred, busy");
			cf.tries++;
			cf.crc = 0;
			cf.gen = cdrv_read_gen(p);
			if (cf.gen & 1u) return 1;
		}
		if (p->mem_read(p->ctx, PL_CDRV_BASE + cf.off, cdrv_buf + NGL_CDRV_HDR_BYTES + cf.off, NGAGE_CDRV_STEP))
			return cdrv_fail("deferred, read error");
		cf.crc = mz_crc32(cf.crc, cdrv_buf + NGL_CDRV_HDR_BYTES + cf.off, NGAGE_CDRV_STEP);
		cf.off += NGAGE_CDRV_STEP;
		if (cf.off < NGL_CDRV_PAYLOAD_BYTES) return 1;
		cf.off = 0;
		// seqlock: the copy holds only if no C: write moved the even gen from the first chunk to the last
		if (cdrv_read_gen(p) != cf.gen) return 1;
		ngl_cdrv_hdr(cdrv_buf, cdrv_rom, (uint32_t)cf.crc);
		cf.phase = CF_WRITE;
		return 1;
	case CF_BG:
		if (bg_running(&cdrv_job)) return 1;
		return cdrv_end(cdrv_job.rc, cdrv_job.err);
	}
	if (cf.phase == CF_WRITE && !cf.off)
	{
		create_path(SAVE_DIR, cdrv_core);
		// the guest MMC driver times out after 60 ms and this thread serves its reads: the card IO runs on a worker
		if (cf.scheduled)
		{
			cf.io = CF_WRITE;
			cf.phase = CF_BG;
			return bg_start(&cdrv_job, cdrv_worker) ? cdrv_end(-1, errno) : 1;
		}
	}
	rc = cdrv_io(&cf.phase);
	return cdrv_end(rc, errno);
}

static int cdrv_tick(const struct ngl_io *p)
{
	unsigned long t0 = GetTimer(0);
	int rc = cdrv_step(p);

	if (GetTimer(0) - t0 > cf.slow) cf.slow = GetTimer(0) - t0;
	return rc;
}

static int cdrv_flush(const char *why, int scheduled)
{
	const struct ngl_io *p = io_get();
	uint32_t now = (uint32_t)GetTimer(0), row = 0;
	int rc;

	if (!cdrv_on || !p) return 0;
	if (scheduled && cf.phase != CF_IDLE) return 0;
	if (!scheduled) cdrv_abort(why);
	if (!scheduled && cdrv_run) ngl_cdrv_observe(&cdrv_sch, p->rd32(p->ctx, NGL_ST(cdrv_gen)), now);
	if (!ngl_cdrv_due(&cdrv_sch, now, !scheduled)) return 0;
	if (cdrv_guard(p, why, &row)) return -1;
	cdrv_begin(why, scheduled, CF_COPY, row, now);
	// a 4 MiB flush in one poll held the E: service for ~0.9 s; ngage_poll runs one step per call
	if (scheduled) return 0;
	while ((rc = cdrv_tick(p)) > 0) {}
	return rc;
}

static int svcrom_failed;

static int svcrom_put(const char *part, const uint8_t *out)
{
	uint8_t chunk[65536];
	size_t off = 0;
	ssize_t n;
	int fd, rc, ok = 1;

	fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	rc = fd < 0 || fd_write(&fd, 0, out, NGL_SVCROM_SIZE) || fsync(fd);
#ifdef POSIX_FADV_DONTNEED
	// the pages are clean after fsync: drop them so the read-back below comes from the card
	if (!rc && (n = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))) flog("svcrom: fadvise %d", (int)n);
#endif
	if (fd >= 0 && close(fd)) rc = 1;
	if (rc) return -1;
	fd = open(part, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	while (ok)
	{
		n = read(fd, chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		if (off + (size_t)n > NGL_SVCROM_SIZE || memcmp(chunk, out + off, (size_t)n)) ok = 0;
		off += (size_t)n;
	}
	if (n < 0) ok = 0;
	close(fd);
	return ok && off == NGL_SVCROM_SIZE ? 0 : -1;
}

static int svcrom_run(char **in, const char *path)
{
	char part[1040], dir[1024];
	uint8_t *out = (uint8_t *)malloc(NGL_SVCROM_SIZE);
	int rc, dfd;

	info("building base.rom");
	if (!out)
	{
		flog("svcrom: out of memory");
		info("base.rom build failed");
		return -1;
	}
	rc = ngl_svcrom_build((const uint8_t *)in[0], NGL_SVCROM_3C1_LEN, (const uint8_t *)in[1], NGL_SVCROM_326_LEN, out,
		NGL_SVCROM_SIZE);
	free(in[0]);
	free(in[1]);
	in[0] = in[1] = NULL;
	if (rc)
	{
		flog("svcrom: build: %s", ngl_svcrom_strerror(rc));
		info("base.rom build failed");
		free(out);
		return -1;
	}
	if (ngl_svcrom_check(out, NGL_SVCROM_SIZE, NGL_SVCROM_OUT))
	{
		flog("svcrom: image check failed");
		info("base.rom build failed");
		free(out);
		return -1;
	}
	snprintf(part, sizeof(part), "%s.part", path);
	rc = svcrom_put(part, out);
	if (rc) flog("svcrom: write %s failed", part);
	else if (rename(part, path))
	{
		flog("svcrom: rename %s: %s", part, strerror(errno));
		rc = -1;
	}
	free(out);
	if (rc)
	{
		unlink(part);
		info("cannot write base.rom");
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s", getFullPath(HomeDir()));
	// vfat keeps the new dir entry in cache until writeback: a power-off could lose the rename
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0 || fsync(dfd)) flog("svcrom: dir sync failed: %s", strerror(errno));
	if (dfd >= 0) close(dfd);
	return 0;
}

static void svcrom_make()
{
	static const char *const nm[2] = {"nem440.3c1", "nem440.326"};
	static const size_t want[2] = {NGL_SVCROM_3C1_LEN, NGL_SVCROM_326_LEN};
	unsigned long t0 = GetTimer(0);
	char path[1024], part[1040], full[2][1024];
	char *in[2] = {NULL, NULL};
	struct stat st;
	struct statvfs sv;
	size_t len = 0;
	int i, err, miss = 0;

	svcrom_failed = 0;
	snprintf(path, sizeof(path), "%s/%s", HomeDir(), NGAGE_BASE_ROM);
	snprintf(path, sizeof(path), "%s", getFullPath(path));
	snprintf(part, sizeof(part), "%s.part", path);
	if (!lstat(part, &st) && S_ISREG(st.st_mode))
	{
		if (unlink(part)) flog("svcrom: unlink %s: %s", part, strerror(errno));
		else flog("svcrom: removed stale %s", part);
	}
	err = lstat(path, &st) ? errno : 0;
	if (err != ENOENT)
	{
		if (err) flog("svcrom: %s stat error %d", NGAGE_BASE_ROM, err);
		else flog("svcrom: %s present", NGAGE_BASE_ROM);
		return;
	}
	for (i = 0; i < 2; i++)
	{
		snprintf(full[i], sizeof(full[i]), "%s/%s", HomeDir(), nm[i]);
		snprintf(full[i], sizeof(full[i]), "%s", getFullPath(full[i]));
		if (access(full[i], F_OK)) miss |= 1 << i;
	}
	if (miss == 3)
	{
		flog("svcrom: no service files");
		return;
	}
	if (miss)
	{
		info("no %s", nm[miss - 1]);
		svcrom_failed = 1;
		return;
	}
	if (statvfs(getFullPath(HomeDir()), &sv)) flog("svcrom: statvfs: %s", strerror(errno));
	else if (sv.f_flag & ST_RDONLY)
	{
		info("card is read-only");
		svcrom_failed = 1;
		return;
	}
	else if ((uint64_t)sv.f_bavail * sv.f_frsize < NGL_SVCROM_SIZE)
	{
		info("no space for base.rom");
		svcrom_failed = 1;
		return;
	}
	for (i = 0; i < 2; i++)
	{
		in[i] = read_file(full[i], want[i], &len, 0);
		if (in[i] && len == want[i] && !ngl_svcrom_check((const uint8_t *)in[i], len, i)) continue;
		info("bad %s", nm[i]);
		svcrom_failed = 1;
		break;
	}
	if (!svcrom_failed && svcrom_run(in, path)) svcrom_failed = 1;
	free(in[0]);
	free(in[1]);
	if (svcrom_failed) return;
	info("base.rom ready");
	flog("svcrom: built in %lu ms", GetTimer(0) - t0);
}

static void cdrv_open()
{
	const struct ngl_io *p = io_get();
	struct ngl_cdrv_info ci;
	struct ngl_cdrv_action a;
	struct stat st;
	char rom[1024], aside[1100] = "";
	unsigned long t0 = GetTimer(0);
	uint8_t *b = NULL, *rb = NULL;
	uint32_t crc, got = 0;
	size_t len = 0, n;
	char *r;
	int rc, fd, bad;

	free(cdrv_buf);
	cdrv_buf = NULL;
	cdrv_on = 0;
	cdrv_run = 0;
	cdrv_nonblank = 0;
	cdrv_guarded = 0;
	cdrv_err = 0;
	memset(&cdrv_sch, 0, sizeof(cdrv_sch));
	rom_names(NULL, 0);
	if (!p) return;
	snprintf(rom, sizeof(rom), "%s/%s", HomeDir(), NGAGE_BASE_ROM);
	r = read_file(getFullPath(rom), NGAGE_ROM_MAX, &len, 0);
	if (!r)
	{
		flog("cdrv: cannot read %s, route off", rom);
		if (!svcrom_failed) info("cannot read %s, C: off", NGAGE_BASE_ROM);
		return;
	}
	cdrv_rom = ngl_sav_crc32(r, len);
	rom_names((const uint8_t *)r, len);
	free(r);
	flog("cdrv: rom id 0x%08x in %lu ms", (unsigned)cdrv_rom, GetTimer(0) - t0);
	snprintf(cdrv_core, sizeof(cdrv_core), "%s", CoreName2);
	snprintf(cdrv_dir, sizeof(cdrv_dir), "%s/%s", SAVE_DIR, cdrv_core);
	snprintf(cdrv_dir, sizeof(cdrv_dir), "%s", getFullPath(cdrv_dir));
	b = cdrv_buf = (uint8_t *)malloc(NGL_CDRV_FILE_BYTES);
	rb = (uint8_t *)malloc(NGL_CDRV_PAYLOAD_BYTES);
	if (ngl_cdrv_path(cdrv_dir, NULL, cdrv_path, sizeof(cdrv_path)) || !b || !rb)
	{
		flog("cdrv: %s, route off", !b || !rb ? "out of memory" : "path too long");
		info("C: off");
		free(b);
		free(rb);
		cdrv_buf = NULL;
		return;
	}

	t0 = GetTimer(0);
	memset(&ci, 0, sizeof(ci));
	if (lstat(cdrv_path, &st)) rc = errno == ENOENT ? NGL_CDRV_ABSENT : NGL_CDRV_IOERR;
	else if (!S_ISREG(st.st_mode)) rc = NGL_CDRV_FOREIGN;
	else if (!st.st_size) rc = NGL_CDRV_ABSENT;
	else
	{
		n = st.st_size < (off_t)NGL_CDRV_FILE_BYTES ? (size_t)st.st_size : NGL_CDRV_FILE_BYTES;
		fd = open(cdrv_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) rc = NGL_CDRV_IOERR;
		else if (fd_read(fd, b, n)) rc = NGL_CDRV_IOERR;
		// any other size is judged by its header only: FOREIGN stays untouched, the rest is damaged
		else rc = ngl_cdrv_parse(b, st.st_size == (off_t)NGL_CDRV_FILE_BYTES ? n : n < NGL_CDRV_HDR_BYTES ? n :
			NGL_CDRV_HDR_BYTES, cdrv_rom, &ci);
		if (fd >= 0) close(fd);
	}
	a = ngl_cdrv_action(rc);
	cdrv_act = a;
	cdrv_fid = ci.id;
	flog("cdrv: %s %s file id 0x%08x crc 0x%08x in %lu ms", cdrv_path, ngl_cdrv_str(rc), (unsigned)ci.id,
		(unsigned)ci.crc, GetTimer(0) - t0);

	t0 = GetTimer(0);
	if (!a.use_payload) memset(b + NGL_CDRV_HDR_BYTES, 0xFF, NGL_CDRV_PAYLOAD_BYTES);
	crc = a.use_payload ? ci.crc : ngl_sav_crc32(b + NGL_CDRV_HDR_BYTES, NGL_CDRV_PAYLOAD_BYTES);
	for (uint32_t i = 0; a.use_payload && !cdrv_nonblank && i < NGL_CDRV_PAYLOAD_BYTES; i++)
		cdrv_nonblank = b[NGL_CDRV_HDR_BYTES + i] != 0xFF;
	bad = p->mem_write(p->ctx, PL_CDRV_BASE, b + NGL_CDRV_HDR_BYTES, NGL_CDRV_PAYLOAD_BYTES) ||
		p->mem_read(p->ctx, PL_CDRV_BASE, rb, NGL_CDRV_PAYLOAD_BYTES);
	if (!bad) got = ngl_sav_crc32(rb, NGL_CDRV_PAYLOAD_BYTES);
	free(rb);
	flog("cdrv: load %s nonblank %d in %lu ms, readback crc 0x%08x want 0x%08x", a.use_payload ? "payload" : "ff",
		cdrv_nonblank, GetTimer(0) - t0, (unsigned)got, (unsigned)crc);
	if (bad || got != crc)
	{
		flog("cdrv: shadow %s, route off", bad ? "access failed" : "readback mismatch");
		info("C: memory check failed, C: off");
		free(b);
		cdrv_buf = NULL;
		return;
	}

	cdrv_on = a.persist;
	if (cdrv_on && a.aside != NGL_CDRV_ASIDE_NONE)
	{
		if (ngl_sav_aside_name(cdrv_path, ci.id, sav_exists, NULL, aside, sizeof(aside)) ||
			(a.aside == NGL_CDRV_ASIDE_RENAME ? rename(cdrv_path, aside) : cdrv_copy(b, aside)))
		{
			cdrv_on = 0;
			flog("cdrv: aside of %s to %s failed: %s, persistence off", cdrv_path, aside[0] ? aside : "-",
				strerror(errno));
			info("cannot set %s aside, C: saves off", NGL_CDRV_NAME);
		}
		else flog("cdrv: %s %s set aside as %s by %s", ngl_cdrv_str(rc), cdrv_path, aside,
			a.aside == NGL_CDRV_ASIDE_RENAME ? "rename" : "copy");
	}
	if (a.info == NGL_CDRV_INFO_ROM_CHANGED && cdrv_on) info("ROM changed, C: kept, backup %s", base_name(aside));
	else if (a.info == NGL_CDRV_INFO_DAMAGED && cdrv_on) info("C: damaged, kept as %s", base_name(aside));
	else if (a.info == NGL_CDRV_INFO_FOREIGN) info("%s is not an N-Gage C:, C: saves off", NGL_CDRV_NAME);
	else if (a.info == NGL_CDRV_INFO_CANNOT_READ) info("cannot read %s, C: saves off", NGL_CDRV_NAME);
	if (cdrv_on && a.write_now)
	{
		t0 = GetTimer(0);
		ngl_cdrv_hdr(b, cdrv_rom, ci.crc);
		cdrv_begin("write-back", 0, CF_WRITE, 0, (uint32_t)GetTimer(0));
		while ((bad = cdrv_tick(p)) > 0) {}
		if (bad) ngl_cdrv_mark_pending(&cdrv_sch, (uint32_t)GetTimer(0));
		else flog("cdrv: write-back rom id 0x%08x crc 0x%08x in %lu ms", (unsigned)cdrv_rom, (unsigned)ci.crc,
			GetTimer(0) - t0);
	}
	if (!cdrv_on)
	{
		free(cdrv_buf);
		cdrv_buf = NULL;
	}
	fk.cdrv = 1;
	flog("cdrv: route on, persistence %s", cdrv_on ? "on" : "off");
}

int ngage_save_flush(const char *why)
{
	int rc = cdrv_flush(why, 0);
	return sav_flush(why, 0) || rc;
}

void ngage_recover()
{
	const struct ngl_io *p = io_get();
	int rc;

	if (!p) return;
	rc = ngl_cpu1_recover(p);
	flog("recover: %s", ngl_strerror(rc));
	if (rc) info("cpu1: %s", ngl_strerror(rc));
}

static void load_rom(const char *tag)
{
	char rom[1024];
	uint32_t addr = 0;

	for (int i = 2; ; i++)
	{
		char *p = user_io_get_confstr(i);
		if (!p) break;
		if (ngl_confstr_skip(p)[0] == 'F')
		{
			addr = ngl_confstr_load_addr(p);
			break;
		}
	}
	snprintf(rom, sizeof(rom), "%s/%s", HomeDir(), NGAGE_BASE_ROM);
	if (!FileExists(rom, 0))
	{
		flog("%s: no %s", tag, rom);
		return;
	}
	flog("%s: %s load_addr 0x%08x", tag, rom, (unsigned)addr);
	// field 3 of the core's own F entry picks the DDR path (h03) or the SPI path (h02)
	if (user_io_file_tx(rom, 0, 0, 0, 0, addr)) ngl_fork_rom(&fk);
	else info("cannot load %s", rom);
	flog("%s: %s done", tag, rom);
}

static int stop(const struct ngl_io *p, const char *tag)
{
	char state[16] = "-", reason[16] = "-";
	int rc = ngl_fork_stop(&fk, p);

	if (fk.stop_state != NGL_FORK_UNREAD) snprintf(state, sizeof(state), "%u", (unsigned)fk.stop_state);
	if (fk.stop_reason != NGL_FORK_UNREAD) snprintf(reason, sizeof(reason), "%u", (unsigned)fk.stop_reason);
	flog("%s: stop rc %s phase %d state %s park_reason %s", tag, ngl_strerror(rc), fk.stop_phase, state, reason);
	return rc;
}

static int fork_now(struct ngl_civil *c)
{
	return ngl_civil_local((int64_t)time(NULL), c);
}

void ngage_init()
{
	ngl_fork_init(&fk);
	fk.now = fork_now;
	own = 0;
	ngl_sink_mem_free(&img);
	flog("dev channel 1");
	svcrom_make();
	load_rom("init");
	cdrv_open();
}

static void select_body(int index, const char *path)
{
	const struct ngl_io *p = io_get();
	char starter[NGL_SLOT_CHARS + 1] = "";
	char full[1024];
	int unit;

	if (p)
	{
		int rc = stop(p, "select");
		if (rc) info("stop: %s", ngl_strerror(rc));
	}
	ngage_save_flush("select");
	sav_drops_log();
	ngl_fork_eject(&fk);
	own = 0;
	ngl_sink_mem_free(&img);
	flog("select %d %s", index, path ? path : "");
	if (index || !path || !path[0])
	{
		user_io_file_mount(path ? path : "", index);
		return;
	}

	snprintf(full, sizeof(full), "%s", getFullPath(path));
	if (PathIsDir(path, 0))
	{
		info("zip only");
		return;
	}
	unit = has_ext(path, ".zip");
	if (load_start_files())
	{
		if (!unit) user_io_file_mount(path, 0);
		return;
	}
	// the last session patched the ROM in DDR, the next one starts from a pristine copy
	if (ngl_fork_reload(&fk)) load_rom("reload");
	if (!unit)
	{
		// legacy: the image file goes the generic way, no Starter rewrite
		user_io_file_mount(path, 0);
		if (!FileExists(path, 0)) return;
	}
	else
	{
		if (build(full, starter)) return;
		sav_open(full);
		unlink(NGAGE_RAM_IMG);
		own = 1;
		user_io_file_mount(NGAGE_RAM_IMG, 0, 1, (int)NGL_IMG_BYTES);
	}
	if (ngl_fork_image(&fk, mon_buf, mon_len, patch_buf, patch_len, starter)) info("bad %s", NGAGE_MON);
}

void ngage_set_image(int index, const char *path)
{
	cid_timer = 0;
	// while a guest runs the fork waits for a quiet C: first, then ngage_poll runs the body on NGL_FE_SELECT
	if (ngl_fork_select_req(&fk, index, path)) return;
	select_body(index, path);
}

int ngage_sd_service(int disk, int op, uint32_t lba, int sz, int ack)
{
	static uint8_t buf[UIO_BUFFER_SIZE];
	uint64_t off = (uint64_t)lba * 512;

	if (disk) return 0;
	if (!own || !img.buf)
	{
		// a legacy image stays read-only; an undrained sd_wr would hold the core's MMC DMA
		if (op != 2 || !is_ngage()) return 0;
		if (sz < 0 || sz > (int)sizeof(buf)) return -1;
		EnableIO();
		spi_w(UIO_SECTOR_WR | ack);
		spi_block_read(buf, user_io_get_width(), sz);
		DisableIO();
		fk.mmc_writes++;
		if (!sav_drops++) flog("sav: write lba %u dropped, no own image", (unsigned)lba);
		return 1;
	}
	if (!is_ngage()) return 0;
	if (sz < 0 || sz > (int)sizeof(buf)) return -1;
	if (op == 2)
	{
		int rc;

		EnableIO();
		spi_w(UIO_SECTOR_WR | ack);
		spi_block_read(buf, user_io_get_width(), sz);
		DisableIO();
		fk.mmc_writes++;
		rc = ngl_sav_store(&sav, img.buf, off, buf, (uint32_t)sz, (uint32_t)GetTimer(0));
		if (!sav_writes++) flog("sav: write lba %u", (unsigned)lba);
		sav_bytes += sz;
		if (rc == NGL_SAV_FULL) info("save full (%u sectors), new sectors not kept", (unsigned)NGL_SAV_MAX_RECORDS);
		else if (rc) flog("sav: write lba %u size %d: %s", (unsigned)lba, sz, ngl_sav_str(rc));
	}
	else if (op & 1)
	{
		const uint8_t *src = buf;
		diskled_on();
		if (off + sz <= NGL_IMG_BYTES) src = img.buf + off;
		else memset(buf, 0, sz);
		EnableIO();
		spi_w(UIO_SECTOR_RD | ack);
		spi_block_write(src, user_io_get_width(), sz);
		DisableIO();
	}
	else return -1;
	return 1;
}

static int dev_path_ok(const char *path)
{
	if (strncmp(path, "/media/fat/", 11) && strncmp(path, NGAGE_LOG_DIR "/", sizeof(NGAGE_LOG_DIR))) return 0;
	if (strstr(path, "..")) return 0;
	for (const char *c = path; *c; c++)
		if ((unsigned char)*c < 0x20) return 0;
	return has_ext(path, ".zip");
}

static void dev_select()
{
	char path[1024];
	struct stat st;
	size_t n = 0;
	int ok;
	FILE *f = fopen(NGAGE_DEV_SEL, "rb");

	if (!f) return;
	ok = !fstat(fileno(f), &st) && S_ISREG(st.st_mode) && !st.st_uid;
	if (ok) n = fread(path, 1, sizeof(path) - 1, f);
	fclose(f);
	// the writer creates the file befor it writes the path: an empty file is read again next poll
	if (ok && !n) return;
	unlink(NGAGE_DEV_SEL);
	path[n] = 0;
	while (n && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' ')) path[--n] = 0;
	if (!dev_path_ok(path))
	{
		flog("dev select refused: %s", path);
		return;
	}
	flog("dev select %s", path);
	ngage_set_image(0, path);
}

void ngage_poll()
{
	const struct ngl_io *p;
	int ev, rc, cid = 0;

	if (cf.phase != CF_IDLE && (p = io_get())) cdrv_tick(p);
	if (poll_timer && !CheckTimer(poll_timer)) return;
	poll_timer = GetTimer(NGAGE_POLL_MS);
	if (cid_timer && CheckTimer(cid_timer)) cid_show();
	sav_flush("sched", 1);
	dev_select();
	p = io_get();
	if (!p) return;
	ev = ngl_fork_step(&fk, p, mgl_get()->done, NGAGE_POLL_MS);
	switch (ev)
	{
	case NGL_FE_START:
		flog("start: cpu1 released into the monitor (%u bytes, path %s)", (unsigned)mon_len, fk.path[0] ? fk.path : "-");
		flog("start: %s", fk.tmsg);
		flog("start: status 0x%08x state %u park_reason %u heartbeat %u",
			(unsigned)p->rd32(p->ctx, HYB_WIN_PHYS + HYB_REG_OFF(HYB_R_STATUS)), (unsigned)p->rd32(p->ctx, NGL_ST(state)),
			(unsigned)p->rd32(p->ctx, NGL_ST(park_reason)), (unsigned)p->rd32(p->ctx, NGL_ST(heartbeat)));
		if (fk.cdrv)
		{
			ngl_cdrv_rebase(&cdrv_sch, 0);
			cdrv_run = 1;
			flog("cdrv: start guard slot %d", fk.cdrv_slot);
		}
		break;
	case NGL_FE_LIVE:
		info("%s", fk.err);
		break;
	case NGL_FE_FAIL:
		info("%s", fk.err);
		if (fk.rc2) info("cpu1: %s", ngl_strerror(fk.rc2));
		break;
	case NGL_FE_PARK:
		flog("parked: reason %u fatal_kind 0x%08x cpu1 %s", (unsigned)fk.w.reason, (unsigned)fk.w.fatal_kind,
			ngl_strerror(fk.rc));
		if (fk.w.reason == NGST_PARK_FATAL) info("guest fatal 0x%08x", (unsigned)fk.w.fatal_kind);
		else if (fk.w.reason != NGST_PARK_REQUEST) info("monitor parked (reason %u)", (unsigned)fk.w.reason);
		if (fk.rc) info("cpu1: %s", ngl_strerror(fk.rc));
		break;
	case NGL_FE_EXIT:
		// the game quit; the fork parked after the settle, so the saves are flushed before the stay
		errno = 0;
		rc = ngage_save_flush("exit");
		flog("exit: seen 0x%08x settle %u ms park %s state %u park_reason %u flush %d",
			(unsigned)fk.exit_seen, (unsigned)fk.exit_ms, ngl_strerror(fk.rc), (unsigned)fk.stop_state,
			(unsigned)fk.stop_reason, rc);
		// the parked monitor keeps its status page until the next load wipes DDR
		{
			struct ngl_xcap xc;
			char xl[NGL_XCAP_LINES][NGL_XCAP_LINE];
			int e = errno;

			ngl_xcap_read(p, &xc);
			for (int i = 0, k = ngl_xcap_fmt(&xc, xl, NGL_XCAP_LINES); i < k; i++) flog("%s", xl[i]);
			cid = ngl_xcap_cid(&xc) ? 1 : ngl_xcap_preframe(&xc) ? 2 : 0;
			// flog may set errno, which the saves note below reports
			errno = e;
		}
		// after a good park no monitor owns PAHI and cpu1 is Linux's; the saves are already written, so a fault
		// in the screen write loses nothing
		if (!fk.rc)
		{
			unsigned long t0 = GetTimer(0);
			int e = errno, n = ngl_lcd_black(p);

			if (n < 0) flog("exit: screen not cleared: %s", ngl_strerror(n));
			else flog("exit: screen black, %u words in %lu ms", (unsigned)n, GetTimer(0) - t0);
			errno = e;
		}
		if (fk.rc) info("exit: %s, core stays", ngl_strerror(fk.rc));
		else if (rc) info("saves not written: %s", errno ? strerror(errno) : "flush failed");
		// no menu load, the parked monitor stays as in the fk.rc branch; the user picks a game or quits in the OSD
		else if (cid) cid_stay(cid);
		else flog("exit: game ended, core stays");
		break;
	case NGL_FE_SELECT:
		// C: and the MMC stayed quiet (or the cap passed); the body parks and switches as before
		{
			char xs[32] = "";

			if (fk.exit_seen) snprintf(xs, sizeof(xs), " exit_seen 0x%08x", (unsigned)fk.exit_seen);
			flog("select: C: quiet %u ms after %u ms, gen %u -> %u, mmc %u -> %u%s%s",
				(unsigned)fk.sel_quiet, (unsigned)fk.sel_ms, (unsigned)fk.sel_gen0, (unsigned)fk.sel_gen,
				(unsigned)fk.sel_mmc0, (unsigned)fk.sel_mmc, fk.sel_capped ? " (capped)" : "", xs);
			if (fk.exit_seen)
			{
				struct ngl_xcap xc;
				char xl[NGL_XCAP_LINES][NGL_XCAP_LINE];

				ngl_xcap_read(p, &xc);
				for (int i = 0, k = ngl_xcap_fmt(&xc, xl, NGL_XCAP_LINES); i < k; i++) flog("%s", xl[i]);
			}
		}
		select_body(fk.sel_index, fk.sel_path);
		break;
	}
	if (cdrv_run && cdrv_on) ngl_cdrv_observe(&cdrv_sch, p->rd32(p->ctx, NGL_ST(cdrv_gen)), (uint32_t)GetTimer(0));
	cdrv_flush("sched", 1);
}

void ngage_unload()
{
	const struct ngl_io *p;

	cid_timer = 0;
	p = io_get();
	ngl_fork_select_drop(&fk);
	if (p) stop(p, "unload");
	ngage_save_flush("unload");
	sav_drops_log();
}

static const char *confstr_entry(void *, int i)
{
	return user_io_get_confstr(i);
}

int ngage_core_ok()
{
	return ngl_core_ok(user_io_get_core_name(1), confstr_entry, NULL);
}

int ngage_browse_zip(const char *path)
{
	int r = ngl_src_zip_path(path);
	flog("browse %s noenter=%d", path, r);
	return r;
}
