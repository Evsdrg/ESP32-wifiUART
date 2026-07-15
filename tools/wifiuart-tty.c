// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Linux CUSE serial facade for ESP WiFi UART Bridge.
 *
 * The device created by this tool is intentionally named ttyUSBx by default so
 * software that only scans /dev/ttyUSB* can use the Wi-Fi bridge through the
 * normal pyserial/esptool POSIX serial path.
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <arpa/inet.h>
#include <cuse_lowlevel.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_opt.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#ifndef TCGETS
#define TCGETS 0x5401
#endif
#ifndef TCSETS
#define TCSETS 0x5402
#endif
#ifndef TCSETSW
#define TCSETSW 0x5403
#endif
#ifndef TCSETSF
#define TCSETSF 0x5404
#endif
#ifndef TCSBRK
#define TCSBRK 0x5409
#endif
#ifndef TCFLSH
#define TCFLSH 0x540B
#endif
#ifndef TIOCEXCL
#define TIOCEXCL 0x540C
#endif
#ifndef TIOCNXCL
#define TIOCNXCL 0x540D
#endif
#ifndef TIOCOUTQ
#define TIOCOUTQ 0x5411
#endif
#ifndef TIOCINQ
#ifdef FIONREAD
#define TIOCINQ FIONREAD
#else
#define TIOCINQ 0x541B
#endif
#endif
#ifndef TIOCSBRK
#define TIOCSBRK 0x5427
#endif
#ifndef TIOCCBRK
#define TIOCCBRK 0x5428
#endif
#ifdef TCGETS2
#undef TCGETS2
#endif
#define TCGETS2 0x802C542A
#ifdef TCSETS2
#undef TCSETS2
#endif
#define TCSETS2 0x402C542B
#ifdef TCSETSW2
#undef TCSETSW2
#endif
#define TCSETSW2 0x402C542C
#ifdef TCSETSF2
#undef TCSETSF2
#endif
#define TCSETSF2 0x402C542D
#ifndef BOTHER
#define BOTHER 0010000
#endif
#ifndef CBAUD
#define CBAUD 0010017
#endif
#ifndef TIOCMGET
#define TIOCMGET 0x5415
#endif
#ifndef TIOCMBIS
#define TIOCMBIS 0x5416
#endif
#ifndef TIOCMBIC
#define TIOCMBIC 0x5417
#endif
#ifndef TIOCMSET
#define TIOCMSET 0x5418
#endif
#ifndef TIOCM_DTR
#define TIOCM_DTR 0x002
#endif
#ifndef TIOCM_RTS
#define TIOCM_RTS 0x004
#endif
#ifndef TIOCM_CTS
#define TIOCM_CTS 0x020
#endif
#ifndef TIOCM_CAR
#define TIOCM_CAR 0x040
#endif
#ifndef TIOCM_DSR
#define TIOCM_DSR 0x100
#endif

#define DEFAULT_DEVICE_NAME "ttyUSB10"
#define DEFAULT_RFC2217_PORT "2217"
#define DEFAULT_BAUD_RATE 115200U
#define RX_BUFFER_SIZE (64U * 1024U)

enum {
  TELNET_SE = 0xF0,
  TELNET_SB = 0xFA,
  TELNET_WILL = 0xFB,
  TELNET_WONT = 0xFC,
  TELNET_DO = 0xFD,
  TELNET_DONT = 0xFE,
  TELNET_IAC = 0xFF,
  TELNET_OPT_BINARY = 0x00,
  TELNET_OPT_ECHO = 0x01,
  TELNET_OPT_SGA = 0x03,
  TELNET_OPT_COM_PORT = 0x2C,
};

enum {
  RFC_SET_BAUDRATE = 0x01,
  RFC_SET_DATASIZE = 0x02,
  RFC_SET_PARITY = 0x03,
  RFC_SET_STOPSIZE = 0x04,
  RFC_SET_CONTROL = 0x05,
  RFC_PURGE_DATA = 0x0C,
};

enum {
  RFC_CONTROL_BREAK_ON = 0x05,
  RFC_CONTROL_BREAK_OFF = 0x06,
  RFC_CONTROL_DTR_ON = 0x08,
  RFC_CONTROL_DTR_OFF = 0x09,
  RFC_CONTROL_RTS_ON = 0x0B,
  RFC_CONTROL_RTS_OFF = 0x0C,
};

enum {
  RFC_PURGE_RX = 0x01,
  RFC_PURGE_TX = 0x02,
  RFC_PURGE_BOTH = 0x03,
};

enum telnet_state {
  TELNET_NORMAL,
  TELNET_IAC_SEEN,
  TELNET_NEGOTIATE,
  TELNET_SUBOPTION,
  TELNET_SUBOPTION_IAC,
};

struct termios2_wire {
  unsigned int c_iflag;
  unsigned int c_oflag;
  unsigned int c_cflag;
  unsigned int c_lflag;
  unsigned char c_line;
  unsigned char c_cc[19];
  unsigned int c_ispeed;
  unsigned int c_ospeed;
};

