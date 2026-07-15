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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
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
#define DEFAULT_FLUSH_CONTROL_PORT "2218"
#define DEFAULT_BAUD_RATE 115200U
#define RX_BUFFER_SIZE (64U * 1024U)
#define TX_BUFFER_SIZE (64U * 1024U)

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
  RFC_RESUME_AFTER_FLUSH = 0x63,
  RFC_SERVER_SET_BAUDRATE = 0x65,
  RFC_SERVER_SET_DATASIZE = 0x66,
  RFC_SERVER_SET_PARITY = 0x67,
  RFC_SERVER_SET_STOPSIZE = 0x68,
  RFC_SERVER_SET_CONTROL = 0x69,
  RFC_SERVER_PURGE_DATA = 0x70,
};

enum {
  ACK_BAUD = 1U << 0,
  ACK_DATA = 1U << 1,
  ACK_PARITY = 1U << 2,
  ACK_STOP = 1U << 3,
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
  char control_port[16];
  char dev_name[64];
  mode_t dev_mode;
  bool verbose;
  bool suppress_open_lines;

  pthread_mutex_t lock;
  pthread_cond_t tx_cond;
  pthread_cond_t ack_cond;
  struct ring_buffer rx;
  struct ring_buffer tx;
  struct ring_buffer tx_frame_end;
  uint64_t tx_generation;
  struct poll_waiter *poll_waiters;

  int sock;
  int control_sock;
  uint32_t next_flush_token;
  bool connected;
  bool open;
  bool reader_started;
  pthread_t reader_thread;
  bool writer_started;
  pthread_t writer_thread;
  bool writer_sending;
  bool tx_at_frame_boundary;
  bool flush_pending;

  struct termios tio;
  struct termios2_wire tio2;
  unsigned int baud_rate;
  unsigned int modem_bits;
  unsigned int suppressed_initial_lines;
  uint64_t suppress_lines_until_ms;
  bool remote_dtr;
  bool remote_rts;
  enum telnet_state telnet_state;
  uint8_t telnet_command;
  uint8_t local_option_enabled;
  uint8_t local_option_requested;
  uint8_t remote_option_enabled;
  uint8_t remote_option_requested;
  uint8_t telnet_suboption[32];
  size_t telnet_suboption_length;

  unsigned int pending_serial_acks;
  struct serial_settings expected_serial;
  uint8_t expected_control_acks[4];
  size_t expected_control_ack_count;
  size_t expected_control_ack_index;
  bool pending_purge_ack;
  uint8_t expected_purge_ack;
  bool ack_failed;
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

static size_t ring_peek(const struct ring_buffer *ring, uint8_t *data, size_t size)
{
  size_t read = 0;
  size_t position = ring->tail;
  while (read < size && read < ring->size) {
    data[read++] = ring->data[position];
    position = (position + 1) % ring->capacity;
  }
  return read;
}

static void ring_discard(struct ring_buffer *ring, size_t size)
{
  const size_t discard = size < ring->size ? size : ring->size;
  ring->tail = (ring->tail + discard) % ring->capacity;
  ring->size -= discard;
}

static size_t ring_free_space(const struct ring_buffer *ring)
{
  return ring->capacity - ring->size;
}

static uint64_t monotonic_ms(void)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
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
  g_state.baud_rate = DEFAULT_BAUD_RATE;
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
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
      break;
    }
    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);
  return fd;
}

static int queue_tx_locked(const uint8_t *data, size_t size)
{
  if (g_state.sock < 0 || !g_state.connected) {
    return -ENOTCONN;
  }
  if (size == 0 || ring_free_space(&g_state.tx) < size ||
      ring_free_space(&g_state.tx_frame_end) < size) {
    return -EAGAIN;
  }
  ring_push(&g_state.tx, data, size);
  for (size_t i = 0; i < size; ++i) {
    const uint8_t frameEnd = i + 1 == size ? 1 : 0;
    ring_push(&g_state.tx_frame_end, &frameEnd, 1);
  }
  pthread_cond_signal(&g_state.tx_cond);
  return 0;
}

static int send_all_locked(const uint8_t *data, size_t size)
{
  pthread_mutex_lock(&g_state.lock);
  const int rc = queue_tx_locked(data, size);
  pthread_mutex_unlock(&g_state.lock);
  return rc;
}

static int send_telnet_option(uint8_t command, uint8_t option)
{
  const uint8_t bytes[] = {TELNET_IAC, command, option};
  return send_all_locked(bytes, sizeof(bytes));
}

static int append_rfc2217_suboption(uint8_t *buffer,
                                    size_t capacity,
                                    size_t *out,
                                    uint8_t option,
                                    const uint8_t *payload,
                                    size_t size)
{
  if (*out + 6 > capacity) {
    return -ENOSPC;
  }
  buffer[(*out)++] = TELNET_IAC;
  buffer[(*out)++] = TELNET_SB;
  buffer[(*out)++] = TELNET_OPT_COM_PORT;
  buffer[(*out)++] = option;
  for (size_t i = 0; i < size; ++i) {
    if (*out + 3 > capacity) {
      return -ENOSPC;
    }
    buffer[(*out)++] = payload[i];
    if (payload[i] == TELNET_IAC) {
      buffer[(*out)++] = TELNET_IAC;
    }
  }
  buffer[(*out)++] = TELNET_IAC;
  buffer[(*out)++] = TELNET_SE;
  return 0;
}

static uint8_t telnet_option_bit(uint8_t option)
{
  switch (option) {
    case TELNET_OPT_BINARY: return 1U << 0;
    case TELNET_OPT_ECHO: return 1U << 1;
    case TELNET_OPT_SGA: return 1U << 2;
    case TELNET_OPT_COM_PORT: return 1U << 3;
    default: return 0;
  }
}

static void request_local_option(uint8_t option)
{
  const uint8_t bit = telnet_option_bit(option);
  if (bit == 0 || (g_state.local_option_enabled & bit) != 0 ||
      (g_state.local_option_requested & bit) != 0) {
    return;
  }
  g_state.local_option_requested |= bit;
  send_telnet_option(TELNET_WILL, option);
}

static void request_remote_option(uint8_t option)
{
  const uint8_t bit = telnet_option_bit(option);
  if (bit == 0 || (g_state.remote_option_enabled & bit) != 0 ||
      (g_state.remote_option_requested & bit) != 0) {
    return;
  }
  g_state.remote_option_requested |= bit;
  send_telnet_option(TELNET_DO, option);
}

static void send_initial_negotiation(void)
{
  request_remote_option(TELNET_OPT_BINARY);
  request_local_option(TELNET_OPT_BINARY);
  request_remote_option(TELNET_OPT_SGA);
  request_local_option(TELNET_OPT_SGA);
  request_remote_option(TELNET_OPT_COM_PORT);
  request_local_option(TELNET_OPT_COM_PORT);
}

