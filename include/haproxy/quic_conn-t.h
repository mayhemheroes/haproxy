/*
 * include/haproxy/quic_conn-t.h
 *
 * Copyright 2019 HAProxy Technologies, Frederic Lecaille <flecaille@haproxy.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_QUIC_CONN_T_H
#define _HAPROXY_QUIC_CONN_T_H

#ifdef USE_QUIC
#ifndef USE_OPENSSL
#error "Must define USE_OPENSSL"
#endif

#include <sys/socket.h>

#include <haproxy/cbuf-t.h>
#include <haproxy/list.h>

#include <haproxy/openssl-compat.h>
#include <haproxy/mux_quic-t.h>
#include <haproxy/quic_cid-t.h>
#include <haproxy/quic_cc-t.h>
#include <haproxy/quic_loss-t.h>
#include <haproxy/quic_openssl_compat-t.h>
#include <haproxy/quic_stats-t.h>
#include <haproxy/quic_tls-t.h>
#include <haproxy/quic_tp-t.h>
#include <haproxy/task.h>

#include <import/ebtree-t.h>

typedef unsigned long long ull;

#define QUIC_PROTOCOL_VERSION_DRAFT_29   0xff00001d /* draft-29 */
#define QUIC_PROTOCOL_VERSION_1          0x00000001 /* V1 */
#define QUIC_PROTOCOL_VERSION_2          0x6b3343cf /* V2 */

#define QUIC_INITIAL_IPV4_MTU      1252 /* (bytes) */
#define QUIC_INITIAL_IPV6_MTU      1232

/* The minimum length of Initial packets. */
#define QUIC_INITIAL_PACKET_MINLEN 1200

/* Lengths of the QUIC CIDs generated by the haproxy implementation. Current
 * value is used to match 64 bits hash produced when deriving ODCID.
 */
#define QUIC_HAP_CID_LEN               8

/* Common definitions for short and long QUIC packet headers. */
/* QUIC original destination connection ID minial length */
#define QUIC_ODCID_MINLEN              8 /* bytes */
/*
 * All QUIC packets with long headers are made of at least (in bytes):
 * flags(1), version(4), DCID length(1), DCID(0..20), SCID length(1), SCID(0..20)
 */
#define QUIC_LONG_PACKET_MINLEN            7
/* DCID offset from beginning of a long packet */
#define QUIC_LONG_PACKET_DCID_OFF         (1 + sizeof(uint32_t))
/*
 * All QUIC packets with short headers are made of at least (in bytes):
 * flags(1), DCID(0..20)
 */
#define QUIC_SHORT_PACKET_MINLEN           1
/* DCID offset from beginning of a short packet */
#define QUIC_SHORT_PACKET_DCID_OFF         1

/* Byte 0 of QUIC packets. */
#define QUIC_PACKET_LONG_HEADER_BIT  0x80 /* Long header format if set, short if not. */
#define QUIC_PACKET_FIXED_BIT        0x40 /* Must always be set for all the headers. */

/* Tokens formats */
/* Format for Retry tokens sent by a QUIC server */
#define QUIC_TOKEN_FMT_RETRY 0x9c
/* Format for token sent for new connections after a Retry token was sent */
#define  QUIC_TOKEN_FMT_NEW  0xb7
/* Salt length used to derive retry token secret */
#define QUIC_RETRY_TOKEN_SALTLEN       16 /* bytes */
/* Retry token duration */
#define QUIC_RETRY_DURATION_SEC       10
/* Default Retry threshold */
#define QUIC_DFLT_RETRY_THRESHOLD     100 /* in connection openings */
/* Default limit of loss detection on a single frame. If exceeded, connection is closed. */
#define QUIC_DFLT_MAX_FRAME_LOSS       10

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+
 * |1|1|T|T|X|X|X|X|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                         Version (32)                          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | DCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Destination Connection ID (0..160)            ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | SCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 Source Connection ID (0..160)               ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *                      Long Header Packet Format
 */

/* Two bits (T) for QUIC packet types. */
#define QUIC_PACKET_TYPE_BITMASK     0x03
#define QUIC_PACKET_TYPE_SHIFT       4

