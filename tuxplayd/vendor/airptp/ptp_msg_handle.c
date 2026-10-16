/*
MIT License

Copyright (c) 2026 OwnTone

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <inttypes.h>

#include "airptp_internal.h"
#include "ptp_definitions.h"
#include "daemon.h"

// Debugging
#define AIRPTP_LOG_RECEIVED 0
#define AIRPTP_LOG_SENT 0

#define NS_PER_SEC 1000000000LL
#define NS_TO_MS(ns) ((double)(ns) / 1e6)

// What a Mac announces (captured from macOS 26): an ordinary clock, which a
// HomePod's or Apple TV's own clock (priority1 248) outranks. The receivers
// therefore keep their grandmaster, and the sender follows it.
#define FOLLOW_PRIORITY1 250
#define FOLLOW_QUALITY 0xF821436A // class 248, accuracy 0x21, variance 0x436A
#define FOLLOW_PRIORITY2 239
#define FOLLOW_UTC_OFFSET 37
#define FOLLOW_TIME_SOURCE 0xA0 // internal oscillator

// Each offset estimate uses the fastest exchange in each direction over this
// window: a wireless hop only ever adds delay, so the minimum is the closest
// to the true path.
#define FOLLOW_WINDOW_NS (4 * NS_PER_SEC)
// Beyond this the published timeline is stepped instead of slewed
#define FOLLOW_STEP_NS (20 * 1000000LL)
// How long the lock outlives the last peer, for sessions that reconnect
#define PEERS_GONE_GRACE_NS (10 * NS_PER_SEC)
// A remaining error is worked off over this long, bending the rate by at most
// this much (200 ppm is inaudible)
#define FOLLOW_SLEW_SECS 5.0
#define FOLLOW_SLEW_MAX 200e-6
#define FOLLOW_RATE_MAX 500e-6
#define FOLLOW_STATS_NS (60 * NS_PER_SEC)
// A sample this much faster than the window's median is on another timescale
#define FOLLOW_OUTLIER_NS (NS_PER_SEC / 2)

// Forward tlv handlers
static int tlv_handle_org_subtype_generic(struct airptp_daemon *, const char *, struct ptp_tlv_org_subtype_map *, uint8_t *, size_t);
static int tlv_handle_org_subtype_message_internal(struct airptp_daemon *, const char *, struct ptp_tlv_org_subtype_map *, uint8_t *, size_t);
static int tlv_handle_org_subtype_peer_add(struct airptp_daemon *, const char *, struct ptp_tlv_org_subtype_map *, uint8_t *, size_t);
static int tlv_handle_org_subtype_peer_del(struct airptp_daemon *, const char *, struct ptp_tlv_org_subtype_map *, uint8_t *, size_t);

static struct ptp_tlv_org_subtype_map ptp_tlv_ieee_subtypes[] =
{
  { PTP_TLV_ORG_IEEE_FOLLOW_UP_INFO, { 0x00, 0x00, 0x01 }, "Follow_Up information TLV", tlv_handle_org_subtype_generic },
  { PTP_TLV_ORG_IEEE_MESSAGE_INTERNAL_REQUEST, { 0x00, 0x00, 0x02 }, "Message internal request TLV", tlv_handle_org_subtype_message_internal },
};

static struct ptp_tlv_org_subtype_map ptp_tlv_apple_subtypes[] =
{
  { PTP_TLV_ORG_APPLE_UNKNOWN1, { 0x00, 0x00, 0x01 }, "Unknown subtype 1", tlv_handle_org_subtype_generic },
  { PTP_TLV_ORG_APPLE_CLOCK_ID, { 0x00, 0x00, 0x04 }, "Clock ID TLV", tlv_handle_org_subtype_generic },
  { PTP_TLV_ORG_APPLE_UNKNOWN5, { 0x00, 0x00, 0x05 }, "Unknown subtype 5", tlv_handle_org_subtype_generic },
};

static struct ptp_tlv_org_subtype_map ptp_tlv_own_subtypes[] =
{
  { PTP_TLV_ORG_OWN_PEER_ADD, { 0x00, 0x00, 0x01 }, "Add peer", tlv_handle_org_subtype_peer_add },
  { PTP_TLV_ORG_OWN_PEER_DEL, { 0x00, 0x00, 0x02 }, "Remove peer", tlv_handle_org_subtype_peer_del },
};

static struct ptp_tlv_org_map ptp_tlv_orgs[] =
{
  { PTP_TLV_ORG_IEEE, { 0x00, 0x80, 0xc2 }, "IEEE 802.1 Chair", ptp_tlv_ieee_subtypes, ARRAY_SIZE(ptp_tlv_ieee_subtypes) },
  { PTP_TLV_ORG_APPLE, { 0x00, 0x0d, 0x93 }, "Apple, Inc", ptp_tlv_apple_subtypes, ARRAY_SIZE(ptp_tlv_apple_subtypes) },
  { PTP_TLV_ORG_OWN, { 0x99, 0x99, 0x99 }, "OwnTone Ltd", ptp_tlv_own_subtypes, ARRAY_SIZE(ptp_tlv_own_subtypes) },
};


/* --------------------------------- Helpers -------------------------------- */

static inline struct ptp_timestamp
ptp_timestamp_betoh(struct ptp_timestamp *in)
{
  struct ptp_timestamp out;

  out.seconds_hi = be16toh(in->seconds_hi);
  out.seconds_low = be32toh(in->seconds_low);
  out.nanoseconds = be32toh(in->nanoseconds);
  return out;
}

static inline struct ptp_timestamp
ptp_timestamp_htobe(struct ptp_timestamp *in)
{
  struct ptp_timestamp out;

  out.seconds_hi = htobe16(in->seconds_hi);
  out.seconds_low = htobe32(in->seconds_low);
  out.nanoseconds = htobe32(in->nanoseconds);
  return out;
}

static inline struct ptp_timestamp
current_time_get(void)
{
  struct timespec now;
  struct ptp_timestamp out;

  clock_gettime(CLOCK_MONOTONIC, &now);
  out.seconds_hi = ((uint64_t)now.tv_sec) >> 32;
  out.seconds_low = (uint32_t)now.tv_sec;
  out.nanoseconds = (uint32_t)now.tv_nsec;
  return out;
}

static void
port_id_htobe(uint8_t *out, uint8_t *in)
{
  uint64_t be64;
  uint64_t clock_id;

  memcpy(&clock_id, in, sizeof(clock_id));
  be64 = htobe64(clock_id);
  memcpy(out, &be64, sizeof(be64));
  memcpy(out + sizeof(be64), in + sizeof(be64), PTP_PORT_ID_SIZE - sizeof(be64)); // The last two bytes
}

static void
port_set(union utils_net_sockaddr *naddr, unsigned short port)
{
  if (naddr->sa.sa_family == AF_INET6)
    naddr->sin6.sin6_port = htons(port);
  else if (naddr->sa.sa_family == AF_INET)
    naddr->sin.sin_port = htons(port);
}

#if AIRPTP_LOG_RECEIVED
static void
log_received(const char *name, struct ptp_header *header, uint64_t clock_id, struct ptp_timestamp *ts)
{
  uint64_t tv_sec;
  uint32_t tv_nsec;

  tv_sec = ts->seconds_hi;
  tv_sec = (tv_sec << 32);
  tv_sec += ts->seconds_low;
  tv_nsec = ts->nanoseconds;

  int8_t logint = header->logMessageInterval;

  airptp_logmsg("Received %s from clock %" PRIx64 ", logint=%" PRIi8 " with timestamp %" PRIu64 ".%" PRIu32, name, clock_id, logint, tv_sec, tv_nsec);
}
#else
static void
log_received(const char *name, struct ptp_header *header, uint64_t clock_id, struct ptp_timestamp *ts)
{
  return;
}
#endif

