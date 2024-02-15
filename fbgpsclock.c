#include "fbgpsclock.h"

static unsigned short keep_running = 1;
void set_running(unsigned short value) {
  pthread_mutex_lock(&__mutex_keep_running);
    keep_running = value;
  pthread_mutex_unlock(&__mutex_keep_running);
}

unsigned short get_running() {
  unsigned short value;
  pthread_mutex_lock(&__mutex_keep_running);
    value = keep_running;
  pthread_mutex_unlock(&__mutex_keep_running);
  return value;
}

static void sigHandler(int sig, siginfo_t *info, void *extra) {
  log_debug("Caught signal: %d", sig);
  set_running(0);
}

static void assign_sig_handler() {
  struct sigaction sa;

  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = sigHandler;
  sa.sa_flags = SA_SIGINFO;
  sigfillset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static void set_config(config_params value) {
  pthread_mutex_lock(&__mutex_config);
    global_config = value;
  pthread_mutex_unlock(&__mutex_config);
}

static config_params get_config() {
  config_params value;
  pthread_mutex_lock(&__mutex_config);
    value = global_config;
  pthread_mutex_unlock(&__mutex_config);
  return value;
}

static unsigned short status_colour = 0x00;
void set_status_colour(unsigned short value) {
  pthread_mutex_lock(&__mutex_status_colour);
    status_colour = value;
  pthread_mutex_unlock(&__mutex_status_colour);
}

unsigned short get_status_colour() {
  unsigned short value;
  pthread_mutex_lock(&__mutex_status_colour);
    value = status_colour;
  pthread_mutex_unlock(&__mutex_status_colour);
  return value;
}

static void log_lock(bool lock, void *udata) {
  if (lock)
    pthread_mutex_lock(udata);
  else
    pthread_mutex_unlock(udata);
}

int get_framebuffer_info(FBDisplay *disp) {
  if (ioctl(disp->file, FBIOGET_FSCREENINFO, &disp->finfo) == -1) {
    log_debug("FBIOGET_FSCREENINFO failed.");
    return -1;
  }
  if (ioctl(disp->file, FBIOGET_VSCREENINFO, &disp->vinfo) == -1) {
    log_debug("FBIOGET_VSCREENINFO failed.\n");
    return -1;
  }
  return 0;
}

int map_framebuffer(FBDisplay *disp) {
  disp->file = open(disp->fbdev, O_RDWR | O_NONBLOCK);

  if (disp->file == -1)
    log_fatal("failed to open frame buffer at %s", disp->fbdev);
  else if (get_framebuffer_info(disp) == 0) {
    disp->video = mmap(NULL, disp->finfo.smem_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED, disp->file, 0);

    if (disp->video != MAP_FAILED) return 0;

    close(disp->file);
    log_fatal("mmap disp->video failed");
  }
  return 1;
}

// initialize pps_args and return success/failure for the open
bool open_pps(struct pps_thread_args *pps_args, config_params config) {
  if (config.pps_flash) {
    int fd = open(config.pps_device, O_RDWR);
    if (fd < 0)
      log_error("Can't open PPS at %s", config.pps_device);
    else {
      if (time_pps_create(fd, &pps_args->handle) < 0)
        log_error("Can't create PPS source from %s", config.pps_device);
      else {
        log_debug("Found PPS source at %s", config.pps_device);

        pps_params_t pps_params;

        if (time_pps_getcap(pps_args->handle, &pps_args->avail_mode) < 0)
          log_error("Cannot get PPS capabilities");
        else if ((pps_args->avail_mode & PPS_CAPTUREASSERT) == 0)
          log_error("PPS cannot CAPTUREASSERT");
        else if (time_pps_getparams(pps_args->handle, &pps_params) < 0)
          log_error("Cannot get PPS parameters");
        else {
          pps_params.mode |= PPS_CAPTUREASSERT;
          if ((pps_args->avail_mode & PPS_OFFSETASSERT) != 0) {
            pps_params.mode |= PPS_OFFSETASSERT;
            pps_params.assert_offset = offset_assert;
          }
          if(time_pps_setparams(pps_args->handle, &pps_params) < 0)
            log_error("Cannot set parameters");
          else
            return 0;
        }
      }
      close(fd);
    }
  }
  return 1;
}

// clear entire lines on the display
void clear_lines(FBDisplay *disp, int start_line, unsigned short height_px) {
  unsigned char *offset =
    (unsigned char *)disp->video + start_line * disp->finfo.line_length;

  // don't write beyond the size of the mmap
  if (start_line + height_px >= disp->vinfo.yres)
    height_px = disp->vinfo.yres - start_line - 1;

  memset(offset, disp->bg_colour, height_px * disp->finfo.line_length);
}

// clear a rectangular area on the display
void clear_block(FBDisplay *disp, unsigned short start_x, unsigned short end_x,
                  unsigned short start_line, unsigned short height_px) {

  // make sure we're going in the right direction
  unsigned short x = (end_x < start_x) ? end_x : start_x;

  unsigned char *offset =
    (unsigned char *)disp->video + start_line * disp->finfo.line_length
      + x * disp->bytes_per_pixel;

  unsigned short clear_bytes = abs(end_x - start_x) * disp->bytes_per_pixel;

  for (unsigned short i = height_px; i > 0; i--) {
    memset(offset, disp->bg_colour, clear_bytes);
    offset += disp->finfo.line_length;

    // don't write beyond the size of the mmap
    if (offset >= disp->max_offset)
      break;
  }
}

// clear an individual text element's area on the screen, using the 'end_x' value
// from the previous draw to determine the widths of the the block to clear
void clear_element(FBDisplay *disp, DisplayElement *el) {
  // end_x won't be initialized on the first run when drawing an element,
  // but it also means there's nothing to clear
  if (el->end_x) {
    unsigned short x = el->x;

    // reverse the indent for right-to-left alignment
    if(el->alignment[0] == 'r')
      x = disp->vinfo.xres - el->x;

    clear_block(disp, x, el->end_x, el->y, el->font.ch_height);
  }
}

// this really shoud be made part of draw_char(), but i keep confusing myself
// with the layers of recursion, so it's a seprate function for the time being :)
unsigned short draw_padding(FBDisplay *disp, unsigned short x,
                                    unsigned short y, unsigned short height,
                                    short width, unsigned short colour) {

  if (width > 0) {
    unsigned char *offset;
    offset = (unsigned char *)disp->video + y * disp->finfo.line_length
                + x * disp->bytes_per_pixel;

    for (int row = 0; row < height; row++) {
      for (int row_bit = 0; row_bit < width; row_bit++) {
        memset(offset, colour, disp->bytes_per_pixel);
        offset += disp->bytes_per_pixel;
      }
      offset += disp->finfo.line_length - width * disp->bytes_per_pixel;

      // don't write beyond the size of the mmap
      if (offset >= disp->max_offset)
        break;
    }
  }
  return width;
}

static int div_ceil(int a, int b)  { return a/b + (a%b!=0 && (a^b)>0); }

int draw_char(FBDisplay *disp, short x, short y, short ch, const char* ch_widths,
              short ch_height, const char** ch_addr, unsigned short colour,
              unsigned short bg_colour) {

  unsigned char *offset;
  unsigned short ch_width = ch_widths[ch];
  short bytes_per_row = div_ceil(ch_width, 8);

  offset = (unsigned char *)disp->video + y * disp->finfo.line_length
              + x * disp->bytes_per_pixel;

  short byte_num = 0;

  // iterate through the bitmap rows, top to bottom
  for (int row = 0; row < ch_height; row++) {
    int this_bit = 0;

    // iterate across the row, depending on how many bytes wide the row is
    for (int row_byte = 0; row_byte < bytes_per_row; row_byte++) {
      int this_byte = *(ch_addr[ch]+byte_num++);

      for (int k = 7; k >= 0 && this_bit < ch_width; k--, this_bit++) {
        int mask = (1 << k);
        if (this_byte & mask)
          memset(offset, colour, disp->bytes_per_pixel);
        else
          memset(offset, bg_colour, disp->bytes_per_pixel);
        offset += disp->bytes_per_pixel;
      }
    }
    offset += disp->finfo.line_length - ch_width * disp->bytes_per_pixel;

    // don't write beyond the size of the mmap
    if (offset >= disp->max_offset)
      break;
  }
  return ch_width;
}

// draw_string()
// draw a string, left to right, return the x position the string ends at
int draw_string(FBDisplay *disp, DisplayElement *el, char *text) {
  short text_i = 0;
  short x = el->start_x;

  while (text_i < strlen(text) && x >= 0) {
    unsigned short ch = text[text_i];
    x += draw_padding(disp, x, el->y, el->font.ch_height, el->padding,
                            el->bg_colour);

    x += draw_char(disp, x, el->y, ch, el->font.ch_widths, el->font.ch_height,
                            el->font.ch_addr, el->colour, el->bg_colour);

    x += draw_padding(disp, x, el->y, el->font.ch_height, el->padding,
                            el->bg_colour);
    text_i++;
  }
  return x;
}

unsigned short get_text_pixel_width(DisplayElement *el, unsigned short offset,
                                      char *text) {
  for (int i = 0; i < strlen(text); i++)
    offset += (el->padding * 2) + el->font.ch_widths[(unsigned short) text[i]];
  return offset;
}

// draw a display element
// determine the co-ordinates, factoring in alignment, either by calculation or
// using previous values for a static size element
// set start_x and end_x values in the DisplayElement
void draw_element(FBDisplay *disp, DisplayElement *el, char *text) {

  // clear the area first
  if (el->static_size == false)
    clear_element(disp, el);

  switch(el->alignment[0]) {
    // centre aligned
    case 'c':
      if (el->static_size == false || el->end_x == 0)
        el->start_x =
          (disp->vinfo.xres - get_text_pixel_width(el, 0, text)) / 2;

      el->end_x = draw_string(disp, el, text);
      break;

    // right aligned
    case 'r':
      if (el->static_size == false || el->end_x == 0)
        el->start_x =
          (disp->vinfo.xres - get_text_pixel_width(el, el->x, text));

      draw_string(disp, el, text);
      el->end_x = el->start_x;
      break;

    // assume left aligned for everything else
    default:
      el->start_x = el->x;
      el->end_x = draw_string(disp, el, text);
  }
}

// changes the colour of an existing DisplayElement to red on white, so we can easily
// dump an error message in place of the value that's errored and know it will be
// visible regardless of existing background colours
void draw_error(FBDisplay *disp, DisplayElement el, char *text) {
  el.colour = 0xE0;
  el.bg_colour = 0xFF;
  clear_lines(disp, el.y, el.font.ch_height);
  draw_element(disp, &el, text);
}

// the main display
// this currently draws all elements except the PPS-triggered status flash
void run_display(FBDisplay *disp, struct gps_data_t *gps_data) {
  config_params params = get_config();
  char text[params.max_text_length];

  long int gps_last_nsec = 0;
  unsigned short error_count = 0;
  bool error_triggered = false;

  char time_string[128];
  struct tm local_time = { .tm_hour = 0 };

  const unsigned short error_limit =
    params.gps_nochange_limit * 1000 / params.gps_poll;

  struct timespec polling_interval = {
    .tv_sec = params.gps_poll / 1000,
    .tv_nsec = (params.gps_poll % 1000) * 1000000
  };

  clear_lines(disp, 0, disp->vinfo.yres);
  log_info("Started display.");

  while (get_running() == 1) {
    // get the time
    time_t now = time(NULL);

    int last_tm_sec = local_time.tm_sec;
    int last_tm_hour = local_time.tm_hour;

    if (now != (time_t)-1) {
      // draw error on failure
      if (!localtime_r(&now, &local_time)) {
        snprintf(text, params.max_text_length, "TIME READ ERROR");
        draw_error(disp, params.date, text);
      } else {
        // draw the time
        if (local_time.tm_sec != last_tm_sec || params.always_redraw) {
          strftime(text, sizeof(time_string), params.time.format, &local_time);
          draw_element(disp, &params.time, text);
        }

        // we don't need to redraw date and zone every refresh, once a day would
        // be enough, but lets go with once an hour because it's not a lot of work
        if (local_time.tm_hour != last_tm_hour || params.always_redraw) {
          // draw the date
          strftime(text, sizeof(time_string), params.date.format, &local_time);
          draw_element(disp, &params.date, text);

          // draw the zone
          strftime(text, sizeof(time_string), params.zone.format, &local_time);
          draw_element(disp, &params.zone, text);
        }
      }
    }

    unsigned short last_visible = gps_data->satellites_visible;
    unsigned short last_used = gps_data->satellites_used;

    // get GPS data, draw error on failure
    if (-1 == gps_read(gps_data, NULL, 0)) {
      snprintf(text, params.max_text_length, "GPS READ ERROR");
      draw_error(disp, params.sat, text);
    } else if (gps_data->satellites_visible != last_visible
                || gps_data->satellites_used != last_used
                || params.always_redraw) {
      // draw the satellite count
      snprintf(text, params.max_text_length, params.sat.format,
                gps_data->satellites_used, gps_data->satellites_visible);
      draw_element(disp, &params.sat, text);
    }

    // If the nanoseconds counter hasn't changed since last poll then we're not
    // communicating with the GPS device properly and the gps_data->fix.mode
    // value won't be reliable.
    if (gps_data->online.tv_nsec == 0
        || gps_data->online.tv_nsec == gps_last_nsec) {
      // error_triggered prevents overflowing error_count
      if (!error_triggered) error_count += 1;
      if (error_count >= error_limit) error_triggered = true;
    } else {
      error_triggered = false;
      error_count = 0;
    }
    gps_last_nsec = gps_data->online.tv_nsec;

    unsigned short last_status = params.status.colour;
    if (error_triggered)
      params.status.colour = params.error_colour;
    else switch(gps_data->fix.mode) {
      case MODE_3D:
        params.status.colour = params.full_fix_colour;
        break;
      case MODE_2D:
        params.status.colour = params.partial_fix_colour;
        break;
      case MODE_NO_FIX:
        params.status.colour = params.no_fix_colour;
        break;
      default:
        params.status.colour = params.status.bg_colour;
    }

    // only update if the status has changed, leaving the PPS thread free to
    // flash the status symbol
    if (params.status.colour != last_status) {
      set_status_colour(params.status.colour);
      snprintf(text, params.max_text_length, "%c", params.status_symbol);
      draw_element(disp, &params.status, text);
    }

    // random testing stuff..

    // gps_last_nsec changes quite frequently and will be variable length, so
    // it's useful to diagnose issues with placement of text and clearing of
    // the screen under it
    // snprintf(text, params.max_text_length, "nsec: %ld", gps_last_nsec);

    // draw_element(disp, &params.test, text);

    nanosleep(&polling_interval, &polling_interval);
  }

  clear_lines(disp, 0, disp->vinfo.yres);
  // we don't seem to wait for clear_lines(), but sleeping briefly means the
  // screen gets cleared
  sleep(1);

  munmap(disp->video, disp->finfo.smem_len);
  close(disp->file);
  log_debug("run_display(): exiting");
}

void *run_backlight_pwm(void *_args) {
  config_params params = get_config();

  bool high = true, low = false;
  if(params.bl_invert) {
    high = false;
    low = true;
  }

  const unsigned int t_on =
    (10000000 / params.bl_freq * params.bl_duty);
  const unsigned int t_off =
    (10000000 / params.bl_freq) * (100 - params.bl_duty);

  struct timespec time_on = {
    .tv_sec = t_on / 1000000000,
    .tv_nsec = t_on
  };

  struct timespec time_off = {
    .tv_sec = t_off / 1000000000,
    .tv_nsec = t_off
  };

  struct gpiohandle_request req;
  struct gpiohandle_data data;

  const char *dev_name = params.bl_device;

  int fd = open(dev_name, O_RDONLY);
  if (fd < 0) pthread_exit(NULL);

  req.lineoffsets[0] = params.bl_pin;
  req.flags = GPIOHANDLE_REQUEST_OUTPUT;
  req.lines = 1;

  int ret = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req);
  close(fd);

  if (ret == -1) pthread_exit(NULL);

  log_info("Started backlight PWM: pin %d, %d%% at %udHz (on: %lduS, off: %lduS)",
              params.bl_pin, params.bl_duty, params.bl_freq,
              time_on.tv_nsec / 1000, time_off.tv_nsec / 1000);

  while (get_running() == 1) {
    data.values[0] = low;
    ioctl(req.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
    nanosleep(&time_off, &time_off);

    data.values[0] = high;
    ioctl(req.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
    nanosleep(&time_on, &time_on);
  }

  // turn off the screen when we're done
  data.values[0] = low;
  ioctl(req.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
  sleep(1);

  close(req.fd);

  log_debug("run_backlight_pwm(): exiting");
  pthread_exit(EXIT_SUCCESS);
}

void *run_pps_monitor(void *_args) {
  config_params params = get_config();

  // get the status symbol character
  char text[params.max_text_length];
  snprintf(text, params.max_text_length, "%c", params.status_symbol);

  struct pps_thread_args *args = (struct pps_thread_args *) _args;

  // PPS_CANWAIT means we can use this as a blocking function, and time actions
  // based on recieving an PPS pulse, rather than polling for the PPS
  //
  // if we can't use the ioctl call to PPS-FETCH as a blocking function, just
  // poll the PPS source once per second instead
  unsigned int flash_usec = params.pps_flash_length * 1000;
  unsigned int sleep_usec = 0;
  if (!(args->avail_mode & PPS_CANWAIT))
    sleep_usec = 1000000 - flash_usec;
  else
    log_debug("PPS_CANWAIT");

  struct pps_fdata fetch_data;
  fetch_data.timeout.flags = PPS_TIME_INVALID;    // wait forever for pulse

  log_info("Started PPS monitor on %s.", params.pps_device);

  while (get_running() == 1) {
    if (ioctl(args->handle, PPS_FETCH, &fetch_data) < 0)
      pthread_exit((void *)EXIT_FAILURE);

    // flash the status symbol
    params.status.colour = get_status_colour();
    draw_element(args->disp, &params.status, text);
    usleep(flash_usec);

    params.status.colour = args->disp->bg_colour;
    draw_element(args->disp, &params.status, text);
    usleep(sleep_usec);
  }
  log_debug("run_pps_monitor(): exiting");
  pthread_exit(EXIT_SUCCESS);
}

static int iniHandler(void* user, const char* section, const char* name,
                      const char* value) {
  config_params* pconfig = (config_params*)user;
  #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
  if (MATCH("general", "log_level"))
    pconfig->log_level = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("pps", "device"))
    snprintf(pconfig->pps_device, sizeof(pconfig->pps_device), value);
  else if (MATCH("pps", "enable"))
    pconfig->pps_enable = (bool) strtoul(value, NULL, 0);
  else if (MATCH("pps", "flash_status"))
    pconfig->pps_flash = (bool) strtoul(value, NULL, 0);
  else if (MATCH("pps", "flash_length"))
    pconfig->pps_flash_length = (short) strtoul(value, NULL, 0);
  else if (MATCH("gps", "host"))
    snprintf(pconfig->gps_host, sizeof(pconfig->gps_host), value);
  else if (MATCH("gps", "port"))
    snprintf(pconfig->gps_port, sizeof(pconfig->gps_port), value);
  else if (MATCH("gps", "nochange_limit"))
    pconfig->gps_nochange_limit = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("gps", "poll_interval"))
    pconfig->gps_poll = (unsigned int) strtoul(value, NULL, 0);
  else if (MATCH("display", "device"))
    snprintf(pconfig->fb_device, sizeof(pconfig->fb_device), value);
  else if (MATCH("display", "bg_colour"))
    pconfig->bg_colour = (short) strtoul(value, NULL, 0);
  else if (MATCH("display", "always_redraw"))
    pconfig->always_redraw = (bool) strtoul(value, NULL, 0);
  else if (MATCH("display", "backlight_enable"))
    pconfig->bl_enable = (bool) strtoul(value, NULL, 0);
  else if (MATCH("display", "backlight_pin"))
    pconfig->bl_pin = (short) strtoul(value, NULL, 0);
  else if (MATCH("display", "backlight_duty"))
    pconfig->bl_duty = (short) strtoul(value, NULL, 0);
  else if (MATCH("display", "backlight_freq"))
    pconfig->bl_freq = (int) strtoul(value, NULL, 0);
  else if (MATCH("display", "backlight_invert"))
    pconfig->bl_invert = (bool) strtoul(value, NULL, 0);
  else if (MATCH("display", "backlight_device"))
    snprintf(pconfig->bl_device, sizeof(pconfig->bl_device), value);
  else if (MATCH("display", "max_text_length"))
    pconfig->max_text_length = (short) strtoul(value, NULL, 0);
  else if (MATCH("display", "x_offset"))
    pconfig->x_offset = (short) strtoul(value, NULL, 0);
  else if (MATCH("time", "format"))
    pconfig->time.format = strdup(value);
  else if (MATCH("time", "alignment"))
    snprintf(pconfig->time.alignment, sizeof(pconfig->time.alignment), value);
  else if (MATCH("time", "colour"))
    pconfig->time.colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("time", "bg_colour"))
    pconfig->time.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("time", "x_indent"))
    pconfig->time.x = (short) strtoul(value, NULL, 0);
  else if (MATCH("time", "y_pos"))
    pconfig->time.y = (short) strtoul(value, NULL, 0);
  else if (MATCH("time", "padding"))
    pconfig->time.padding = (short) strtoul(value, NULL, 0);
  else if (MATCH("time", "static"))
    pconfig->time.static_size = (bool) strtoul(value, NULL, 0);
  else if (MATCH("date", "format"))
    pconfig->date.format = strdup(value);
  else if (MATCH("date", "alignment"))
    snprintf(pconfig->date.alignment, sizeof(pconfig->date.alignment), value);
  else if (MATCH("date", "colour"))
    pconfig->date.colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("date", "bg_colour"))
    pconfig->date.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("date", "x_indent"))
    pconfig->date.x = (short) strtoul(value, NULL, 0);
  else if (MATCH("date", "y_pos"))
    pconfig->date.y = (short) strtoul(value, NULL, 0);
  else if (MATCH("date", "padding"))
    pconfig->date.padding = (short) strtoul(value, NULL, 0);
  else if (MATCH("date", "static"))
    pconfig->date.static_size = (bool) strtoul(value, NULL, 0);
  else if (MATCH("zone", "format"))
    pconfig->zone.format = strdup(value);
  else if (MATCH("zone", "alignment"))
    snprintf(pconfig->zone.alignment, sizeof(pconfig->zone.alignment), value);
  else if (MATCH("zone", "colour"))
    pconfig->zone.colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("zone", "bg_colour"))
    pconfig->zone.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("zone", "bg_colour"))
    pconfig->zone.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("zone", "x_indent"))
    pconfig->zone.x = (short) strtoul(value, NULL, 0);
  else if (MATCH("zone", "y_pos"))
    pconfig->zone.y = (short) strtoul(value, NULL, 0);
  else if (MATCH("zone", "padding"))
    pconfig->zone.padding = (short) strtoul(value, NULL, 0);
  else if (MATCH("zone", "static"))
    pconfig->zone.static_size = (bool) strtoul(value, NULL, 0);
  else if (MATCH("satellites", "format"))
    pconfig->sat.format = strdup(value);
  else if (MATCH("satellites", "alignment"))
    snprintf(pconfig->sat.alignment, sizeof(pconfig->sat.alignment), value);
  else if (MATCH("satellites", "colour"))
    pconfig->sat.colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("satellites", "bg_colour"))
    pconfig->sat.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("satellites", "x_indent"))
    pconfig->sat.x = (short) strtoul(value, NULL, 0);
  else if (MATCH("satellites", "y_pos"))
    pconfig->sat.y = (short) strtoul(value, NULL, 0);
  else if (MATCH("satellites", "padding"))
    pconfig->sat.padding = (short) strtoul(value, NULL, 0);
  else if (MATCH("satellites", "static"))
    pconfig->sat.static_size = (bool) strtoul(value, NULL, 0);
  else if (MATCH("test", "alignment"))
    snprintf(pconfig->test.alignment, sizeof(pconfig->test.alignment), value);
  else if (MATCH("test", "colour"))
    pconfig->test.colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("test", "bg_colour"))
    pconfig->test.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("test", "x_indent"))
    pconfig->test.x = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("test", "y_pos"))
    pconfig->test.y = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("test", "padding"))
    pconfig->test.padding = (short) strtoul(value, NULL, 0);
  else if (MATCH("test", "static"))
    pconfig->test.static_size = (bool) strtoul(value, NULL, 0);
  else if (MATCH("status", "alignment"))
    snprintf(pconfig->status.alignment, sizeof(pconfig->status.alignment), value);
  else if (MATCH("status", "bg_colour"))
    pconfig->status.bg_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "error_colour"))
    pconfig->error_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "no_fix_colour"))
    pconfig->no_fix_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "partial_fix_colour"))
    pconfig->partial_fix_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "full_fix_colour"))
    pconfig->full_fix_colour = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "x_indent"))
    pconfig->status.x = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "y_pos"))
    pconfig->status.y = (unsigned short) strtoul(value, NULL, 0);
  else if (MATCH("status", "padding"))
    pconfig->status.padding = (short) strtoul(value, NULL, 0);
  else if (MATCH("status", "static"))
    pconfig->status.static_size = (bool) strtoul(value, NULL, 0);
  else if (MATCH("status", "symbol"))
    pconfig->status_symbol = (unsigned short) strtoul(value, NULL, 0);
  else
    return 0;
  return 1;
}