enum quic_pkt_type {
	QUIC_PACKET_TYPE_INITIAL,
	QUIC_PACKET_TYPE_0RTT,
	QUIC_PACKET_TYPE_HANDSHAKE,
	QUIC_PACKET_TYPE_RETRY,
	/*
	 * The following one is not defined by the RFC but we define it for our
	 * own convenience.
	 */
	QUIC_PACKET_TYPE_SHORT,

	/* unknown type */
	QUIC_PACKET_TYPE_UNKNOWN
};

/* Packet number field length. */
#define QUIC_PACKET_PNL_BITMASK      0x03
#define QUIC_PACKET_PN_MAXLEN        4

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+
 * |0|1|S|R|R|K|P|P|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                Destination Connection ID (0..160)           ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     Packet Number (8/16/24/32)              ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     Protected Payload (*)                   ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *                      Short Header Packet Format
 */

/* Bit (S) of short header. */
#define QUIC_PACKET_SPIN_BIT         0x20

/* Reserved Bits (R):  The next two bits of byte 0 are reserved.
 * These bits are protected using header protection
 * (see Section 5.4 of [QUIC-TLS]). The value included
 * prior to protection MUST be set to 0. An endpoint MUST treat
 * receipt of a packet that has a non-zero value for these bits,
 * after removing both packet and header protection, as a connection
 * error of type PROTOCOL_VIOLATION. Discarding such a packet after
 * only removing header protection can expose the endpoint to attacks
 * (see Section 9.3 of [QUIC-TLS]).
 */
#define QUIC_PACKET_RESERVED_BITS    0x18 /* (protected) */

#define QUIC_PACKET_KEY_PHASE_BIT    0x04 /* (protected) */

/* The maximum number of QUIC packets stored by the fd I/O handler by QUIC
 * connection. Must be a power of two.
 */
#define QUIC_CONN_MAX_PACKET  64

#define QUIC_STATELESS_RESET_PACKET_HEADER_LEN 5
#define QUIC_STATELESS_RESET_PACKET_MINLEN     (22 + QUIC_HAP_CID_LEN)

/* Similar to kernel min()/max() definitions. */
#define QUIC_MIN(a, b) ({ \
    typeof(a) _a = (a);   \
    typeof(b) _b = (b);   \
    (void) (&_a == &_b);  \
    _a < _b ? _a : _b; })

#define QUIC_MAX(a, b) ({ \
    typeof(a) _a = (a);   \
    typeof(b) _b = (b);   \
    (void) (&_a == &_b);  \
    _a > _b ? _a : _b; })

/* Size of the QUIC RX buffer for the connections */
#define QUIC_CONN_RX_BUFSZ (1UL << 16)

extern struct pool_head *pool_head_quic_crypto_buf;

struct quic_version {
	uint32_t num;
	const unsigned char *initial_salt;
	size_t initial_salt_len;
	const unsigned char *key_label;
	size_t key_label_len;
	const unsigned char *iv_label;
	size_t iv_label_len;
	const unsigned char *hp_label;
	size_t hp_label_len;
	const unsigned char *ku_label;
	size_t ku_label_len;
	/* Retry tag */
	const unsigned char *retry_tag_key;
	const unsigned char *retry_tag_nonce;
};

extern const struct quic_version quic_versions[];
extern const size_t quic_versions_nb;
extern const struct quic_version *preferred_version;

/* unused: 0x01 */
/* Flag the packet number space as requiring an ACK frame to be sent. */
#define QUIC_FL_PKTNS_ACK_REQUIRED  (1UL << 1)
/* Flag the packet number space as needing probing */
#define QUIC_FL_PKTNS_PROBE_NEEDED  (1UL << 2)
/* Flag the packet number space as having received a packet with a new largest
 * packet number, to be acknowledege
 */
#define QUIC_FL_PKTNS_NEW_LARGEST_PN (1UL << 3)

/* The maximum number of dgrams which may be sent upon PTO expirations. */
#define QUIC_MAX_NB_PTO_DGRAMS         2

/* QUIC datagram */
struct quic_dgram {
	void *owner;
	unsigned char *buf;
	size_t len;
	unsigned char *dcid;
	size_t dcid_len;
	struct sockaddr_storage saddr;
	struct sockaddr_storage daddr;
	struct quic_conn *qc;

	struct list recv_list; /* elemt to quic_receiver_buf <dgram_list>. */
	struct mt_list handler_list; /* elem to quic_dghdlr <dgrams>. */
};

