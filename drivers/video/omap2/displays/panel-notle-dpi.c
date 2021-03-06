/*
 * Notle Panel support.
 *
 * Copyright (C) 2011 Google, Inc.
 * Author: John Hawley <madsci@google.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/crc32.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <plat/omap_device.h>
#include <plat/omap_hwmod.h>
#include <plat/omap-pm.h>


#include <video/omapdss.h>
#include <video/omap-panel-notle.h>

#define LOG_TAG         "panel-notle: "

/*
 * Special value for LCOS init regs to delay initialization
 * and enable DISP_ENB.
 */
#define REG_DELAY       0x100
/*
 * Special value for LCOS init to send gamma table.
 */
#define REG_GAMMA       0x200
#define MAX_BRIGHTNESS  0xFF

/* iCE40 registers */
#define ICE40_REVISION   0x00

#define ICE40_PIPELINE   0x01
#define ICE40_PIPELINE_AUTO     0x70
#define ICE40_PIPELINE_TESTPAT  0x07

#define ICE40_BACKLIGHT  0x10
#define ICE40_BACKLIGHT_SYNC    0xC0
#define ICE40_BACKLIGHT_MONO    0xA0
#define ICE40_BACKLIGHT_LEDEN   0x10
#define ICE40_BACKLIGHT_CPSEL   0x08
#define ICE40_BACKLIGHT_FORCER  0x04
#define ICE40_BACKLIGHT_FORCEG  0x02
#define ICE40_BACKLIGHT_FORCEB  0x01

static const u8 led_matrix_addr[4][3] = {
  { 0x11, 0x17, 0x23 },
  { 0x19, 0x13, 0x25 },
  { 0x1B, 0x27, 0x15 },
  { 0x29, 0x2B, 0x2D }
};

#define ICE40_LED_RR_H     (led_matrix_addr[0][0])
#define ICE40_LED_RR_L     (led_matrix_addr[0][0]+1)
#define ICE40_LED_GG_H     (led_matrix_addr[1][1])
#define ICE40_LED_GG_L     (led_matrix_addr[1][1]+1)
#define ICE40_LED_BB_H     (led_matrix_addr[2][2])
#define ICE40_LED_BB_L     (led_matrix_addr[2][2]+1)

#define ICE40_LCOS         0x03
#define ICE40_LCOS_DISP_ENB     0x01

#define FINAL_LINECUT_BASED_FPGA_REVISION   0x3e
#define FIRST_MONO_ALL_FIELDS_FPGA_REVISION 0x42
/*
 * TODO(petermalkin): remove definitions of notle_version from here.
 * Move them to some place else where they could be shared by other
 * kernel modules that need to be aware of board version ID.
 */
typedef enum {
        UNVERSIONED = 7,
        V1_EVT1     = 1,
        V1_EVT2     = 2,
        V1_EVT3     = 3,
        V1_DVT1     = 4,
        V1_5_PROTO  = 5,
        SUPPORTED_FROM = V1_EVT1,
        SUPPORTED_TO = V1_5_PROTO,
} notle_version;

enum {
        NOTLE_I2C_PANEL = 1,
};

struct init_register_value {
        u16 reg;
        u8 value;
};

static const u8 ice40_regs[] = {
  ICE40_REVISION,
  ICE40_PIPELINE,
  ICE40_LCOS,
  ICE40_BACKLIGHT,
};

struct gamma_point {
  unsigned char red_p;
  unsigned char green_p;
  unsigned char blue_p;
  unsigned char red_n;
  unsigned char green_n;
  unsigned char blue_n;
};

static const struct gamma_point gamma_curve_legacy[] = {
  {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF},
  {0x62, 0x62, 0x49, 0x9D, 0x9D, 0xB6},
  {0x80, 0x7B, 0x67, 0x7F, 0x84, 0x98},
  {0x95, 0x90, 0x79, 0x6A, 0x6F, 0x86},
  {0xA5, 0xA1, 0x85, 0x5A, 0x5E, 0x7A},
  {0xB7, 0xB3, 0x90, 0x48, 0x4C, 0x6F},
  {0xC6, 0xC8, 0x93, 0x39, 0x37, 0x6C},
  {0xFF, 0xDC, 0xC7, 0x00, 0x23, 0x38},
};

static const struct gamma_point gamma_curve_default[] = {
  {  0,   0,   0, 255, 255, 255},
  { 98,  98,  93, 157, 157, 162},
  {128, 123, 116, 127, 132, 139},
  {149, 142, 135, 106, 113, 120},
  {165, 159, 150,  90,  96, 105},
  {183, 176, 166,  72,  79,  89},
  {198, 190, 177,  57,  65,  78},
  {235, 220, 199,  20,  35,  56},
};

static struct gamma_point gamma_curve[8];

static const struct init_register_value panel_init_regs[] = {
  { 0x00, 0x85 },
  { 0x01, 0xC3 },
  { 0x02, 0xC3 },
  { 0x13, 0x45 },
  { 0x14, 0x80 },
  { 0x15, 0x55 },
  { 0x16, 0x55 },
  { 0x17, 0x04 },
  { 0x18, 0x48 },
  { 0x19, 0x12 },
  { 0x1A, 0xE9 },
  { REG_GAMMA, 0x21 },
  { REG_DELAY, 0x05 },
  { 0x00, 0x81 },
  { REG_DELAY, 0x04 },
};

static const struct init_register_value panel_shutdown_regs[] = {
  { 0x00, 0x85 },
  { REG_DELAY, 0x05 },
  { 0x00, 0x81 },
  { REG_DELAY, 0x04 },
};

static int debug = 0;

static notle_version version;

struct led_config {
  unsigned red_percent;     /* 100 * percent red in output (100 = 1% red) */
  unsigned green_percent;   /* 100 * percent green in output */
  unsigned blue_percent;    /* 100 * percent blue in output */
  unsigned brightness;      /* Total brightness */
  unsigned brightness_limit;/* Maximum allowable brightness */
  unsigned brightness_multiplier;/* Accessibility feature (100 * percentage boost) */
};

/*
 * Initialized in probe from board-notle.c with normalized values where 10,000
 * is 100%.  If formula in colormix_store changes, revise board file values
 */
static struct led_config led_config;

static struct {
  u8 pipeline;
  u8 backlight;
} ice40_defaults = {
  .pipeline =   0x0,
  .backlight =  0x0,
};

static struct panel_notle_busses {
        struct i2c_client *panel_client;
        struct spi_device *ice40_device;
} bus_data;

struct panel_config {
        struct omap_video_timings timings;

        int acbi;        /* ac-bias pin transitions per interrupt */
        /* Unit: line clocks */
        int acb;        /* ac-bias pin frequency */

        enum omap_panel_config config;

        /* Delay in ms between DISPC dis/enable and display dis/enable */
        int power_on_delay;
        int power_off_delay;
};

/* Notle NHD Panel */
static struct panel_config notle_config = {
        .timings = {
                .x_res          = 640,
                .y_res          = 360,
                .pixel_clock    = 85333,

                .hfp            = 10,
                .hsw            = 68,
                .hbp            = 10,

                .vfp            = 5,
                .vsw            = 10,
                .vbp            = 5,
        },
        .acbi                   = 0x0,
        .acb                    = 0x0,
        .config                 = OMAP_DSS_LCD_TFT,
        .power_on_delay         = 0,
        .power_off_delay        = 0,
};

/* Note that this enum and the string array below must match. */
typedef enum {
        TESTPATTERN_NONE = 0,
        TESTPATTERN_COARSE_CHECK,
        TESTPATTERN_FINE_CHECK,
        TESTPATTERN_COLORBARS,
        TESTPATTERN_ALIGNMENT,
        TESTPATTERN_CALIBRATION,
        TESTPATTERN_ALL_OFF,
        TESTPATTERN_ALL_ON,
} testpattern;
static const char* const testpattern_names[] = {
        "none",
        "coarse_checkerboard",
        "fine_checkerboard",
        "color_bars",
        "alignment",
        "calibration",
        "all_px_off",
        "all_px_on",
};

struct notle_drv_data {
        struct omap_dss_device *dssdev;
        struct panel_config *panel_config;
        struct kobject kobj;
        int enabled;
        testpattern pattern;
};

static inline int notle_version_before( notle_version then ) {
        return version < then;
}

static inline int notle_version_after( notle_version then ) {
        return version > then;
}

static inline int notle_version_supported( void ) {
        return ( version>=SUPPORTED_FROM && version <= SUPPORTED_TO );
}

static inline const char* testpattern_name(testpattern pattern) {
        return testpattern_names[pattern];
}

static inline struct panel_notle_data
*get_panel_data(const struct omap_dss_device *dssdev) {
        return (struct panel_notle_data *) dssdev->data;
}

static int requested_l3_throughput;
/* Local functions used by the sysfs interface */
static char tmp_buf[PAGE_SIZE];
static int fpga_rev = -1;
static void panel_notle_power_off(struct omap_dss_device *dssdev);
static int panel_notle_power_on(struct omap_dss_device *dssdev);
static int ice40_read_register(u8 reg_addr);
static int ice40_write_register(u8 reg_addr, u8 reg_value);
static int ice40_set_backlight(int led_en, int rev, int (*rgbmat)[3]);
static int fpga_read_revision(int do_log);
static void led_config_to_linecuts(struct omap_dss_device *dssdev,
                                   struct led_config *led,
                                   int rev,
                                   int (*rgbmat)[3]);
static void led_config_pwm(struct omap_dss_device *dssdev,
                              struct led_config *led,
                              int rev,
                              int (*rgbmat)[3]);
static void fpga_reconfigure(struct notle_drv_data *notle_data);

/* Delayed work to check if FPGA needs reconfiguring */
#define RECONFIGURE_FPGA_CHECK_INTERVAL (3000) /* msec*/
struct workqueue_struct * reconfigure_fpga_wq;

typedef struct {
        struct delayed_work work;
        struct notle_drv_data *notle_data;
} reconfigure_fpga_work_struct;

static struct mutex panel_power_lock;

static reconfigure_fpga_work_struct reconfigure_fpga_work;