static bool ack_transaction_complete_locked(void)
{
  return g_state.pending_serial_acks == 0 &&
         g_state.expected_control_ack_index >= g_state.expected_control_ack_count &&
         !g_state.pending_purge_ack;
}

static void clear_pending_acks_locked(void)
{
  g_state.pending_serial_acks = 0;
  g_state.expected_control_ack_count = 0;
  g_state.expected_control_ack_index = 0;
  g_state.pending_purge_ack = false;
  g_state.ack_failed = false;
}

static void mark_remote_disconnected(int fd);

static int wait_for_pending_acks(unsigned int timeout_ms)
{
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return -errno;
  }
  deadline.tv_sec += timeout_ms / 1000U;
  deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  int result = 0;
  bool invalidate_connection = false;
  int fd = -1;
  pthread_mutex_lock(&g_state.lock);
  while (g_state.connected && !g_state.ack_failed &&
         !ack_transaction_complete_locked()) {
    const int rc = pthread_cond_timedwait(&g_state.ack_cond, &g_state.lock, &deadline);
    if (rc == ETIMEDOUT) {
      result = -ETIMEDOUT;
      invalidate_connection = true;
      break;
    }
    if (rc != 0) {
      result = -rc;
      invalidate_connection = true;
      break;
    }
  }
  const bool connected = g_state.connected;
  const bool failed = g_state.ack_failed;
  if (result == 0 && failed) {
    result = -EREMOTEIO;
    invalidate_connection = true;
  } else if (result == 0 && !connected) {
    result = -ENOTCONN;
  }
  if (invalidate_connection) {
    fd = g_state.sock;
  }
  clear_pending_acks_locked();
  pthread_mutex_unlock(&g_state.lock);
  if (invalidate_connection && fd >= 0) {
    mark_remote_disconnected(fd);
  }
  return result;
}

static unsigned int remote_uart_operation_timeout_ms(void)
{
  pthread_mutex_lock(&g_state.lock);
  const unsigned int current_baud = g_state.baud_rate;
  const int fd = g_state.sock;
  const size_t localBytes = g_state.tx.size;
  pthread_mutex_unlock(&g_state.lock);
  int socketBytes = 0;
  if (fd >= 0 && ioctl(fd, TIOCOUTQ, &socketBytes) != 0) {
    socketBytes = 0;
  }
  const unsigned int baud = current_baud >= 300 ? current_baud : 300;
  const uint64_t outstandingBytes =
      TX_BUFFER_SIZE + localBytes + (uint64_t)(socketBytes > 0 ? socketBytes : 0);
  const uint64_t queued_bits = (outstandingBytes + 4096ULL + 128ULL) * 12ULL;
  const uint64_t timeout = (queued_bits * 1000ULL + baud - 1) / baud + 2000ULL;
  return (unsigned int)(timeout >= UINT_MAX ? UINT_MAX - 1U : timeout);
}

static int queue_serial_settings(struct serial_settings settings)
{
  uint8_t buffer[64];
  size_t out = 0;
  int rc = 0;
  if (settings.baud > 0) {
    const uint8_t baud[] = {
        (uint8_t)((settings.baud >> 24) & 0xff),
        (uint8_t)((settings.baud >> 16) & 0xff),
        (uint8_t)((settings.baud >> 8) & 0xff),
        (uint8_t)(settings.baud & 0xff),
    };
    rc = append_rfc2217_suboption(
        buffer,
        sizeof(buffer),
        &out,
        RFC_SET_BAUDRATE,
        baud,
        sizeof(baud));
    if (rc < 0) {
      return rc;
    }
  }
  const uint8_t data_bits = (uint8_t)settings.data_bits;
  const uint8_t parity = (uint8_t)settings.parity_code;
  const uint8_t stop = (uint8_t)settings.stop_code;
  if ((rc = append_rfc2217_suboption(buffer, sizeof(buffer), &out,
                                     RFC_SET_DATASIZE, &data_bits, 1)) < 0 ||
      (rc = append_rfc2217_suboption(buffer, sizeof(buffer), &out,
                                     RFC_SET_PARITY, &parity, 1)) < 0 ||
      (rc = append_rfc2217_suboption(buffer, sizeof(buffer), &out,
                                     RFC_SET_STOPSIZE, &stop, 1)) < 0) {
    return rc;
  }
  rc = send_all_locked(buffer, out);
  if (rc < 0) {
    return rc;
  }
  log_debug("serial: baud=%u data=%u parity=%u stop=%u\n",
            settings.baud,
            settings.data_bits,
            settings.parity_code,
            settings.stop_code);
  return 0;
}

static int send_serial_settings(struct serial_settings settings)
{
  pthread_mutex_lock(&g_state.lock);
  if (!ack_transaction_complete_locked()) {
    pthread_mutex_unlock(&g_state.lock);
    return -EBUSY;
  }
  g_state.expected_serial = settings;
  g_state.pending_serial_acks = ACK_DATA | ACK_PARITY | ACK_STOP;
  if (settings.baud > 0) {
    g_state.pending_serial_acks |= ACK_BAUD;
  }
  g_state.ack_failed = false;
  pthread_mutex_unlock(&g_state.lock);

  const int rc = queue_serial_settings(settings);
  if (rc < 0) {
    pthread_mutex_lock(&g_state.lock);
    clear_pending_acks_locked();
    pthread_mutex_unlock(&g_state.lock);
    return rc;
  }
  return wait_for_pending_acks(remote_uart_operation_timeout_ms());
}

static ssize_t send_data_bytes(const char *data, size_t size)
{
  pthread_mutex_lock(&g_state.lock);
  if (g_state.sock < 0 || !g_state.connected) {
    pthread_mutex_unlock(&g_state.lock);
    return -ENOTCONN;
  }

  size_t accepted = 0;
  while (accepted < size) {
    const uint8_t value = (uint8_t)data[accepted];
    const size_t encoded_size = value == TELNET_IAC ? 2 : 1;
    if (ring_free_space(&g_state.tx) < encoded_size ||
        ring_free_space(&g_state.tx_frame_end) < encoded_size) {
      break;
    }
    ring_push(&g_state.tx, &value, 1);
    const uint8_t notEnd = 0;
    const uint8_t frameEnd = 1;
    if (encoded_size == 2) {
      ring_push(&g_state.tx_frame_end, &notEnd, 1);
      ring_push(&g_state.tx, &value, 1);
      ring_push(&g_state.tx_frame_end, &frameEnd, 1);
    } else {
      ring_push(&g_state.tx_frame_end, &frameEnd, 1);
    }
    accepted++;
  }
  if (accepted > 0) {
    pthread_cond_signal(&g_state.tx_cond);
  }
  pthread_mutex_unlock(&g_state.lock);
  return accepted > 0 ? (ssize_t)accepted : -EAGAIN;
}