#if AIRPTP_LOG_SENT
static void
log_sent(uint8_t *msg, uint16_t port)
{
  const char *name;
  struct ptp_timestamp bets = { 0 };
  struct ptp_timestamp ts;
  uint64_t tv_sec;
  uint32_t tv_nsec;
  uint64_t clock_id;
  uint64_t be64;

  switch(msg[0] & 0x0F)
    {
      case PTP_MSGTYPE_SYNC:
	name = "PTP_MSGTYPE_SYNC";
	bets = ((struct ptp_sync_message *)msg)->originTimestamp;
	break;
      case PTP_MSGTYPE_DELAY_REQ:
	name = "PTP_MSGTYPE_DELAY_REQ";
	bets = ((struct ptp_delay_req_message *)msg)->originTimestamp;
	break;
      case PTP_MSGTYPE_PDELAY_REQ:
	name = "PTP_MSGTYPE_PDELAY_REQ";
	bets = ((struct ptp_pdelay_req_message *)msg)->originTimestamp;
	break;
      case PTP_MSGTYPE_PDELAY_RESP:
	name = "PTP_MSGTYPE_PDELAY_RESP";
	bets = ((struct ptp_pdelay_resp_message *)msg)->requestReceiptTimestamp;
	break;
      case PTP_MSGTYPE_FOLLOW_UP:
	name = "PTP_MSGTYPE_FOLLOW_UP";
	bets = ((struct ptp_follow_up_message *)msg)->preciseOriginTimestamp;
	break;
      case PTP_MSGTYPE_DELAY_RESP:
	name = "PTP_MSGTYPE_DELAY_RESP";
	bets = ((struct ptp_delay_resp_message *)msg)->receiveTimestamp;
	break;
      case PTP_MSGTYPE_PDELAY_RESP_FOLLOW_UP:
	name = "PTP_MSGTYPE_PDELAY_RESP_FOLLOW_UP";
	bets = ((struct ptp_pdelay_resp_follow_up_message *)msg)->responseOriginTimestamp;
	break;
      case PTP_MSGTYPE_ANNOUNCE:
	name = "PTP_MSGTYPE_ANNOUNCE";
	bets = ((struct ptp_announce_message *)msg)->originTimestamp;
	break;
      case PTP_MSGTYPE_SIGNALING:
	name = "PTP_MSGTYPE_SIGNALING";
	break;
      default:
	name = "unknown";
    }

  ts = ptp_timestamp_betoh(&bets);
  tv_sec = ts.seconds_hi;
  tv_sec = (tv_sec << 32);
  tv_sec += ts.seconds_low;
  tv_nsec = ts.nanoseconds;

  memcpy(&be64, ((struct ptp_header *)msg)->sourcePortIdentity, sizeof(be64));
  clock_id = be64toh(be64);

  airptp_logmsg("Sent %s to port %hu, clock_id=%" PRIx64 ", ts=%" PRIu64 ".%" PRIu32, name, port, clock_id, tv_sec, tv_nsec);
}
#else
static void
log_sent(uint8_t *msg, uint16_t port)
{
  return;
}
#endif


/* --------------------------- Message construction ------------------------- */

static void
header_init(struct ptp_header *hdr, uint8_t type, uint16_t msg_len, uint64_t clock_id, uint16_t sequence_id, int8_t log_interval, uint16_t flags)
{
  uint64_t be64;

  memset(hdr, 0, sizeof(struct ptp_header));
  hdr->messageType = type | 0x10; // 0x10 -> TranSpec = 1 which is expected by nqptp
  hdr->versionPTP = 0x02; // PTPv2
  hdr->messageLength = htobe16(msg_len);
  hdr->domainNumber = AIRPTP_DOMAIN;
  hdr->flags = htobe16(flags);
  hdr->correctionField = 0;

  // Source port identity: 8 bytes clock ID + 2 bytes port number
  be64 = htobe64(clock_id);
  memcpy(hdr->sourcePortIdentity, &be64, sizeof(be64));
  // Same as iOS
  hdr->sourcePortIdentity[8] = 0x80;
  hdr->sourcePortIdentity[9] = 0x05;

  hdr->sequenceId = htobe16(sequence_id);
  hdr->controlField = 0x00;
  hdr->logMessageInterval = log_interval;
}

static void
header_read(struct ptp_header *hdr, uint64_t *clock_id_ptr, uint8_t *req)
{
  struct ptp_header *in = (struct ptp_header *)req;
  uint64_t be64;
  uint64_t clock_id;

  memcpy(hdr, in, sizeof(struct ptp_header));

  hdr->messageLength = be16toh(in->messageLength);
  hdr->flags = be16toh(in->flags);
  hdr->correctionField = be64toh(in->correctionField);
  hdr->sequenceId = be16toh(in->sequenceId);

  memcpy(&be64, in->sourcePortIdentity, sizeof(be64));
  clock_id = be64toh(be64);
  memcpy(hdr->sourcePortIdentity, &clock_id, sizeof(clock_id));

  if (clock_id_ptr)
    *clock_id_ptr = clock_id;
}

static void
msg_tlv_write(uint8_t *tlv_dst, size_t tlv_dst_size, uint16_t type, uint16_t length, void *data)
{
  uint16_t be16;

  assert(tlv_dst_size == 2 * sizeof(be16) + length);

  be16 = htobe16(type);
  memcpy(tlv_dst, &be16, sizeof(be16));
  tlv_dst += sizeof(be16);

  be16 = htobe16(length);
  memcpy(tlv_dst, &be16, sizeof(be16));
  tlv_dst += sizeof(be16);

  memcpy(tlv_dst, data, length);
}

