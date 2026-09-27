/*
 * fantech-ds4-proxy.c
 *
 * Userspace DualShock 4 passthrough for the Fantech Shooter III (WGP13S) and
 * other DS4-clone controllers that present a genuine DualShock 4 HID report
 * descriptor but whose USB IDs are not known to hid-playstation:
 *
 *   0f0d:0162  Fantech Shooter III dongle (DS4 / D-input mode)
 *   0f00:0570  Fantech Shooter III wired (DS4 mode)
 *
 * This daemon mirrors the physical controller's raw HID report descriptor into
 * a /dev/uhid device that advertises genuine Sony IDs (054c:05c4, BUS_USB). The
 * hid-playstation driver binds to that virtual device and builds a real
 * DualShock 4 (gamepad + motion sensors + touchpad). This program sits in the
 * middle and proxies:
 *
 *   - input reports   : physical hidraw -> UHID_INPUT2
 *   - feature GET     : UHID_GET_REPORT -> HIDIOCGFEATURE -> reply
 *   - feature/output  : UHID_SET_REPORT / UHID_OUTPUT -> HIDIOCSFEATURE /
 *                       write() / HIDIOCSOUTPUT
 *   - UHID_START/STOP/OPEN/CLOSE are logged
 *
 * The buggy accelerometer block of the calibration feature report 0x02 and the
 * static ~5% battery reading are corrected on the way through.
 *
 * The physical controller is bound by hid-generic, which exposes a mis-mapped
 * generic pad. The daemon EVIOCGRABs its evdev nodes and installs a udev rule
 * that makes its hidraw/evdev/js nodes unreachable by userspace, so the only
 * visible controller is the virtual genuine DS4.
 *
 * License: GPL-2.0-or-later
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <glob.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/hid.h>
#include <linux/hidraw.h>
#include <linux/uhid.h>

#define DRIVER_VERSION "1.0.0"

/* Genuine Sony DualShock 4 USB identifiers we impersonate. */
#define SONY_VENDOR_ID      0x054c
#define SONY_PRODUCT_DS4    0x05c4

/* Feature report sizes hid-playstation (and Steam) ask for. */
#define FEATURE_CALIBRATION       0x02
#define FEATURE_CALIBRATION_SIZE  37
#define FEATURE_PAIRING_INFO      0x12
#define FEATURE_PAIRING_INFO_SIZE 16
#define FEATURE_FIRMWARE_INFO     0xa3
#define FEATURE_FIRMWARE_INFO_SIZE 49
#define FEATURE_BT_CALIBRATION    0x05
#define FEATURE_BT_CALIBRATION_SIZE 41
#define FEATURE_DEFAULT_SIZE      64

struct supported_id {
	uint16_t vendor;
	uint16_t product;
	const char *description;
};

static const struct supported_id SUPPORTED_DEVICES[] = {
	{ 0x0f0d, 0x0162, "Fantech Shooter III WGP13S (wireless dongle, DS4 mode)" },
	{ 0x0f00, 0x0570, "Fantech Shooter III WGP13S (wired, DS4 mode)" },
	{ 0, 0, NULL }
};

static volatile sig_atomic_t g_running = true;

/* Active session state. */
static int    g_phys_fd   = -1;
static int    g_uhid_fd   = -1;
static char   g_phys_name[64];      /* "hidrawN" */
static char   g_phys_path[128];     /* "/dev/hidrawN" */

#define MAX_GRABS 16
static int    g_grab_fds[MAX_GRABS];
static int    g_grab_count = 0;

static bool   g_started = false;
static bool   g_verbose = false;

static struct uhid_event g_in_ev;
static struct uhid_event g_out_ev;
static struct uhid_event g_reply_ev;

/* Descriptor the live virtual DS4 was built from (for reconnect reuse). */
static uint8_t g_uhid_desc[HID_MAX_DESCRIPTOR_SIZE];
static int     g_uhid_desc_size = 0;

/* Monotonic ms at which the physical controller went away (0 = attached). */
static uint64_t g_phys_lost_at = 0;
static int      g_reconnect_grace_ms = 5000;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void handle_signal(int sig)
{
	(void)sig;
	g_running = false;
}

static void log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void log_verbose(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static ssize_t read_file(const char *path, char *buf, size_t buflen)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, buflen - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return n;
}