/* The QUIC packet numbers are 62-bits integers */
#define QUIC_MAX_PACKET_NUM      ((1ULL << 62) - 1)

/* QUIC datagram handler */
struct quic_dghdlr {
	struct mt_list dgrams;
	struct tasklet *task;
};

/* Structure to store enough information about the RX CRYPTO frames. */
struct quic_rx_crypto_frm {
	struct eb64_node offset_node;
	uint64_t len;
	const unsigned char *data;
	struct quic_rx_packet *pkt;
};

#define QUIC_CRYPTO_BUF_SHIFT  10
#define QUIC_CRYPTO_BUF_MASK   ((1UL << QUIC_CRYPTO_BUF_SHIFT) - 1)
/* The maximum allowed size of CRYPTO data buffer provided by the TLS stack. */
#define QUIC_CRYPTO_BUF_SZ    (1UL << QUIC_CRYPTO_BUF_SHIFT) /* 1 KB */

/* The maximum number of bytes of CRYPTO data in flight during handshakes. */
#define QUIC_CRYPTO_IN_FLIGHT_MAX 4096

/*
 * CRYPTO buffer struct.
 * Such buffers are used to send CRYPTO data.
 */
struct quic_crypto_buf {
	unsigned char data[QUIC_CRYPTO_BUF_SZ];
	size_t sz;
};

/* Crypto data stream (one by encryption level) */
struct quic_cstream {
	struct {
		uint64_t offset;       /* absolute current base offset of ncbuf */
		struct ncbuf ncbuf;    /* receive buffer - can handle out-of-order offset frames */
	} rx;
	struct {
		uint64_t offset;      /* last offset of data ready to be sent */
		uint64_t sent_offset; /* last offset sent by transport layer */
		struct buffer buf;    /* transmit buffer before sending via xprt */
	} tx;

	struct qc_stream_desc *desc;
};

struct quic_path {
	/* Control congestion. */
	struct quic_cc cc;
	/* Packet loss detection information. */
	struct quic_loss loss;

	/* MTU. */
	size_t mtu;
	/* Congestion window. */
	uint64_t cwnd;
	/* The current maximum congestion window value reached. */
	uint64_t mcwnd;
	/* The maximum congestion window value which can be reached. */
	uint64_t max_cwnd;
	/* Minimum congestion window. */
	uint64_t min_cwnd;
	/* Prepared data to be sent (in bytes). */
	uint64_t prep_in_flight;
	/* Outstanding data (in bytes). */
	uint64_t in_flight;
	/* Number of in flight ack-eliciting packets. */
	uint64_t ifae_pkts;
};

/* Status of the connection/mux layer. This defines how to handle app data.
 *
 * During a standard quic_conn lifetime it transitions like this :
 * QC_MUX_NULL -> QC_MUX_READY -> QC_MUX_RELEASED
 */
enum qc_mux_state {
	QC_MUX_NULL,     /* not allocated, data should be buffered */
	QC_MUX_READY,    /* allocated, ready to handle data */
	QC_MUX_RELEASED, /* released, data can be dropped */
};

/* Counters at QUIC connection level */
struct quic_conn_cntrs {
	long long dropped_pkt;           /* total number of dropped packets */
	long long dropped_pkt_bufoverrun;/* total number of dropped packets because of buffer overrun */
	long long dropped_parsing;       /* total number of dropped packets upon parsing errors */
	long long socket_full;           /* total number of EAGAIN errors on sendto() calls */
	long long sendto_err;            /* total number of errors on sendto() calls, EAGAIN excepted */
	long long sendto_err_unknown;    /* total number of errors on sendto() calls which are currently not supported */
	long long sent_pkt;              /* total number of sent packets */
	long long lost_pkt;              /* total number of lost packets */
	long long conn_migration_done;   /* total number of connection migration handled */
	/* Streams related counters */
	long long data_blocked;              /* total number of times DATA_BLOCKED frame was received */
	long long stream_data_blocked;       /* total number of times STREAM_DATA_BLOCKED frame was received */
	long long streams_blocked_bidi;      /* total number of times STREAMS_BLOCKED_BIDI frame was received */
	long long streams_blocked_uni;       /* total number of times STREAMS_BLOCKED_UNI frame was received */
};