static void
msg_announce_make(struct ptp_announce_message *msg, uint64_t clock_id, uint16_t sequence_id, struct ptp_timestamp ts, enum airptp_role role)
{
  uint64_t be64_clock_id = htobe64(clock_id);
  // iOS sets flags to 0x0408 -> UNICAST and TIMESCALE
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE;

  header_init(&msg->header, PTP_MSGTYPE_ANNOUNCE, sizeof(struct ptp_announce_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_ANNOUNCE, flags);

  msg->originTimestamp = ptp_timestamp_htobe(&ts);

  // Present as a strong, authoritative grandmaster (clockClass 6/GPS,
  // priority1/2 128) rather than a marginal oscillator-grade one. This is not
  // about winning BMCA (we win either way against a receiver's own clock) but
  // about how quickly a strict receiver commits to and locks onto our
  // timeline: a same-tier (class 248, priority 247) announce can leave strict
  // receivers slow to lock, delaying or desynchronizing playback at session
  // start, whereas an overwhelming candidate is locked
  // onto promptly.
  msg->currentUtcOffset = 0;
  msg->reserved = 0;
  msg->grandmasterPriority1 = 128;

  msg->grandmasterClockQuality = htobe32(0x06210000 | 0x436A);
  msg->grandmasterPriority2 = 128;

  msg->grandmasterIdentity = be64_clock_id;

  msg->stepsRemoved = 0;
  msg->timeSource = 0x20; // GPS

  if (role == AIRPTP_ROLE_FOLLOW)
    {
      msg->currentUtcOffset = htobe16(FOLLOW_UTC_OFFSET);
      msg->grandmasterPriority1 = FOLLOW_PRIORITY1;
      msg->grandmasterClockQuality = htobe32(FOLLOW_QUALITY);
      msg->grandmasterPriority2 = FOLLOW_PRIORITY2;
      msg->timeSource = FOLLOW_TIME_SOURCE;
    }

  // iOS adding the clock ID again as TLV, wtf?
  msg_tlv_write(msg->tlv_path_trace, sizeof(msg->tlv_path_trace), PTP_TLV_PATH_TRACE, sizeof(be64_clock_id), &be64_clock_id);
}

static void
msg_signaling_make(struct ptp_signaling_message *msg, uint64_t clock_id, uint16_t sequence_id, uint8_t *target_port_id)
{
  uint8_t apple_val1[sizeof(msg->tlv_apple1) - 4] = { 0 }; // 22 bytes
  uint8_t apple_val2[sizeof(msg->tlv_apple2) - 4] = { 0 }; // 32 bytes
  uint8_t apple_unknown[4] = { 0x00, 0x00, 0x03, 0x01 };
  // iOS sets flags to 0x0408 -> UNICAST and TIMESCALE
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE;
  uint8_t *ptr;

  header_init(&msg->header, PTP_MSGTYPE_SIGNALING, sizeof(struct ptp_signaling_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_SIGNALING, flags);

  msg->header.controlField = 0x05; // Other Message

  if (target_port_id)
    port_id_htobe(msg->targetPortIdentity, target_port_id);
  else
    memset(msg->targetPortIdentity, 0, sizeof(msg->targetPortIdentity));

  // TLV 1
  // Some fixed value, no clue what it means
  ptr = apple_val1;
  memcpy(ptr, ptp_tlv_orgs[PTP_TLV_ORG_APPLE].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, ptp_tlv_apple_subtypes[PTP_TLV_ORG_APPLE_UNKNOWN1].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, apple_unknown, sizeof(apple_unknown));
  msg_tlv_write(msg->tlv_apple1, sizeof(msg->tlv_apple1), PTP_TLV_ORG_EXTENSION, sizeof(apple_val1), apple_val1);

  // TLV 2
  // Same unknown value, but this is a longer field
  ptr = apple_val2;
  memcpy(ptr, ptp_tlv_orgs[PTP_TLV_ORG_APPLE].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, ptp_tlv_apple_subtypes[PTP_TLV_ORG_APPLE_UNKNOWN5].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, apple_unknown, sizeof(apple_unknown));
  msg_tlv_write(msg->tlv_apple2, sizeof(msg->tlv_apple2), PTP_TLV_ORG_EXTENSION, sizeof(apple_val2), apple_val2);
}

static void
msg_sync_make(struct ptp_sync_message *msg, uint64_t clock_id, uint16_t sequence_id, struct ptp_timestamp ts)
{
  // iOS sets flags to 0x0608 -> UNICAST and TIMESCALE and TWO_STEP
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE | PTP_FLAG_TWO_STEP;

  header_init(&msg->header, PTP_MSGTYPE_SYNC, sizeof(struct ptp_sync_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_SYNC, flags);

  msg->originTimestamp = ptp_timestamp_htobe(&ts);
}

static void
msg_sync_follow_up_make(struct ptp_follow_up_message *msg, uint64_t clock_id, uint16_t sequence_id, struct ptp_timestamp ts)
{
  uint8_t apple_val1[sizeof(msg->tlv_apple1) - 4] = { 0 };
  uint8_t apple_val2[sizeof(msg->tlv_apple2) - 4] = { 0 };
  uint64_t be64_clock_id = htobe64(clock_id);
  // iOS sets flags to 0x0408 -> UNICAST and TIMESCALE
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE;
  uint8_t *ptr;

  header_init(&msg->header, PTP_MSGTYPE_FOLLOW_UP, sizeof(struct ptp_follow_up_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_SYNC, flags);

  msg->preciseOriginTimestamp = ptp_timestamp_htobe(&ts);

  // TLV 1
  // iOS sets pos 6->9 (4 bytes) all zeros, Wireshark says it's "cumulativeScaledRateOffset"
  // iOS sets pos 10->11 (2 bytes) all zeros, Wireshark says it's "gmTimeBaseIndicator",
  // which is index identifying the grandmaster's time source.

  // iOS sets pos 12->23 (12 bytes), Wireshark says it's "lastGmPhaseChange"
  // Claude says it "contains information about the last discontinuous change
  // in the phase (time offset) of the Grandmaster clock" and is struct ptp_scaled_ns.
  // iOS example: 0x0000 0000fff117f85390 fadc -> 281410,954351504 (excl. ns_frac)
  // but clock is 145864, so that's strange? Zero here since we don't know better.

  // iOS sets pos 24->27 (4 bytes), Wireshark says it's "scaledLastGmFreqChange"
  // Positive val means GM is running faster than true time (how would it know?)
  // by (scaledLastGmFreqChange / 2^41) nanoseconds per second. Example:
  // 0xf9a33395 = -106744939 -> -106,744,939 / 2,199,023,255,552 = 0.000048 ns/s
  // We set zero because we have no idea if our rate is off from true time and
  // frankly don't care.
  ptr = apple_val1;
  memcpy(ptr, ptp_tlv_orgs[PTP_TLV_ORG_IEEE].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, ptp_tlv_ieee_subtypes[PTP_TLV_ORG_IEEE_FOLLOW_UP_INFO].code, PTP_TLV_ORG_CODE_SIZE);
  msg_tlv_write(msg->tlv_apple1, sizeof(msg->tlv_apple1), PTP_TLV_ORG_EXTENSION, sizeof(apple_val1), apple_val1);

  // TLV 2
  // Apple TLV with clock ID, who knows why
  ptr = apple_val2;
  memcpy(ptr, ptp_tlv_orgs[PTP_TLV_ORG_APPLE].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, ptp_tlv_apple_subtypes[PTP_TLV_ORG_APPLE_CLOCK_ID].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE;
  memcpy(ptr, &be64_clock_id, sizeof(be64_clock_id));
  msg_tlv_write(msg->tlv_apple2, sizeof(msg->tlv_apple2), PTP_TLV_ORG_EXTENSION, sizeof(apple_val2), apple_val2);
}

static void
msg_delay_resp_make(struct ptp_delay_resp_message *msg,
 uint64_t clock_id, uint16_t sequence_id, struct ptp_header *req_header, struct ptp_timestamp ts)
{
  // iOS sets flags to 0x0608 -> UNICAST and TIMESCALE and TWO_STEP
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE | PTP_FLAG_TWO_STEP;

  header_init(&msg->header, PTP_MSGTYPE_DELAY_RESP, sizeof(struct ptp_delay_resp_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_DELAY_RESP, flags);

  msg->receiveTimestamp = ptp_timestamp_htobe(&ts);

  port_id_htobe(msg->requestingPortIdentity, req_header->sourcePortIdentity);
}

// Haven't seen these messages from iOS, so the implementation is a guess
static void
msg_pdelay_resp_make(struct ptp_pdelay_resp_message *msg,
 uint64_t clock_id, uint16_t sequence_id, struct ptp_header *req_header, struct ptp_timestamp ts)
{
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE | PTP_FLAG_TWO_STEP;

  header_init(&msg->header, PTP_MSGTYPE_PDELAY_RESP, sizeof(struct ptp_pdelay_resp_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_SYNC, flags);

  msg->requestReceiptTimestamp = ptp_timestamp_htobe(&ts);

  port_id_htobe(msg->requestingPortIdentity, req_header->sourcePortIdentity);
}

// Haven't seen these messages from iOS, so the implementation is a guess
static void
msg_pdelay_resp_follow_up_make(struct ptp_pdelay_resp_follow_up_message *msg,
 uint64_t clock_id, uint16_t sequence_id, struct ptp_header *req_header, struct ptp_timestamp ts)
{
  uint16_t flags = PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE;

  header_init(&msg->header, PTP_MSGTYPE_PDELAY_RESP_FOLLOW_UP, sizeof(struct ptp_pdelay_resp_follow_up_message), clock_id, sequence_id, AIRPTP_LOGMESSAGEINT_SYNC, flags);

  msg->responseOriginTimestamp = ptp_timestamp_htobe(&ts);

  port_id_htobe(msg->requestingPortIdentity, req_header->sourcePortIdentity);
}

static void
msg_peer_add_make(struct ptp_peer_signaling_message *msg, struct airptp_peer *peer, uint64_t clock_id)
{
  uint8_t peerinfo[sizeof(msg->tlv_peer_info) - 4] = { 0 };
  uint32_t be32_peer_id = htobe32(peer->id);
  uint8_t addr_len = peer->naddr_len;
  uint8_t *ptr;

  header_init(&msg->header, PTP_MSGTYPE_SIGNALING, sizeof(struct ptp_peer_signaling_message), clock_id, 0, 0, PTP_FLAG_UNICAST);

  memset(msg->targetPortIdentity, 0, PTP_PORT_ID_SIZE);

  assert(addr_len <= 28); // sizeof(struct sockaddr_in6)

  ptr = peerinfo;
  memcpy(ptr, ptp_tlv_orgs[PTP_TLV_ORG_OWN].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE; // 3
  memcpy(ptr, ptp_tlv_own_subtypes[PTP_TLV_ORG_OWN_PEER_ADD].code, PTP_TLV_ORG_CODE_SIZE);  // Add
  ptr += PTP_TLV_ORG_CODE_SIZE; // 3
  memcpy(ptr, &be32_peer_id, sizeof(be32_peer_id));
  ptr += sizeof(be32_peer_id); // 4
  memcpy(ptr, &addr_len, sizeof(addr_len));
  ptr += sizeof(addr_len); // 1
  memcpy(ptr, &peer->naddr, addr_len);
  msg_tlv_write(msg->tlv_peer_info, sizeof(msg->tlv_peer_info), PTP_TLV_ORG_EXTENSION, sizeof(peerinfo), peerinfo);
}

static void
msg_peer_del_make(struct ptp_peer_signaling_message *msg, struct airptp_peer *peer, uint64_t clock_id)
{
  uint8_t peerinfo[sizeof(msg->tlv_peer_info) - 4] = { 0 };
  uint32_t be32_peer_id = htobe32(peer->id);
  uint8_t *ptr;

  header_init(&msg->header, PTP_MSGTYPE_SIGNALING, sizeof(struct ptp_peer_signaling_message), clock_id, 0, 0, PTP_FLAG_UNICAST);

  memset(msg->targetPortIdentity, 0, PTP_PORT_ID_SIZE);

  ptr = peerinfo;
  memcpy(ptr, ptp_tlv_orgs[PTP_TLV_ORG_OWN].code, PTP_TLV_ORG_CODE_SIZE);
  ptr += PTP_TLV_ORG_CODE_SIZE; // 3
  memcpy(ptr, ptp_tlv_own_subtypes[PTP_TLV_ORG_OWN_PEER_DEL].code, PTP_TLV_ORG_CODE_SIZE); // Delete
  ptr += PTP_TLV_ORG_CODE_SIZE; // 3
  memcpy(ptr, &be32_peer_id, sizeof(be32_peer_id));
  msg_tlv_write(msg->tlv_peer_info, sizeof(msg->tlv_peer_info), PTP_TLV_ORG_EXTENSION, sizeof(peerinfo), peerinfo);
}


/* ------------------------------ Follower role ----------------------------- */

// An Apple sender does not make the receivers follow it. They elect a
// grandmaster among themselves (in a stereo pair, one member's clock), the
// sender follows that clock like any other, and every time it hands the
// receivers is on the grandmaster's timeline under the grandmaster's identity.
// The receivers then share one clock that is theirs, so whatever error the
// sender's own estimate has moves all of them together. Forcing our own clock
// on them instead has each receiver track it separately over its own wireless
// link, and those errors differ between them.

static inline int64_t
mono_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

// A 10 byte PTP timestamp as it is on the wire
static inline int64_t
wire_ts_ns(const uint8_t *p)
{
  uint64_t secs = 0;
  uint32_t nsecs;
  int i;

  for (i = 0; i < 6; i++)
    secs = (secs << 8) | p[i];
  nsecs = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 8) | p[9];
  return (int64_t)secs * NS_PER_SEC + nsecs;
}

// correctionField of a message, nanoseconds scaled by 2^16. A receiver that
// relays its partner's clock stamps messages with its own clock and puts the
// difference, tens of ms, here: it is added to the Follow_Up origin and to
// the Delay_Resp receive time alike (checked against a Mac's exchanges, where
// only that convention gives a plausible path delay).
static inline int64_t
wire_correction_ns(const uint8_t *msg)
{
  int64_t be;

  memcpy(&be, msg + 8, sizeof(be));
  return (int64_t)be64toh(be) / 65536;
}

static inline uint16_t
wire_seq(const uint8_t *msg)
{
  return ((uint16_t)msg[30] << 8) | msg[31];
}

// IEEE 1588 dataset comparison, negative when a is the better grandmaster.
// clockQuality packs class, accuracy and variance in comparison order.
static int
gm_compare(uint8_t p1a, uint32_t qa, uint8_t p2a, uint64_t ida, uint8_t p1b, uint32_t qb, uint8_t p2b, uint64_t idb)
{
  if (p1a != p1b)
    return (p1a < p1b) ? -1 : 1;
  if (qa != qb)
    return (qa < qb) ? -1 : 1;
  if (p2a != p2b)
    return (p2a < p2b) ? -1 : 1;
  if (ida != idb)
    return (ida < idb) ? -1 : 1;
  return 0;
}

static bool
foreign_is_better(struct airptp_foreign *a, struct airptp_foreign *b)
{
  int cmp = gm_compare(a->p1, a->quality, a->p2, a->gm_id, b->p1, b->quality, b->p2, b->gm_id);

  if (cmp != 0)
    return cmp < 0;
  if (a->steps != b->steps)
    return a->steps < b->steps;
  return memcmp(a->port_id, b->port_id, AIRPTP_PORT_ID_SIZE) < 0;
}

static void
sample_push(struct airptp_sample *buf, int *n, int max, int64_t local_ns, int64_t value_ns)
{
  if (*n == max)
    {
      memmove(buf, buf + 1, (max - 1) * sizeof(*buf));
      (*n)--;
    }

  buf[*n].local_ns = local_ns;
  buf[*n].value_ns = value_ns;
  (*n)++;
}

static int
int64_cmp(const void *a, const void *b)
{
  int64_t x = *(const int64_t *)a;
  int64_t y = *(const int64_t *)b;

  return (x > y) - (x < y);
}

// The fastest exchange in the window. Only among samples on the same timescale
// as most of them, though: a receiver's first replies can still be stamped on
// its own clock rather than the grandmaster's, hours away, and a minimum taken
// over one of those is meaningless. Wireless delay spikes only ever make a
// sample larger, so rejecting what is impossibly fast loses nothing real.
static int
window_min(struct airptp_sample *buf, int n, int64_t since_ns, int64_t *min_ns)
{
  int64_t values[AIRPTP_SAMPLES_MAX];
  int64_t median;
  int count = 0;
  int i;

  for (i = 0; i < n; i++)
    {
      if (buf[i].local_ns >= since_ns)
	values[count++] = buf[i].value_ns;
    }

  if (count == 0)
    return 0;

  qsort(values, count, sizeof(values[0]), int64_cmp);
  median = values[count / 2];

  for (i = 0; i < count; i++)
    {
      if (values[i] >= median - FOLLOW_OUTLIER_NS)
	{
	  *min_ns = values[i];
	  return count - i;
	}
    }

  return 0;
}

// offset is local minus grandmaster time
static bool
follower_estimate(struct airptp_follower *f, int64_t now_ns, int min_samples, int64_t *offset_ns, int64_t *delay_ns)
{
  int64_t fwd_min = 0;
  int64_t rev_min = 0;

  if (window_min(f->fwd, f->fwd_n, now_ns - FOLLOW_WINDOW_NS, &fwd_min) < min_samples ||
      window_min(f->rev, f->rev_n, now_ns - FOLLOW_WINDOW_NS, &rev_min) < min_samples)
    return false;

  *delay_ns = (fwd_min + rev_min) / 2;
  *offset_ns = (fwd_min - rev_min) / 2;

  // Neither direction can be faster than instant, so a clearly negative round
  // trip means the window straddles a change on the grandmaster's side
  return *delay_ns > -2000000LL;
}

// Least squares line through the recent offset estimates, refitted once
// without the estimates that sit well off it
static void
fit_line(struct airptp_follower *f, int64_t now_ns, double prev_rate, int64_t *offset_now_ns, double *rate, double *jitter_ms)
{
  bool outlier[AIRPTP_FIT_POINTS_MAX] = { false };
  double base = (double)f->fit[f->fit_n - 1].value_ns;
  double sx, sy, sxx, sxy, n, x, y, slope, icpt, res, var, limit;
  int pass, i;

  *offset_now_ns = f->fit[f->fit_n - 1].value_ns;
  *rate = prev_rate;
  *jitter_ms = 0;

  if (f->fit_n < 3 || now_ns - f->fit[0].local_ns < 3 * NS_PER_SEC)
    return;

  for (pass = 0; pass < 2; pass++)
    {
      sx = sy = sxx = sxy = n = 0;
      for (i = 0; i < f->fit_n; i++)
	{
	  if (outlier[i])
	    continue;
	  x = (double)(f->fit[i].local_ns - now_ns) / NS_PER_SEC;
	  y = (double)f->fit[i].value_ns - base;
	  sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
	}
      if (n < 3 || n * sxx - sx * sx <= 0)
	return;

      slope = (n * sxy - sx * sy) / (n * sxx - sx * sx); // ns per second
      icpt = (sy - slope * sx) / n;                      // at now

      var = 0;
      for (i = 0; i < f->fit_n; i++)
	{
	  if (outlier[i])
	    continue;
	  res = ((double)f->fit[i].value_ns - base) - (icpt + slope * (double)(f->fit[i].local_ns - now_ns) / NS_PER_SEC);
	  var += res * res;
	}
      var /= n;

      if (pass == 0)
	{
	  limit = 3 * sqrt(var);
	  if (limit < 500000)
	    limit = 500000;
	  for (i = 0; i < f->fit_n; i++)
	    {
	      res = ((double)f->fit[i].value_ns - base) - (icpt + slope * (double)(f->fit[i].local_ns - now_ns) / NS_PER_SEC);
	      outlier[i] = (fabs(res) > limit);
	    }
	}
    }

  *offset_now_ns = (int64_t)(base + icpt);
  *rate = slope / NS_PER_SEC;
  if (*rate > FOLLOW_RATE_MAX)
    *rate = FOLLOW_RATE_MAX;
  else if (*rate < -FOLLOW_RATE_MAX)
    *rate = -FOLLOW_RATE_MAX;
  *jitter_ms = sqrt(var) / 1e6;
}

static void
timeline_publish(struct airptp_daemon *daemon, bool following, int64_t ref_ns, int64_t offset_ns, double rate)
{
  struct airptp_follower *f = &daemon->follower;

  pthread_mutex_lock(&daemon->timeline_lck);
  if (following && f->master >= 0)
    {
      daemon->timeline.state = AIRPTP_TIMELINE_FOLLOWING;
      daemon->timeline.clock_id = f->foreign[f->master].gm_id;
      daemon->timeline.ref_ns = ref_ns;
      daemon->timeline.offset_ns = offset_ns;
      daemon->timeline.rate = rate;
    }
  else
    {
      daemon->timeline.state = AIRPTP_TIMELINE_OWN;
      daemon->timeline.clock_id = daemon->clock_id;
      daemon->timeline.ref_ns = 0;
      daemon->timeline.offset_ns = 0;
      daemon->timeline.rate = 0;
    }
  daemon->timeline_first_peer_ns = f->first_peer_ns;
  pthread_mutex_unlock(&daemon->timeline_lck);
}

static void
follower_samples_clear(struct airptp_follower *f)
{
  f->sync_pending = false;
  f->dreq_pending = false;
  f->fwd_n = 0;
  f->rev_n = 0;
  f->fit_n = 0;
  f->locked = false;
}

static void
follower_lose(struct airptp_daemon *daemon, const char *why)
{
  struct airptp_follower *f = &daemon->follower;

  if (f->master < 0)
    return;

  airptp_lognotice("PTP: no longer following grandmaster %016" PRIx64 " (%s), using our own clock", f->foreign[f->master].gm_id, why);
  f->master = -1;
  follower_samples_clear(f);
  timeline_publish(daemon, false, 0, 0, 0);
}

static void
follower_select(struct airptp_daemon *daemon, int64_t now_ns)
{
  struct airptp_follower *f = &daemon->follower;
  struct airptp_foreign *c;
  int best = -1;
  int64_t timeout;
  char addr[64];
  int i;

  for (i = 0; i < AIRPTP_FOREIGN_MAX; i++)
    {
      c = &f->foreign[i];
      if (!c->in_use)
	continue;

      // Four announce intervals, but never under 3 s: receivers slow their
      // announces to one a second once a domain has settled
      if (c->logint >= 0 && c->logint < 8)
	timeout = (4 * NS_PER_SEC) << c->logint;
      else if (c->logint < 0 && c->logint > -8)
	timeout = (4 * NS_PER_SEC) >> (-c->logint);
      else
	timeout = 4 * NS_PER_SEC;
      if (timeout < 3 * NS_PER_SEC)
	timeout = 3 * NS_PER_SEC;

      if (now_ns - c->last_announce_ns > timeout)
	{
	  utils_net_address_get(addr, sizeof(addr), &c->naddr);
	  airptp_logmsg("PTP clock offer from %s for grandmaster %016" PRIx64 " expired", addr, c->gm_id);
	  c->in_use = false;
	  if (f->master == i)
	    follower_lose(daemon, "it stopped announcing");
	  continue;
	}

      if (best < 0 || foreign_is_better(c, &f->foreign[best]))
	best = i;
    }

  if (best < 0 || gm_compare(f->foreign[best].p1, f->foreign[best].quality, f->foreign[best].p2, f->foreign[best].gm_id,
			     FOLLOW_PRIORITY1, FOLLOW_QUALITY, FOLLOW_PRIORITY2, daemon->clock_id) >= 0)
    {
      follower_lose(daemon, "no receiver offers a better clock");
      return;
    }

  if (f->master == best)
    return;

  // The same grandmaster reached through another receiver: stay on the path
  // the timing samples were taken on, unless the new path is shorter.
  // tuxplay: which member's announce arrives first is luck, and a pair member
  // relaying its partner's clock (stepsRemoved 1) adds its own tracking error
  // to every sample. Measured on a HomePod pair: the same grandmaster was
  // followed directly on one start and through the relaying member on the next.
  if (f->master >= 0 && f->foreign[f->master].gm_id == f->foreign[best].gm_id)
    {
      if (f->foreign[best].steps >= f->foreign[f->master].steps)
	return;

      // Same timeline, so the published mapping stays exactly as it is while
      // audio is timed by it. Only the per-exchange samples belong to the old
      // path; the once-a-second fit points are offsets to the same grandmaster
      // and carry over, so the servo continues from them.
      utils_net_address_get(addr, sizeof(addr), &f->foreign[best].naddr);
      airptp_lognotice("PTP: grandmaster %016" PRIx64 " also announced directly by %s (stepsRemoved %u), switching to that path",
		       f->foreign[best].gm_id, addr, f->foreign[best].steps);
      f->master = best;
      f->sync_pending = false;
      f->dreq_pending = false;
      f->fwd_n = 0;
      f->rev_n = 0;
      return;
    }

  c = &f->foreign[best];
  utils_net_address_get(addr, sizeof(addr), &c->naddr);
  airptp_lognotice("PTP: following grandmaster %016" PRIx64 " announced by %s (priority1 %u, class %u, stepsRemoved %u)",
		   c->gm_id, addr, c->p1, (c->quality >> 24) & 0xff, c->steps);

  f->master = best;
  f->master_since_ns = now_ns;
  f->stats_ns = now_ns;
  follower_samples_clear(f);
  timeline_publish(daemon, false, 0, 0, 0);
}

static void
follower_try_lock(struct airptp_daemon *daemon, int64_t now_ns)
{
  struct airptp_follower *f = &daemon->follower;
  int64_t offset_ns;
  int64_t delay_ns;

  if (!follower_estimate(f, now_ns, 2, &offset_ns, &delay_ns))
    return;

  f->locked = true;
  f->delay_ms = NS_TO_MS(delay_ns);
  sample_push(f->fit, &f->fit_n, AIRPTP_FIT_POINTS_MAX, now_ns, offset_ns);
  timeline_publish(daemon, true, now_ns, offset_ns, 0.0);

  airptp_lognotice("PTP: locked to grandmaster %016" PRIx64 " %.0f ms after choosing it, path delay %.1f ms",
		   f->foreign[f->master].gm_id, NS_TO_MS(now_ns - f->master_since_ns), f->delay_ms);
}

bool
ptp_follower_is_grandmaster(struct airptp_daemon *daemon)
{
  return (daemon->role != AIRPTP_ROLE_FOLLOW || daemon->follower.master < 0);
}

void
ptp_follower_peers_changed(struct airptp_daemon *daemon)
{
  struct airptp_follower *f = &daemon->follower;

  if (daemon->num_peers > 0 && f->first_peer_ns == 0)
    f->first_peer_ns = mono_ns();
  else if (daemon->num_peers == 0)
    f->first_peer_ns = 0;

  // With no receivers left nothing keeps the lock current (the timers stop).
  // Sessions that reconnect at once (after a network change) bring the same
  // grandmaster back, so keep the lock for a moment and only start from
  // nothing when they took longer, rather than from a mapping gone stale.
  if (daemon->num_peers == 0 && f->master >= 0)
    {
      if (!f->peers_gone_ns)
	f->peers_gone_ns = mono_ns();
      return;
    }
  if (daemon->num_peers > 0 && f->peers_gone_ns)
    {
      bool stale = mono_ns() - f->peers_gone_ns > PEERS_GONE_GRACE_NS;

      f->peers_gone_ns = 0;
      if (stale && f->master >= 0)
	{
	  airptp_logmsg("PTP: no peers for a while, dropping the lock on grandmaster %016" PRIx64, f->foreign[f->master].gm_id);
	  f->master = -1;
	  follower_samples_clear(f);
	  timeline_publish(daemon, false, 0, 0, 0);
	}
    }

  pthread_mutex_lock(&daemon->timeline_lck);
  daemon->timeline_first_peer_ns = f->first_peer_ns;
  pthread_mutex_unlock(&daemon->timeline_lck);
}

void
ptp_follower_servo(struct airptp_daemon *daemon)
{
  struct airptp_follower *f = &daemon->follower;
  struct airptp_timeline *tl = &daemon->timeline; // only this thread writes it
  int64_t now_ns = mono_ns();
  int64_t offset_ns;
  int64_t delay_ns;
  int64_t fit_offset_ns;
  int64_t published_ns;
  int64_t err_ns;
  double fit_rate;
  double slew;
  char addr[64];

  if (daemon->role != AIRPTP_ROLE_FOLLOW)
    return;

  follower_select(daemon, now_ns);
  if (f->master < 0 || !f->locked)
    return;

  if (!follower_estimate(f, now_ns, 4, &offset_ns, &delay_ns))
    return;

  f->delay_ms = NS_TO_MS(delay_ns);
  sample_push(f->fit, &f->fit_n, AIRPTP_FIT_POINTS_MAX, now_ns, offset_ns);
  fit_line(f, now_ns, tl->rate, &fit_offset_ns, &fit_rate, &f->jitter_ms);

  // The published mapping must stay continuous while audio is timed by it, so
  // a small error is slewed out through the rate, and only a large one steps
  published_ns = tl->offset_ns + (int64_t)(tl->rate * (double)(now_ns - tl->ref_ns));
  err_ns = fit_offset_ns - published_ns;
  if (err_ns > FOLLOW_STEP_NS || err_ns < -FOLLOW_STEP_NS)
    {
      airptp_lognotice("PTP: timeline stepped by %+.1f ms", NS_TO_MS(err_ns));
      timeline_publish(daemon, true, now_ns, fit_offset_ns, fit_rate);
    }
  else
    {
      slew = (double)err_ns / (FOLLOW_SLEW_SECS * NS_PER_SEC);
      if (slew > FOLLOW_SLEW_MAX)
	slew = FOLLOW_SLEW_MAX;
      else if (slew < -FOLLOW_SLEW_MAX)
	slew = -FOLLOW_SLEW_MAX;
      timeline_publish(daemon, true, now_ns, published_ns, fit_rate + slew);
    }

  if (now_ns - f->stats_ns >= FOLLOW_STATS_NS)
    {
      f->stats_ns = now_ns;
      utils_net_address_get(addr, sizeof(addr), &f->foreign[f->master].naddr);
      airptp_lognotice("PTP: following %016" PRIx64 " via %s: path delay %.1f ms, estimate jitter %.2f ms, rate %+.1f ppm, correcting %+.2f ms",
		       f->foreign[f->master].gm_id, addr, f->delay_ms, f->jitter_ms, fit_rate * 1e6, NS_TO_MS(err_ns));
    }
}

void
ptp_msg_delay_req_send(struct airptp_daemon *daemon)
{
  struct airptp_follower *f = &daemon->follower;
  struct ptp_delay_req_message msg;
  union utils_net_sockaddr naddr;
  ssize_t len;

  if (daemon->role != AIRPTP_ROLE_FOLLOW || f->master < 0)
    return;

  // As macOS 27 sends it (measured): unicast and timescale flags,
  // controlField 0, no origin
  header_init(&msg.header, PTP_MSGTYPE_DELAY_REQ, sizeof(msg), daemon->clock_id, daemon->delay_req_seq, AIRPTP_LOGMESSAGEINT_SYNC, PTP_FLAG_UNICAST | PTP_FLAG_TIMESCALE);
  msg.header.controlField = 0x00;
  memset(&msg.originTimestamp, 0, sizeof(msg.originTimestamp));

  memcpy(&naddr, &f->foreign[f->master].naddr, sizeof(naddr));
  port_set(&naddr, daemon->event_svc.port);

  f->dreq_seq = daemon->delay_req_seq++;
  f->dreq_pending = true;
  f->dreq_tx_ns = mono_ns();
  len = utils_net_sendto(&daemon->event_svc.socket, &msg, sizeof(msg), &naddr);
  if (len != sizeof(msg))
    {
      f->dreq_pending = false;
      airptp_logmsg("Error sending Delay_Req: %s", strerror(errno));
    }

  // macOS 27 also sends Delay_Req to every other receiver offering a clock (a
  // pair's second member). Receivers that get none treat us as not synced to
  // them and keep announcing four times a second. Only the master's answer
  // feeds the servo; delay_resp_handle() drops the others.
  for (int i = 0; i < AIRPTP_FOREIGN_MAX; i++)
    {
      if (i == f->master || !f->foreign[i].in_use)
	continue;
      memcpy(&naddr, &f->foreign[i].naddr, sizeof(naddr));
      port_set(&naddr, daemon->event_svc.port);
      utils_net_sendto(&daemon->event_svc.socket, &msg, sizeof(msg), &naddr);
    }
}

static bool
from_master(struct airptp_daemon *daemon, uint8_t *req)
{
  struct airptp_follower *f = &daemon->follower;

  // Clock identity only, see announce_handle()
  return (daemon->role == AIRPTP_ROLE_FOLLOW && f->master >= 0 &&
	  memcmp(req + 20, f->foreign[f->master].port_id, sizeof(uint64_t)) == 0);
}


/* ------------------------ Incoming message handling ----------------------- */

static void
sync_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len, int64_t rx_ns)
{
  struct airptp_follower *f = &daemon->follower;

  struct ptp_sync_message *in = (struct ptp_sync_message *)req;
  struct ptp_sync_message sync = { 0 };
  uint64_t clock_id;

  if (req_len < sizeof(struct ptp_sync_message))
    return;

  header_read(&sync.header, &clock_id, req);
  sync.originTimestamp = ptp_timestamp_betoh(&in->originTimestamp);

  log_received("Sync", &sync.header, clock_id, &sync.originTimestamp);

  // Two-step: the time it left comes in the Follow_Up
  if (!from_master(daemon, req))
    return;

  f->sync_pending = true;
  f->sync_seq = wire_seq(req);
  f->sync_rx_ns = rx_ns;
  f->sync_corr_ns = wire_correction_ns(req);
}

static void
follow_up_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len, int64_t rx_ns)
{
  struct airptp_follower *f = &daemon->follower;
  int64_t origin_ns;

  struct ptp_follow_up_message *in = (struct ptp_follow_up_message *)req;
  struct ptp_follow_up_message follow_up = { 0 };
  uint64_t clock_id;

  if (req_len < sizeof(struct ptp_sync_message))
    return;

  if (from_master(daemon, req) && f->sync_pending && wire_seq(req) == f->sync_seq)
    {
      f->sync_pending = false;
      origin_ns = wire_ts_ns(req + sizeof(struct ptp_header)) + f->sync_corr_ns + wire_correction_ns(req);
      sample_push(f->fwd, &f->fwd_n, AIRPTP_SAMPLES_MAX, f->sync_rx_ns, f->sync_rx_ns - origin_ns);
    }

  if (req_len < sizeof(struct ptp_follow_up_message))
    return;

  header_read(&follow_up.header, &clock_id, req);
  follow_up.preciseOriginTimestamp = ptp_timestamp_betoh(&in->preciseOriginTimestamp);

  log_received("Follow Up", &follow_up.header, clock_id, &follow_up.preciseOriginTimestamp);
}

static void
delay_msg_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len)
{
  struct ptp_delay_req_message *in = (struct ptp_delay_req_message *)req;
  struct ptp_delay_req_message delay_req = { 0 };
  uint64_t clock_id;
  struct ptp_delay_resp_message	delay_resp;
  struct ptp_timestamp ts;
  ssize_t len;

  if (req_len < sizeof(struct ptp_delay_req_message))
    return;

  header_read(&delay_req.header, &clock_id, req);
  delay_req.originTimestamp = ptp_timestamp_betoh(&in->originTimestamp);

  log_received("Delay Req", &delay_req.header, clock_id, &delay_req.originTimestamp);

  ts = current_time_get();
  msg_delay_resp_make(&delay_resp, daemon->clock_id, delay_req.header.sequenceId, &delay_req.header, ts);

  port_set(peer_addr, daemon->general_svc.port);
  len = utils_net_sendto(&daemon->general_svc.socket, &delay_resp, sizeof(delay_resp), peer_addr);
  if (len != sizeof(delay_resp))
    airptp_logmsg("Incomplete send of struct ptp_pdelay_resp_follow_up_message");

  log_sent((uint8_t *)&delay_resp, daemon->general_svc.port);
}

// Every receiver announces the grandmaster it follows, which may be its own
// clock or its stereo partner's. Only the follower role acts on that.
static void
announce_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len, int64_t rx_ns)
{
  struct airptp_follower *f = &daemon->follower;
  struct airptp_foreign *c = NULL;
  struct airptp_foreign *slot = NULL;
  uint64_t be64;
  uint32_t be32;
  uint16_t be16;
  uint64_t src_clock_id;
  uint64_t prev_gm_id;
  int i;

  if (req_len < 64 || daemon->role != AIRPTP_ROLE_FOLLOW)
    return;

  memcpy(&be64, req + 20, sizeof(be64));
  src_clock_id = be64toh(be64);
  if (src_clock_id == daemon->clock_id)
    return;

  // Keyed by clock identity, not port identity: a HomePod opens a new PTP
  // port when a session registers with it, and moves everything it sends us
  // to that port a moment after its first announce
  for (i = 0; i < AIRPTP_FOREIGN_MAX; i++)
    {
      if (f->foreign[i].in_use && memcmp(f->foreign[i].port_id, req + 20, sizeof(src_clock_id)) == 0)
	c = &f->foreign[i];
      else if (!f->foreign[i].in_use && !slot)
	slot = &f->foreign[i];
    }
  if (!c)
    {
      if (!slot)
	return;
      c = slot;
      memset(c, 0, sizeof(*c));
      c->in_use = true;
    }
  memcpy(c->port_id, req + 20, AIRPTP_PORT_ID_SIZE);

  prev_gm_id = c->gm_id;
  memset(&c->naddr, 0, sizeof(c->naddr));
  memcpy(&c->naddr, peer_addr, (peer_addr_len < sizeof(c->naddr)) ? peer_addr_len : sizeof(c->naddr));
  c->p1 = req[47];
  memcpy(&be32, req + 48, sizeof(be32));
  c->quality = be32toh(be32);
  c->p2 = req[52];
  memcpy(&be64, req + 53, sizeof(be64));
  c->gm_id = be64toh(be64);
  memcpy(&be16, req + 61, sizeof(be16));
  c->steps = be16toh(be16);
  c->logint = (int8_t)req[33];
  c->last_announce_ns = rx_ns;

  // While a pair settles, a receiver can announce its own clock and then its
  // partner's. Samples taken against the old one say nothing about the new.
  if (f->master == (int)(c - f->foreign) && prev_gm_id != c->gm_id)
    {
      airptp_lognotice("PTP: the clock we follow now passes on grandmaster %016" PRIx64 " instead of %016" PRIx64 ", relocking", c->gm_id, prev_gm_id);
      follower_samples_clear(f);
      f->master_since_ns = rx_ns;
      timeline_publish(daemon, false, 0, 0, 0);
    }

  // A receiver passing our own clock along offers nothing new
  if (c->gm_id == daemon->clock_id)
    {
      if (f->master == (int)(c - f->foreign))
	follower_lose(daemon, "it now follows our clock");
      c->in_use = false;
      return;
    }

  airptp_logmsg("PTP Announce from %016" PRIx64 ": grandmaster %016" PRIx64 ", p1 %u, quality %08x, p2 %u, steps %u",
		src_clock_id, c->gm_id, c->p1, c->quality, c->p2, c->steps);

  follower_select(daemon, rx_ns);
}

static void
delay_resp_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len, int64_t rx_ns)
{
  struct airptp_follower *f = &daemon->follower;
  uint64_t be64;
  int64_t rx_gm_ns;

  if (req_len < sizeof(struct ptp_delay_resp_message) || !from_master(daemon, req))
    return;

  // requestingPortIdentity must be ours, and answer the Delay_Req we timed
  memcpy(&be64, req + sizeof(struct ptp_header) + 10, sizeof(be64));
  if (be64toh(be64) != daemon->clock_id || !f->dreq_pending || wire_seq(req) != f->dreq_seq)
    return;

  f->dreq_pending = false;
  rx_gm_ns = wire_ts_ns(req + sizeof(struct ptp_header)) + wire_correction_ns(req);
  sample_push(f->rev, &f->rev_n, AIRPTP_SAMPLES_MAX, f->dreq_tx_ns, rx_gm_ns - f->dreq_tx_ns);

  if (!f->locked)
    follower_try_lock(daemon, rx_ns);
}

static void
pdelay_msg_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len)
{
  struct ptp_header header;
  struct ptp_pdelay_resp_message resp;
  struct ptp_pdelay_resp_follow_up_message followup;
  struct ptp_timestamp ts;
  ssize_t len;

  if (req_len < sizeof(struct ptp_header))
    return;

  header_read(&header, NULL, req);

  ts = current_time_get();
  msg_pdelay_resp_make(&resp, daemon->clock_id, header.sequenceId, &header, ts);

  port_set(peer_addr, daemon->event_svc.port);
  len = utils_net_sendto(&daemon->event_svc.socket, &resp, sizeof(resp), peer_addr);
  if (len != sizeof(resp))
    airptp_logmsg("Incomplete send of struct ptp_pdelay_resp_message");

  log_sent((uint8_t *)&resp, daemon->event_svc.port);

  ts = current_time_get();
  msg_pdelay_resp_follow_up_make(&followup, daemon->clock_id, header.sequenceId, &header, ts);

  port_set(peer_addr, daemon->general_svc.port);
  len = utils_net_sendto(&daemon->general_svc.socket, &followup, sizeof(followup), peer_addr);
  if (len != sizeof(followup))
    airptp_logmsg("Incomplete send of struct ptp_pdelay_resp_follow_up_message");

  log_sent((uint8_t *)&followup, daemon->general_svc.port);
}

static int
tlv_handle_org_subtype_generic(struct airptp_daemon *daemon, const char *org, struct ptp_tlv_org_subtype_map *subtype, uint8_t *data, size_t len)
{
  airptp_logmsg("Received '%s' TLV org extension, subtype '%s', length %zu", org, subtype->name, len);
  return 0;
}

static int
tlv_handle_org_subtype_message_internal(struct airptp_daemon *daemon, const char *org, struct ptp_tlv_org_subtype_map *subtype, uint8_t *data, size_t len)
{
  if (len < 6)
    return -1;

  airptp_logmsg("Ignoring PTP signaling linkDelayInterval=%hhd, timeSyncInterval=%hhd, announceInterval=%hhd", data[0], data[1], data[2]);
  return 0;
}

static int
tlv_handle_org_subtype_peer_add(struct airptp_daemon *daemon, const char *org, struct ptp_tlv_org_subtype_map *subtype, uint8_t *data, size_t len)
{
  uint32_t be32_peer_id;
  uint8_t addr_len;
  struct airptp_peer peer = { 0 };
  uint8_t *ptr;

  if (len < sizeof(be32_peer_id) + sizeof(addr_len))
    goto error;

  ptr = data;
  memcpy(&be32_peer_id, ptr, sizeof(be32_peer_id));
  ptr += sizeof(be32_peer_id);
  memcpy(&addr_len, ptr, sizeof(addr_len));
  ptr += sizeof(addr_len);

  if (len < (ptr - data) + addr_len)
    goto error;

  memcpy(&peer.naddr, ptr, addr_len);
  peer.id = be32toh(be32_peer_id);
  peer.naddr_len = addr_len;

  daemon_peer_add(daemon, &peer);
  return 0;

 error:
  return -1;
}

static int
tlv_handle_org_subtype_peer_del(struct airptp_daemon *daemon, const char *org, struct ptp_tlv_org_subtype_map *subtype, uint8_t *data, size_t len)
{
  uint32_t be32_peer_id;
  struct airptp_peer peer = { 0 };

  if (len < sizeof(be32_peer_id))
    goto error;

  memcpy(&be32_peer_id, data, sizeof(be32_peer_id));

  peer.id = be32toh(be32_peer_id);

  daemon_peer_del(daemon, &peer);
  return 0;

 error:
  return -1;
}

static int
tlv_handle_org_extension(struct airptp_daemon *daemon, uint8_t *data, uint16_t len)
{
  uint8_t orgcode[PTP_TLV_ORG_CODE_SIZE];
  uint8_t subtype[PTP_TLV_ORG_CODE_SIZE];
  uint16_t offset = sizeof(orgcode) + sizeof(subtype);
  struct ptp_tlv_org_map *org;

  if (len < offset)
    return -1;

  memcpy(orgcode, data, PTP_TLV_ORG_CODE_SIZE);
  memcpy(subtype, data + PTP_TLV_ORG_CODE_SIZE, PTP_TLV_ORG_CODE_SIZE);

  for (int i = 0; i < ARRAY_SIZE(ptp_tlv_orgs); i++)
    {
      org = &ptp_tlv_orgs[i];

      if (memcmp(orgcode, org->code, PTP_TLV_ORG_CODE_SIZE) != 0)
	continue;

      for (int j = 0; j < org->n_subtypes; j++)
	{
	  if (memcmp(subtype, org->subtypes[j].code, PTP_TLV_ORG_CODE_SIZE) != 0)
	    continue;

	  return org->subtypes[j].handler(daemon, org->name, &org->subtypes[j], data + offset, len - offset);
	}
    }

  return -1;
}

static int
tlv_handle_path_trace(struct airptp_daemon *daemon, uint8_t *data, uint16_t len)
{
  // Normally we get the 8 byte clock ID here, log if something unexpected
  if (len != 8)
    airptp_hexdump("TLV path trace with unexpected length", data, len);

  return 0;
}

// Returns length of tlv consumed (0 if no tlv), negative if error
static ssize_t
tlv_handle(struct airptp_daemon *daemon, uint8_t *tlv, ssize_t tlv_max_size)
{
  uint8_t *ptr = tlv;
  uint16_t be16;
  uint16_t type;
  uint16_t len;
  int ret;

  if (tlv_max_size == 0)
    return 0;
  else if (tlv_max_size < PTP_TLV_MIN_SIZE)
    return -1;

  memcpy(&be16, ptr, sizeof(be16));
  ptr += sizeof(be16);
  type = be16toh(be16);

  memcpy(&be16, ptr, sizeof(be16));
  ptr += sizeof(be16);
  len = be16toh(be16);

  if (tlv_max_size < PTP_TLV_MIN_SIZE + len)
    return -1;

  if (type == PTP_TLV_ORG_EXTENSION)
    ret = tlv_handle_org_extension(daemon, ptr, len);
  else if (type == PTP_TLV_PATH_TRACE)
    ret = tlv_handle_path_trace(daemon, ptr, len);
  else
    ret = -1;

  return (ret < 0) ? ret : PTP_TLV_MIN_SIZE + len;
}

static void
signaling_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len)
{
  // 34 bytes header and then 10 bytes targetPortIdentity
  size_t tlv_offset = sizeof(struct ptp_header) + PTP_PORT_ID_SIZE;
  ssize_t req_remaining = req_len - tlv_offset;
  ssize_t tlv_size;

  while ((tlv_size = tlv_handle(daemon, req + tlv_offset, req_remaining)) > 0)
    {
      req_remaining -= tlv_size;
      tlv_offset += tlv_size;
    }

  if (tlv_size < 0)
    airptp_hexdump("Received invalid or unknown PTP_MSGTYPE_SIGNALING", req, req_len);
}