static const char *basename_ptr(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

/* ------------------------------------------------------------------ */
/* physical device discovery                                           */
/* ------------------------------------------------------------------ */

/*
 * Scan /sys/class/hidraw for a hidraw node whose VID:PID is supported.
 * Returns 0 and fills @name ("hidrawN") on success, -1 otherwise.
 */
static int find_physical_hidraw(char *name, size_t name_len)
{
	DIR *dir;
	struct dirent *ent;
	int found = -1;

	dir = opendir("/sys/class/hidraw");
	if (!dir)
		return -1;

	while ((ent = readdir(dir)) != NULL) {
		char uevent_path[512];
		char uevent[1024];
		char *line;
		unsigned int bus, vendor, product;
		int i;

		if (strncmp(ent->d_name, "hidraw", 6) != 0)
			continue;

		snprintf(uevent_path, sizeof(uevent_path),
			 "/sys/class/hidraw/%s/device/uevent", ent->d_name);
		if (read_file(uevent_path, uevent, sizeof(uevent)) < 0)
			continue;

		line = strstr(uevent, "HID_ID=");
		if (!line)
			continue;
		if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vendor, &product) != 3)
			continue;

		for (i = 0; SUPPORTED_DEVICES[i].vendor != 0; i++) {
			if (SUPPORTED_DEVICES[i].vendor == vendor &&
			    SUPPORTED_DEVICES[i].product == product) {
				size_t len = strlen(ent->d_name);

				if (len >= name_len)
					len = name_len - 1;
				memcpy(name, ent->d_name, len);
				name[len] = '\0';
				log_info("[proxy] Found %s (%04x:%04x) at /dev/%s: %s\n",
					 ent->d_name, vendor, product, ent->d_name,
					 SUPPORTED_DEVICES[i].description);
				found = 0;
				break;
			}
		}
		if (found == 0)
			break;
	}

	closedir(dir);
	return found;
}

static int read_report_descriptor(int fd, struct hidraw_report_descriptor *desc,
				  int *out_size)
{
	int size = 0;

	if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0) {
		perror("[proxy] HIDIOCGRDESCSIZE");
		return -1;
	}
	if (size <= 0 || size > HID_MAX_DESCRIPTOR_SIZE) {
		fprintf(stderr, "[proxy] Bogus report descriptor size %d\n", size);
		return -1;
	}

	memset(desc, 0, sizeof(*desc));
	desc->size = size;
	if (ioctl(fd, HIDIOCGRDESC, desc) < 0) {
		perror("[proxy] HIDIOCGRDESC");
		return -1;
	}
	if (desc->size == 0 || desc->size > HID_MAX_DESCRIPTOR_SIZE)
		desc->size = size;

	*out_size = desc->size;
	return 0;
}

/*
 * The physical controller is handled by hid-generic, which creates a
 * mis-mapped generic pad (eventN / jsN). Grab every evdev node below the
 * physical hidraw's sysfs input directory so userspace only sees our virtual,
 * correctly mapped DualShock 4.
 */