/* Flags at connection level */
#define QUIC_FL_CONN_ANTI_AMPLIFICATION_REACHED  (1U << 0)
#define QUIC_FL_CONN_SPIN_BIT                    (1U << 1) /* Spin bit set by remote peer */
#define QUIC_FL_CONN_NEED_POST_HANDSHAKE_FRMS    (1U << 2) /* HANDSHAKE_DONE must be sent */
#define QUIC_FL_CONN_LISTENER                    (1U << 3)
#define QUIC_FL_CONN_ACCEPT_REGISTERED           (1U << 4)
#define QUIC_FL_CONN_TX_MUX_CONTEXT              (1U << 5) /* sending in progress from the MUX layer */
#define QUIC_FL_CONN_IDLE_TIMER_RESTARTED_AFTER_READ (1U << 6)
#define QUIC_FL_CONN_RETRANS_NEEDED              (1U << 7)
#define QUIC_FL_CONN_RETRANS_OLD_DATA            (1U << 8) /* retransmission in progress for probing with already sent data */
#define QUIC_FL_CONN_TLS_ALERT                   (1U << 9)
#define QUIC_FL_CONN_AFFINITY_CHANGED            (1U << 10) /* qc_finalize_affinity_rebind() must be called to finalize affinity rebind */
/* gap here */
#define QUIC_FL_CONN_HALF_OPEN_CNT_DECREMENTED   (1U << 11) /* The half-open connection counter was decremented */
#define QUIC_FL_CONN_HANDSHAKE_SPEED_UP          (1U << 12) /* Handshake speeding up was done */
#define QUIC_FL_CONN_ACK_TIMER_FIRED             (1U << 13) /* idle timer triggered for acknowledgements */
#define QUIC_FL_CONN_IO_TO_REQUEUE               (1U << 14) /* IO handler must be requeued on new thread after connection migration */
#define QUIC_FL_CONN_IPKTNS_DCD                  (1U << 15) /* Initial packet number space discarded  */
#define QUIC_FL_CONN_HPKTNS_DCD                  (1U << 16) /* Handshake packet number space discarded  */
#define QUIC_FL_CONN_PEER_VALIDATED_ADDR         (1U << 17) /* Peer address is considered as validated for this connection. */
#define QUIC_FL_CONN_TO_KILL                     (1U << 24) /* Unusable connection, to be killed */
#define QUIC_FL_CONN_TX_TP_RECEIVED              (1U << 25) /* Peer transport parameters have been received (used for the transmitting part) */
#define QUIC_FL_CONN_FINALIZED                   (1U << 26) /* QUIC connection finalized (functional, ready to send/receive) */
/* gap here */
#define QUIC_FL_CONN_EXP_TIMER                   (1U << 28) /* timer has expired, quic-conn can be freed */
#define QUIC_FL_CONN_CLOSING                     (1U << 29) /* closing state, entered on CONNECTION_CLOSE emission */
#define QUIC_FL_CONN_DRAINING                    (1U << 30) /* draining state, entered on CONNECTION_CLOSE reception */
#define QUIC_FL_CONN_IMMEDIATE_CLOSE             (1U << 31) /* A CONNECTION_CLOSE must be sent */

#define QUIC_CONN_COMMON                               \
    struct {                                           \
        /* Connection owned socket FD. */              \
        int fd;                                        \
        unsigned int flags;                            \
        struct quic_err err;                           \
        /* When in closing state, number of packet before sending CC */  \
        unsigned int nb_pkt_for_cc;                    \
        /* When in closing state, number of packet since receiving CC */ \
        unsigned int nb_pkt_since_cc;                  \
        struct wait_event wait_event;                  \
        struct wait_event *subs;                       \
        struct sockaddr_storage local_addr;            \
        struct sockaddr_storage peer_addr;             \
        struct {                                       \
            /* Number of bytes for prepared packets */ \
            uint64_t prep;                             \
            /* Number of sent bytes. */                \
            uint64_t tx;                               \
            /* Number of received bytes. */            \
            uint64_t rx;                               \
        } bytes;                                       \
        /* First DCID used by client on its Initial packet. */                 \
        struct quic_cid odcid;                                                 \
        /* DCID of our endpoint - not updated when a new DCID is used */       \
        struct quic_cid dcid;                                                  \
        /* first SCID of our endpoint - not updated when a new SCID is used */ \
        struct quic_cid scid;                                                  \
        /* tree of quic_connection_id - used to match a received packet DCID   \
         * with a connection                                                   \
         */                                                                    \
        struct eb_root *cids;                                                  \
        struct listener *li; /* only valid for frontend connections */         \
        /* Idle timer task */                                                  \
        struct task *idle_timer_task;                                          \
        unsigned int idle_expire;                                              \
        /* QUIC connection level counters */                                   \
        struct quic_conn_cntrs cntrs;                                          \
        struct connection *conn;                                               \
    }