static int send_remote_control_state(
    unsigned int new_bits,
    unsigned int changed_mask,
    bool force)
{
  const bool next_dtr = (new_bits & TIOCM_DTR) != 0;
  const bool next_rts = (new_bits & TIOCM_RTS) != 0;
  pthread_mutex_lock(&g_state.lock);
  const bool old_dtr = g_state.remote_dtr;
  const bool old_rts = g_state.remote_rts;
  pthread_mutex_unlock(&g_state.lock);

  const bool change_dtr = force ||
      (((changed_mask & TIOCM_DTR) != 0) && old_dtr != next_dtr);
  const bool change_rts = force ||
      (((changed_mask & TIOCM_RTS) != 0) && old_rts != next_rts);
  if (!change_dtr && !change_rts) {
    return 0;
  }

  /*
   * Preserve esptool reset intent over the network: assert RTS before DTR
   * when entering reset, but set DTR before releasing RTS when leaving reset.
   */
  uint8_t buffer[32];
  uint8_t expected[4];
  size_t expected_count = 0;
  size_t out = 0;
  int rc = 0;
#define APPEND_CONTROL(value) \
  do { \
    const uint8_t control_value = (value); \
    expected[expected_count++] = control_value; \
    rc = append_rfc2217_suboption( \
        buffer, sizeof(buffer), &out, RFC_SET_CONTROL, &control_value, 1); \
    if (rc < 0) { \
      return rc; \
    } \
  } while (0)

  if (change_rts && old_rts && !next_rts) {
    if (change_dtr) {
      APPEND_CONTROL(next_dtr ? RFC_CONTROL_DTR_ON : RFC_CONTROL_DTR_OFF);
    }
    APPEND_CONTROL(RFC_CONTROL_RTS_OFF);
  } else {
    if (change_rts) {
      APPEND_CONTROL(next_rts ? RFC_CONTROL_RTS_ON : RFC_CONTROL_RTS_OFF);
    }
    if (change_dtr) {
      APPEND_CONTROL(next_dtr ? RFC_CONTROL_DTR_ON : RFC_CONTROL_DTR_OFF);
    }
  }
#undef APPEND_CONTROL

  pthread_mutex_lock(&g_state.lock);
  if (!ack_transaction_complete_locked()) {
    pthread_mutex_unlock(&g_state.lock);
    return -EBUSY;
  }
  memcpy(g_state.expected_control_acks, expected, expected_count);
  g_state.expected_control_ack_count = expected_count;
  g_state.expected_control_ack_index = 0;
  g_state.ack_failed = false;
  pthread_mutex_unlock(&g_state.lock);

  rc = send_all_locked(buffer, out);
  if (rc < 0) {
    pthread_mutex_lock(&g_state.lock);
    clear_pending_acks_locked();
    pthread_mutex_unlock(&g_state.lock);
    return rc;
  }
  rc = wait_for_pending_acks(remote_uart_operation_timeout_ms());
  if (rc < 0) {
    return rc;
  }

  pthread_mutex_lock(&g_state.lock);
  if (change_dtr) {
    g_state.remote_dtr = next_dtr;
  }
  if (change_rts) {
    g_state.remote_rts = next_rts;
  }
  const bool remote_dtr = g_state.remote_dtr;
  const bool remote_rts = g_state.remote_rts;
  pthread_mutex_unlock(&g_state.lock);
  log_debug("modem: DTR=%u RTS=%u%s\n", remote_dtr, remote_rts, force ? " force" : "");
  return 0;
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

static void process_rfc2217_suboption(void)
{
  if (g_state.telnet_suboption_length < 2 ||
      g_state.telnet_suboption[0] != TELNET_OPT_COM_PORT) {
    return;
  }

  const uint8_t option = g_state.telnet_suboption[1];
  const uint8_t *payload = &g_state.telnet_suboption[2];
  const size_t payload_size = g_state.telnet_suboption_length - 2;
  bool handled = false;

  pthread_mutex_lock(&g_state.lock);
  switch (option) {
    case RFC_SERVER_SET_BAUDRATE:
      if ((g_state.pending_serial_acks & ACK_BAUD) != 0 && payload_size >= 4) {
        const unsigned int baud = ((unsigned int)payload[0] << 24) |
                                  ((unsigned int)payload[1] << 16) |
                                  ((unsigned int)payload[2] << 8) |
                                  (unsigned int)payload[3];
        g_state.ack_failed |= baud != g_state.expected_serial.baud;
        g_state.pending_serial_acks &= ~ACK_BAUD;
        handled = true;
      }
      break;
    case RFC_SERVER_SET_DATASIZE:
      if ((g_state.pending_serial_acks & ACK_DATA) != 0 && payload_size >= 1) {
        g_state.ack_failed |= payload[0] != g_state.expected_serial.data_bits;
        g_state.pending_serial_acks &= ~ACK_DATA;
        handled = true;
      }
      break;
    case RFC_SERVER_SET_PARITY:
      if ((g_state.pending_serial_acks & ACK_PARITY) != 0 && payload_size >= 1) {
        g_state.ack_failed |= payload[0] != g_state.expected_serial.parity_code;
        g_state.pending_serial_acks &= ~ACK_PARITY;
        handled = true;
      }
      break;
    case RFC_SERVER_SET_STOPSIZE:
      if ((g_state.pending_serial_acks & ACK_STOP) != 0 && payload_size >= 1) {
        g_state.ack_failed |= payload[0] != g_state.expected_serial.stop_code;
        g_state.pending_serial_acks &= ~ACK_STOP;
        handled = true;
      }
      break;
    case RFC_SERVER_SET_CONTROL:
      if (g_state.expected_control_ack_index < g_state.expected_control_ack_count &&
          payload_size >= 1) {
        g_state.ack_failed |=
            payload[0] != g_state.expected_control_acks[g_state.expected_control_ack_index];
        g_state.expected_control_ack_index++;
        handled = true;
      }
      break;
    case RFC_SERVER_PURGE_DATA:
      if (g_state.pending_purge_ack && payload_size >= 1) {
        g_state.ack_failed |= payload[0] != g_state.expected_purge_ack;
        g_state.pending_purge_ack = false;
        handled = true;
      }
      break;
    default:
      break;
  }
  if (handled) {
    pthread_cond_broadcast(&g_state.ack_cond);
  }
  pthread_mutex_unlock(&g_state.lock);
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
        g_state.telnet_suboption_length = 0;
        g_state.telnet_state = TELNET_SUBOPTION;
      } else {
        g_state.telnet_state = TELNET_NORMAL;
      }
      break;
    case TELNET_NEGOTIATE:
      {
      const uint8_t bit = telnet_option_bit(value);
      if (g_state.telnet_command == TELNET_DO) {
        if (!is_supported_telnet_option(value) || bit == 0) {
          send_telnet_option(TELNET_WONT, value);
        } else {
          const bool requested = (g_state.local_option_requested & bit) != 0;
          g_state.local_option_requested &= (uint8_t)~bit;
          if ((g_state.local_option_enabled & bit) == 0) {
            g_state.local_option_enabled |= bit;
            if (!requested) {
              send_telnet_option(TELNET_WILL, value);
            }
          }
        }
      } else if (g_state.telnet_command == TELNET_DONT) {
        if (bit != 0 &&
            ((g_state.local_option_enabled | g_state.local_option_requested) & bit) != 0) {
          g_state.local_option_enabled &= (uint8_t)~bit;
          g_state.local_option_requested &= (uint8_t)~bit;
          send_telnet_option(TELNET_WONT, value);
        }
      } else if (g_state.telnet_command == TELNET_WILL) {
        if (!is_supported_telnet_option(value) || bit == 0) {
          send_telnet_option(TELNET_DONT, value);
        } else {
          const bool requested = (g_state.remote_option_requested & bit) != 0;
          g_state.remote_option_requested &= (uint8_t)~bit;
          if ((g_state.remote_option_enabled & bit) == 0) {
            g_state.remote_option_enabled |= bit;
            if (!requested) {
              send_telnet_option(TELNET_DO, value);
            }
          }
        }
      } else if (g_state.telnet_command == TELNET_WONT) {
        if (bit != 0 &&
            ((g_state.remote_option_enabled | g_state.remote_option_requested) & bit) != 0) {
          g_state.remote_option_enabled &= (uint8_t)~bit;
          g_state.remote_option_requested &= (uint8_t)~bit;
          send_telnet_option(TELNET_DONT, value);
        }
      }
      g_state.telnet_state = TELNET_NORMAL;
      break;
      }
    case TELNET_SUBOPTION:
      if (value == TELNET_IAC) {
        g_state.telnet_state = TELNET_SUBOPTION_IAC;
      } else if (g_state.telnet_suboption_length < sizeof(g_state.telnet_suboption)) {
        g_state.telnet_suboption[g_state.telnet_suboption_length++] = value;
      }
      break;
    case TELNET_SUBOPTION_IAC:
      if (value == TELNET_SE) {
        process_rfc2217_suboption();
        g_state.telnet_state = TELNET_NORMAL;
      } else if (value == TELNET_IAC) {
        if (g_state.telnet_suboption_length < sizeof(g_state.telnet_suboption)) {
          g_state.telnet_suboption[g_state.telnet_suboption_length++] = value;
        }
        g_state.telnet_state = TELNET_SUBOPTION;
      } else {
        g_state.telnet_state = TELNET_SUBOPTION;
      }
      break;
  }
}