/* If screen is on, but FPGA is deconfigured for some reason, reconfigure it */
static void reconfigure_fpga_work_fn(struct work_struct *work) {
        reconfigure_fpga_work_struct * fpga_work = (reconfigure_fpga_work_struct *) work;

        mutex_lock(&panel_power_lock);

        if (fpga_work->notle_data->enabled
            && ice40_read_register(ICE40_REVISION) == 0xff) {
          panel_notle_power_off(fpga_work->notle_data->dssdev);
          fpga_work->notle_data->dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
          fpga_reconfigure(fpga_work->notle_data);
          msleep(100);
          if (!panel_notle_power_on(fpga_work->notle_data->dssdev)) {
            fpga_work->notle_data->dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
          }
        }

        mutex_unlock(&panel_power_lock);

        queue_delayed_work(reconfigure_fpga_wq,
                           (struct delayed_work *)&reconfigure_fpga_work,
                           msecs_to_jiffies(RECONFIGURE_FPGA_CHECK_INTERVAL));
}

/* Sysfs interface */
static ssize_t debug_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "0x%02x\n", debug);
}

static ssize_t debug_store(struct notle_drv_data *notle_data,
                              const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
          return r;

        debug = value;

        return size;
}

static ssize_t sysfs_reset(struct notle_drv_data *notle_data,
                           const char *buf, size_t size) {
        int r, value;

        mutex_lock(&panel_power_lock);

        panel_notle_power_off(notle_data->dssdev);
        notle_data->dssdev->state = OMAP_DSS_DISPLAY_DISABLED;

        r = kstrtoint(buf, 0, &value);
        memcpy(gamma_curve, gamma_curve_legacy, sizeof gamma_curve);
        if (!r && value) {
          fpga_reconfigure(notle_data);
        }

        msleep(100);
        if (!panel_notle_power_on(notle_data->dssdev)) {
          notle_data->dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
        }

        mutex_unlock(&panel_power_lock);

        return size;
}

static ssize_t fpga_revision(struct notle_drv_data *notle_data, char *buf) {
        int rev = fpga_read_revision(1);

        if (rev > 0) {
          /*
           * Cache the fpga revision so we can still print this when
           * the panel is powered off.
           */
           fpga_rev = rev;
        }

        if (fpga_rev < 0) {
          printk(KERN_ERR LOG_TAG "No cached FPGA revision\n");
          return -EIO;
        }

        return snprintf(buf, PAGE_SIZE, "0x%02x\n", fpga_rev);
}

static ssize_t dump_regs(struct notle_drv_data *notle_data,
                         char *buf) {
        int i, j, val;

        *buf = '\0';
        if (notle_version_supported()) {
          for (i = 0; i < sizeof(ice40_regs); ++i) {
            val = ice40_read_register(ice40_regs[i]);
            if (val < 0) {
              snprintf(tmp_buf, PAGE_SIZE, "%s0x%02x: FAILED\n",
                       buf, ice40_regs[i]);
            } else {
              snprintf(tmp_buf, PAGE_SIZE, "%s0x%02x: 0x%02x\n",
                       buf, ice40_regs[i], (val & 0xff));
            }
            strncpy(buf, tmp_buf, PAGE_SIZE);
          }
          for (i = 0; i < sizeof(led_matrix_addr)/sizeof(led_matrix_addr[0]); i++)
            for (j = 0; j < sizeof(led_matrix_addr[0])/sizeof(led_matrix_addr[0][0]); j++) {
              u8 addr = led_matrix_addr[i][j];
              int val_hi, val_lo;
              val_hi = ice40_read_register(addr);
              val_lo = ice40_read_register(addr+1);
              if (val_hi < 0 ) {
                snprintf(tmp_buf, PAGE_SIZE, "%s0x%02x: FAILED\n",
                         buf, addr);
              } else if (val_lo < 0 ) {
                snprintf(tmp_buf, PAGE_SIZE, "%s0x%02x: FAILED\n",
                         buf, addr+1);
              } else {
                snprintf(tmp_buf, PAGE_SIZE, "%s0x%02x: 0x%02x%02x\n",
                         buf, addr, (val_hi & 0xff), (val_lo & 0xff));
              }
              strncpy(buf, tmp_buf, PAGE_SIZE);
            }
        }

        return strlen(buf);
}

static ssize_t enabled_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "%d\n",
                        notle_data->enabled);
}
static ssize_t enabled_store(struct notle_drv_data *notle_data,
                             const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
                return r;

        value = !!value;

        mutex_lock(&panel_power_lock);

        if (value) {
          if (!panel_notle_power_on(notle_data->dssdev)) {
            notle_data->dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
          }
        } else {
          panel_notle_power_off(notle_data->dssdev);
          notle_data->dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
        }

        mutex_unlock(&panel_power_lock);

        return size;
}

static u8 reg_addr = 0;
static ssize_t reg_addr_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "0x%02x\n",
                        reg_addr);
}

static ssize_t reg_addr_store(struct notle_drv_data *notle_data,
                              const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
          return r;

        reg_addr = (u8)(value & 0xff);

        return size;
}

static ssize_t reg_value_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "0x%02x\n",
                        ice40_read_register(reg_addr));
}

static ssize_t reg_value_store(struct notle_drv_data *notle_data,
                               const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
          return r;

        ice40_write_register(reg_addr, (u8)(value & 0xff));

        return size;
}

static ssize_t colormix_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "%u/%u/%u\n",
                        led_config.red_percent,
                        led_config.green_percent,
                        led_config.blue_percent);

}

static ssize_t colormix_store(struct notle_drv_data *notle_data,
                               const char *buf, size_t size) {
        int red, green, blue, total;

        if (sscanf(buf, "%u/%u/%u", &red, &green, &blue) != 3) {
          printk(KERN_ERR LOG_TAG "Failed to colormix_store, malformed"
                 " colormix: %s\n", buf);
          return -EINVAL;
        }

        if (red > 10000 || green > 10000 || blue > 10000) {
          printk(KERN_ERR LOG_TAG "Failed to colormix_store, maximum color"
                 " value of 10000 exceeded: %s\n", buf);
          return -EINVAL;
        }

        total = red + green + blue;
        led_config.red_percent = (red * 10000) / total;
        led_config.green_percent = (green * 10000) / total;
        led_config.blue_percent = (blue * 10000) / total;

        /*
         * If the display is enabled, write the new FPGA config immediately,
         * otherwise it will be written when the display is enabled.
         */
        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
        }

        if (notle_data->enabled && led_config.brightness) {
              int rgbmat[4][3];
              const int rev = fpga_read_revision(0);
              led_config_pwm(notle_data->dssdev, &led_config, rev, rgbmat);
              if (ice40_set_backlight(1, rev, rgbmat)) {
                printk(KERN_ERR LOG_TAG "Failed to colormix_store:"
                       " spi write failed\n");
              }
        }

        return size;
}

static ssize_t list_testpatterns(struct notle_drv_data *notle_data, char *buf) {
        int i;

        *buf = '\0';
        for (i = 0; i < sizeof(testpattern_names) / sizeof(testpattern_names[0]); ++i) {
                if (strlen(buf) + strlen(testpattern_names[i]) + 2 > PAGE_SIZE) {
                        return -EINVAL;
                }
                strcat(buf, testpattern_names[i]);
                strcat(buf, "\n");
        }
        return strlen(buf);
}

static ssize_t testpattern_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "%s\n", testpattern_name(notle_data->pattern));
}

static ssize_t testpattern_store(struct notle_drv_data *notle_data,
                                 const char *buf, size_t size) {
        int i;
        char value[128];

        if (size > 128) return -EINVAL;

        sscanf(buf, "%s", value);

        for (i = 0; i < sizeof(testpattern_names) / sizeof(testpattern_names[0]); i++) {
                if (!strncmp(value, testpattern_names[i], size)) {
                        notle_data->pattern = (testpattern)i;
                }
        }

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        i = ice40_read_register(ICE40_PIPELINE);
        if (i < 0) {
                printk(KERN_ERR LOG_TAG "Failed to testpattern_store: "
                       "register read failed: %i\n", i);
                return -EIO;
        }
        i = (i & ~ICE40_PIPELINE_TESTPAT) | notle_data->pattern;
        if ((i = ice40_write_register(ICE40_PIPELINE, (u8)(i & 0xff))) < 0) {
                printk(KERN_ERR LOG_TAG "Failed to testpattern_store: "
                       "register write failed: %i\n", i);
                return -EIO;
        }

        return size;
}