struct ring_buffer {
  uint8_t *data;
  size_t capacity;
  size_t head;
  size_t tail;
  size_t size;
};

struct poll_waiter {
  struct fuse_pollhandle *handle;
  struct poll_waiter *next;
};

struct serial_settings {
  unsigned int baud;
  unsigned int data_bits;
  unsigned int parity_code;
  unsigned int stop_code;
};

struct app_state {
  char host[256];
  char port[16];
  char dev_name[64];
  mode_t dev_mode;
  bool verbose;
  bool suppress_open_lines;

  pthread_mutex_t lock;
  pthread_mutex_t send_lock;
  struct ring_buffer rx;
  struct poll_waiter *poll_waiters;

  int sock;
  bool connected;
  bool open;
  bool reader_started;
  pthread_t reader_thread;

  struct termios tio;
  struct termios2_wire tio2;
  unsigned int modem_bits;
  bool control_enabled;
  bool remote_dtr;
  bool remote_rts;
  enum telnet_state telnet_state;
  uint8_t telnet_command;
};

static struct app_state g_state;

static void log_debug(const char *fmt, ...)
{
  if (!g_state.verbose) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static void ring_clear(struct ring_buffer *ring)
{
  ring->head = 0;
  ring->tail = 0;
  ring->size = 0;
}

static bool ring_init(struct ring_buffer *ring, size_t capacity)
{
  ring->data = calloc(capacity, 1);
  if (!ring->data) {
    return false;
  }
  ring->capacity = capacity;
  ring_clear(ring);
  return true;
}

static void ring_free(struct ring_buffer *ring)
{
  free(ring->data);
  memset(ring, 0, sizeof(*ring));
}

static size_t ring_push(struct ring_buffer *ring, const uint8_t *data, size_t size)
{
  size_t written = 0;
  while (written < size && ring->size < ring->capacity) {
    ring->data[ring->head] = data[written++];
    ring->head = (ring->head + 1) % ring->capacity;
    ring->size++;
  }
  return written;
}

static size_t ring_pop(struct ring_buffer *ring, uint8_t *data, size_t size)
{
  size_t read = 0;
  while (read < size && ring->size > 0) {
    data[read++] = ring->data[ring->tail];
    ring->tail = (ring->tail + 1) % ring->capacity;
    ring->size--;
  }
  return read;
}

static speed_t baud_to_speed(unsigned int baud)
{
  switch (baud) {
    case 0: return B0;
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
#ifdef B57600
    case 57600: return B57600;
#endif
#ifdef B115200
    case 115200: return B115200;
#endif
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B576000
    case 576000: return B576000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B1152000
    case 1152000: return B1152000;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
#ifdef B2500000
    case 2500000: return B2500000;
#endif
#ifdef B3000000
    case 3000000: return B3000000;
#endif
#ifdef B3500000
    case 3500000: return B3500000;
#endif
#ifdef B4000000
    case 4000000: return B4000000;
#endif
    default: return B38400;
  }
}

static unsigned int speed_to_baud(speed_t speed, unsigned int fallback)
{
  switch (speed) {
    case B0: return 0;
    case B50: return 50;
    case B75: return 75;
    case B110: return 110;
    case B134: return 134;
    case B150: return 150;
    case B200: return 200;
    case B300: return 300;
    case B600: return 600;
    case B1200: return 1200;
    case B1800: return 1800;
    case B2400: return 2400;
    case B4800: return 4800;
    case B9600: return 9600;
    case B19200: return 19200;
    case B38400: return 38400;
#ifdef B57600
    case B57600: return 57600;
#endif
#ifdef B115200
    case B115200: return 115200;
#endif
#ifdef B230400
    case B230400: return 230400;
#endif
#ifdef B460800
    case B460800: return 460800;
#endif
#ifdef B500000
    case B500000: return 500000;
#endif
#ifdef B576000
    case B576000: return 576000;
#endif
#ifdef B921600
    case B921600: return 921600;
#endif
#ifdef B1000000
    case B1000000: return 1000000;
#endif
#ifdef B1152000
    case B1152000: return 1152000;
#endif
#ifdef B1500000
    case B1500000: return 1500000;
#endif
#ifdef B2000000
    case B2000000: return 2000000;
#endif
#ifdef B2500000
    case B2500000: return 2500000;
#endif
#ifdef B3000000
    case B3000000: return 3000000;
#endif
#ifdef B3500000
    case B3500000: return 3500000;
#endif
#ifdef B4000000
    case B4000000: return 4000000;
#endif
    default: return fallback;
  }
}

static void fill_termios2_from_termios(unsigned int baud)
{
  memset(&g_state.tio2, 0, sizeof(g_state.tio2));
  g_state.tio2.c_iflag = g_state.tio.c_iflag;
  g_state.tio2.c_oflag = g_state.tio.c_oflag;
  g_state.tio2.c_cflag = g_state.tio.c_cflag;
  g_state.tio2.c_lflag = g_state.tio.c_lflag;
  g_state.tio2.c_line = 0;
  const size_t count = sizeof(g_state.tio2.c_cc) < sizeof(g_state.tio.c_cc) ?
                           sizeof(g_state.tio2.c_cc) :
                           sizeof(g_state.tio.c_cc);
  memcpy(g_state.tio2.c_cc, g_state.tio.c_cc, count);
  g_state.tio2.c_ispeed = baud;
  g_state.tio2.c_ospeed = baud;
}

static void init_termios_state(void)
{
  memset(&g_state.tio, 0, sizeof(g_state.tio));
  g_state.tio.c_iflag = 0;
  g_state.tio.c_oflag = 0;
  g_state.tio.c_cflag = CLOCAL | CREAD | CS8 | baud_to_speed(DEFAULT_BAUD_RATE);
  g_state.tio.c_lflag = 0;
  g_state.tio.c_cc[VMIN] = 0;
  g_state.tio.c_cc[VTIME] = 0;
  cfsetispeed(&g_state.tio, baud_to_speed(DEFAULT_BAUD_RATE));
  cfsetospeed(&g_state.tio, baud_to_speed(DEFAULT_BAUD_RATE));
  fill_termios2_from_termios(DEFAULT_BAUD_RATE);
}

static struct serial_settings settings_from_termios(const struct termios *tio)
{
  struct serial_settings settings = {0};
  settings.baud = speed_to_baud(cfgetospeed(tio), DEFAULT_BAUD_RATE);

  switch (tio->c_cflag & CSIZE) {
    case CS5: settings.data_bits = 5; break;
    case CS6: settings.data_bits = 6; break;
    case CS7: settings.data_bits = 7; break;
    case CS8:
    default: settings.data_bits = 8; break;
  }

  if (!(tio->c_cflag & PARENB)) {
    settings.parity_code = 1;
  } else if (tio->c_cflag & PARODD) {
    settings.parity_code = 2;
  } else {
    settings.parity_code = 3;
  }

  settings.stop_code = (tio->c_cflag & CSTOPB) ? 2 : 1;
  return settings;
}

static int connect_tcp(const char *host, const char *port)
{
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;

  const int gai = getaddrinfo(host, port, &hints, &result);
  if (gai != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(gai));
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      int yes = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
      break;
    }
    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);
  return fd;
}