static void
management_handle(struct airptp_daemon *daemon, uint8_t *req, ssize_t req_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addr_len)
{
  airptp_hexdump("Received PTP_MSGTYPE_MANAGEMENT", req, req_len);
}


/* ----------------------------- Message sending ---------------------------- */

static int
localhost_msg_send(void *msg, size_t msg_len, unsigned short port)
{
  struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
  struct addrinfo *info = NULL;
  char strport[8];
  int fd = -1;
  ssize_t len = -1;

  snprintf(strport, sizeof(strport), "%hu", port);
  if (getaddrinfo("localhost", strport, &hints, &info) != 0)
    goto error;

  fd = socket(info->ai_family, SOCK_DGRAM, 0);
  if (fd < 0)
    goto error;

  len = sendto(fd, msg, msg_len, 0, info->ai_addr, info->ai_addrlen);

 error:
  if (fd >= 0)
    close(fd);
  if (info)
    freeaddrinfo(info);

  return (len == msg_len) ? 0 : -1;
}

static void
peers_msg_send(struct airptp_daemon *daemon, void *msg, size_t msg_len, struct airptp_service *svc)
{
  struct airptp_peer *peer;
  ssize_t len;
  union utils_net_sockaddr naddr;
  uint8_t *msg_bin = msg;
  uint64_t now = time(NULL);

  for (int i = 0; i < daemon->num_peers; i++) {
    peer = &daemon->peers[i];

    peer->is_active = (peer->last_seen + AIRPTP_STALE_SECS > now);
    if (!peer->is_active)
      continue;

    // Copy because we don't want to modify list elements
    memcpy(&naddr, &peer->naddr, peer->naddr_len);

    port_set(&naddr, svc->port);
    len = utils_net_sendto(&svc->socket, msg, msg_len, &naddr);
    if (len < 0) {
      airptp_logmsg("Error sending PTP msg %02x: %s", msg_bin[0], strerror(errno));
      peer->is_active = false; // Will be removed deferred by peers_prune()
    }
    else if (len != msg_len)
      airptp_logmsg("Incomplete send of msg %02x", msg_bin[0]);
    else
      log_sent(msg_bin, svc->port);
  }
}