int main() {
  FBDisplay *disp = (FBDisplay*) malloc(sizeof(FBDisplay));

  config_params config = {
    .pps_enable = true,
    .pps_device = "/dev/pps0",
    .pps_flash = true,
    .pps_flash_length = 300,
    .gps_host = "GPSD_SHARED_MEMORY",
    .gps_port = "2947",
    .gps_nochange_limit = 5,
    .gps_poll = 1000,
    .fb_device = "/dev/fb0",
    .bg_colour = 0x00,
    .bl_enable = true,
    .bl_pin = 18,
    .bl_duty = 50,
    .bl_invert = false,
    .bl_freq = 19600,
    .bl_device = "/dev/gpiochip0",
    .x_offset = 0,
    .time.format = "%H:%M.%S",
    .time.font = timeFont,
    .time.alignment = "c",
    .time.colour = 0xFF,
    .time.x = 0,
    .time.y = 45,
    .time.static_size = false,
    .time.padding = 0,
    .date.format = "%a, %d %b %Y",
    .date.font = dateFont,
    .date.alignment = "c",
    .date.colour = 0x3C,
    .date.x = 0,
    .date.y = 5,
    .date.padding = 0,
    .date.static_size = false,
    .zone.format = "%Y",
    .zone.font = zoneFont,
    .zone.alignment = "c",
    .zone.colour = 0xFF,
    .zone.x = 0,
    .zone.y = 145,
    .zone.padding = 0,
    .zone.static_size = false,
    .sat.format = "Satellites: %2d/%2d",
    .sat.font = satFont,
    .sat.alignment = "l",
    .sat.colour = 0xFF,
    .sat.x = 10,
    .sat.y = 200,
    .sat.padding = 0,
    .sat.static_size = false,
    .test.font = testFont,
    .test.alignment = "c",
    .test.colour = 0xFF,
    .test.x = 0,
    .test.y = 145,
    .test.padding = 0,
    .test.static_size = false,
    .status.font = statusFont,
    .status.alignment = "r",
    .status.colour = 0x00,
    .status.x = 10,
    .status.y = 200,
    .status.padding = 0,
    .status.static_size = true,
    .error_colour = 0xE0,
    .no_fix_colour = 0x00,
    .partial_fix_colour = 0xE3,
    .full_fix_colour = 0x07,
    .status_symbol = 1,
    .always_redraw = false,
    .log_level = LOG_INFO
  };

  // set log level to default and configure locking
  log_set_level(config.log_level);
  log_set_lock(log_lock, &__mutex_log);

  const char ini_paths[3][16] = {"./", "/usr/local/etc/", "/etc/"};

  char ini_file_path[128];
  FILE* file;

  for (short i = 0; i < 3; i++) {
    if (!snprintf(ini_file_path, sizeof(ini_file_path), "%s%s", ini_paths[i], INI_FILE)) {
      log_warn("Error processing path: %s%s", ini_paths[i], INI_FILE);
      break;
    }
    log_debug("Checking for config in %s", ini_paths[i]);
    file = fopen(ini_file_path, "r");
    if (file) break;
  }

  if (file) {
    fclose(file);
    if(ini_parse(ini_file_path, iniHandler, &config) < 0) {
        log_error("Can't parse config at '%s'.", ini_file_path);
        return 1;
    }
    log_info("Loaded config from %s", ini_file_path);
    log_set_level(config.log_level);
    log_info("Settings log level to %s", log_level_string(config.log_level));
  } else
    log_warn("No config (%s) found.", INI_FILE);

  // apply x offset to all values that require it
  unsigned short *x_vars[] = { &config.time.x, &config.date.x, &config.zone.x,
                          &config.sat.x, &config.status.x, &config.test.x };
  for (unsigned short i = 0; i < *(&x_vars + 1) - x_vars; i++)
    *(x_vars[i]) = *(x_vars[i]) + config.x_offset;

  // set bg_colour of elements to background, if not already set by user config
  unsigned short *c_vars[] = {&config.time.bg_colour, &config.date.bg_colour,
                              &config.zone.bg_colour, &config.sat.bg_colour,
                              &config.status.bg_colour, &config.test.bg_colour};
  for (unsigned short i = 0; i < *(&c_vars + 1) - c_vars; i++)
    if(!(*(c_vars[i])))
      *(c_vars[i]) = config.bg_colour;

  set_config(config);
  set_status_colour(config.no_fix_colour);

  assign_sig_handler();

  switch(fork()) {
    case -1:
      log_fatal("Could not fork.");
      return 1;
    case 0: break;
    default: _exit(EXIT_SUCCESS);
  }

  if (setsid() < 0) {
    log_fatal("Could not set session ID");
    _exit(EXIT_FAILURE);
  }

  chdir("/");

  disp->fbdev = strdup(config.fb_device);
  if (map_framebuffer(disp) != 0) {
    log_fatal("Could not map framebuffer device at %s", config.fb_device);
    _exit(EXIT_FAILURE);
  }

  disp->bytes_per_pixel = disp->vinfo.bits_per_pixel / 8;
  disp->max_offset =
    (unsigned char *)disp->video + disp->vinfo.yres * disp->finfo.line_length;
  disp->bg_colour = config.bg_colour;


  // prepare GPS source
  char *this_host = GPSD_SHARED_MEMORY, *this_port = NULL;
  if(0 != strcmp(config.gps_host, "GPSD_SHARED_MEMORY")) {
    this_host = config.gps_host;
    this_port = config.gps_port;
  }

  // open the GPS source
  struct gps_data_t *gps_data = malloc(sizeof(struct gps_data_t));

  if (gps_open(this_host, this_port, gps_data) != 0)
    log_error("Can't open GPS.");
  else {
    log_info("Opened GPS at %s %s", this_host, this_port);
    gps_data->satellites_used = 0;
    gps_data->satellites_visible = 0;
  }

  // if enabled, start controlling the backlight
  pthread_t pwm_thread;
  if(config.bl_enable)
    pthread_create(&pwm_thread, NULL, run_backlight_pwm, NULL);
  else
    log_info("Backlight PWM: disabled");

  // if enabled, try to open then start monitoring the PPS signal
  bool got_pps = false;

  pthread_t pps_thread;
  struct pps_thread_args pps_args;

  if (config.pps_enable && open_pps(&pps_args, config) == 0) {
    got_pps = true;
    pps_args.disp = disp;
    pthread_create(&pps_thread, NULL, run_pps_monitor, &pps_args);
  } else
    log_info("PPS monitor: disabled");

  // start the main display
  run_display(disp, gps_data);

  if(config.bl_enable) pthread_join(pwm_thread, NULL);
  log_debug("joined pwm_thread");
  if(got_pps) pthread_join(pps_thread, NULL);
  log_debug("joined pps_thread");

  close(disp->file);
  free(disp);

  log_info("Exiting.\n");
  return EXIT_SUCCESS;
}