static ssize_t forcer_show(struct notle_drv_data *notle_data, char *buf) {
        int val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        val = ice40_read_register(ICE40_BACKLIGHT);
        if (val < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forcer_store: "
                       "spi read failed: %i\n", val);
                return -EIO;
        }
        return snprintf(buf, PAGE_SIZE, "%d\n",
                !!(val & ICE40_BACKLIGHT_FORCER));
}
static ssize_t forcer_store(struct notle_drv_data *notle_data,
                                 const char *buf, size_t size) {
        int r, val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        r = kstrtoint(buf, 0, &val);
        if (r)
                return r;

        r = ice40_read_register(ICE40_BACKLIGHT);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forcer_store: "
                       "spi read failed: %i\n", r);
                return -EIO;
        }

        if (val) {
                r |= ICE40_BACKLIGHT_FORCER;
        } else {
                r &= ~ICE40_BACKLIGHT_FORCER;
        }

        r = ice40_write_register(ICE40_BACKLIGHT, r);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forcer_store: "
                       "spi write failed: %i\n", r);
                return -EIO;
        }

        return size;
}
static ssize_t forceg_show(struct notle_drv_data *notle_data, char *buf) {
        int val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        val = ice40_read_register(ICE40_BACKLIGHT);
        if (val < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forceg_store: "
                       "spi read failed: %i\n", val);
                return -EIO;
        }
        return snprintf(buf, PAGE_SIZE, "%d\n",
                !!(val & ICE40_BACKLIGHT_FORCEG));
}
static ssize_t forceg_store(struct notle_drv_data *notle_data,
                                 const char *buf, size_t size) {
        int r, val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        r = kstrtoint(buf, 0, &val);
        if (r)
                return r;

        r = ice40_read_register(ICE40_BACKLIGHT);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forceg_store: "
                       "spi read failed: %i\n", r);
                return -EIO;
        }

        if (val) {
                r |= ICE40_BACKLIGHT_FORCEG;
        } else {
                r &= ~ICE40_BACKLIGHT_FORCEG;
        }

        r = ice40_write_register(ICE40_BACKLIGHT, r);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forceg_store: "
                       "spi write failed: %i\n", r);
                return -EIO;
        }

        return size;
}
static ssize_t forceb_show(struct notle_drv_data *notle_data, char *buf) {
        int val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        val = ice40_read_register(ICE40_BACKLIGHT);
        if (val < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forceb_store: "
                       "spi read failed: %i\n", val);
                return -EIO;
        }
        return snprintf(buf, PAGE_SIZE, "%d\n",
                !!(val & ICE40_BACKLIGHT_FORCEB));
}
static ssize_t forceb_store(struct notle_drv_data *notle_data,
                                 const char *buf, size_t size) {
        int r, val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        r = kstrtoint(buf, 0, &val);
        if (r)
                return r;

        r = ice40_read_register(ICE40_BACKLIGHT);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forceb_store: "
                       "spi read failed: %i\n", r);
                return -EIO;
        }

        if (val) {
                r |= ICE40_BACKLIGHT_FORCEB;
        } else {
                r &= ~ICE40_BACKLIGHT_FORCEB;
        }

        r = ice40_write_register(ICE40_BACKLIGHT, r);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to forceb_store: "
                       "spi write failed: %i\n", r);
                return -EIO;
        }

        return size;
}
static ssize_t cpsel_show(struct notle_drv_data *notle_data, char *buf) {
        int val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        val = ice40_read_register(ICE40_BACKLIGHT);
        if (val < 0) {
                printk(KERN_ERR LOG_TAG "Failed to cpsel_store: "
                       "spi read failed: %i\n", val);
                return -EIO;
        }
        return snprintf(buf, PAGE_SIZE, "%d\n",
                !!(val & ICE40_BACKLIGHT_CPSEL));
}
static ssize_t cpsel_store(struct notle_drv_data *notle_data,
                                 const char *buf, size_t size) {
        int r, val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        r = kstrtoint(buf, 0, &val);
        if (r)
                return r;

        r = ice40_read_register(ICE40_BACKLIGHT);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to cpsel_store: "
                       "spi read failed: %i\n", r);
                return -EIO;
        }

        if (val) {
                r |= ICE40_BACKLIGHT_CPSEL;
        } else {
                r &= ~ICE40_BACKLIGHT_CPSEL;
        }

        r = ice40_write_register(ICE40_BACKLIGHT, r);
        if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to cpsel_store: "
                       "spi write failed: %i\n", r);
                return -EIO;
        }

        return size;
}
static ssize_t mono_show(struct notle_drv_data *notle_data, char *buf) {
        int val;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        val = ice40_read_register(ICE40_BACKLIGHT);
        if (val < 0) {
            printk(KERN_ERR LOG_TAG "Failed to read iCE40 register: "
                   "0x%02x\n", ICE40_BACKLIGHT);
            return -EIO;
        }
        val = !!(val & ICE40_BACKLIGHT_MONO);

        return snprintf(buf, PAGE_SIZE, "%d\n", val);
}
static ssize_t mono_store(struct notle_drv_data *notle_data,
                                 const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
                return r;

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        if (value) {
            ice40_defaults.backlight |= ICE40_BACKLIGHT_MONO;
        } else {
            ice40_defaults.backlight &= ~ICE40_BACKLIGHT_MONO;
        }

        if (notle_data->dssdev->state == OMAP_DSS_DISPLAY_ACTIVE) {
            r = ice40_read_register(ICE40_BACKLIGHT);
            if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to read iCE40 register: "
                       "0x%02x\n", ICE40_BACKLIGHT);
                return -EIO;
            }
            if (value) {
                r |= ICE40_BACKLIGHT_MONO;
            } else {
                r &= ~ICE40_BACKLIGHT_MONO;
            }
            r = ice40_write_register(ICE40_BACKLIGHT, r);
            if (r < 0) {
                printk(KERN_ERR LOG_TAG "Failed to write iCE40 register: "
                       "0x%02x\n", ICE40_BACKLIGHT);
                return -EIO;
            }
        }

        return size;
}

static inline int range_expand(int v)
{
  /* Interim, should probably be scaled exponential */
  if (v <= 223)
    return (v*293)>>8;
  else
    return v*16-(255*16-255*3);
}

static inline int range_reduce(int v)
{
  if (v <= 255)
    return (v*256+292)/293;
  else
    return (v+(255*16-255*3))>>4;
}

static ssize_t brightness_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "%d\n", range_reduce(led_config.brightness));
}

static ssize_t brightness_store(struct notle_drv_data *notle_data,
                                const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
          return r;

        if (value < 0) {
          printk(KERN_ERR LOG_TAG "Failed to brightness_store: "
                 "invalid brightness: %i\n", value);
          return -EINVAL;
        }

        led_config.brightness = range_expand(value);

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        /*
         * If the display is enabled, write the new FPGA config immediately,
         * otherwise it will be written when the display is enabled.
         */
        if (notle_data->enabled) {
            if (led_config.brightness) {
                int rgbmat[4][3];
                const int rev = fpga_read_revision(0);
                led_config_pwm(notle_data->dssdev, &led_config, rev, rgbmat);
                if (ice40_set_backlight(1, rev, rgbmat)) {
                    printk(KERN_ERR LOG_TAG "Failed to brightness_store: "
                         "spi write failed\n");
                }
            } else {
                if (ice40_set_backlight(0, 0, NULL)) {
                    printk(KERN_ERR LOG_TAG "Failed to brightness_store: "
                       "spi write failed\n");
                }
            }
        }

        return size;
}

static ssize_t brightness_limit_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "%d\n", range_reduce(led_config.brightness_limit));
}

static ssize_t brightness_limit_store(struct notle_drv_data *notle_data,
                                const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
          return r;

        if (value < 0) {
          printk(KERN_ERR LOG_TAG "Failed to brightness_limit_store: "
                 "invalid brightness_limit: %i\n", value);
          return -EINVAL;
        }

        led_config.brightness_limit = range_expand(value);

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
                return -EINVAL;
        }

        /*
         * If the display is enabled, write the new FPGA config immediately,
         * otherwise it will be written when the display is enabled.
         */
        if (notle_data->enabled) {
            if (led_config.brightness_limit < led_config.brightness) {
                int rgbmat[4][3];
                const int rev = fpga_read_revision(0);
                led_config_pwm(notle_data->dssdev, &led_config, rev, rgbmat);
                if (ice40_set_backlight(1, rev, rgbmat)) {
                    printk(KERN_ERR LOG_TAG "Failed to brightness_limit_store: "
                         "spi write failed\n");
                }
            }
        }

        return size;
}

static ssize_t brightness_multiplier_show(struct notle_drv_data *notle_data, char *buf) {
        return snprintf(buf, PAGE_SIZE, "%d\n", led_config.brightness_multiplier);
}

static ssize_t brightness_multiplier_store(struct notle_drv_data *notle_data,
                                const char *buf, size_t size) {
        int r, value;
        r = kstrtoint(buf, 0, &value);
        if (r)
          return r;

        if (value < 0) {
          printk(KERN_ERR LOG_TAG "Failed to brightness_multiplier_store: "
                 "invalid brightness_multiplier: %i\n", value);
          return -EINVAL;
        }

        led_config.brightness_multiplier = value;

        return size;
}

static ssize_t gamma_show(struct notle_drv_data *notle_data, char *buf,
                          int gamma) {
        return snprintf(buf, PAGE_SIZE, "%d %d %d %d %d %d\n",
                        gamma_curve[gamma].red_p,
                        gamma_curve[gamma].green_p,
                        gamma_curve[gamma].blue_p,
                        gamma_curve[gamma].red_n,
                        gamma_curve[gamma].green_n,
                        gamma_curve[gamma].blue_n);
}

static int panel_write_register(u8 reg, u8 value);

static void panel_write_gamma(int n)
{
  const int gamma_reg = 0x21;

  panel_write_register(gamma_reg + (6 * n) + 0, gamma_curve[n].red_p);
  panel_write_register(gamma_reg + (6 * n) + 1, gamma_curve[n].green_p);
  panel_write_register(gamma_reg + (6 * n) + 2, gamma_curve[n].blue_p);
  panel_write_register(gamma_reg + (6 * n) + 3, gamma_curve[n].red_n);
  panel_write_register(gamma_reg + (6 * n) + 4, gamma_curve[n].green_n);
  panel_write_register(gamma_reg + (6 * n) + 5, gamma_curve[n].blue_n);
}

static ssize_t gamma_store(struct notle_drv_data *notle_data, const char *buf,
                           size_t size, int gamma) {
        unsigned int r_p, g_p, b_p, r_n, g_n, b_n;
        if (sscanf(buf, "%u %u %u %u %u %u", &r_p, &g_p, &b_p,
                   &r_n, &g_n, &b_n) != 6) {
          printk(KERN_ERR LOG_TAG "Failed to gamma_store, malformed"
                 " gamma: %s\n", buf);
          return -EINVAL;
        }

        if ((r_p | g_p | b_p | r_n | g_n | b_n) & ~0xFF) {
          printk(KERN_ERR LOG_TAG "Failed to gamma_store, invalid value, "
                 "expected single bytes: %s\n", buf);
          return -EINVAL;
        }

        gamma_curve[gamma].red_p   = (u8)(r_p & 0xFF);
        gamma_curve[gamma].green_p = (u8)(g_p & 0xFF);
        gamma_curve[gamma].blue_p  = (u8)(b_p & 0xFF);
        gamma_curve[gamma].red_n   = (u8)(r_n & 0xFF);
        gamma_curve[gamma].green_n = (u8)(g_n & 0xFF);
        gamma_curve[gamma].blue_n  = (u8)(b_n & 0xFF);
        panel_write_gamma(gamma);
        return size;
}
#define GAMMA_SHOW_STORE(x) \
static ssize_t gamma ## x ## _show(struct notle_drv_data *d, \
                                   char *buf) { \
        return gamma_show(d, buf, x - 1); \
} \
static ssize_t gamma ## x ## _store(struct notle_drv_data *d, \
                                    const char *buf, size_t size) { \
        return gamma_store(d, buf, size, x - 1); \
}
GAMMA_SHOW_STORE(1)
GAMMA_SHOW_STORE(2)
GAMMA_SHOW_STORE(3)
GAMMA_SHOW_STORE(4)
GAMMA_SHOW_STORE(5)
GAMMA_SHOW_STORE(6)
GAMMA_SHOW_STORE(7)
GAMMA_SHOW_STORE(8)