void
ptp_msg_announce_send(struct airptp_daemon *daemon)
{
  struct ptp_announce_message annnounce;
  struct ptp_timestamp ts = { 0 };

  // iOS just sends 0 as originTimestamp, we do the same
  msg_announce_make(&annnounce, daemon->clock_id, daemon->announce_seq, ts, daemon->role);
  peers_msg_send(daemon, &annnounce, sizeof(annnounce), &daemon->general_svc);

  daemon->announce_seq++;
}

void
ptp_msg_signaling_send(struct airptp_daemon *daemon)
{
  struct ptp_signaling_message signaling;

  // TODO iOS sets targetPortIdentity, we probably also should
  msg_signaling_make(&signaling, daemon->clock_id, daemon->signaling_seq, NULL);
  peers_msg_send(daemon, &signaling, sizeof(signaling), &daemon->general_svc);

  daemon->signaling_seq++;
}

void
ptp_msg_sync_send(struct airptp_daemon *daemon)
{
  struct ptp_sync_message sync;
  struct ptp_follow_up_message followup;
  struct ptp_timestamp ts = { 0 };

  // Two-step PTP is a Sync with a 0 ts and then a Follow-Up with the ts of Sync
  msg_sync_make(&sync, daemon->clock_id, daemon->sync_seq, ts);
  ts = current_time_get();

  peers_msg_send(daemon, &sync, sizeof(sync), &daemon->event_svc);

  // Send Follow Up with precise timestamp after a small delay
  usleep(100);
  msg_sync_follow_up_make(&followup, daemon->clock_id, daemon->sync_seq, ts);
  peers_msg_send(daemon, &followup, sizeof(followup), &daemon->general_svc);

  daemon->sync_seq++;
}

