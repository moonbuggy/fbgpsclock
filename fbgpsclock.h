#ifndef FBGPSCLOCK_H_   /* Include guard */
#define FBGPSCLOCK_H_

// fonts
//
#include "fonts/BigShoulders_light_96_time.h"
// #include "fonts/Droid_regular_96_time.h"
#include "fonts/Lato_regular_24_variable.h"
#include "fonts/Lato_regular_32_variable.h"
// #include "fonts/Lato_regular_60_time.h"
// #include "fonts/Lato_regular_64_time.h"
// #include "fonts/Mostane_regular_96_time.h"
// #include "fonts/Montserrat_regular_32_variable.h"
// #include "fonts/RobotoMono_light_24_14_wide.h"
// #include "fonts/RobotoMono_light_24_18_wide.h"
// #include "fonts/RobotoMono_light_32_19_wide.h"
// #include "fonts/RobotoMono_light_32_20_wide.h"
// #include "fonts/RobotoMono_light_48.h"
// #include "fonts/RobotoMono_light_64.h"
// #include "fonts/RobotoMono_light_64_narrow_punctuation.h"
// #include "fonts/TallFilms_regular_96_time.h"

#define CLOCK_FONT(x) BigShoulders_light_96_time_##x
// #define CLOCK_FONT(x) Droid_regular_96_time_##x
// #define CLOCK_FONT(x) Lato_regular_60_time_##x
// #define CLOCK_FONT(x) Lato_regular_64_time_##x
// #define CLOCK_FONT(x) RobotoMono_light_64_##x
// #define CLOCK_FONT(x) Mostane_regular_96_time_##x
// #define CLOCK_FONT(x) TallFilms_regular_96_time_##x

#define DATE_FONT(x) Lato_regular_32_variable_##x
#define ZONE_FONT(x) Lato_regular_24_variable_##x

#define SAT_FONT(x) Lato_regular_32_variable_##x
// #define SAT_FONT(x) Lato_regular_24_variable_##x
// #define SAT_FONT(x) RobotoMono_light_24_##x

#include "fonts/shapes_24.h"
#define SHAPES_FONT(x) shapes_24_##x

// #define TEST_FONT(x) Montserrat_regular_32_variable_##x
#define TEST_FONT(x) Lato_regular_24_variable_##x

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <math.h>
#include <gps.h>
#include <sys/timepps.h>
#include "include/ini.h"
#include "include/log.h"

#define INI_FILE "fbgpsclock.ini"

typedef struct {
  const char** ch_addr;
  unsigned short ch_height;
  const char* ch_widths;
} BitmapFont;

typedef struct {
  char *format;
  BitmapFont font;
  unsigned short x;
  unsigned short y;
  unsigned short colour;
  unsigned short bg_colour;
  char alignment[2];
  short padding;
  bool static_size;
  unsigned short start_x;
  unsigned short end_x;
} DisplayElement;

typedef struct {
  char *fbdev;
  int file;
  struct fb_fix_screeninfo finfo;
  struct fb_var_screeninfo vinfo;
  unsigned char *max_offset;
  void *video;
  unsigned short bytes_per_pixel;
  unsigned short bg_colour;
} FBDisplay;

typedef struct {
  bool pps_enable;
  char pps_device[12];
  bool pps_flash;
  unsigned short pps_flash_length;
  char gps_host[32];
  char gps_port[5];
  unsigned short gps_nochange_limit;
  unsigned int gps_poll;
  char fb_device[12];
  unsigned short bg_colour;
  short x_offset;
  bool bl_enable;
  char bl_device[32];
  unsigned short bl_pin;
  unsigned short bl_duty;
  unsigned int bl_freq;
  bool bl_invert;
  unsigned short max_text_length;
  DisplayElement time;
  DisplayElement zone;
  DisplayElement date;
  DisplayElement sat;
  DisplayElement status;
  unsigned short error_colour;
  unsigned short no_fix_colour;
  unsigned short partial_fix_colour;
  unsigned short full_fix_colour;
  unsigned short status_symbol;
  DisplayElement test;
  bool always_redraw;
  unsigned short log_level;
} config_params;

BitmapFont timeFont =
  {(CLOCK_FONT(char_addr)), CLOCK_FONT(height_px), CLOCK_FONT(char_width)};

BitmapFont dateFont =
  {(DATE_FONT(char_addr)), DATE_FONT(height_px), DATE_FONT(char_width)};

BitmapFont zoneFont =
  {(ZONE_FONT(char_addr)), ZONE_FONT(height_px), ZONE_FONT(char_width)};

BitmapFont satFont =
  {(SAT_FONT(char_addr)), SAT_FONT(height_px), SAT_FONT(char_width)};

BitmapFont statusFont =
  {(SHAPES_FONT(char_addr)), SHAPES_FONT(height_px), SHAPES_FONT(char_width)};

BitmapFont testFont =
  {(TEST_FONT(char_addr)), TEST_FONT(height_px), TEST_FONT(char_width)};

static struct timespec offset_assert = {0, 0};

struct pps_thread_args {
  pps_handle_t handle;
  int avail_mode;
  FBDisplay *disp;
};

static pthread_mutex_t __mutex_keep_running =
              (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t __mutex_config =
              (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t __mutex_status_colour =
              (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t __mutex_log =
              (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

static config_params global_config;

static void set_running(unsigned short value);
static unsigned short get_running();
static void sigHandler(int sig, siginfo_t *info, void *extra);
static void assign_sig_handler();
static void set_config(config_params value);
static config_params get_config();
static void set_status_colour(unsigned short value);
static unsigned short get_status_colour();
static void log_lock(bool lock, void *udata);
int get_framebuffer_info(FBDisplay *disp);
int map_framebuffer(FBDisplay *disp);
bool open_pps(struct pps_thread_args *pps_args, config_params config);
void clear_lines(FBDisplay *disp, int start_line, unsigned short height_px);
void clear_block(FBDisplay *disp, unsigned short start_x, unsigned short end_x,
                  unsigned short start_line, unsigned short height_px);
void clear_element(FBDisplay *disp, DisplayElement *el);
unsigned short draw_padding(FBDisplay *disp, unsigned short x, unsigned short y,
                              unsigned short height, short width,
                              unsigned short colour);
static int div_ceil(int a, int b);
int draw_char(FBDisplay *disp, short x, short y, short ch,
              const char* char_widths, short char_height,
              const char** char_addr, unsigned short colour,
              unsigned short bg_colour);
int draw_string(FBDisplay *disp, DisplayElement *format, char *text);
unsigned short get_text_pixel_width(DisplayElement *el, unsigned short offset,
                                      char *text);
void draw_element(FBDisplay *disp, DisplayElement *element, char *text);
void draw_error(FBDisplay *disp, DisplayElement element, char *text);
void run_display(FBDisplay *disp, struct gps_data_t *gps_data);
void *run_backlight_pwm(void *_args);
void *run_pps_monitor(void *_args);
static int iniHandler(void* user, const char* section, const char* name,
                      const char* value);
int main();

#endif