static ssize_t gamma_preset_show(struct notle_drv_data *notle_data, char *buf) {
  if (memcmp(gamma_curve, gamma_curve_legacy, sizeof gamma_curve) == 0)
    snprintf(buf, PAGE_SIZE, "%s\n", "legacy");
  else if (memcmp(gamma_curve, gamma_curve_default, sizeof gamma_curve) == 0)
    snprintf(buf, PAGE_SIZE, "%s\n", "default");
  else
    strcpy(buf, "custom");
  return strlen(buf);
}

static ssize_t gamma_preset_store(struct notle_drv_data *notle_data,
                                  const char *buf, size_t size) {
  int j;
  char value[128];

  if (size > 128) return -EINVAL;

  sscanf(buf, "%s", value);
  if (strcmp(value, "legacy") == 0)
    memcpy(gamma_curve, gamma_curve_legacy, sizeof gamma_curve);
  else if (strcmp(value, "default") == 0)
    memcpy(gamma_curve, gamma_curve_default, sizeof gamma_curve);
  else
    return -EINVAL;
  for (j = 0; j < (sizeof(gamma_curve) /
                   sizeof(struct gamma_point)); ++j) {
    panel_write_gamma(j);
  }
  return size;
}

/* Sysfs attribute wrappers for show/store functions */
struct panel_notle_attribute {
        struct attribute attr;
        ssize_t (*show)(struct notle_drv_data *, char *);
        ssize_t (*store)(struct notle_drv_data *, const char *, size_t);
};

#define NOTLE_ATTR(_name, _mode, _show, _store) \
        struct panel_notle_attribute panel_notle_attr_##_name = \
        __ATTR(_name, _mode, _show, _store)

static NOTLE_ATTR(debug, S_IRUGO|S_IWUSR,
                  debug_show, debug_store);
static NOTLE_ATTR(reset, S_IWUSR, NULL, sysfs_reset);
static NOTLE_ATTR(fpga_revision, S_IRUGO, fpga_revision, NULL);
static NOTLE_ATTR(dump_regs, S_IRUGO, dump_regs, NULL);
static NOTLE_ATTR(list_testpatterns, S_IRUGO, list_testpatterns, NULL);
static NOTLE_ATTR(enabled, S_IRUGO|S_IWUSR,
                  enabled_show, enabled_store);
static NOTLE_ATTR(reg_addr, S_IRUGO|S_IWUSR,
                  reg_addr_show, reg_addr_store);
static NOTLE_ATTR(reg_value, S_IRUGO|S_IWUSR,
                  reg_value_show, reg_value_store);
static NOTLE_ATTR(colormix, S_IRUGO|S_IWUSR,
                  colormix_show, colormix_store);
static NOTLE_ATTR(testpattern, S_IRUGO|S_IWUSR,
                  testpattern_show, testpattern_store);
static NOTLE_ATTR(forcer, S_IRUGO|S_IWUSR,
                  forcer_show, forcer_store);
static NOTLE_ATTR(forceg, S_IRUGO|S_IWUSR,
                  forceg_show, forceg_store);
static NOTLE_ATTR(forceb, S_IRUGO|S_IWUSR,
                  forceb_show, forceb_store);
static NOTLE_ATTR(cpsel, S_IRUGO|S_IWUSR,
                  cpsel_show, cpsel_store);
static NOTLE_ATTR(mono, S_IRUGO|S_IWUSR,
                  mono_show, mono_store);
static NOTLE_ATTR(brightness, S_IRUGO|S_IWUSR,
                  brightness_show, brightness_store);
static NOTLE_ATTR(brightness_limit, S_IRUGO|S_IWUSR,
                  brightness_limit_show, brightness_limit_store);
static NOTLE_ATTR(brightness_multiplier, S_IRUGO|S_IWUSR,
                  brightness_multiplier_show, brightness_multiplier_store);
static NOTLE_ATTR(gamma1, S_IRUGO|S_IWUSR,
                  gamma1_show, gamma1_store);
static NOTLE_ATTR(gamma2, S_IRUGO|S_IWUSR,
                  gamma2_show, gamma2_store);
static NOTLE_ATTR(gamma3, S_IRUGO|S_IWUSR,
                  gamma3_show, gamma3_store);
static NOTLE_ATTR(gamma4, S_IRUGO|S_IWUSR,
                  gamma4_show, gamma4_store);
static NOTLE_ATTR(gamma5, S_IRUGO|S_IWUSR,
                  gamma5_show, gamma5_store);
static NOTLE_ATTR(gamma6, S_IRUGO|S_IWUSR,
                  gamma6_show, gamma6_store);
static NOTLE_ATTR(gamma7, S_IRUGO|S_IWUSR,
                  gamma7_show, gamma7_store);
static NOTLE_ATTR(gamma8, S_IRUGO|S_IWUSR,
                  gamma8_show, gamma8_store);
static NOTLE_ATTR(gamma_preset, S_IRUGO|S_IWUSR,
                  gamma_preset_show, gamma_preset_store);

static struct attribute *panel_notle_sysfs_attrs[] = {
        &panel_notle_attr_debug.attr,
        &panel_notle_attr_reset.attr,
        &panel_notle_attr_fpga_revision.attr,
        &panel_notle_attr_dump_regs.attr,
        &panel_notle_attr_list_testpatterns.attr,
        &panel_notle_attr_enabled.attr,
        &panel_notle_attr_reg_addr.attr,
        &panel_notle_attr_reg_value.attr,
        &panel_notle_attr_colormix.attr,
        &panel_notle_attr_testpattern.attr,
        &panel_notle_attr_forcer.attr,
        &panel_notle_attr_forceg.attr,
        &panel_notle_attr_forceb.attr,
        &panel_notle_attr_cpsel.attr,
        &panel_notle_attr_mono.attr,
        &panel_notle_attr_brightness.attr,
        &panel_notle_attr_brightness_limit.attr,
        &panel_notle_attr_brightness_multiplier.attr,
        &panel_notle_attr_gamma1.attr,
        &panel_notle_attr_gamma2.attr,
        &panel_notle_attr_gamma3.attr,
        &panel_notle_attr_gamma4.attr,
        &panel_notle_attr_gamma5.attr,
        &panel_notle_attr_gamma6.attr,
        &panel_notle_attr_gamma7.attr,
        &panel_notle_attr_gamma8.attr,
        &panel_notle_attr_gamma_preset.attr,
        NULL,
};

static ssize_t panel_notle_attr_show(struct kobject *kobj, struct attribute *attr,
                                     char *buf) {
        struct notle_drv_data *panel_notle;
        struct panel_notle_attribute *panel_notle_attr;

        panel_notle = container_of(kobj, struct notle_drv_data, kobj);
        panel_notle_attr = container_of(attr, struct panel_notle_attribute, attr);

        if (!panel_notle_attr->show)
                return -ENOENT;

        return panel_notle_attr->show(panel_notle, buf);
}

static ssize_t panel_notle_attr_store(struct kobject *kobj, struct attribute *attr,
                                      const char *buf, size_t size) {
        struct notle_drv_data *panel_notle;
        struct panel_notle_attribute *panel_notle_attr;

        panel_notle = container_of(kobj, struct notle_drv_data, kobj);
        panel_notle_attr = container_of(attr, struct panel_notle_attribute, attr);

        if (!panel_notle_attr->store)
                return -ENOENT;

        return panel_notle_attr->store(panel_notle, buf, size);
}

static const struct sysfs_ops panel_notle_sysfs_ops = {
        .show = panel_notle_attr_show,
        .store = panel_notle_attr_store,
};

static struct kobj_type panel_notle_ktype = {
        .sysfs_ops = &panel_notle_sysfs_ops,
        .default_attrs = panel_notle_sysfs_attrs,
};

/* Utility functions */
static int bclamp(int b)
{
  return b > MAX_BRIGHTNESS ? MAX_BRIGHTNESS : b;
}