static void grab_physical_evdevs(const char *hidraw_name)
{
	char pattern[512];
	glob_t gl;
	size_t i;

	snprintf(pattern, sizeof(pattern),
		 "/sys/class/hidraw/%s/device/input/input*/event*", hidraw_name);

	memset(&gl, 0, sizeof(gl));
	if (glob(pattern, 0, NULL, &gl) != 0)
		return;

	for (i = 0; i < gl.gl_pathc && g_grab_count < MAX_GRABS; i++) {
		const char *base = basename_ptr(gl.gl_pathv[i]);
		char dev_path[128];
		int fd;

		snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", base);
		fd = open(dev_path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			fd = open(dev_path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			log_info("[proxy] Could not open %s to grab: %s\n",
				 dev_path, strerror(errno));
			continue;
		}
		if (ioctl(fd, EVIOCGRAB, 1) < 0) {
			log_info("[proxy] EVIOCGRAB on %s failed: %s\n",
				 dev_path, strerror(errno));
			close(fd);
			continue;
		}
		g_grab_fds[g_grab_count++] = fd;
		log_info("[proxy] Grabbed %s (hiding hid-generic pad)\n", dev_path);
	}

	globfree(&gl);
}

static void release_grabs(void)
{
	while (g_grab_count > 0) {
		int fd = g_grab_fds[--g_grab_count];

		ioctl(fd, EVIOCGRAB, 0);
		close(fd);
	}
}

/* ------------------------------------------------------------------ */
/* hiding the physical controller from userspace                       */
/* ------------------------------------------------------------------ */

/*
 * Grabbing the evdev only stops event delivery; Steam/SDL still enumerate and
 * list the physical pad because they can open its device nodes. InputPlumber
 * solves this by making the source devnodes unreachable (mode 000 + ACL
 * removal). Doing that at runtime races with logind re-adding the uaccess ACL,
 * so instead we drop a udev rule in /run and let udev hide the nodes as they
 * appear. The rule lives for the daemon's lifetime and is removed on exit, at
 * which point the physical controller becomes usable again.
 *
 * IMPORTANT: the rule must sort *after* 73-seat-late.rules. The uaccess ACL is
 * applied by a RUN{builtin} in that file, and RUN commands execute in the order
 * they were queued, so a rule numbered <=73 would have its "setfacl -b" run
 * before logind's ACL is added. A 96- rule queues its ACL removal afterwards.
 */
#define HIDE_RULE_DIR  "/run/udev/rules.d"
#define HIDE_RULE_PATH HIDE_RULE_DIR "/96-fantech-ds4-proxy-hide.rules"

static const char HIDE_RULE_CONTENT[] =
	"# Managed by fantech-ds4-proxy; removed when the daemon exits.\n"
	"ACTION==\"add|change\", SUBSYSTEM==\"input\", KERNEL==\"js[0-9]*|event[0-9]*\", "
	"ATTRS{idVendor}==\"0f0d\", ATTRS{idProduct}==\"0162\", "
	"MODE:=\"0000\", GROUP:=\"root\", TAG-=\"uaccess\", "
	"RUN+=\"/usr/bin/chmod 000 /dev/input/%k\", RUN+=\"/usr/bin/setfacl -b /dev/input/%k\"\n"
	"ACTION==\"add|change\", SUBSYSTEM==\"hidraw\", "
	"ATTRS{idVendor}==\"0f0d\", ATTRS{idProduct}==\"0162\", "
	"MODE:=\"0000\", GROUP:=\"root\", TAG-=\"uaccess\", "
	"RUN+=\"/usr/bin/chmod 000 /dev/%k\", RUN+=\"/usr/bin/setfacl -b /dev/%k\"\n"
	"ACTION==\"add|change\", SUBSYSTEM==\"input\", KERNEL==\"js[0-9]*|event[0-9]*\", "
	"ATTRS{idVendor}==\"0f00\", ATTRS{idProduct}==\"0570\", "
	"MODE:=\"0000\", GROUP:=\"root\", TAG-=\"uaccess\", "
	"RUN+=\"/usr/bin/chmod 000 /dev/input/%k\", RUN+=\"/usr/bin/setfacl -b /dev/input/%k\"\n"
	"ACTION==\"add|change\", SUBSYSTEM==\"hidraw\", "
	"ATTRS{idVendor}==\"0f00\", ATTRS{idProduct}==\"0570\", "
	"MODE:=\"0000\", GROUP:=\"root\", TAG-=\"uaccess\", "
	"RUN+=\"/usr/bin/chmod 000 /dev/%k\", RUN+=\"/usr/bin/setfacl -b /dev/%k\"\n";

static void run_udevadm(const char *args)
{
	char cmd[1024];

	snprintf(cmd, sizeof(cmd), "udevadm %s >/dev/null 2>&1", args);
	(void)system(cmd);
}

static void install_hide_rules(void)
{
	FILE *f;

	if (mkdir(HIDE_RULE_DIR, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "[proxy] Cannot create %s: %s\n",
			HIDE_RULE_DIR, strerror(errno));
		return;
	}

	f = fopen(HIDE_RULE_PATH, "w");
	if (!f) {
		fprintf(stderr, "[proxy] Cannot write %s: %s\n",
			HIDE_RULE_PATH, strerror(errno));
		return;
	}
	fputs(HIDE_RULE_CONTENT, f);
	fclose(f);
	run_udevadm("control --reload-rules");
	log_info("[proxy] Installed udev hide rule\n");
}

static void remove_hide_rules(void)
{
	if (unlink(HIDE_RULE_PATH) == 0) {
		run_udevadm("control --reload-rules");
		log_info("[proxy] Removed udev hide rule\n");
	}
}

static void trigger_devnode(const char *action, const char *dev)
{
	char args[512];

	snprintf(args, sizeof(args), "trigger --action=%s %s", action, dev);
	run_udevadm(args);
}

static void trigger_physical_devnodes(const char *action)
{
	static const char *patterns[] = {
		"/sys/class/hidraw/%s/device/input/input*/event*",
		"/sys/class/hidraw/%s/device/input/input*/js*",
	};
	glob_t gl;
	size_t i;

	if (g_phys_path[0])
		trigger_devnode(action, g_phys_path);

	for (i = 0; i < 2; i++) {
		char pattern[512];

		snprintf(pattern, sizeof(pattern), patterns[i], g_phys_name);
		memset(&gl, 0, sizeof(gl));
		if (glob(pattern, 0, NULL, &gl) != 0)
			continue;

		for (size_t j = 0; j < gl.gl_pathc; j++) {
			char dev[160];

			snprintf(dev, sizeof(dev), "/dev/input/%s",
				 basename_ptr(gl.gl_pathv[j]));
			trigger_devnode(action, dev);
		}
		globfree(&gl);
	}
}

/* ------------------------------------------------------------------ */
/* hid-playstation driver availability                                 */
/* ------------------------------------------------------------------ */

/*
 * hid-generic only defers to a specialised driver if that driver is already
 * registered when the HID device appears. If hid-playstation is not loaded yet
 * it would grab our virtual DS4 and never let go, so make sure it is present
 * before creating the uhid device.
 */
static int ensure_playstation_driver(void)
{
	const char *driver_dir = "/sys/bus/hid/drivers/playstation";
	bool tried_modprobe = false;
	int i;

	for (i = 0; i < 50; i++) {
		if (access(driver_dir, F_OK) == 0)
			return 0;

		if (!tried_modprobe) {
			tried_modprobe = true;
			log_info("[proxy] Loading hid_playstation module...\n");
			if (system("/sbin/modprobe hid_playstation 2>/dev/null") != 0)
				system("/usr/sbin/modprobe hid_playstation 2>/dev/null");
		}
		usleep(100000);
	}

	fprintf(stderr, "[proxy] hid-playstation is not available. Load it with "
		"'modprobe hid_playstation' and retry.\n");
	return -1;
}

/* ------------------------------------------------------------------ */
/* uhid device creation                                                */
/* ------------------------------------------------------------------ */

static int create_uhid_device(const struct hidraw_report_descriptor *desc)
{
	int fd;
	struct uhid_event ev;
	struct uhid_create2_req *create;

	fd = open("/dev/uhid", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "[proxy] Cannot open /dev/uhid: %s\n",
			strerror(errno));
		fprintf(stderr, "        (load the 'uhid' module and check permissions)\n");
		return -1;
	}

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	create = &ev.u.create2;

	snprintf((char *)create->name, sizeof(create->name),
		 "Wireless Controller");
	snprintf((char *)create->phys, sizeof(create->phys),
		 "%s", g_phys_name);

	create->rd_size = desc->size;
	create->bus     = BUS_USB;
	create->vendor  = SONY_VENDOR_ID;
	create->product = SONY_PRODUCT_DS4;
	create->version = 0x0100;
	create->country = 0;
	memcpy(create->rd_data, desc->value, desc->size);

	if (write(fd, &ev, sizeof(ev)) < 0) {
		fprintf(stderr, "[proxy] UHID_CREATE2 failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	log_info("[proxy] Created virtual DualShock 4 (%04x:%04x, bus=USB, %u byte descriptor)\n",
		 SONY_VENDOR_ID, SONY_PRODUCT_DS4, desc->size);

	return fd;
}

/* ------------------------------------------------------------------ */
/* feature report proxying                                             */
/* ------------------------------------------------------------------ */

static int feature_report_size(uint8_t rnum)
{
	switch (rnum) {
	case FEATURE_CALIBRATION:
		return FEATURE_CALIBRATION_SIZE;
	case FEATURE_PAIRING_INFO:
		return FEATURE_PAIRING_INFO_SIZE;
	case FEATURE_FIRMWARE_INFO:
		return FEATURE_FIRMWARE_INFO_SIZE;
	case FEATURE_BT_CALIBRATION:
		return FEATURE_BT_CALIBRATION_SIZE;
	default:
		return FEATURE_DEFAULT_SIZE;
	}
}

/*
 * The Fantech firmware returns a calibration report 0x02 with a broken
 * accelerometer block (negative / zero ranges). Replace acc_{x,y,z}_plus/minus
 * with the values that make hid-playstation's sanity fallback equivalent:
 *   range_2g = 32767, bias = 0, sens = DS4_ACC_RANGE / S16_MAX
 * Gyro calibration is left untouched.
 */
static void rewrite_accel_calibration(uint8_t *buf, int size)
{
	int i;

	/* acc_{x,y,z}_plus/minus live at offsets 23..34 (incl. report id). */
	if (size < 35)
		return;

	for (i = 0; i < 3; i++) {
		put_le16(&buf[23 + i * 4], (uint16_t)16383);       /* plus  */
		put_le16(&buf[25 + i * 4], (uint16_t)-16384);      /* minus */
	}
}

static void synth_feature_report(uint8_t rnum, uint8_t *buf, int len)
{
	memset(buf, 0, len);
	buf[0] = rnum;

	if (rnum == FEATURE_PAIRING_INFO && len >= 7) {
		/* A non-zero MAC is enough to make hid-playstation happy. */
		static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

		memcpy(&buf[1], mac, sizeof(mac));
	} else if (rnum == FEATURE_CALIBRATION) {
		rewrite_accel_calibration(buf, len);
	}
}

static int phys_get_feature(uint8_t rnum, uint8_t *buf, int len)
{
	int ret;

	if (g_phys_fd < 0)
		return -1;

	buf[0] = rnum;
	ret = ioctl(g_phys_fd, HIDIOCGFEATURE(len), buf);
	return ret;
}

static void handle_get_report(const struct uhid_get_report_req *req)
{
	uint8_t buf[FEATURE_DEFAULT_SIZE + 1];
	int want = feature_report_size(req->rnum);
	int got = -1;

	if (want > (int)sizeof(buf))
		want = sizeof(buf);
	memset(buf, 0, sizeof(buf));

	if (req->rtype == UHID_FEATURE_REPORT) {
		got = phys_get_feature(req->rnum, buf, want);
		if (got < 0) {
			log_verbose("[proxy] GET feature 0x%02x not supported by hardware, synthesizing\n",
				    req->rnum);
		} else if (got != want) {
			log_verbose("[proxy] GET feature 0x%02x returned %d bytes, expected %d, synthesizing\n",
				    req->rnum, got, want);
			got = -1;
		}
	}

	if (got < 0) {
		synth_feature_report(req->rnum, buf, want);
		got = want;
	}

	if (req->rnum == FEATURE_CALIBRATION)
		rewrite_accel_calibration(buf, got);

	memset(&g_reply_ev, 0, sizeof(g_reply_ev));
	g_reply_ev.type = UHID_GET_REPORT_REPLY;
	g_reply_ev.u.get_report_reply.id = req->id;
	g_reply_ev.u.get_report_reply.err = 0;
	g_reply_ev.u.get_report_reply.size = got;
	memcpy(g_reply_ev.u.get_report_reply.data, buf, got);

	if (write(g_uhid_fd, &g_reply_ev, sizeof(g_reply_ev)) < 0)
		fprintf(stderr, "[proxy] GET_REPORT reply failed: %s\n", strerror(errno));
	else
		log_verbose("[proxy] GET feature 0x%02x -> %d bytes\n", req->rnum, got);
}

static void phys_write_output(const uint8_t *data, uint16_t size)
{
	ssize_t ret;

	if (size == 0 || g_phys_fd < 0)
		return;

	ret = write(g_phys_fd, data, size);
	if (ret < 0) {
		if (ioctl(g_phys_fd, HIDIOCSOUTPUT(size), (void *)data) < 0)
			log_verbose("[proxy] Output report 0x%02x write failed: %s\n",
				    data[0], strerror(errno));
		else
			log_verbose("[proxy] Output report 0x%02x sent via HIDIOCSOUTPUT\n",
				    data[0]);
	} else {
		log_verbose("[proxy] Output report 0x%02x sent (%u bytes)\n",
			    data[0], size);
	}
}

static void phys_set_feature(const uint8_t *data, uint16_t size)
{
	if (size == 0 || g_phys_fd < 0)
		return;

	if (ioctl(g_phys_fd, HIDIOCSFEATURE(size), (void *)data) < 0)
		log_verbose("[proxy] SET feature 0x%02x failed: %s\n",
			    data[0], strerror(errno));
	else
		log_verbose("[proxy] SET feature 0x%02x sent (%u bytes)\n",
			    data[0], size);
}

static void handle_set_report(const struct uhid_set_report_req *req)
{
	if (req->rtype == UHID_OUTPUT_REPORT)
		phys_write_output(req->data, req->size);
	else if (req->rtype == UHID_FEATURE_REPORT)
		phys_set_feature(req->data, req->size);

	memset(&g_reply_ev, 0, sizeof(g_reply_ev));
	g_reply_ev.type = UHID_SET_REPORT_REPLY;
	g_reply_ev.u.set_report_reply.id = req->id;
	g_reply_ev.u.set_report_reply.err = 0;
	if (write(g_uhid_fd, &g_reply_ev, sizeof(g_reply_ev)) < 0)
		fprintf(stderr, "[proxy] SET_REPORT reply failed: %s\n", strerror(errno));
}

static void handle_output(const struct uhid_output_req *out)
{
	if (out->rtype == UHID_OUTPUT_REPORT)
		phys_write_output(out->data, out->size);
	else if (out->rtype == UHID_FEATURE_REPORT)
		phys_set_feature(out->data, out->size);
}

/* ------------------------------------------------------------------ */
/* input report fixups                                                 */
/* ------------------------------------------------------------------ */

/* Offsets inside the DS4 USB input report (report ID 1, 64 bytes). */
#define DS4_USB_REPORT_ID        0x01
#define DS4_USB_REPORT_SIZE      64
#define DS4_OFF_STATUS0          30
#define DS4_OFF_NUM_TOUCH        33
#define DS4_OFF_TOUCH_BASE       34   /* touch_reports[0].timestamp */
#define DS4_TOUCH_REPORT_SIZE    9    /* timestamp + 2 x 4-byte points */
#define DS4_TOUCH_INACTIVE       0x80 /* point->contact bit 7 */
/* No cable (BIT4 clear) + battery_data == 10 -> hid-playstation reports 100% */
#define DS4_STATUS0_FAKE_FULL    0x0A

/*
 * Fixups applied to the clone's report ID 1 before it reaches hid-playstation:
 *
 *   - battery: the firmware reports a static ~5%; force a believable 100%.
 *   - touch: hid-playstation rejects the *entire* report (buttons and IMU
 *     included) if num_touch_reports > 3, so clamp it to 0 and mark the
 *     contacts inactive. This controller has no touchpad; its Snap/Fn button
 *     is a normal button bit and is left untouched.
 *
 * Note: the idle touchpad cursor some viewers draw is not removed by this. The
 * touchpad input device is created unconditionally by hid-playstation, and
 * hiding it would also hide Snap/Fn's BTN_LEFT.
 */
static void rewrite_ds4_input_report(uint8_t *buf, ssize_t size)
{
	int i;

	if (size < DS4_USB_REPORT_SIZE || buf[0] != DS4_USB_REPORT_ID)
		return;

	buf[DS4_OFF_STATUS0] = DS4_STATUS0_FAKE_FULL;

	buf[DS4_OFF_NUM_TOUCH] = 0;
	for (i = 0; i < 3; i++) {
		int base = DS4_OFF_TOUCH_BASE + i * DS4_TOUCH_REPORT_SIZE;

		if (base + DS4_TOUCH_REPORT_SIZE > size)
			break;
		buf[base + 1] |= DS4_TOUCH_INACTIVE; /* points[0].contact */
		buf[base + 5] |= DS4_TOUCH_INACTIVE; /* points[1].contact */
	}
}

/* Push a centred/no-button report so nothing stays stuck while the physical
 * controller is away but the virtual DS4 is being kept alive. */
static void send_neutral_report(void)
{
	struct uhid_event *ev = &g_out_ev;
	uint8_t *b;

	if (g_uhid_fd < 0)
		return;

	memset(ev, 0, sizeof(*ev));
	ev->type = UHID_INPUT2;
	ev->u.input2.size = DS4_USB_REPORT_SIZE;
	b = ev->u.input2.data;
	b[0] = DS4_USB_REPORT_ID;
	b[1] = b[2] = b[3] = b[4] = 128; /* sticks centred */
	b[5] = 0x08;                     /* hat centred, no buttons */
	b[DS4_OFF_STATUS0] = DS4_STATUS0_FAKE_FULL;

	if (write(g_uhid_fd, ev, sizeof(*ev)) < 0)
		log_verbose("[proxy] Could not send neutral report: %s\n",
			    strerror(errno));
}

/* ------------------------------------------------------------------ */
/* event pumps                                                         */
/* ------------------------------------------------------------------ */

/* Returns false when the physical device went away. */
static bool pump_physical(void)
{
	uint8_t buf[4096];
	ssize_t n;
	struct uhid_event *ev = &g_out_ev;

	n = read(g_phys_fd, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return true;
		fprintf(stderr, "[proxy] Physical hidraw read failed: %s\n", strerror(errno));
		return false;
	}
	if (n == 0)
		return false;

	if (n > UHID_DATA_MAX)
		n = UHID_DATA_MAX;

	rewrite_ds4_input_report(buf, n);

	memset(ev, 0, sizeof(*ev));
	ev->type = UHID_INPUT2;
	ev->u.input2.size = (uint16_t)n;
	memcpy(ev->u.input2.data, buf, n);

	if (write(g_uhid_fd, ev, sizeof(*ev)) < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return true;
		fprintf(stderr, "[proxy] UHID_INPUT2 write failed: %s\n", strerror(errno));
		return false;
	}

	return true;
}

/* Returns false when the uhid device went away. */
static bool pump_uhid(void)
{
	struct uhid_event *ev = &g_in_ev;

	for (;;) {
		ssize_t n = read(g_uhid_fd, ev, sizeof(*ev));

		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return true;
			fprintf(stderr, "[proxy] uhid read failed: %s\n", strerror(errno));
			return false;
		}
		if (n == 0)
			return false;
		if (n < (ssize_t)sizeof(ev->type))
			continue;

		switch (ev->type) {
		case UHID_START:
			g_started = true;
			log_info("[proxy] Virtual DS4 started (dev_flags=0x%llx)\n",
				 (unsigned long long)ev->u.start.dev_flags);
			break;
		case UHID_STOP:
			g_started = false;
			log_info("[proxy] Virtual DS4 stopped\n");
			break;
		case UHID_OPEN:
			log_verbose("[proxy] Virtual DS4 opened by a client\n");
			break;
		case UHID_CLOSE:
			log_verbose("[proxy] Virtual DS4 closed\n");
			break;
		case UHID_GET_REPORT:
			handle_get_report(&ev->u.get_report);
			break;
		case UHID_SET_REPORT:
			handle_set_report(&ev->u.set_report);
			break;
		case UHID_OUTPUT:
			handle_output(&ev->u.output);
			break;
		default:
			log_verbose("[proxy] Unhandled uhid event %u\n", ev->type);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* raw report diagnostic (--dump)                                      */
/* ------------------------------------------------------------------ */

static int16_t rd_s16le(const uint8_t *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

static int dump_physical(void)
{
	char name[64];
	char path[128];
	uint8_t buf[4096];
	struct timespec last = { 0, 0 };
	int fd;

	if (find_physical_hidraw(name, sizeof(name)) < 0) {
		fprintf(stderr, "[dump] No supported controller found.\n");
		return 1;
	}
	snprintf(path, sizeof(path), "/dev/%s", name);

	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "[dump] Cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}

	log_info("[dump] Raw HID reports from %s (offsets are DS4-standard).\n", path);
	log_info("[dump] Move the controller *without* touching anything and watch gyro/accel.\n");

	while (g_running) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int ret;

		ret = poll(&pfd, 1, 1000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0)
			continue;

		for (;;) {
			struct timespec now;
			long dt_ms;
			ssize_t n = read(fd, buf, sizeof(buf));

			if (n < 0) {
				if (errno == EAGAIN || errno == EINTR)
					break;
				goto out;
			}
			if (n == 0)
				break;

			clock_gettime(CLOCK_MONOTONIC, &now);
			dt_ms = (now.tv_sec - last.tv_sec) * 1000 +
				(now.tv_nsec - last.tv_nsec) / 1000000;
			if (dt_ms < 20)  /* ~50 Hz */
				continue;
			last = now;

			printf("[dump] id=0x%02x len=%-3zd", buf[0], n);
			if (buf[0] == 0x01 && n >= 34) {
				printf(" ts=%u sticks=(%u,%u,%u,%u) trig=(%u,%u) btn=%02x/%02x/%02x"
				       " gyro=(%d,%d,%d) accel=(%d,%d,%d) status=0x%02x ntouch=%u",
				       (unsigned)(buf[10] | (buf[11] << 8)),
				       buf[1], buf[2], buf[3], buf[4], buf[8], buf[9],
				       buf[5], buf[6], buf[7],
				       rd_s16le(&buf[13]), rd_s16le(&buf[15]), rd_s16le(&buf[17]),
				       rd_s16le(&buf[19]), rd_s16le(&buf[21]), rd_s16le(&buf[23]),
				       buf[30], buf[33]);
			}
			printf("\n");
			fflush(stdout);
		}
	}

out:
	close(fd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* session management                                                  */
/* ------------------------------------------------------------------ */

static void close_physical(void)
{
	if (g_phys_fd >= 0) {
		release_grabs();
		close(g_phys_fd);
		g_phys_fd = -1;
	}
	g_started = false;
	g_phys_name[0] = '\0';
	g_phys_path[0] = '\0';
}

static void destroy_uhid(void)
{
	if (g_uhid_fd >= 0) {
		close(g_uhid_fd);
		g_uhid_fd = -1;
	}
	g_uhid_desc_size = 0;
	g_started = false;
}

static int open_physical(struct hidraw_report_descriptor *desc, int *desc_size)
{
	if (find_physical_hidraw(g_phys_name, sizeof(g_phys_name)) < 0)
		return -1;

	snprintf(g_phys_path, sizeof(g_phys_path), "/dev/%s", g_phys_name);
	g_phys_fd = open(g_phys_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (g_phys_fd < 0) {
		fprintf(stderr, "[proxy] Cannot open %s: %s\n",
			g_phys_path, strerror(errno));
		return -1;
	}

	if (read_report_descriptor(g_phys_fd, desc, desc_size) < 0) {
		close(g_phys_fd);
		g_phys_fd = -1;
		return -1;
	}
	return 0;
}

/*
 * Keep the same virtual DS4 across a physical reconnect when the report
 * descriptor is unchanged. Steam caches per-controller state (notably the gyro
 * binding) and fails to pick up the IMU again if the virtual device is
 * re-created during a brief dongle soft-reset.
 */
static int ensure_uhid(const struct hidraw_report_descriptor *desc, int desc_size)
{
	if (g_uhid_fd >= 0) {
		if (g_uhid_desc_size == desc_size &&
		    memcmp(g_uhid_desc, desc->value, desc_size) == 0)
			return 0;

		log_info("[proxy] Report descriptor changed, recreating virtual DS4\n");
		destroy_uhid();
	}

	if (ensure_playstation_driver() < 0)
		return -1;

	g_uhid_fd = create_uhid_device(desc);
	if (g_uhid_fd < 0)
		return -1;

	memcpy(g_uhid_desc, desc->value, desc_size);
	g_uhid_desc_size = desc_size;
	return 0;
}

static int attach_physical(void)
{
	struct hidraw_report_descriptor desc;
	int desc_size = 0;

	memset(&desc, 0, sizeof(desc));

	if (open_physical(&desc, &desc_size) < 0)
		return -1;

	log_info("[proxy] Read %d byte report descriptor from %s\n",
		 desc_size, g_phys_path);

	if (ensure_uhid(&desc, desc_size) < 0) {
		close_physical();
		return -1;
	}

	grab_physical_evdevs(g_phys_name);
	trigger_physical_devnodes("change");
	g_phys_lost_at = 0;
	log_info("[proxy] Attached to %s\n", g_phys_path);
	return 0;
}

int main(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
			g_verbose = true;
		else if (strcmp(argv[i], "--dump") == 0)
			return dump_physical();
		else if (strncmp(argv[i], "--grace=", 8) == 0)
			g_reconnect_grace_ms = atoi(argv[i] + 8) * 1000;
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [-v|--verbose] [--dump] [--grace=SECONDS]\n",
			       argv[0]);
			printf("Proxies Fantech Shooter III (0f0d:0162 / 0f00:0570) as a\n");
			printf("genuine DualShock 4 (054c:05c4) through /dev/uhid.\n");
			printf("  --dump           print raw physical HID reports and exit\n");
			printf("  --grace=SECONDS  keep the virtual DS4 alive across a\n");
			printf("                   physical disconnect for this long (default 5)\n");
			return 0;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	log_info("=========================================================\n");
	log_info("  Fantech Shooter III -> DualShock 4 uhid proxy\n");
	log_info("  Version: %s\n", DRIVER_VERSION);
	log_info("=========================================================\n");

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGPIPE, SIG_IGN);

	install_hide_rules();

	while (g_running) {
		struct pollfd pfds[2];
		int nfds = 0;
		int ret;

		if (g_phys_fd < 0)
			(void)attach_physical();

		if (g_phys_fd >= 0) {
			pfds[nfds].fd = g_phys_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}
		if (g_uhid_fd >= 0) {
			pfds[nfds].fd = g_uhid_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		if (nfds == 0) {
			usleep(250000);
			continue;
		}

		ret = poll(pfds, nfds, g_phys_fd >= 0 ? 1000 : 250);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("[proxy] poll");
			break;
		}

		for (i = 0; ret > 0 && i < nfds; i++) {
			if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;

			if (g_phys_fd >= 0 && pfds[i].fd == g_phys_fd) {
				if (!pump_physical()) {
					log_info("[proxy] Controller disconnected; holding "
						 "virtual DS4 for %d ms\n",
						 g_reconnect_grace_ms);
					send_neutral_report();
					close_physical();
					g_phys_lost_at = now_ms();
				}
			} else if (g_uhid_fd >= 0 && pfds[i].fd == g_uhid_fd) {
				if (!pump_uhid()) {
					log_info("[proxy] Virtual DS4 went away; rebuilding\n");
					destroy_uhid();
					close_physical();
				}
			}
		}

		if (g_phys_fd < 0 && g_uhid_fd >= 0 && g_phys_lost_at != 0 &&
		    now_ms() - g_phys_lost_at >= (uint64_t)g_reconnect_grace_ms) {
			log_info("[proxy] Reconnect grace expired; destroying virtual DS4\n");
			destroy_uhid();
			g_phys_lost_at = 0;
		}
	}

	log_info("\n[proxy] Shutting down...\n");
	remove_hide_rules();
	/* Re-run udev on the physical nodes so the default permissions return. */
	if (g_phys_fd >= 0)
		trigger_physical_devnodes("add");
	close_physical();
	destroy_uhid();
	log_info("[proxy] Exited cleanly.\n");
	return 0;
}