static int send_all_locked(const uint8_t *data, size_t size)
{
  pthread_mutex_lock(&g_state.lock);
  const int fd = g_state.sock;
  const bool connected = g_state.connected;
  pthread_mutex_unlock(&g_state.lock);

  if (fd < 0 || !connected) {
    return -ENOTCONN;
  }

  pthread_mutex_lock(&g_state.send_lock);
  size_t sent = 0;
  while (sent < size) {
    const ssize_t rc = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (rc < 0) {
      const int err = errno;
      pthread_mutex_unlock(&g_state.send_lock);
      return -err;
    }
    if (rc == 0) {
      pthread_mutex_unlock(&g_state.send_lock);
      return -EIO;
    }
    sent += (size_t)rc;
  }
  pthread_mutex_unlock(&g_state.send_lock);
  return 0;
}

static int send_telnet_option(uint8_t command, uint8_t option)
{
  const uint8_t bytes[] = {TELNET_IAC, command, option};
  return send_all_locked(bytes, sizeof(bytes));
}

static int send_rfc2217_suboption(uint8_t option, const uint8_t *payload, size_t size)
{
  uint8_t buffer[64];
  size_t out = 0;
  buffer[out++] = TELNET_IAC;
  buffer[out++] = TELNET_SB;
  buffer[out++] = TELNET_OPT_COM_PORT;
  buffer[out++] = option;
  for (size_t i = 0; i < size && out + 2 < sizeof(buffer); ++i) {
    buffer[out++] = payload[i];
    if (payload[i] == TELNET_IAC && out < sizeof(buffer)) {
      buffer[out++] = TELNET_IAC;
    }
  }
  buffer[out++] = TELNET_IAC;
  buffer[out++] = TELNET_SE;
  return send_all_locked(buffer, out);
}

static int send_rfc2217_byte(uint8_t option, uint8_t value)
{
  return send_rfc2217_suboption(option, &value, 1);
}

static int send_rfc2217_u32(uint8_t option, unsigned int value)
{
  const uint8_t payload[] = {
      (uint8_t)((value >> 24) & 0xff),
      (uint8_t)((value >> 16) & 0xff),
      (uint8_t)((value >> 8) & 0xff),
      (uint8_t)(value & 0xff),
  };
  return send_rfc2217_suboption(option, payload, sizeof(payload));
}

static void send_initial_negotiation(void)
{
  send_telnet_option(TELNET_DO, TELNET_OPT_BINARY);
  send_telnet_option(TELNET_WILL, TELNET_OPT_BINARY);
  send_telnet_option(TELNET_DO, TELNET_OPT_SGA);
  send_telnet_option(TELNET_WILL, TELNET_OPT_SGA);
  send_telnet_option(TELNET_DO, TELNET_OPT_COM_PORT);
  send_telnet_option(TELNET_WILL, TELNET_OPT_COM_PORT);
}