static void led_config_to_linecuts(struct omap_dss_device *dssdev,
                                   struct led_config *led,
                                   int rev,
                                   int (*rgbmat)[3]) {
        int red, grn, blu;
        int *red_linecut = rgbmat[0]+0;
        int *grn_linecut = rgbmat[1]+1;
        int *blu_linecut = rgbmat[2]+2;
        const struct omap_video_timings *t = &(dssdev->panel.timings);
        int total_lines = t->y_res +
            t->vfp +
            t->vsw +
            t->vbp;
        struct panel_notle_data *panel_data = get_panel_data(dssdev);

        red = *red_linecut   = (int)(total_lines *
                                  (10000 - (
                                   (3 * led->red_percent * bclamp(led->brightness) * panel_data->limit_mw) /
                                   (panel_data->red_max_mw * MAX_BRIGHTNESS)))) /
                                 10000;
        grn = *grn_linecut = (int)(total_lines *
                                  (10000 - (
                                   (3 * led->green_percent * bclamp(led->brightness) * panel_data->limit_mw) /
                                   (panel_data->green_max_mw * MAX_BRIGHTNESS)))) /
                                 10000;
        blu =  *blu_linecut = (int)(total_lines *
                                  (10000 - (
                                   (3 * led->blue_percent * bclamp(led->brightness) * panel_data->limit_mw) /
                                   (panel_data->blue_max_mw * MAX_BRIGHTNESS)))) /
                                 10000;

        /*
         * This will cause a slight color shift at very dim brightness values,
         * but the altnerative is to cause a sudden color shift by dropping
         * the lowest LED entirely.  This is a side effect of the way the fpga
         * is implemented - there's no way to dim a color channel less than a
         * single line.
         */
        if (*red_linecut > t->y_res - 3)
          *red_linecut = t->y_res - 3;
        if (*grn_linecut > t->y_res - 3)
          *grn_linecut = t->y_res - 3;
        if (*blu_linecut > t->y_res - 3)
          *blu_linecut = t->y_res - 3;

        /* Disable any channels that are explicitly at zero percent */
        if (!led->red_percent) *red_linecut = total_lines;
        if (!led->green_percent) *grn_linecut = total_lines;
        if (!led->blue_percent) *blu_linecut = total_lines;

        /* Set to full-brightness any channels that overflowed */
        if (*red_linecut < 0) *red_linecut = 0;
        if (*grn_linecut < 0) *grn_linecut = 0;
        if (*blu_linecut < 0) *blu_linecut = 0;

        if (red != *red_linecut || grn != *grn_linecut || blu != *blu_linecut) {
          if (debug & 1)
            printk(KERN_INFO LOG_TAG "Linecuts truncated: %i/%i/%i -> %i/%i/%i"
                   ", Config: %u/%u/%u/%u\n",
                   red, grn, blu, *red_linecut, *grn_linecut, *blu_linecut,
                   bclamp(led->brightness), led->red_percent, led->green_percent, led->blue_percent);
        }

        if (rev > FINAL_LINECUT_BASED_FPGA_REVISION) {
          int i,j;
          const int vres = t->y_res;
          const int htot = t->hfp + t->hsw + t->x_res + t->hbp;
          const int pixels_in_frame = htot*(t->vfp + t->vsw + vres + t->vbp);
          const int count_rescale = 7;
          const int backlight_state = ice40_read_register(ICE40_BACKLIGHT);
          struct omap_overlay_manager_info info;
          for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
              if (i != j)
                rgbmat[i][j] = 0;
          // 357 -> 3 lines -> 25us -> 728*3/128 = 17
          // 0 -> 360 lines -> 3.06ms -> 728*360/128 = 2047
          /* convert to pclk counts */
          for (i = 0; i < 3; i++) {
            rgbmat[i][i] = htot*(vres-rgbmat[i][i]);
          }
          red = *red_linecut;
          grn = *grn_linecut;
          blu = *blu_linecut;
          /* WARNING: Do not rely on panel_data->cpr_enable.  It is used at
           * driver probe time to set cpr_enable in dssdev->manager->info.cpr_enable
           * but is not updated on subsequent changes made through
           * /sys/devices/platform/omapdss/manager2/cpr_enable
           */
          dssdev->manager->get_manager_info(dssdev->manager, &info);
          if (info.cpr_enable == 0) {
            /* Apply cpr in the LED illumination schedule.
             * My preference would be a for loop, but struct omap_dss_cpr_coefs
             * declares the "matrix" as a list of scalars :-(
             */
            const struct omap_dss_cpr_coefs *c = &(info.cpr_coefs);
            rgbmat[0][0] = c->rr < 0 ? 0 : (red*c->rr+128)>>8;
            rgbmat[0][1] = c->rg < 0 ? 0 : (grn*c->rg+128)>>8;
            rgbmat[0][2] = c->rb < 0 ? 0 : (blu*c->rb+128)>>8;
            rgbmat[1][0] = c->gr < 0 ? 0 : (red*c->gr+128)>>8;
            rgbmat[1][1] = c->gg < 0 ? 0 : (grn*c->gg+128)>>8;
            rgbmat[1][2] = c->gb < 0 ? 0 : (blu*c->gb+128)>>8;
            rgbmat[2][0] = c->br < 0 ? 0 : (red*c->br+128)>>8;
            rgbmat[2][1] = c->bg < 0 ? 0 : (grn*c->bg+128)>>8;
            rgbmat[2][2] = c->bb < 0 ? 0 : (blu*c->bb+128)>>8;
          }
          for (i = 0; i < 3; i++) {
            rgbmat[3][i] = rgbmat[i][0] + rgbmat[i][1] + rgbmat[i][2];
            if ((backlight_state & 0x80) == 0x80)
              rgbmat[3][i] /= 3;
            if (rgbmat[3][i] > pixels_in_frame)
              rgbmat[3][i] = pixels_in_frame;
          }

          /* convert to rescaled counts from end of frame
           * NB LED is on from some moment in the middle of the frame to the
           * end of frame so that LC transition occurs as much as possible
           * while LEDs are off.
           */
          for (i = 0; i < 4; i++)
            for (j = 0; j < 3; j++)
              rgbmat[i][j] = (pixels_in_frame-rgbmat[i][j])>>count_rescale;
        }

        return;
}

static void led_config_pwm(struct omap_dss_device *dssdev,
                           struct led_config *led,
                           int rev,
                           int (*rgbmat)[3]) {
  if (rev <= FINAL_LINECUT_BASED_FPGA_REVISION) {
    // revert to legacy method
    led_config_to_linecuts(dssdev, led, rev, rgbmat);
    return;
  }

  {
    const struct omap_video_timings *t = &(dssdev->panel.timings);
    const int htot = t->hfp + t->hsw + t->x_res + t->hbp;
    const int vtot = t->vfp + t->vsw + t->y_res + t->vbp;
    const int count_rescale = 7;
    const int ticks_in_frame = (htot*vtot)>>count_rescale;
    struct panel_notle_data *pd = get_panel_data(dssdev);
    const int scaled_b = range_expand(
        range_reduce(led->brightness) * led->brightness_multiplier / 10000);
    const int blimit = led->brightness_limit;
    const int b = scaled_b <= blimit ? scaled_b : blimit;
    const int backlight_state = ice40_read_register(ICE40_BACKLIGHT);
    const int ledaux_state = ice40_read_register(ICE40_LCOS);
    struct omap_overlay_manager_info info;
    int i,j;
    int red, grn, blu;
    dssdev->manager->get_manager_info(dssdev->manager, &info);

    red = (ticks_in_frame * (( (3 * led->red_percent   * b * pd->limit_mw) /  (pd->red_max_mw   * MAX_BRIGHTNESS)))) / 10000;
    grn = (ticks_in_frame * (( (3 * led->green_percent * b * pd->limit_mw) /  (pd->green_max_mw * MAX_BRIGHTNESS)))) / 10000;
    blu = (ticks_in_frame * (( (3 * led->blue_percent  * b * pd->limit_mw) /  (pd->blue_max_mw  * MAX_BRIGHTNESS)))) / 10000;

    const struct omap_dss_cpr_coefs *c;
    static const struct omap_dss_cpr_coefs identity =
    { 256, 0, 0, 0, 256, 0, 0, 0, 256 };
    if (info.cpr_enable == 0)
      /* Apply cpr in the LED illumination schedule. */
      c = &(info.cpr_coefs);
    else
      c = &identity;

    rgbmat[0][0] = (red*c->rr)>>8;
    rgbmat[0][1] = (grn*c->rg)>>8;
    rgbmat[0][2] = (blu*c->rb)>>8;
    rgbmat[1][0] = (red*c->gr)>>8;
    rgbmat[1][1] = (grn*c->gg)>>8;
    rgbmat[1][2] = (blu*c->gb)>>8;
    rgbmat[2][0] = (red*c->br)>>8;
    rgbmat[2][1] = (grn*c->bg)>>8;
    rgbmat[2][2] = (blu*c->bb)>>8;

    if (ledaux_state & 0x02) {
      /* Double strength green
       * LED is sub-linear at 60mA, From data sheet luminosity boost
       * is ~1.6 = 410/256
       */
      for (i = 0; i < 3; i++) rgbmat[1][i] = (rgbmat[1][i]*410)>>8;
    }

    /* Calculate monochrome PWM */
    int max_pwm = 0;
    int max_chan = -1;
    for (i = 0; i < 3; i++) {
      rgbmat[3][i] = rgbmat[i][0] + rgbmat[i][1] + rgbmat[i][2];
      if (max_pwm < rgbmat[3][i]) {
        max_pwm = rgbmat[3][i];
        max_chan = i;
      }
    }
    if (debug & 1)
      printk(KERN_INFO LOG_TAG "rawmat %6d: %d %d %d %d %d %d %d %d %d %d %d %d\n",
             b,
             rgbmat[0][0], rgbmat[0][1], rgbmat[0][2],
             rgbmat[1][0], rgbmat[1][1], rgbmat[1][2],
             rgbmat[2][0], rgbmat[2][1], rgbmat[2][2],
             rgbmat[3][0], rgbmat[3][1], rgbmat[3][2]);
    if (max_pwm > 3*ticks_in_frame)
    {
      /* Exceeds 100% duty cycle!  Scale back in preference to distorting
       * white balance.
       */
      const int s = (3*ticks_in_frame<<16)/max_pwm;
      if (debug & 1)
        printk(KERN_INFO LOG_TAG "over-committed PWM channel=%d: %d > %d,"
               " rescaling ...\n",
               max_chan, max_pwm, 3*ticks_in_frame);
      for (i = 0; i < 4; i++)
        for (j = 0; j < 3; j++)
          rgbmat[i][j] = (rgbmat[i][j] * s + (1<<15))>>16;
      if (debug & 1)
        printk(KERN_INFO LOG_TAG
               "scamat %6d: %d %d %d %d %d %d %d %d %d %d %d %d\n",
               s,
               rgbmat[0][0], rgbmat[0][1], rgbmat[0][2],
               rgbmat[1][0], rgbmat[1][1], rgbmat[1][2],
               rgbmat[2][0], rgbmat[2][1], rgbmat[2][2],
               rgbmat[3][0], rgbmat[3][1], rgbmat[3][2]);
    }
    for (i = 0; i < 3; i++) {
      if (rev >= FIRST_MONO_ALL_FIELDS_FPGA_REVISION
          || (backlight_state & 0x80) == 0x80)
        rgbmat[3][i] /= 3;
    }
    /* clamp gray */
    for (i = 0; i < 3; i++) {
      if (rgbmat[3][i] > ticks_in_frame)
        rgbmat[3][i] = ticks_in_frame;
      if (rgbmat[3][i] < 0)
        rgbmat[3][i] = 0;
    }

    /* redistribute overflows -- because of rescale, must fit*/
    for (i = 0; i < 3; i++) {
      if (rgbmat[i][i] > ticks_in_frame) {
        int excess;
        const dirn = i < 2 ? +1 : -1;
        for (j = dirn > 0 ? 1 : 5; j != 3; j+=dirn) {
          excess = rgbmat[i][(i+j-dirn)%3] - ticks_in_frame;
          if (excess > 0) {
            rgbmat[i][(i+j-dirn)%3] = ticks_in_frame;
            rgbmat[i][(i+j)%3] += excess;
          }
        }
      }
    }

    if (debug & 1)
      printk(KERN_INFO LOG_TAG
             "dismat %6d: %d %d %d %d %d %d %d %d %d %d %d %d\n",
             0,
             rgbmat[0][0], rgbmat[0][1], rgbmat[0][2],
             rgbmat[1][0], rgbmat[1][1], rgbmat[1][2],
             rgbmat[2][0], rgbmat[2][1], rgbmat[2][2],
             rgbmat[3][0], rgbmat[3][1], rgbmat[3][2]);
    /* convert to rescaled counts from end of frame
     * NB LED is on from some moment in the middle of the frame to the
     * end of frame so that LC transition occurs as much as possible
     * while LEDs are off.
     */
    for (i = 0; i < 4; i++)
      for (j = 0; j < 3; j++) {
        if (rgbmat[i][j] < 0) {
          if (debug & 1)
            printk(KERN_INFO LOG_TAG "pwm undercommit %d %d: %d\n",
                   i,j, rgbmat[i][j]);
          rgbmat[i][j] = 0;
        }
        else if (rgbmat[i][j] > ticks_in_frame) {
          if (debug & 1)
            printk(KERN_INFO LOG_TAG "pwm overcommit %d %d: %d\n",
                   i,j, rgbmat[i][j]);
          rgbmat[i][j] = ticks_in_frame;
        }
        rgbmat[i][j] = (ticks_in_frame-rgbmat[i][j]);
      }
  }
}