static void mark_remote_disconnected(int fd)
{
  struct poll_waiter *waiters = NULL;
  bool owned = false;
  int controlFd = -1;
  pthread_mutex_lock(&g_state.lock);
  if (g_state.sock == fd) {
    owned = true;
    controlFd = g_state.control_sock;
    g_state.control_sock = -1;
    g_state.connected = false;
    waiters = g_state.poll_waiters;
    g_state.poll_waiters = NULL;
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_cond_broadcast(&g_state.ack_cond);
  }
  pthread_mutex_unlock(&g_state.lock);
  if (owned) {
    shutdown(fd, SHUT_RDWR);
    if (controlFd >= 0) {
      shutdown(controlFd, SHUT_RDWR);
      close(controlFd);
    }
  }
  notify_poll_waiters(waiters);
}

static void *writer_thread_main(void *arg)
{
  (void)arg;
  uint8_t buffer[1024];
  uint8_t frameEnds[1024];

  for (;;) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.connected &&
           (g_state.tx.size == 0 ||
            (g_state.flush_pending && g_state.tx_at_frame_boundary))) {
      pthread_cond_wait(&g_state.tx_cond, &g_state.lock);
    }
    if (!g_state.connected || g_state.sock < 0) {
      pthread_mutex_unlock(&g_state.lock);
      break;
    }
    const int fd = g_state.sock;
    size_t size = ring_peek(&g_state.tx, buffer, sizeof(buffer));
    ring_peek(&g_state.tx_frame_end, frameEnds, size);
    if (g_state.flush_pending && !g_state.tx_at_frame_boundary) {
      for (size_t i = 0; i < size; ++i) {
        if (frameEnds[i] != 0) {
          size = i + 1;
          break;
        }
      }
    }
    const uint64_t generation = g_state.tx_generation;
    g_state.writer_sending = true;
    pthread_mutex_unlock(&g_state.lock);

    const ssize_t rc = send(fd, buffer, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (rc > 0) {
      struct poll_waiter *waiters = NULL;
      pthread_mutex_lock(&g_state.lock);
      g_state.writer_sending = false;
      if (g_state.tx_generation == generation) {
        g_state.tx_at_frame_boundary = frameEnds[rc - 1] != 0;
        ring_discard(&g_state.tx, (size_t)rc);
        ring_discard(&g_state.tx_frame_end, (size_t)rc);
      }
      pthread_cond_broadcast(&g_state.tx_cond);
      waiters = g_state.poll_waiters;
      g_state.poll_waiters = NULL;
      pthread_mutex_unlock(&g_state.lock);
      notify_poll_waiters(waiters);
      continue;
    }
    if (rc < 0 && errno == EINTR) {
      pthread_mutex_lock(&g_state.lock);
      g_state.writer_sending = false;
      pthread_cond_broadcast(&g_state.tx_cond);
      pthread_mutex_unlock(&g_state.lock);
      continue;
    }
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pthread_mutex_lock(&g_state.lock);
      g_state.writer_sending = false;
      pthread_cond_broadcast(&g_state.tx_cond);
      pthread_mutex_unlock(&g_state.lock);
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      const int poll_rc = poll(&pfd, 1, 1000);
      if (poll_rc >= 0 || errno == EINTR) {
        continue;
      }
    }
    pthread_mutex_lock(&g_state.lock);
    g_state.writer_sending = false;
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_mutex_unlock(&g_state.lock);
    mark_remote_disconnected(fd);
    break;
  }

  return NULL;
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
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      const int pollRc = poll(&pfd, 1, 1000);
      if (pollRc >= 0 || errno == EINTR) {
        continue;
      }
    }
    break;
  }

  pthread_mutex_lock(&g_state.lock);
  const int fd = g_state.sock;
  pthread_mutex_unlock(&g_state.lock);
  if (fd >= 0) {
    mark_remote_disconnected(fd);
  }
  log_debug("remote connection closed\n");
  return NULL;
}