static void send_serial_settings(struct serial_settings settings)
{
  if (settings.baud > 0) {
    send_rfc2217_u32(RFC_SET_BAUDRATE, settings.baud);
  }
  send_rfc2217_byte(RFC_SET_DATASIZE, (uint8_t)settings.data_bits);
  send_rfc2217_byte(RFC_SET_PARITY, (uint8_t)settings.parity_code);
  send_rfc2217_byte(RFC_SET_STOPSIZE, (uint8_t)settings.stop_code);
  log_debug("serial: baud=%u data=%u parity=%u stop=%u\n",
            settings.baud,
            settings.data_bits,
            settings.parity_code,
            settings.stop_code);
}

static int send_data_bytes(const char *data, size_t size)
{
  uint8_t buffer[1024];
  size_t out = 0;
  for (size_t i = 0; i < size; ++i) {
    if (out + 2 >= sizeof(buffer)) {
      const int rc = send_all_locked(buffer, out);
      if (rc < 0) {
        return rc;
      }
      out = 0;
    }
    buffer[out++] = (uint8_t)data[i];
    if ((uint8_t)data[i] == TELNET_IAC) {
      buffer[out++] = TELNET_IAC;
    }
  }
  if (out > 0) {
    return send_all_locked(buffer, out);
  }
  return 0;
}

static void send_remote_control_state(unsigned int new_bits, bool force)
{
  const bool next_dtr = (new_bits & TIOCM_DTR) != 0;
  const bool next_rts = (new_bits & TIOCM_RTS) != 0;
  const bool old_dtr = g_state.remote_dtr;
  const bool old_rts = g_state.remote_rts;

  if (!force && old_dtr == next_dtr && old_rts == next_rts) {
    return;
  }

  /*
   * Preserve esptool reset intent over the network: assert RTS before DTR
   * when entering reset, but set DTR before releasing RTS when leaving reset.
   */
  if (old_rts && !next_rts) {
    if (force || old_dtr != next_dtr) {
      send_rfc2217_byte(RFC_SET_CONTROL, next_dtr ? RFC_CONTROL_DTR_ON : RFC_CONTROL_DTR_OFF);
    }
    send_rfc2217_byte(RFC_SET_CONTROL, RFC_CONTROL_RTS_OFF);
  } else {
    if (force || old_rts != next_rts) {
      send_rfc2217_byte(RFC_SET_CONTROL, next_rts ? RFC_CONTROL_RTS_ON : RFC_CONTROL_RTS_OFF);
    }
    if (force || old_dtr != next_dtr) {
      send_rfc2217_byte(RFC_SET_CONTROL, next_dtr ? RFC_CONTROL_DTR_ON : RFC_CONTROL_DTR_OFF);
    }
  }

  g_state.remote_dtr = next_dtr;
  g_state.remote_rts = next_rts;
  log_debug("modem: DTR=%u RTS=%u%s\n", next_dtr, next_rts, force ? " force" : "");
}

static void notify_poll_waiters(struct poll_waiter *waiters)
{
  while (waiters) {
    struct poll_waiter *next = waiters->next;
    fuse_lowlevel_notify_poll(waiters->handle);
    fuse_pollhandle_destroy(waiters->handle);
    free(waiters);
    waiters = next;
  }
}

static void queue_rx_byte(uint8_t value)
{
  struct poll_waiter *waiters = NULL;
  pthread_mutex_lock(&g_state.lock);
  const size_t written = ring_push(&g_state.rx, &value, 1);
  if (written == 1) {
    waiters = g_state.poll_waiters;
    g_state.poll_waiters = NULL;
  }
  pthread_mutex_unlock(&g_state.lock);

  if (written != 1) {
    log_debug("rx buffer full, dropped one byte\n");
  }
  notify_poll_waiters(waiters);
}

static bool is_supported_telnet_option(uint8_t option)
{
  return option == TELNET_OPT_BINARY || option == TELNET_OPT_SGA || option == TELNET_OPT_COM_PORT;
}