static int panel_write_register(u8 reg, u8 value) {
        static int printed_error = 0;
        u8 buf[2];
        int r;
        struct i2c_msg msgs[1];

        if (!bus_data.panel_client) {
                printk(KERN_ERR LOG_TAG
                       "No I2C data set in panel_write_register\n");
                return -1;
        }

        buf[0] = reg;
        buf[1] = value;

        msgs[0].addr = bus_data.panel_client->addr;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(buf);
        msgs[0].buf = buf;

        r = i2c_transfer(bus_data.panel_client->adapter, msgs, 1);
        if (r < 0) {
                if (!printed_error) {
                        printk(KERN_ERR LOG_TAG
                               "Failed to write 0x%02x to panel "
                               "register 0x%02x: %i\n", value, reg, r);
                        printed_error = 1;
                }
                return r;
        }

        return 0;
}

static int ice40_read_register(u8 reg_addr) {
  int val;

  if (!bus_data.ice40_device) {
    printk(KERN_ERR LOG_TAG "No iCE40 bus data set in ice40_read_register()\n");
    return -1;
  }
  val = spi_w8r8(bus_data.ice40_device, (reg_addr & 0x7f));
  return val;
}

static int ice40_write_register(u8 reg_addr, u8 reg_value) {
  u8 buf[] = {reg_addr | 0x80, reg_value};

  if (!bus_data.ice40_device) {
    printk(KERN_ERR LOG_TAG "No iCE40 bus data set in ice40_write_register()\n");
    return -1;
  }
  return spi_write(bus_data.ice40_device, buf, sizeof(buf));
}

/*
 * Set backlight parameters.  Pass -1 to any argument to ignore that value and
 * not set it in the relevant register.
 */
static int ice40_set_backlight(int led_en, int rev, int (*rgbmat)[3]) {
  int val;
  int ret = 0;

  if (rgbmat != NULL) {
    const int r = rgbmat[0][0];
    const int g = rgbmat[1][1];
    const int b = rgbmat[2][2];

    ice40_read_register(ICE40_BACKLIGHT);

    if (rev <= FINAL_LINECUT_BASED_FPGA_REVISION) {
      ret |= ice40_write_register(ICE40_LED_RR_H, (r & 0xff00) >> 8);
      ret |= ice40_write_register(ICE40_LED_RR_L, (r & 0xff));
      ret |= ice40_write_register(ICE40_LED_GG_H, (g & 0xff00) >> 8);
      ret |= ice40_write_register(ICE40_LED_GG_L, (g & 0xff));
      ret |= ice40_write_register(ICE40_LED_BB_H, (b & 0xff00) >> 8);
      ret |= ice40_write_register(ICE40_LED_BB_L, (b & 0xff));
    } else {
      int i,j;
      for (i = 0; i < sizeof(led_matrix_addr)/sizeof(led_matrix_addr[0]); i++)
        for (j = 0; j < sizeof(led_matrix_addr[0])/sizeof(led_matrix_addr[0][0]); j++) {
          const u8 addr = led_matrix_addr[i][j];
          // TODO consider temporarily writing hi byte ff to protect against glitches--probably unnecessary
          ret |= ice40_write_register(addr+0, (rgbmat[i][j]>>8)&0xff);
          ret |= ice40_write_register(addr+1,  rgbmat[i][j]    &0xff);
        }
    }
  }

  if (led_en > -1) {
    val = ice40_read_register(ICE40_BACKLIGHT);
    if (val < 0) {
      ret |= val;
    } else if (led_en) {
      val |= ICE40_BACKLIGHT_LEDEN;
    } else {
      val &= ~ICE40_BACKLIGHT_LEDEN;
    }
    ret |= ice40_write_register(ICE40_BACKLIGHT, val);
  }

  return ret;
}

static int fpga_read_revision(int do_log) {
        int r, rev = -1;

        if ((r = ice40_read_register(ICE40_REVISION)) < 0) {
                printk(KERN_ERR LOG_TAG "Failed to read iCE40 FPGA config: %i\n", r);
        }
        rev = r;

        if (rev > 0 && do_log) {
          printk(KERN_INFO LOG_TAG "FPGA Revision: 0x%02x, Notle Version: %i\n",
                 (u8)rev, version);
        }

        return rev;
}

/* reconfigure FPGA */

/* struct declarations duplicated from u-boot !!! */

struct fpga_header {
  u8 magic[4]; /* Two variants are supported:
                  "FPGA" for padded format, "fpga" for unpadded */
  u32 entry_count;
  /*
   * CRC32 of the fpga_header (with crc32 zeroed) and fpga_entries,
   * but does not include any padding in the fpga_image struct.
   */
  u32 crc32;
} __attribute__ ((packed));

struct fpga_entry {
  u32 revision;
  u8 supported_board_revs[8];
  u32 raw_length;  /* Length of the raw image in bytes */
  u8 raw_image[4]; /* placeholder, size is determined by raw_length */
} __attribute__ ((packed));

inline static struct fpga_entry *next_entry(const struct fpga_entry *entry) {
  return (struct fpga_entry *)
      (((u8 *) entry)
       + offsetof(struct fpga_entry, raw_image)
       + entry->raw_length);
}

static int ice40_load(const u32 size, const u8 *bits, struct notle_drv_data *notle_data) {
  struct omap_dss_device *dssdev = notle_data->dssdev;
  struct panel_notle_data *panel_data = get_panel_data(dssdev);
  u8 zero_byte = 0;
  int i;
  int r;
  u8 *bits_buffer;
  const int bufsz = 32768;
  int status = -1;

  bits_buffer = kmalloc(bufsz, GFP_KERNEL);
  if (!bus_data.ice40_device) {
    printk(KERN_ERR LOG_TAG "ice40_load: No iCE40 bus data set in ice40_load()\n");
    goto err;
  }
  printk(KERN_WARNING LOG_TAG "ice40_load: CDONE before deconfig %d\n", gpio_get_value(panel_data->gpio_fpga_cdone));
  /* set CS polarity *active* high so it is low when creset goes high */
  bus_data.ice40_device->mode |= SPI_CS_HIGH;
  spi_setup(bus_data.ice40_device);
  gpio_set_value(panel_data->gpio_fpga_creset_b, 0);
  mdelay(1);
  gpio_set_value(panel_data->gpio_fpga_creset_b, 1);
  mdelay(1);

  if (gpio_get_value(panel_data->gpio_fpga_cdone) == 1) {
    printk(KERN_WARNING LOG_TAG "CDONE high after reset wait\n");
    goto err;
  }
  /* Send blank byte preamble */
  spi_write(bus_data.ice40_device, &zero_byte, sizeof zero_byte);
  /* Can't send firmware image data directly to SPI driver, due to
   * DMA accessibility issues.  Need a local copy.
   */
  for (i = 0; i < size; i+=bufsz) {
    memcpy(bits_buffer, bits + i, (size-i) > bufsz ? bufsz : size-i);
    spi_write(bus_data.ice40_device, bits_buffer,
              (size-i) > bufsz ? bufsz : size-i);
  }
  for (i = 0; i < 13; i++)
    spi_write(bus_data.ice40_device, &zero_byte, sizeof zero_byte);
  /* Wait for CDONE */
  for (i = 0; i < 1000; ++i) {
    if (gpio_get_value(panel_data->gpio_fpga_cdone) == 1) {
      break;
    }
  }
  if (i == 1000) {
    printk(KERN_WARNING LOG_TAG "WARNING: Timeout waiting for CDONE\n");
    goto err;
  }

  /* restore CS polarity */
  bus_data.ice40_device->mode &= ~SPI_CS_HIGH;
  spi_setup(bus_data.ice40_device);
  if (panel_data->platform_enable) {
    r = panel_data->platform_enable(dssdev);
    if (r) {
      printk(KERN_ERR LOG_TAG "Failed to platform_enable\n");
      goto err;
    }
  }
  if ((r = ice40_read_register(ICE40_REVISION)) < 0) {
    printk(KERN_WARNING LOG_TAG "Failed to read iCE40 FPGA config: %i\n", r);
    goto err;
  }
  if (r == 0xff) {
    printk(KERN_WARNING LOG_TAG "ERROR: FPGA revision 0xff is invalid\n");
    goto err;
  }
  status = 0;
err:
  kfree(bits_buffer);
  return status;
}