static int control_socket_transfer(
    int fd,
    uint8_t *buffer,
    size_t size,
    bool writeData,
    unsigned int timeoutMs)
{
  size_t offset = 0;
  const uint64_t deadline = monotonic_ms() + timeoutMs;
  while (offset < size) {
    const ssize_t rc = writeData
        ? send(fd, buffer + offset, size - offset, MSG_NOSIGNAL)
        : recv(fd, buffer + offset, size - offset, 0);
    if (rc > 0) {
      offset += (size_t)rc;
      continue;
    }
    if (rc == 0) {
      return -ENOTCONN;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return -errno;
    }
    const uint64_t now = monotonic_ms();
    if (now >= deadline) {
      return -ETIMEDOUT;
    }
    struct pollfd pfd = {
        .fd = fd,
        .events = writeData ? POLLOUT : POLLIN,
    };
    const int waitMs = (int)(deadline - now > 1000 ? 1000 : deadline - now);
    if (poll(&pfd, 1, waitMs) < 0 && errno != EINTR) {
      return -errno;
    }
  }
  return 0;
}

static int wait_flush_control_ack(
    int fd,
    uint8_t phase,
    uint32_t token,
    unsigned int timeoutMs)
{
  uint8_t ack[10];
  int rc = control_socket_transfer(fd, ack, sizeof(ack), false, timeoutMs);
  if (rc < 0) {
    return rc;
  }
  const uint32_t ackToken = ((uint32_t)ack[6] << 24) |
      ((uint32_t)ack[7] << 16) |
      ((uint32_t)ack[8] << 8) |
      (uint32_t)ack[9];
  if (memcmp(ack, "WUA1", 4) != 0 || ack[4] != phase || ack[5] != 0 ||
      ackToken != token) {
    return -EPROTO;
  }
  return 0;
}

static int begin_remote_output_flush(int fd, uint8_t command, uint32_t token)
{
  uint8_t request[9] = {
      'W', 'U', 'F', '1',
      command,
      (uint8_t)(token >> 24),
      (uint8_t)(token >> 16),
      (uint8_t)(token >> 8),
      (uint8_t)token,
  };
  int rc = control_socket_transfer(fd, request, sizeof(request), true, 5000);
  return rc < 0 ? rc : wait_flush_control_ack(fd, 1, token, 5000);
}

static unsigned int flush_resume_timeout_ms(int dataFd)
{
  int socketQueued = 0;
  if (dataFd >= 0) {
    ioctl(dataFd, TIOCOUTQ, &socketQueued);
  }
  const uint64_t bytes = TX_BUFFER_SIZE +
      (uint64_t)(socketQueued > 0 ? socketQueued : 0);
  const uint64_t timeout = (bytes * 1000ULL + 4095ULL) / 4096ULL + 5000ULL;
  return timeout >= UINT_MAX ? UINT_MAX - 1U : (unsigned int)timeout;
}

static int flush_buffers(int queue)
{
  if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH) {
    return -EINVAL;
  }

  if (queue == TCIFLUSH) {
    const uint8_t purge = RFC_PURGE_RX;
    uint8_t frame[16];
    size_t frameSize = 0;
    int rc = append_rfc2217_suboption(
        frame, sizeof(frame), &frameSize, RFC_PURGE_DATA, &purge, 1);
    if (rc < 0) {
      return rc;
    }
    pthread_mutex_lock(&g_state.lock);
    if (!ack_transaction_complete_locked()) {
      pthread_mutex_unlock(&g_state.lock);
      return -EBUSY;
    }
    rc = queue_tx_locked(frame, frameSize);
    if (rc == 0) {
      ring_clear(&g_state.rx);
      g_state.pending_purge_ack = true;
      g_state.expected_purge_ack = purge;
      g_state.ack_failed = false;
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc < 0 ? rc : wait_for_pending_acks(remote_uart_operation_timeout_ms());
  }

  struct timespec frameDeadline;
  if (clock_gettime(CLOCK_REALTIME, &frameDeadline) != 0) {
    return -errno;
  }
  frameDeadline.tv_sec += 5;
  pthread_mutex_lock(&g_state.lock);
  if (!ack_transaction_complete_locked()) {
    pthread_mutex_unlock(&g_state.lock);
    return -EBUSY;
  }
  g_state.flush_pending = true;
  pthread_cond_broadcast(&g_state.tx_cond);
  while ((g_state.writer_sending || !g_state.tx_at_frame_boundary) &&
         g_state.connected) {
    const int waitRc = pthread_cond_timedwait(
        &g_state.tx_cond, &g_state.lock, &frameDeadline);
    if (waitRc != 0) {
      g_state.flush_pending = false;
      pthread_cond_broadcast(&g_state.tx_cond);
      pthread_mutex_unlock(&g_state.lock);
      return waitRc == ETIMEDOUT ? -ETIMEDOUT : -waitRc;
    }
  }
  if (!g_state.connected || g_state.control_sock < 0) {
    g_state.flush_pending = false;
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_mutex_unlock(&g_state.lock);
    return -ENOTCONN;
  }
  const int dataFd = g_state.sock;
  const int controlFd = g_state.control_sock;
  uint32_t token = ++g_state.next_flush_token;
  if (token == 0) {
    token = ++g_state.next_flush_token;
  }
  pthread_mutex_unlock(&g_state.lock);

  const uint8_t command = queue == TCIOFLUSH ? 2 : 1;
  int rc = begin_remote_output_flush(controlFd, command, token);
  if (rc < 0) {
    pthread_mutex_lock(&g_state.lock);
    g_state.flush_pending = false;
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_mutex_unlock(&g_state.lock);
    mark_remote_disconnected(dataFd);
    return rc;
  }

  uint8_t tokenBytes[4] = {
      (uint8_t)(token >> 24),
      (uint8_t)(token >> 16),
      (uint8_t)(token >> 8),
      (uint8_t)token,
  };
  uint8_t resumeFrame[16];
  size_t resumeFrameSize = 0;
  rc = append_rfc2217_suboption(
      resumeFrame,
      sizeof(resumeFrame),
      &resumeFrameSize,
      RFC_RESUME_AFTER_FLUSH,
      tokenBytes,
      sizeof(tokenBytes));

  struct poll_waiter *waiters = NULL;
  pthread_mutex_lock(&g_state.lock);
  if (rc == 0 && g_state.connected && g_state.sock == dataFd) {
    ring_clear(&g_state.tx);
    ring_clear(&g_state.tx_frame_end);
    g_state.tx_generation++;
    g_state.tx_at_frame_boundary = true;
    rc = queue_tx_locked(resumeFrame, resumeFrameSize);
    if (queue == TCIOFLUSH) {
      ring_clear(&g_state.rx);
    }
    waiters = g_state.poll_waiters;
    g_state.poll_waiters = NULL;
  } else if (rc == 0) {
    rc = -ENOTCONN;
  }
  g_state.flush_pending = false;
  pthread_cond_broadcast(&g_state.tx_cond);
  pthread_mutex_unlock(&g_state.lock);
  notify_poll_waiters(waiters);
  if (rc < 0) {
    mark_remote_disconnected(dataFd);
    return rc;
  }
  rc = wait_flush_control_ack(
      controlFd,
      2,
      token,
      flush_resume_timeout_ms(dataFd));
  if (rc < 0) {
    mark_remote_disconnected(dataFd);
  }
  return rc;
}