int
ptp_msg_peer_add_send(struct airptp_peer *peer, struct airptp_handle *hdl, unsigned short port)
{
  struct ptp_peer_signaling_message msg;

  msg_peer_add_make(&msg, peer, hdl->daemon_info.clock_id);
  return localhost_msg_send(&msg, sizeof(msg), port);
}

int
ptp_msg_peer_del_send(struct airptp_peer *peer, struct airptp_handle *hdl, unsigned short port)
{
  struct ptp_peer_signaling_message msg;

  msg_peer_del_make(&msg, peer, hdl->daemon_info.clock_id);
  return localhost_msg_send(&msg, sizeof(msg), port);
}


/* ----------------------------- Message handler ---------------------------- */

void
ptp_msg_handle(struct airptp_daemon *daemon, uint8_t *msg, size_t msg_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addrlen, int64_t rx_ns)
{
  uint8_t msg_type = msg[0] & 0x0F; // Only lower bits are message type

  switch (msg_type)
    {
      case PTP_MSGTYPE_ANNOUNCE:
	announce_handle(daemon, msg, msg_len, peer_addr, peer_addrlen, rx_ns);
	break;
      case PTP_MSGTYPE_SYNC:
	sync_handle(daemon, msg, msg_len, peer_addr, peer_addrlen, rx_ns);
	break;
      case PTP_MSGTYPE_FOLLOW_UP:
	follow_up_handle(daemon, msg, msg_len, peer_addr, peer_addrlen, rx_ns);
	break;
      case PTP_MSGTYPE_DELAY_REQ:
	delay_msg_handle(daemon, msg, msg_len, peer_addr, peer_addrlen);
	break;
      case PTP_MSGTYPE_DELAY_RESP:
	delay_resp_handle(daemon, msg, msg_len, peer_addr, peer_addrlen, rx_ns);
	break;
      case PTP_MSGTYPE_PDELAY_REQ:
	pdelay_msg_handle(daemon, msg, msg_len, peer_addr, peer_addrlen);
	break;
      case PTP_MSGTYPE_SIGNALING:
	signaling_handle(daemon, msg, msg_len, peer_addr, peer_addrlen);
	break;
      case PTP_MSGTYPE_MANAGEMENT:
	management_handle(daemon, msg, msg_len, peer_addr, peer_addrlen);
	break;
      default:
	airptp_hexdump("Received unhandled message", msg, msg_len);
	return;
    }
}

int
ptp_msg_handle_init(void)
{
  int i;
  int n;

  // Check alignment of enum and array indices
  n = 0;
  for (i = 0, n++; i < ARRAY_SIZE(ptp_tlv_apple_subtypes); i++)
    assert(ptp_tlv_apple_subtypes[i].index == i);
  for (i = 0, n++; i < ARRAY_SIZE(ptp_tlv_ieee_subtypes); i++)
    assert(ptp_tlv_ieee_subtypes[i].index == i);
  for (i = 0, n++; i < ARRAY_SIZE(ptp_tlv_own_subtypes); i++)
    assert(ptp_tlv_own_subtypes[i].index == i);
  for (i = 0; i < ARRAY_SIZE(ptp_tlv_orgs); i++)
    assert(ptp_tlv_orgs[i].index == i);
  assert(n == i);

  return 0;
}