static void process_telnet_byte(uint8_t value)
{
  switch (g_state.telnet_state) {
    case TELNET_NORMAL:
      if (value == TELNET_IAC) {
        g_state.telnet_state = TELNET_IAC_SEEN;
      } else {
        queue_rx_byte(value);
      }
      break;
    case TELNET_IAC_SEEN:
      if (value == TELNET_IAC) {
        queue_rx_byte(TELNET_IAC);
        g_state.telnet_state = TELNET_NORMAL;
      } else if (value == TELNET_DO || value == TELNET_DONT || value == TELNET_WILL || value == TELNET_WONT) {
        g_state.telnet_command = value;
        g_state.telnet_state = TELNET_NEGOTIATE;
      } else if (value == TELNET_SB) {
        g_state.telnet_state = TELNET_SUBOPTION;
      } else {
        g_state.telnet_state = TELNET_NORMAL;
      }
      break;
    case TELNET_NEGOTIATE:
      if (g_state.telnet_command == TELNET_DO) {
        send_telnet_option(is_supported_telnet_option(value) ? TELNET_WILL : TELNET_WONT, value);
      } else if (g_state.telnet_command == TELNET_WILL) {
        send_telnet_option(is_supported_telnet_option(value) ? TELNET_DO : TELNET_DONT, value);
      }
      g_state.telnet_state = TELNET_NORMAL;
      break;
    case TELNET_SUBOPTION:
      if (value == TELNET_IAC) {
        g_state.telnet_state = TELNET_SUBOPTION_IAC;
      }
      break;
    case TELNET_SUBOPTION_IAC:
      if (value == TELNET_SE) {
        g_state.telnet_state = TELNET_NORMAL;
      } else if (value != TELNET_IAC) {
        g_state.telnet_state = TELNET_SUBOPTION;
      }
      break;
  }
}

static void *reader_thread_main(void *arg)
{
  (void)arg;
  uint8_t buffer[1024];

  for (;;) {
    pthread_mutex_lock(&g_state.lock);
    const int fd = g_state.sock;
    const bool connected = g_state.connected;
    pthread_mutex_unlock(&g_state.lock);
    if (fd < 0 || !connected) {
      break;
    }

    const ssize_t rc = recv(fd, buffer, sizeof(buffer), 0);
    if (rc > 0) {
      for (ssize_t i = 0; i < rc; ++i) {
        process_telnet_byte(buffer[i]);
      }
      continue;
    }
    if (rc < 0 && errno == EINTR) {
      continue;
    }
    break;
  }

  struct poll_waiter *waiters = NULL;
  pthread_mutex_lock(&g_state.lock);
  g_state.connected = false;
  waiters = g_state.poll_waiters;
  g_state.poll_waiters = NULL;
  pthread_mutex_unlock(&g_state.lock);
  notify_poll_waiters(waiters);
  log_debug("remote connection closed\n");
  return NULL;
}

static void flush_buffers(int queue)
{
  bool clear_rx = false;
  uint8_t purge = RFC_PURGE_BOTH;

  if (queue == TCIFLUSH) {
    clear_rx = true;
    purge = RFC_PURGE_RX;
  } else if (queue == TCOFLUSH) {
    purge = RFC_PURGE_TX;
  } else {
    clear_rx = true;
    purge = RFC_PURGE_BOTH;
  }

  if (clear_rx) {
    pthread_mutex_lock(&g_state.lock);
    ring_clear(&g_state.rx);
    pthread_mutex_unlock(&g_state.lock);
  }
  send_rfc2217_byte(RFC_PURGE_DATA, purge);
}

static void reply_ioctl_read(fuse_req_t req, void *arg, const void *data, size_t size, size_t out_bufsz)
{
  if (out_bufsz == 0) {
    struct iovec out_iov = {arg, size};
    fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
    return;
  }
  if (out_bufsz < size) {
    fuse_reply_err(req, EINVAL);
    return;
  }
  fuse_reply_ioctl(req, 0, data, size);
}

static bool request_ioctl_write(fuse_req_t req, void *arg, size_t size, const void *in_buf, size_t in_bufsz)
{
  (void)in_buf;
  if (in_bufsz == 0) {
    struct iovec in_iov = {arg, size};
    fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
    return false;
  }
  if (in_bufsz < size) {
    fuse_reply_err(req, EINVAL);
    return false;
  }
  return true;
}

static void apply_termios(const struct termios *next)
{
  struct serial_settings settings = settings_from_termios(next);
  pthread_mutex_lock(&g_state.lock);
  g_state.tio = *next;
  fill_termios2_from_termios(settings.baud);
  pthread_mutex_unlock(&g_state.lock);
  send_serial_settings(settings);
}

static void apply_termios2(const struct termios2_wire *next)
{
  struct termios tio;
  pthread_mutex_lock(&g_state.lock);
  tio = g_state.tio;
  tio.c_iflag = next->c_iflag;
  tio.c_oflag = next->c_oflag;
  tio.c_cflag = next->c_cflag;
  tio.c_lflag = next->c_lflag;
  const size_t count = sizeof(next->c_cc) < sizeof(tio.c_cc) ? sizeof(next->c_cc) : sizeof(tio.c_cc);
  memcpy(tio.c_cc, next->c_cc, count);

  unsigned int baud = next->c_ospeed;
  if ((next->c_cflag & CBAUD) != BOTHER) {
    baud = speed_to_baud(next->c_cflag & CBAUD, DEFAULT_BAUD_RATE);
  }
  cfsetispeed(&tio, baud_to_speed(baud));
  cfsetospeed(&tio, baud_to_speed(baud));
  g_state.tio = tio;
  g_state.tio2 = *next;
  g_state.tio2.c_ispeed = baud;
  g_state.tio2.c_ospeed = baud;
  pthread_mutex_unlock(&g_state.lock);

  struct serial_settings settings = settings_from_termios(&tio);
  settings.baud = baud;
  send_serial_settings(settings);
}