static int wait_tx_empty(unsigned int timeout_ms)
{
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return -errno;
  }
  deadline.tv_sec += timeout_ms / 1000U;
  deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&g_state.lock);
  while (g_state.connected && g_state.tx.size > 0) {
    const int rc = pthread_cond_timedwait(&g_state.tx_cond, &g_state.lock, &deadline);
    if (rc == ETIMEDOUT) {
      pthread_mutex_unlock(&g_state.lock);
      return -ETIMEDOUT;
    }
    if (rc != 0) {
      pthread_mutex_unlock(&g_state.lock);
      return -rc;
    }
  }
  const bool connected = g_state.connected;
  pthread_mutex_unlock(&g_state.lock);
  return connected ? 0 : -ENOTCONN;
}

static int drain_remote_output(void)
{
  const unsigned int timeout_ms = remote_uart_operation_timeout_ms();
  int rc = wait_tx_empty(timeout_ms);
  if (rc < 0) {
    return rc;
  }
  pthread_mutex_lock(&g_state.lock);
  struct serial_settings settings = settings_from_termios(&g_state.tio);
  settings.baud = g_state.baud_rate;
  pthread_mutex_unlock(&g_state.lock);
  return send_serial_settings(settings);
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

static int apply_termios(const struct termios *next)
{
  struct serial_settings settings = settings_from_termios(next);
  const int rc = send_serial_settings(settings);
  if (rc < 0) {
    return rc;
  }
  pthread_mutex_lock(&g_state.lock);
  g_state.tio = *next;
  fill_termios2_from_termios(settings.baud);
  g_state.baud_rate = settings.baud;
  pthread_mutex_unlock(&g_state.lock);
  return 0;
}

static int apply_termios2(const struct termios2_wire *next)
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
  pthread_mutex_unlock(&g_state.lock);

  struct serial_settings settings = settings_from_termios(&tio);
  settings.baud = baud;
  const int rc = send_serial_settings(settings);
  if (rc < 0) {
    return rc;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.tio = tio;
  g_state.tio2 = *next;
  g_state.tio2.c_ispeed = baud;
  g_state.tio2.c_ospeed = baud;
  g_state.baud_rate = baud;
  pthread_mutex_unlock(&g_state.lock);
  return 0;
}

static int handle_modem_update(unsigned int new_bits, unsigned int changed_mask, bool from_set)
{
  pthread_mutex_lock(&g_state.lock);
  bool should_send = from_set || !g_state.suppress_open_lines;
  if (!should_send && monotonic_ms() <= g_state.suppress_lines_until_ms) {
    const unsigned int line_mask = changed_mask & (TIOCM_DTR | TIOCM_RTS);
    const unsigned int repeated_lines = line_mask & g_state.suppressed_initial_lines;
    g_state.suppressed_initial_lines |= line_mask;
    should_send = repeated_lines != 0;
  } else if (!should_send) {
    should_send = true;
  }
  pthread_mutex_unlock(&g_state.lock);

  if (should_send) {
    const int rc = send_remote_control_state(new_bits, changed_mask, from_set);
    if (rc < 0) {
      return rc;
    }
  } else {
    log_debug("modem: suppressed open-time DTR/RTS update bits=0x%x\n", new_bits);
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.modem_bits = (g_state.modem_bits & ~(TIOCM_DTR | TIOCM_RTS)) |
                       (new_bits & (TIOCM_DTR | TIOCM_RTS));
  pthread_mutex_unlock(&g_state.lock);
  return 0;
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
  g_state.writer_sending = false;
  g_state.tx_at_frame_boundary = true;
  g_state.flush_pending = false;
  g_state.suppressed_initial_lines = 0;
  g_state.remote_dtr = false;
  g_state.remote_rts = false;
  g_state.modem_bits = 0;
  g_state.telnet_state = TELNET_NORMAL;
  g_state.local_option_enabled = 0;
  g_state.local_option_requested = 0;
  g_state.remote_option_enabled = 0;
  g_state.remote_option_requested = 0;
  ring_clear(&g_state.rx);
  ring_clear(&g_state.tx);
  ring_clear(&g_state.tx_frame_end);
  g_state.tx_generation++;
  pthread_mutex_unlock(&g_state.lock);

  const int fd = connect_tcp(g_state.host, g_state.port);
  if (fd < 0) {
    pthread_mutex_lock(&g_state.lock);
    g_state.open = false;
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, ECONNREFUSED);
    return;
  }
  const int controlFd = connect_tcp(g_state.host, g_state.control_port);
  if (controlFd < 0) {
    close(fd);
    pthread_mutex_lock(&g_state.lock);
    g_state.open = false;
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, ECONNREFUSED);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.sock = fd;
  g_state.control_sock = controlFd;
  g_state.connected = true;
  struct serial_settings settings = settings_from_termios(&g_state.tio);
  settings.baud = g_state.baud_rate;
  pthread_mutex_unlock(&g_state.lock);

  send_initial_negotiation();

  if (pthread_create(&g_state.reader_thread, NULL, reader_thread_main, NULL) != 0) {
    close(fd);
    close(controlFd);
    pthread_mutex_lock(&g_state.lock);
    g_state.sock = -1;
    g_state.control_sock = -1;
    g_state.connected = false;
    g_state.open = false;
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, EIO);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.reader_started = true;
  pthread_mutex_unlock(&g_state.lock);

  if (pthread_create(&g_state.writer_thread, NULL, writer_thread_main, NULL) != 0) {
    pthread_mutex_lock(&g_state.lock);
    g_state.connected = false;
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_cond_broadcast(&g_state.ack_cond);
    pthread_mutex_unlock(&g_state.lock);
    shutdown(fd, SHUT_RDWR);
    shutdown(controlFd, SHUT_RDWR);
    pthread_join(g_state.reader_thread, NULL);
    close(fd);
    close(controlFd);
    pthread_mutex_lock(&g_state.lock);
    g_state.sock = -1;
    g_state.control_sock = -1;
    g_state.reader_started = false;
    g_state.open = false;
    pthread_mutex_unlock(&g_state.lock);
    fuse_reply_err(req, EIO);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.writer_started = true;
  pthread_mutex_unlock(&g_state.lock);

  const int settings_rc = send_serial_settings(settings);
  if (settings_rc < 0) {
    pthread_mutex_lock(&g_state.lock);
    g_state.connected = false;
    g_state.open = false;
    g_state.sock = -1;
    g_state.control_sock = -1;
    g_state.reader_started = false;
    g_state.writer_started = false;
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_cond_broadcast(&g_state.ack_cond);
    pthread_mutex_unlock(&g_state.lock);
    shutdown(fd, SHUT_RDWR);
    shutdown(controlFd, SHUT_RDWR);
    pthread_join(g_state.reader_thread, NULL);
    pthread_join(g_state.writer_thread, NULL);
    close(fd);
    close(controlFd);
    fuse_reply_err(req, -settings_rc);
    return;
  }

  pthread_mutex_lock(&g_state.lock);
  g_state.suppress_lines_until_ms = monotonic_ms() + 250ULL;
  pthread_mutex_unlock(&g_state.lock);
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
  const ssize_t rc = send_data_bytes(buf, size);
  if (rc < 0) {
    fuse_reply_err(req, (int)-rc);
    return;
  }
  fuse_reply_write(req, (size_t)rc);
}

static void cuse_tty_flush(fuse_req_t req, struct fuse_file_info *fi)
{
  (void)fi;
  const int rc = drain_remote_output();
  fuse_reply_err(req, rc < 0 ? -rc : 0);
}

static void cuse_tty_release(fuse_req_t req, struct fuse_file_info *fi)
{
  (void)fi;
  pthread_t thread;
  pthread_t writer_thread;
  bool join_reader = false;
  bool join_writer = false;
  int fd = -1;
  int controlFd = -1;
  struct poll_waiter *waiters = NULL;
  const int drain_rc = drain_remote_output();

  pthread_mutex_lock(&g_state.lock);
  if (g_state.open) {
    fd = g_state.sock;
    controlFd = g_state.control_sock;
    g_state.sock = -1;
    g_state.control_sock = -1;
    g_state.connected = false;
    g_state.open = false;
    ring_clear(&g_state.rx);
    ring_clear(&g_state.tx);
    ring_clear(&g_state.tx_frame_end);
    waiters = g_state.poll_waiters;
    g_state.poll_waiters = NULL;
    if (g_state.reader_started) {
      thread = g_state.reader_thread;
      join_reader = true;
      g_state.reader_started = false;
    }
    if (g_state.writer_started) {
      writer_thread = g_state.writer_thread;
      join_writer = true;
      g_state.writer_started = false;
    }
    pthread_cond_broadcast(&g_state.tx_cond);
    pthread_cond_broadcast(&g_state.ack_cond);
  }
  pthread_mutex_unlock(&g_state.lock);

  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
  if (controlFd >= 0) {
    shutdown(controlFd, SHUT_RDWR);
    close(controlFd);
  }
  if (join_reader) {
    pthread_join(thread, NULL);
  }
  if (join_writer) {
    pthread_join(writer_thread, NULL);
  }
  notify_poll_waiters(waiters);
  fuse_reply_err(req, drain_rc < 0 ? -drain_rc : 0);
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
      int rc = 0;
      if (cmd == TCSETSW || cmd == TCSETSF) {
        rc = wait_tx_empty(remote_uart_operation_timeout_ms());
      }
      if (rc == 0 && cmd == TCSETSF) {
        rc = flush_buffers(TCIOFLUSH);
      }
      if (rc == 0) {
        rc = apply_termios((const struct termios *)in_buf);
      }
      if (rc < 0) {
        fuse_reply_err(req, -rc);
      } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
      }
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
      int rc = 0;
      if (cmd == TCSETSW2 || cmd == TCSETSF2) {
        rc = wait_tx_empty(remote_uart_operation_timeout_ms());
      }
      if (rc == 0 && cmd == TCSETSF2) {
        rc = flush_buffers(TCIOFLUSH);
      }
      if (rc == 0) {
        rc = apply_termios2((const struct termios2_wire *)in_buf);
      }
      if (rc < 0) {
        fuse_reply_err(req, -rc);
      } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
      }
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
      const int rc = handle_modem_update(
          bits,
          TIOCM_DTR | TIOCM_RTS,
          true);
      if (rc < 0) {
        fuse_reply_err(req, -rc);
      } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
      }
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
      const int rc = handle_modem_update(bits, mask, false);
      if (rc < 0) {
        fuse_reply_err(req, -rc);
      } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
      }
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
      pthread_mutex_lock(&g_state.lock);
      const size_t localQueued = g_state.tx.size;
      const int fd = g_state.sock;
      pthread_mutex_unlock(&g_state.lock);
      int socketQueued = 0;
      if (fd >= 0) {
        ioctl(fd, TIOCOUTQ, &socketQueued);
      }
      const uint64_t totalQueued = localQueued +
          (uint64_t)(socketQueued > 0 ? socketQueued : 0);
      const unsigned int queued = totalQueued > UINT_MAX
          ? UINT_MAX
          : (unsigned int)totalQueued;
      reply_ioctl_read(req, arg, &queued, sizeof(queued), out_bufsz);
      break;
    }
    case TCFLSH: {
      const int rc = flush_buffers((int)(uintptr_t)arg);
      if (rc < 0) {
        fuse_reply_err(req, -rc);
      } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
      }
      break;
    }
    case TCSBRK:
    case TIOCSBRK:
    case TIOCCBRK:
      fuse_reply_err(req, EOPNOTSUPP);
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
  if (g_state.rx.size > 0) {
    revents |= POLLIN;
  }
  if (g_state.connected) {
    if (ring_free_space(&g_state.tx) >= 2) {
      revents |= POLLOUT;
    }
    if (ph && (g_state.rx.size == 0 || ring_free_space(&g_state.tx) < 2)) {
      store_waiter = true;
    }
  } else {
    revents |= POLLHUP | POLLERR;
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
          "  --control-port <port>  Flush control port (default: %s)\n"
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
          DEFAULT_FLUSH_CONTROL_PORT,
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

#ifndef WIFIUART_TTY_TEST
int main(int argc, char **argv)
{
  memset(&g_state, 0, sizeof(g_state));
  snprintf(g_state.port, sizeof(g_state.port), "%s", DEFAULT_RFC2217_PORT);
  snprintf(
      g_state.control_port,
      sizeof(g_state.control_port),
      "%s",
      DEFAULT_FLUSH_CONTROL_PORT);
  snprintf(g_state.dev_name, sizeof(g_state.dev_name), "%s", DEFAULT_DEVICE_NAME);
  g_state.dev_mode = 0666;
  g_state.suppress_open_lines = true;
  g_state.sock = -1;
  g_state.control_sock = -1;
  pthread_mutex_init(&g_state.lock, NULL);
  pthread_cond_init(&g_state.tx_cond, NULL);
  pthread_cond_init(&g_state.ack_cond, NULL);
  init_termios_state();
  if (!ring_init(&g_state.rx, RX_BUFFER_SIZE)) {
    fprintf(stderr, "failed to allocate RX buffer\n");
    return 1;
  }
  if (!ring_init(&g_state.tx, TX_BUFFER_SIZE)) {
    fprintf(stderr, "failed to allocate TX buffer\n");
    ring_free(&g_state.rx);
    return 1;
  }
  if (!ring_init(&g_state.tx_frame_end, TX_BUFFER_SIZE)) {
    fprintf(stderr, "failed to allocate TX frame buffer\n");
    ring_free(&g_state.rx);
    ring_free(&g_state.tx);
    return 1;
  }
  g_state.tx_at_frame_boundary = true;

  struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
  fuse_opt_add_arg(&args, argv[0]);
  fuse_opt_add_arg(&args, "-s");

  for (int i = 1; i < argc; ++i) {
    const char *value = NULL;
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      ring_free(&g_state.rx);
      ring_free(&g_state.tx);
      ring_free(&g_state.tx_frame_end);
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
    } else if (strcmp(argv[i], "--control-port") == 0 && i + 1 < argc) {
      snprintf(g_state.control_port, sizeof(g_state.control_port), "%s", argv[++i]);
    } else if (parse_option_value(argv[i], "--control-port", &value)) {
      snprintf(g_state.control_port, sizeof(g_state.control_port), "%s", value);
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
    ring_free(&g_state.tx);
    ring_free(&g_state.tx_frame_end);
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
  ring_free(&g_state.tx);
  ring_free(&g_state.tx_frame_end);
  fuse_opt_free_args(&args);
  pthread_mutex_destroy(&g_state.lock);
  pthread_cond_destroy(&g_state.tx_cond);
  pthread_cond_destroy(&g_state.ack_cond);
  return rc;
}
#else
#include <assert.h>

static void reset_test_state(size_t tx_capacity)
{
  memset(&g_state, 0, sizeof(g_state));
  pthread_mutex_init(&g_state.lock, NULL);
  pthread_cond_init(&g_state.tx_cond, NULL);
  pthread_cond_init(&g_state.ack_cond, NULL);
  assert(ring_init(&g_state.rx, 32));
  assert(ring_init(&g_state.tx, tx_capacity));
  assert(ring_init(&g_state.tx_frame_end, tx_capacity));
  g_state.tx_at_frame_boundary = true;
  g_state.sock = 1;
  g_state.control_sock = -1;
  g_state.connected = true;
  g_state.telnet_state = TELNET_NORMAL;
}

static void free_test_state(void)
{
  ring_free(&g_state.rx);
  ring_free(&g_state.tx);
  ring_free(&g_state.tx_frame_end);
  pthread_cond_destroy(&g_state.tx_cond);
  pthread_cond_destroy(&g_state.ack_cond);
  pthread_mutex_destroy(&g_state.lock);
}

static void receive_negotiation(uint8_t command, uint8_t option)
{
  process_telnet_byte(TELNET_IAC);
  process_telnet_byte(command);
  process_telnet_byte(option);
}

int main(void)
{
  reset_test_state(32);
  const uint8_t binary_bit = telnet_option_bit(TELNET_OPT_BINARY);
  g_state.local_option_requested = binary_bit;
  receive_negotiation(TELNET_DO, TELNET_OPT_BINARY);
  assert(g_state.tx.size == 0);
  assert((g_state.local_option_enabled & binary_bit) != 0);
  receive_negotiation(TELNET_DO, TELNET_OPT_BINARY);
  assert(g_state.tx.size == 0);

  receive_negotiation(TELNET_DO, TELNET_OPT_SGA);
  assert(g_state.tx.size == 3);
  receive_negotiation(TELNET_DO, TELNET_OPT_SGA);
  assert(g_state.tx.size == 3);

  g_state.expected_serial.baud = 115200;
  g_state.pending_serial_acks = ACK_BAUD;
  const uint8_t baud_ack[] = {
      TELNET_IAC,
      TELNET_SB,
      TELNET_OPT_COM_PORT,
      RFC_SERVER_SET_BAUDRATE,
      0x00,
      0x01,
      0xC2,
      0x00,
      TELNET_IAC,
      TELNET_SE,
  };
  for (size_t i = 0; i < sizeof(baud_ack); ++i) {
    process_telnet_byte(baud_ack[i]);
  }
  assert(g_state.pending_serial_acks == 0);
  assert(!g_state.ack_failed);

  g_state.expected_serial.baud = 9600;
  g_state.pending_serial_acks = ACK_BAUD;
  for (size_t i = 0; i < sizeof(baud_ack); ++i) {
    process_telnet_byte(baud_ack[i]);
  }
  assert(g_state.pending_serial_acks == 0);
  assert(g_state.ack_failed);

  g_state.baud_rate = 300;
  assert(remote_uart_operation_timeout_ms() >= 2792400U);
  free_test_state();

  reset_test_state(3);
  const char payload[] = {'A', (char)TELNET_IAC, 'B'};
  assert(send_data_bytes(payload, sizeof(payload)) == 2);
  uint8_t encoded[3] = {};
  assert(ring_pop(&g_state.tx, encoded, sizeof(encoded)) == sizeof(encoded));
  assert(encoded[0] == 'A');
  assert(encoded[1] == TELNET_IAC);
  assert(encoded[2] == TELNET_IAC);
  uint8_t frameEnds[3] = {};
  assert(ring_pop(&g_state.tx_frame_end, frameEnds, sizeof(frameEnds)) == sizeof(frameEnds));
  assert(frameEnds[0] == 1);
  assert(frameEnds[1] == 0);
  assert(frameEnds[2] == 1);
  free_test_state();

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  const uint32_t token = 0x12345678U;
  const uint8_t flushAck[] = {
      'W', 'U', 'A', '1', 2, 0, 0x12, 0x34, 0x56, 0x78,
  };
  assert(send(sockets[1], flushAck, sizeof(flushAck), 0) == (ssize_t)sizeof(flushAck));
  assert(wait_flush_control_ack(sockets[0], 2, token, 1000) == 0);
  close(sockets[0]);
  close(sockets[1]);

  puts("wifiuart-tty tests passed");
  return 0;
}
#endif
