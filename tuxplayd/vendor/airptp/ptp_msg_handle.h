#ifndef __PTP_MSG_HANDLE_H__
#define __PTP_MSG_HANDLE_H__

void
ptp_msg_announce_send(struct airptp_daemon *daemon);

void
ptp_msg_signaling_send(struct airptp_daemon *daemon);

void
ptp_msg_sync_send(struct airptp_daemon *daemon);

int
ptp_msg_peer_add_send(struct airptp_peer *peer, struct airptp_handle *hdl, unsigned short port);

int
ptp_msg_peer_del_send(struct airptp_peer *peer, struct airptp_handle *hdl, unsigned short port);

// rx_ns: CLOCK_MONOTONIC when the message was read off the socket
void
ptp_msg_handle(struct airptp_daemon *daemon, uint8_t *msg, size_t msg_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addrlen, int64_t rx_ns);

// Follower role: send the next Delay_Req to the clock we follow
void
ptp_msg_delay_req_send(struct airptp_daemon *daemon);

// Follower role: once a second, fold the timing samples into the timeline
void
ptp_follower_servo(struct airptp_daemon *daemon);

// Whether we are grandmaster right now, i.e. should announce and send Sync
bool
ptp_follower_is_grandmaster(struct airptp_daemon *daemon);

void
ptp_follower_peers_changed(struct airptp_daemon *daemon);

int
ptp_msg_handle_init(void);

#endif // __PTP_MSG_HANDLE_H__