static int fpga_reconfigure_inner(const struct firmware *fw,
                                  struct notle_drv_data *notle_data) {
  const void *fpga_image = fw->data;
  const struct fpga_header *fw_header = fpga_image;
  /* 1st fpga_entry immediately follows header */
  const struct fpga_entry *fw_entry0 = (void *) (fw_header+1);
  const struct fpga_entry *entry;
  const void *fw_lim = (char *) fpga_image + fw->size;
  int i, j;
  u32 expected_crc;
  u32 actual_crc;
  if (!notle_version_supported()) {
    printk(KERN_WARNING LOG_TAG "Unsupported Notle version:"
           " %d\n", version);
    return -1;
  }
  if (memcmp(fw_header->magic, "fpga", 4) != 0) {
    printk(KERN_WARNING LOG_TAG "firmware image: bad magic number (%08x)\n",
           *(u32 *)(fw_header->magic));
    return -1;
  }

  expected_crc = fw_header->crc32;
  {
    const struct fpga_entry *image_end;
    const u32 zero_u32 = 0;
    const u32 zero_posn = offsetof(struct fpga_header, crc32);
    for (i = 0, image_end = fw_entry0;
         i < fw_header->entry_count && ((void *) image_end) < fw_lim;
         i++) {
      image_end = next_entry(image_end);
    }
    /* fpga image CRC requires encapsulating system crc32 with
     * negation (~) on entry/exit
     */
    actual_crc =  crc32(~0, (u8*)fpga_image, zero_posn);
    actual_crc =  crc32( actual_crc, (u8 *) &zero_u32, sizeof zero_u32);
    actual_crc = ~crc32( actual_crc, ((u8*)fpga_image) + zero_posn + sizeof zero_u32,
                       ((u8 *) image_end)
                       - (((u8 *) fpga_image) + zero_posn + sizeof zero_u32));
    if (expected_crc != actual_crc) {
      printk(KERN_WARNING LOG_TAG
             "FPGA image CRC failed, expected 0x%08x, found 0x%08x\n",
             expected_crc, actual_crc);
      return -1;
    }
  }
  /* search available entries for a compatible FPGA bitstream */
  for (i = 0, entry = fw_entry0;
       i < fw_header->entry_count;
       ++i, entry = next_entry(entry)) {
    for (j = 0; (j < sizeof(entry->supported_board_revs)) &&
         entry->supported_board_revs[j];
         ++j) {
      if (entry->supported_board_revs[j] == version) {
        printk(KERN_INFO LOG_TAG
               "ice40_load rev 0x%02x for Board ID 0x%02x (%d bytes)\n",
               entry->revision, version, entry->raw_length);
        if (ice40_load(entry->raw_length, entry->raw_image, notle_data) == 0)
          return 0;
      }
    }
  }
  printk(KERN_WARNING LOG_TAG "Found no FPGA image for Board ID 0x%02x\n",
         version);

  return -1;
}

static void fpga_reconfigure(struct notle_drv_data *notle_data) {
  const struct firmware *fw;
  const char *fpga_img_name = "dss_fpga.img";
  int status;

  if (debug & 1)
    printk(KERN_INFO LOG_TAG "request_firmware %s ...\n", fpga_img_name);
  status = request_firmware(&fw, fpga_img_name, &(notle_data->dssdev->dev));
  if (status) {
    printk(KERN_WARNING LOG_TAG "request_firmware %s failed, status %d\n", fpga_img_name, status);
  }
  else {
    if (debug & 1)
      printk(KERN_INFO LOG_TAG "request_firmware %s size=%d\n",
             fpga_img_name, fw->size);
    if (fpga_reconfigure_inner(fw, notle_data) == 0) {
      ice40_write_register(ICE40_PIPELINE, ice40_defaults.pipeline);
      ice40_write_register(ICE40_BACKLIGHT, ice40_defaults.backlight);
    }
    release_firmware(fw);
  }
}

/* Functions to perform actions on the panel and DSS driver */
static int panel_notle_power_on(struct omap_dss_device *dssdev) {
        int i, j, gamma_reg;
        int status, rev;
        struct panel_notle_data *panel_data = get_panel_data(dssdev);
        struct notle_drv_data *drv_data = dev_get_drvdata(&dssdev->dev);
        struct panel_config *panel_config = drv_data->panel_config;

        if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE) {
          return 0;
        }

        printk(KERN_INFO LOG_TAG "Powering on\n");

        status = omapdss_dpi_display_enable(dssdev);
        if (status) {
          printk(KERN_ERR LOG_TAG "Failed to enable DPI\n");
          goto err0;
        }

        if(dssdev->skip_init) {
          dssdev->skip_init = false;
          return 0;
        }

        if (panel_config->power_on_delay) {
          msleep(panel_config->power_on_delay);
        }

        if (panel_data->platform_enable) {
          status = panel_data->platform_enable(dssdev);
          if (status) {
            printk(KERN_ERR LOG_TAG "Failed to platform_enable\n");
            goto err1;
          }
        }

        /* Check FPGA is reporting a valid revision,
         * if not attempt to reconfigure
         */
        rev = ice40_read_register(ICE40_REVISION);
        if (rev == 0xff) {
          printk(KERN_WARNING LOG_TAG
                 "WARNING: "
                 "Probable deconfiguration of FPGA, reconfiguring ...\n");
          fpga_reconfigure(drv_data);
        }

        /*
        ** TODO(madsci): use fpga version instead of notle version here
        */
        for (i = 0; i < ARRAY_SIZE(panel_init_regs); ++i) {
          if (panel_init_regs[i].reg == REG_DELAY) {
            if ( notle_version_after(V1_EVT1) ){
              status = ice40_write_register(ICE40_LCOS, ICE40_LCOS_DISP_ENB);
              if (status) {
                printk(KERN_ERR LOG_TAG "Failed to panel_enable\n");
                goto err1;
              }
            } else if (panel_data->panel_enable) {
              status = panel_data->panel_enable();
              if (status) {
                printk(KERN_ERR LOG_TAG "Failed to panel_enable\n");
                goto err1;
              }
            }
            msleep(panel_init_regs[i].value);
            continue;
          }
          if (panel_init_regs[i].reg == REG_GAMMA) {
            gamma_reg = panel_init_regs[i].value;
            for (j = 0; j < (sizeof(gamma_curve) /
                             sizeof(struct gamma_point)); ++j) {
              panel_write_gamma(j);
            }
            continue;
          }

          /* Make sure we don't misinterpret any special regs. */
          if (!(panel_init_regs[i].reg & ~0xFF)) {
            panel_write_register((u8)(panel_init_regs[i].reg & 0xFF),
                                 panel_init_regs[i].value);
          } else {
            printk(KERN_WARNING LOG_TAG "Unrecognized special register in"
                   " LCOS initialization: 0x%04x", panel_init_regs[i].reg);
          }
        }

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
              goto err1;
        }

        /* Load defaults */
        ice40_write_register(ICE40_PIPELINE, ice40_defaults.pipeline);
        ice40_write_register(ICE40_BACKLIGHT, ice40_defaults.backlight);

        /* Enable LED backlight if we have nonzero brightness */
        if (led_config.brightness > 0) {
              int rgbmat[4][3];
              msleep(1);
              rev = fpga_read_revision(1);
              led_config_pwm(dssdev, &led_config, rev, rgbmat);
              ice40_set_backlight(1, rev, rgbmat);
        }

        queue_delayed_work(reconfigure_fpga_wq,
                           (struct delayed_work *)&reconfigure_fpga_work,
                           msecs_to_jiffies(RECONFIGURE_FPGA_CHECK_INTERVAL));
        drv_data->enabled = 1;
        return 0;
err1:
        omapdss_dpi_display_disable(dssdev);
err0:
        return status;
}

static void panel_notle_power_off(struct omap_dss_device *dssdev) {
        struct panel_notle_data *panel_data = get_panel_data(dssdev);
        struct notle_drv_data *drv_data = dev_get_drvdata(&dssdev->dev);
        struct panel_config *panel_config = drv_data->panel_config;
        int i;

        if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE) {
          return;
        }

        printk(KERN_INFO LOG_TAG "Powering off\n");

        cancel_delayed_work((struct delayed_work*)&reconfigure_fpga_work);

        if (!notle_version_supported()) {
              printk(KERN_ERR LOG_TAG "Unsupported Notle version:"
                     " %d\n", version);
              return;
        }

        /* Disable LED backlight */
        /* Don't change the color mix, just disable the backlight. */
        if (ice40_set_backlight(0, 0, NULL)) {
          printk(KERN_ERR LOG_TAG "Failed to disable iCE40 FPGA LED_EN\n");
        }
        /* Save register values so we can restore them when we power on. */
        i = ice40_read_register(ICE40_BACKLIGHT);
        if (i >= 0 && i != 0xff) {
          /* if 0xff is an illegal value.  Assume FPGA is in a bad state and do
           * not cache bogus value
           */
          ice40_defaults.backlight = i;
        }

        for (i = 0; i < ARRAY_SIZE(panel_shutdown_regs); ++i) {
          if (panel_shutdown_regs[i].reg == REG_DELAY) {
            msleep(panel_shutdown_regs[i].value);
            continue;
          }

          panel_write_register(panel_shutdown_regs[i].reg,
                               panel_shutdown_regs[i].value);
        }

        /*
         * TODO(madsci): Use fpga version instead of notle version here
         */
        /* Disable DISP_ENB */
        if ( notle_version_after(V1_EVT1) ) {
          ice40_write_register(ICE40_LCOS, 0x0);
        } else if (panel_data->panel_disable) {
          panel_data->panel_disable();
        }

        /* Disable LCD_RST_N */
        if (panel_data->platform_disable) {
          panel_data->platform_disable(dssdev);
        }

        if (panel_config->power_off_delay) {
          msleep(panel_config->power_off_delay);
        }

        omapdss_dpi_display_disable(dssdev);
        drv_data->enabled = 0;

}

static void panel_notle_version_config(int version,
                                       struct omap_dss_device *dssdev)
{
    struct panel_notle_data *panel_data = get_panel_data(dssdev);
    struct omap_overlay_manager_info info;