static void handle_modem_update(unsigned int new_bits, bool from_set)
{
  pthread_mutex_lock(&g_state.lock);
  const bool should_send = from_set || g_state.control_enabled || !g_state.suppress_open_lines;
  g_state.modem_bits = (g_state.modem_bits & ~(TIOCM_DTR | TIOCM_RTS)) |
                       (new_bits & (TIOCM_DTR | TIOCM_RTS));
  if (from_set) {
    g_state.control_enabled = true;
  }
  pthread_mutex_unlock(&g_state.lock);

  if (should_send) {
    send_remote_control_state(new_bits, from_set && !g_state.remote_dtr && !g_state.remote_rts);
  } else {
    log_debug("modem: suppressed open-time DTR/RTS update bits=0x%x\n", new_bits);
  }
}

static void cuse_tty_open(fuse_req_t req, struct fuse_file_info *fi)
{
  pthread_mutex_lock(&g_state.lock);
  if (g_state.open) {
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, EBUSY);
    return;
  }
  g_state.open = true;
  g_state.connected = false;
  g_state.control_enabled = !g_state.suppress_open_lines;
  g_state.remote_dtr = false;
  g_state.remote_rts = false;
  g_state.modem_bits = 0;
  g_state.telnet_state = TELNET_NORMAL;
  ring_clear(&g_state.rx);
  pthread_mutex_unlock(&g_state.lock);

  const int fd = connect_tcp(g_state.host, g_state.port);
  if (fd < 0) {
    pthread_mutex_lock(&g_state.lock);
    g_state.open = false;
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, ECONNREFUSED);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.sock = fd;
  g_state.connected = true;
  pthread_mutex_unlock(&g_state.lock);

  if (pthread_create(&g_state.reader_thread, NULL, reader_thread_main, NULL) != 0) {
    close(fd);
    pthread_mutex_lock(&g_state.lock);
    g_state.sock = -1;
    g_state.connected = false;
    g_state.open = false;
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, EIO);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.reader_started = true;
  struct serial_settings settings = settings_from_termios(&g_state.tio);
  pthread_mutex_unlock(&g_state.lock);

  send_initial_negotiation();
  send_serial_settings(settings);

  fi->direct_io = 1;
  fi->nonseekable = 1;
  fuse_reply_open(req, fi);
}

static void cuse_tty_init_done(void *userdata)
{
  (void)userdata;
  char path[128];
  snprintf(path, sizeof(path), "/dev/%s", g_state.dev_name);
  if (chmod(path, g_state.dev_mode) != 0) {
    fprintf(stderr, "warning: chmod %s failed: %s\n", path, strerror(errno));
  }
}