struct quic_conn {
	QUIC_CONN_COMMON;
	/* Used only to reach the tasklet for the I/O handler from this
	 * quic_conn object.
	 */
	struct ssl_sock_ctx *xprt_ctx;
	const struct quic_version *original_version;
	const struct quic_version *negotiated_version;
	/* Negotiated version Initial TLS context */
	struct quic_tls_ctx *nictx;
	/* QUIC transport parameters TLS extension */
	int tps_tls_ext;
	int state;
	enum qc_mux_state mux_state; /* status of the connection/mux layer */
#ifdef USE_QUIC_OPENSSL_COMPAT
	unsigned char enc_params[QUIC_TP_MAX_ENCLEN]; /* encoded QUIC transport parameters */
	size_t enc_params_len;
#endif

	uint64_t next_cid_seq_num;
	/* Initial hash computed from first ID (derived from ODCID).
	 * it could be reused to derive extra CIDs from the same hash
	 */
	uint64_t hash64;

	/* Initial encryption level */
	struct quic_enc_level *iel;
	/* 0-RTT encryption level */
	struct quic_enc_level *eel;
	/* Handshake encryption level */
	struct quic_enc_level *hel;
	/* 1-RTT encryption level */
	struct quic_enc_level *ael;
	/* List of allocated QUIC TLS encryption level */
	struct list qel_list;

	struct quic_pktns *ipktns;
	struct quic_pktns *hpktns;
	struct quic_pktns *apktns;
	/* List of packet number spaces attached to this connection */
	struct list pktns_list;

#ifdef USE_QUIC_OPENSSL_COMPAT
	struct quic_openssl_compat openssl_compat;
#endif

	struct {
		/* Transport parameters sent by the peer */
		struct quic_transport_params params;
		/* Send buffer used to write datagrams. */
		struct buffer buf;
		/* Send buffer used to send a "connection close" datagram . */
		struct buffer cc_buf;
		char *cc_buf_area;
		/* Length of the "connection close" datagram. */
		size_t cc_dgram_len;
	} tx;
	struct {
		/* Transport parameters the peer will receive */
		struct quic_transport_params params;
		/* RX buffer */
		struct buffer buf;
		struct list pkt_list;
		struct {
			/* Number of open or closed streams */
			uint64_t nb_streams;
		} strms[QCS_MAX_TYPES];
	} rx;
	struct {
		struct quic_tls_kp prv_rx;
		struct quic_tls_kp nxt_rx;
		struct quic_tls_kp nxt_tx;
	} ku;
	unsigned int max_ack_delay;
	unsigned int max_idle_timeout;
	struct quic_path paths[1];
	struct quic_path *path;

	struct mt_list accept_list; /* chaining element used for accept, only valid for frontend connections */

	struct eb_root streams_by_id; /* qc_stream_desc tree */
	int stream_buf_count; /* total count of allocated stream buffers for this connection */

	/* MUX */
	struct qcc *qcc;
	struct task *timer_task;
	unsigned int timer;
	unsigned int ack_expire;
	/* Handshake expiration date */
	unsigned int hs_expire;

	const struct qcc_app_ops *app_ops;
	/* Proxy counters */
	struct quic_counters *prx_counters;

	struct list el_th_ctx; /* list elem in ha_thread_ctx */
	struct list back_refs; /* list head of CLI context currently dumping this connection. */
	unsigned int qc_epoch; /* delimiter for newer instances started after "show quic". */
};

/* QUIC connection in "connection close" state. */
struct quic_cc_conn {
	QUIC_CONN_COMMON;
	char *cc_buf_area;
	/* Length of the "connection close" datagram. */
	size_t cc_dgram_len;
};

#endif /* USE_QUIC */
#endif /* _HAPROXY_QUIC_CONN_T_H */