    /* set up configuration from board file that is version specific */
    led_config.red_percent = panel_data->red_percent;
    led_config.green_percent = panel_data->green_percent;
    led_config.blue_percent = panel_data->blue_percent;
    led_config.brightness_limit = MAX_BRIGHTNESS;
    led_config.brightness_multiplier = 10000;

    dssdev->manager->get_manager_info(dssdev->manager, &info);
    info.cpr_enable = panel_data->cpr_enable;
    info.cpr_coefs = panel_data->cpr_coefs;
    info.gamma_enable = panel_data->gamma_enable;
    if (panel_data->gamma_table != NULL) {
        memcpy(info.gamma_table, panel_data->gamma_table, OMAP_DSS_GAMMA_TABLE_SIZE*sizeof(u32));
        info.gamma_table_dirty = true;
    }
    dssdev->manager->set_manager_info(dssdev->manager, &info);
}

static int panel_notle_probe(struct omap_dss_device *dssdev) {
        int r;
        struct panel_config *panel_config = &notle_config;
        struct panel_notle_data *panel_data = get_panel_data(dssdev);
        struct notle_drv_data *drv_data = NULL;

        dev_warn(&dssdev->dev, "panel_notle_probe start\n");

        version = panel_data->notle_version;
        panel_notle_version_config(version, dssdev);

        dssdev->panel.config = panel_config->config;
        dssdev->panel.timings = panel_config->timings;
        dssdev->panel.acb = panel_config->acb;
        dssdev->panel.acbi = panel_config->acbi;

        drv_data = kzalloc(sizeof(*drv_data), GFP_KERNEL);
        if (!drv_data) {
                return -ENOMEM;
        }

        drv_data->dssdev = dssdev;
        drv_data->panel_config = panel_config;
        drv_data->enabled = 0;
        memcpy(gamma_curve, gamma_curve_legacy, sizeof gamma_curve);

        dev_set_drvdata(&dssdev->dev, drv_data);

        r = kobject_init_and_add(&drv_data->kobj, &panel_notle_ktype,
                        &dssdev->manager->kobj, "panel-notle-dpi");
        if (r) {
                printk(KERN_WARNING LOG_TAG "Failed to create sysfs directory\n");
        }

        printk(KERN_WARNING LOG_TAG "Creating display FPGA reconfigure workueue\n");

        mutex_init(&panel_power_lock);
        reconfigure_fpga_wq = create_freezable_workqueue(dev_name(&dssdev->dev));
        INIT_DELAYED_WORK((struct delayed_work*)&reconfigure_fpga_work,
                           reconfigure_fpga_work_fn);
        reconfigure_fpga_work.notle_data = drv_data;

        queue_delayed_work(reconfigure_fpga_wq,
                           (struct delayed_work *)&reconfigure_fpga_work,
                           msecs_to_jiffies(RECONFIGURE_FPGA_CHECK_INTERVAL));

        dev_warn(&dssdev->dev, "panel_notle_probe done\n");
        return 0;
}

static void __exit panel_notle_remove(struct omap_dss_device *dssdev) {
        struct notle_drv_data *drv_data = dev_get_drvdata(&dssdev->dev);

        dev_dbg(&dssdev->dev, "remove\n");

        kobject_del(&drv_data->kobj);
        kobject_put(&drv_data->kobj);
        kfree(drv_data);

        printk(KERN_WARNING LOG_TAG "Delete display FPGA reconfigure workueue\n");

        cancel_delayed_work((struct delayed_work*)&reconfigure_fpga_work);
        destroy_workqueue(reconfigure_fpga_wq);

        dev_set_drvdata(&dssdev->dev, NULL);
}

static int panel_notle_enable(struct omap_dss_device *dssdev) {
        int r = 0;

        mutex_lock(&panel_power_lock);

        r = panel_notle_power_on(dssdev);
        if (r)
                return r;

        dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

        mutex_unlock(&panel_power_lock);

        return 0;
}

#define L3_TPUT 800000 /* MiB/s */
static int panel_notle_resume(struct omap_dss_device *dssdev) {
        struct device *dss_dev;
        int r = -1;

        /*
         * This is a notle optimization.
         * Hold L3 constraint to OPP 100 (200Mhz) when display is on
         */
        if (!requested_l3_throughput) {
          dss_dev = &omap_hwmod_lookup("dss_core")->od->pdev->dev;//omap_hwmod_name_get_dev("dss_core");
                if (dss_dev) {
                        r = omap_pm_set_min_bus_tput(&dssdev->dev,
                                         OCP_INITIATOR_AGENT,
                                         L3_TPUT);

                        if (!r)
                                requested_l3_throughput = 1;

                }

                if (r)
                        printk(KERN_ERR LOG_TAG "Failed to set L3 bus speed\n");
        }

        mutex_lock(&panel_power_lock);

        r = panel_notle_power_on(dssdev);
        if (r)
                return r;

        dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

        mutex_unlock(&panel_power_lock);

        return 0;
}

static void panel_notle_disable(struct omap_dss_device *dssdev) {

        mutex_lock(&panel_power_lock);

        panel_notle_power_off(dssdev);

        dssdev->state = OMAP_DSS_DISPLAY_DISABLED;

        mutex_unlock(&panel_power_lock);
}

static int panel_notle_suspend(struct omap_dss_device *dssdev) {

        mutex_lock(&panel_power_lock);

        panel_notle_power_off(dssdev);
        dssdev->state = OMAP_DSS_DISPLAY_SUSPENDED;

        mutex_unlock(&panel_power_lock);

        /*
         * Release L3 constraint on display off.
         */
        if (requested_l3_throughput) {
                omap_pm_set_min_bus_tput(&dssdev->dev,
                        OCP_INITIATOR_AGENT, -1);
                requested_l3_throughput = 0;
        }


        return 0;
}

static void panel_notle_set_timings(struct omap_dss_device *dssdev,
                struct omap_video_timings *timings) {
        dpi_set_timings(dssdev, timings);
}

static void panel_notle_get_timings(struct omap_dss_device *dssdev,
                struct omap_video_timings *timings) {
        *timings = dssdev->panel.timings;
}

static int panel_notle_check_timings(struct omap_dss_device *dssdev,
                struct omap_video_timings *timings) {
        return dpi_check_timings(dssdev, timings);
}

static struct omap_dss_driver dpi_driver = {
        .probe                 = panel_notle_probe,
        .remove                = __exit_p(panel_notle_remove),

        .enable                = panel_notle_enable,
        .disable               = panel_notle_disable,
        .suspend               = panel_notle_suspend,
        .resume                = panel_notle_resume,

        .set_timings           = panel_notle_set_timings,
        .get_timings           = panel_notle_get_timings,
        .check_timings         = panel_notle_check_timings,

        .driver                = {
                .name                  = "panel_notle",
                .owner                 = THIS_MODULE,
        },
};

/* Functions to handle initialization of the i2c driver */
static int __devinit i2c_probe(struct i2c_client *client,
                               const struct i2c_device_id *id) {
        switch (id->driver_data) {
          case NOTLE_I2C_PANEL:
            /* panel-notle-panel */
            bus_data.panel_client = client;
            break;
          default:
            printk(KERN_WARNING LOG_TAG "Unrecognized i2c device\n");
            return -EINVAL;
        }

        return 0;
}

static int __devexit i2c_remove(struct i2c_client *client) {
        return 0;
}

/* These are the I2C devices we support */
static const struct i2c_device_id i2c_idtable[] = {
        {"panel-notle-panel", NOTLE_I2C_PANEL},
        {},
};

static struct i2c_driver i2c_driver = {
        .probe = i2c_probe,
        .remove = __exit_p(i2c_remove),
        .id_table = i2c_idtable,
        .driver = {
                   .name  = "panel-notle-i2c",
                   .owner = THIS_MODULE,
        },
};

/* SPI Interface for iCE40 FPGA based systems */
static const struct spi_device_id spi_idtable[] = {
        {"ice40-spi", 0},
        {},
};

static int ice40_spi_probe(struct spi_device *spi) {
        spi->mode = SPI_MODE_3;
        spi->bits_per_word = 8;
        spi_setup(spi);

        bus_data.ice40_device = spi;
        return 0;
}

static int ice40_spi_remove(struct spi_device *spi) {
        return 0;
}

static int ice40_spi_suspend(struct spi_device *spi, pm_message_t mesg) {
        return 0;
}

static int ice40_spi_resume(struct spi_device *spi) {
        spi->mode = SPI_MODE_3;
        spi->bits_per_word = 8;
        spi_setup(spi);

        bus_data.ice40_device = spi;
        return 0;
}

static struct spi_driver spi_driver = {
        .id_table             = spi_idtable,
        .probe                = ice40_spi_probe,
        .remove               = __devexit_p(ice40_spi_remove),
        .suspend              = ice40_spi_suspend,
        .resume               = ice40_spi_resume,
        .driver               = {
                .name             = "ice40-spi",
                .bus              = &spi_bus_type,
                .owner            = THIS_MODULE,
        },
};

static int __init panel_notle_drv_init(void) {
        int r = 0;
        r = i2c_add_driver(&i2c_driver);
        if (r < 0) {
                printk(KERN_WARNING LOG_TAG "I2C driver registration failed\n");
                goto err0;
        }

        r = spi_register_driver(&spi_driver);
        if (r < 0) {
                printk(KERN_WARNING LOG_TAG "SPI driver registration failed\n");
                goto err1;
        }

        r = omap_dss_register_driver(&dpi_driver);
        if (r < 0) {
                printk(KERN_WARNING LOG_TAG "DSS driver registration failed\n");
                goto err2;
        }

        return 0;

err2:
        spi_unregister_driver(&spi_driver);
err1:
        i2c_del_driver(&i2c_driver);
err0:
        return r;
}

static void __exit panel_notle_drv_exit(void) {
        omap_dss_unregister_driver(&dpi_driver);
        i2c_del_driver(&i2c_driver);
        spi_unregister_driver(&spi_driver);
}

module_init(panel_notle_drv_init);
module_exit(panel_notle_drv_exit);

MODULE_DESCRIPTION("Notle Panel Driver");
MODULE_LICENSE("GPL");