static void cuse_tty_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi)
{
  (void)off;
  (void)fi;
  uint8_t *buffer = malloc(size ? size : 1);
  if (!buffer) {
    fuse_reply_err(req, ENOMEM);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  const bool connected = g_state.connected;
  const size_t read = ring_pop(&g_state.rx, buffer, size);
  pthread_mutex_unlock(&g_state.lock);

  if (read > 0) {
    fuse_reply_buf(req, (const char *)buffer, read);
  } else if (!connected) {
    fuse_reply_err(req, EIO);
  } else {
    fuse_reply_err(req, EAGAIN);
  }
  free(buffer);
}

static void cuse_tty_write(fuse_req_t req, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
  (void)off;
  (void)fi;
  const int rc = send_data_bytes(buf, size);
  if (rc < 0) {
    fuse_reply_err(req, -rc);
    return;
  }
  fuse_reply_write(req, size);
}

static void cuse_tty_flush(fuse_req_t req, struct fuse_file_info *fi)
{
  (void)fi;
  fuse_reply_err(req, 0);
}

static void cuse_tty_release(fuse_req_t req, struct fuse_file_info *fi)
{
  (void)fi;
  pthread_t thread;
  bool join_reader = false;
  int fd = -1;
  struct poll_waiter *waiters = NULL;

  pthread_mutex_lock(&g_state.lock);
  if (g_state.open) {
    fd = g_state.sock;
    g_state.sock = -1;
    g_state.connected = false;
    g_state.open = false;
    ring_clear(&g_state.rx);
    waiters = g_state.poll_waiters;
    g_state.poll_waiters = NULL;
    if (g_state.reader_started) {
      thread = g_state.reader_thread;
      join_reader = true;
      g_state.reader_started = false;
    }
  }
  pthread_mutex_unlock(&g_state.lock);

  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
  if (join_reader) {
    pthread_join(thread, NULL);
  }
  notify_poll_waiters(waiters);
  fuse_reply_err(req, 0);
}

static void cuse_tty_ioctl(fuse_req_t req,
                           int cmd,
                           void *arg,
                           struct fuse_file_info *fi,
                           unsigned int flags,
                           const void *in_buf,
                           size_t in_bufsz,
                           size_t out_bufsz)
{
  (void)fi;
  if (flags & FUSE_IOCTL_COMPAT) {
    fuse_reply_err(req, ENOSYS);
    return;
  }

  switch (cmd) {
    case TCGETS: {
      pthread_mutex_lock(&g_state.lock);
      struct termios current = g_state.tio;
      pthread_mutex_unlock(&g_state.lock);
      reply_ioctl_read(req, arg, &current, sizeof(current), out_bufsz);
      break;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
      if (!request_ioctl_write(req, arg, sizeof(struct termios), in_buf, in_bufsz)) {
        return;
      }
      if (cmd == TCSETSF) {
        flush_buffers(TCIOFLUSH);
      }
      apply_termios((const struct termios *)in_buf);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    }
    case TCGETS2: {
      pthread_mutex_lock(&g_state.lock);
      struct termios2_wire current = g_state.tio2;
      pthread_mutex_unlock(&g_state.lock);
      reply_ioctl_read(req, arg, &current, sizeof(current), out_bufsz);
      break;
    }
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
      if (!request_ioctl_write(req, arg, sizeof(struct termios2_wire), in_buf, in_bufsz)) {
        return;
      }
      if (cmd == TCSETSF2) {
        flush_buffers(TCIOFLUSH);
      }
      apply_termios2((const struct termios2_wire *)in_buf);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    }
    case TIOCMGET: {
      pthread_mutex_lock(&g_state.lock);
      unsigned int bits = g_state.modem_bits | TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
      pthread_mutex_unlock(&g_state.lock);
      reply_ioctl_read(req, arg, &bits, sizeof(bits), out_bufsz);
      break;
    }
    case TIOCMSET: {
      if (!request_ioctl_write(req, arg, sizeof(unsigned int), in_buf, in_bufsz)) {
        return;
      }
      unsigned int bits;
      memcpy(&bits, in_buf, sizeof(bits));
      handle_modem_update(bits, true);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    }
    case TIOCMBIS:
    case TIOCMBIC: {
      if (!request_ioctl_write(req, arg, sizeof(unsigned int), in_buf, in_bufsz)) {
        return;
      }
      unsigned int mask;
      memcpy(&mask, in_buf, sizeof(mask));
      pthread_mutex_lock(&g_state.lock);
      unsigned int bits = g_state.modem_bits;
      if (cmd == TIOCMBIS) {
        bits |= mask;
      } else {
        bits &= ~mask;
      }
      pthread_mutex_unlock(&g_state.lock);
      handle_modem_update(bits, false);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    }
    case TIOCINQ: {
      pthread_mutex_lock(&g_state.lock);
      unsigned int available = (unsigned int)g_state.rx.size;
      pthread_mutex_unlock(&g_state.lock);
      reply_ioctl_read(req, arg, &available, sizeof(available), out_bufsz);
      break;
    }
    case TIOCOUTQ: {
      const unsigned int queued = 0;
      reply_ioctl_read(req, arg, &queued, sizeof(queued), out_bufsz);
      break;
    }
    case TCFLSH:
      flush_buffers((int)(uintptr_t)arg);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    case TCSBRK:
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    case TIOCSBRK:
      send_rfc2217_byte(RFC_SET_CONTROL, RFC_CONTROL_BREAK_ON);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    case TIOCCBRK:
      send_rfc2217_byte(RFC_SET_CONTROL, RFC_CONTROL_BREAK_OFF);
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    case TIOCEXCL:
    case TIOCNXCL:
      fuse_reply_ioctl(req, 0, NULL, 0);
      break;
    default:
      fuse_reply_err(req, ENOTTY);
      break;
  }
}

static void cuse_tty_poll(fuse_req_t req, struct fuse_file_info *fi, struct fuse_pollhandle *ph)
{
  (void)fi;
  unsigned revents = 0;
  bool store_waiter = false;

  pthread_mutex_lock(&g_state.lock);
  if (!g_state.connected) {
    revents = POLLHUP | POLLERR;
  } else {
    revents = POLLOUT;
    if (g_state.rx.size > 0) {
      revents |= POLLIN;
    } else if (ph) {
      store_waiter = true;
    }
  }

  if (store_waiter) {
    struct poll_waiter *waiter = calloc(1, sizeof(*waiter));
    if (waiter) {
      waiter->handle = ph;
      waiter->next = g_state.poll_waiters;
      g_state.poll_waiters = waiter;
      ph = NULL;
    }
  }
  pthread_mutex_unlock(&g_state.lock);

  if (ph) {
    fuse_pollhandle_destroy(ph);
  }
  fuse_reply_poll(req, revents);
}

static const struct cuse_lowlevel_ops cuse_tty_ops = {
    .init_done = cuse_tty_init_done,
    .open = cuse_tty_open,
    .read = cuse_tty_read,
    .write = cuse_tty_write,
    .flush = cuse_tty_flush,
    .release = cuse_tty_release,
    .ioctl = cuse_tty_ioctl,
    .poll = cuse_tty_poll,
};

static void usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s --host <esp-ip> [options] [-f]\n"
          "\n"
          "options:\n"
          "  --host <addr>          ESP WiFi UART address (required)\n"
          "  --port <port>          RFC2217 port (default: %s)\n"
          "  --name <ttyUSBx>       CUSE device name (default: %s)\n"
          "  --dev-mode <octal>     Device permissions (default: 0666)\n"
          "  --raw-open-lines       Forward pyserial open-time DTR/RTS changes\n"
          "  --verbose              Print debug logs\n"
          "  -f                     Run in foreground (FUSE option)\n"
          "  -s                     Single-threaded FUSE loop (FUSE option)\n"
          "\n"
          "example:\n"
          "  sudo %s --host 192.168.4.1 --name ttyUSB10 -f\n"
          "  esptool.py --port /dev/ttyUSB10 --baud 460800 chip_id\n",
          argv0,
          DEFAULT_RFC2217_PORT,
          DEFAULT_DEVICE_NAME,
          argv0);
}

static bool parse_option_value(const char *arg, const char *name, const char **value)
{
  const size_t len = strlen(name);
  if (strncmp(arg, name, len) == 0 && arg[len] == '=') {
    *value = arg + len + 1;
    return true;
  }
  return false;
}

static void set_device_name(const char *value)
{
  const char *name = value;
  const char *dev_prefix = "/dev/";
  const size_t prefix_len = strlen(dev_prefix);
  if (strncmp(name, dev_prefix, prefix_len) == 0) {
    name += prefix_len;
  }
  snprintf(g_state.dev_name, sizeof(g_state.dev_name), "%s", name);
}

int main(int argc, char **argv)
{
  memset(&g_state, 0, sizeof(g_state));
  snprintf(g_state.port, sizeof(g_state.port), "%s", DEFAULT_RFC2217_PORT);
  snprintf(g_state.dev_name, sizeof(g_state.dev_name), "%s", DEFAULT_DEVICE_NAME);
  g_state.dev_mode = 0666;
  g_state.suppress_open_lines = true;
  g_state.sock = -1;
  pthread_mutex_init(&g_state.lock, NULL);
  pthread_mutex_init(&g_state.send_lock, NULL);
  init_termios_state();
  if (!ring_init(&g_state.rx, RX_BUFFER_SIZE)) {
    fprintf(stderr, "failed to allocate RX buffer\n");
    return 1;
  }

  struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
  fuse_opt_add_arg(&args, argv[0]);
  fuse_opt_add_arg(&args, "-s");

  for (int i = 1; i < argc; ++i) {
    const char *value = NULL;
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      ring_free(&g_state.rx);
      fuse_opt_free_args(&args);
      return 0;
    } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      snprintf(g_state.host, sizeof(g_state.host), "%s", argv[++i]);
    } else if (parse_option_value(argv[i], "--host", &value)) {
      snprintf(g_state.host, sizeof(g_state.host), "%s", value);
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      snprintf(g_state.port, sizeof(g_state.port), "%s", argv[++i]);
    } else if (parse_option_value(argv[i], "--port", &value)) {
      snprintf(g_state.port, sizeof(g_state.port), "%s", value);
    } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      set_device_name(argv[++i]);
    } else if (parse_option_value(argv[i], "--name", &value)) {
      set_device_name(value);
    } else if (strcmp(argv[i], "--dev-mode") == 0 && i + 1 < argc) {
      g_state.dev_mode = (mode_t)strtoul(argv[++i], NULL, 8);
    } else if (parse_option_value(argv[i], "--dev-mode", &value)) {
      g_state.dev_mode = (mode_t)strtoul(value, NULL, 8);
    } else if (strcmp(argv[i], "--raw-open-lines") == 0) {
      g_state.suppress_open_lines = false;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      g_state.verbose = true;
    } else {
      fuse_opt_add_arg(&args, argv[i]);
    }
  }

  if (g_state.host[0] == '\0') {
    usage(argv[0]);
    ring_free(&g_state.rx);
    fuse_opt_free_args(&args);
    return 2;
  }

  char dev_name_arg[128];
  snprintf(dev_name_arg, sizeof(dev_name_arg), "DEVNAME=%s", g_state.dev_name);
  const char *dev_info_argv[] = {dev_name_arg};
  struct cuse_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.dev_info_argc = 1;
  ci.dev_info_argv = dev_info_argv;
  ci.flags = CUSE_UNRESTRICTED_IOCTL;

  fprintf(stderr, "creating /dev/%s -> %s:%s\n", g_state.dev_name, g_state.host, g_state.port);
  const int rc = cuse_lowlevel_main(args.argc, args.argv, &ci, &cuse_tty_ops, NULL);

  ring_free(&g_state.rx);
  fuse_opt_free_args(&args);
  pthread_mutex_destroy(&g_state.lock);
  pthread_mutex_destroy(&g_state.send_lock);
  return rc;
}
