/*
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#ifndef _if_core_net_stats_h_
#define _if_core_net_stats_h_

#include "common.h"

/***
 *** General statistics
 ***/

enum {
	MSG_UNKNOWN = 0,
	MSG_INIT,
	MSG_INIT_RESPONSE,
	MSG_BYE,
	MSG_QRP,
	MSG_HSEP,
	MSG_RUDP,
	MSG_VENDOR,
	MSG_STANDARD,
	MSG_PUSH_REQUEST,
	MSG_SEARCH,
	MSG_SEARCH_RESULTS,
	MSG_DHT,
	MSG_DHT_PING,
	MSG_DHT_PONG,
	MSG_DHT_STORE,
	MSG_DHT_STORE_ACK,
	MSG_DHT_FIND_NODE,
	MSG_DHT_FOUND_NODE,
	MSG_DHT_FIND_VALUE,
	MSG_DHT_VALUE,
	MSG_TOTAL,     /**< always counted (for all the above types) */
	
	MSG_TYPE_COUNT /**< number of known message types */
};

#define MSG_DHT_BASE	0xd0		/* Base in lookup table for DHT messages */

typedef enum msg_drop_reason {
	MSG_DROP_BAD_SIZE = 0,
	MSG_DROP_TOO_SMALL,
	MSG_DROP_TOO_LARGE,
	MSG_DROP_WAY_TOO_LARGE,
	MSG_DROP_UNKNOWN_TYPE,
	MSG_DROP_UNEXPECTED,
	MSG_DROP_TTL0,
	MSG_DROP_IMPROPER_HOPS_TTL,
	MSG_DROP_MAX_TTL_EXCEEDED,
	MSG_DROP_THROTTLE,
	MSG_DROP_LIMIT,
	MSG_DROP_TRANSIENT,
	MSG_DROP_PONG_UNUSABLE,
	MSG_DROP_HARD_TTL_LIMIT,
	MSG_DROP_MAX_HOP_COUNT,
	MSG_DROP_ROUTE_LOST,
	MSG_DROP_NO_ROUTE,
	MSG_DROP_DUPLICATE,
	MSG_DROP_TO_BANNED,
	MSG_DROP_FROM_BANNED,
	MSG_DROP_SHUTDOWN,
	MSG_DROP_FLOW_CONTROL,
	MSG_DROP_QUERY_NO_NUL,
	MSG_DROP_QUERY_TOO_SHORT,
	MSG_DROP_QUERY_OVERHEAD,
	MSG_DROP_BAD_URN,
	MSG_DROP_MALFORMED_SHA1,
	MSG_DROP_MALFORMED_UTF_8,
	MSG_DROP_BAD_RESULT,
	MSG_DROP_BAD_RETURN_ADDRESS,
	MSG_DROP_HOSTILE_IP,
	MSG_DROP_MORPHEUS_BOGUS,
	MSG_DROP_SPAM,
	MSG_DROP_EVIL,
	MSG_DROP_MEDIA,
	MSG_DROP_INFLATE_ERROR,
	MSG_DROP_UNKNOWN_HEADER_FLAGS,
	MSG_DROP_OWN_RESULT,
	MSG_DROP_OWN_QUERY,
	MSG_DROP_ANCIENT_QUERY,
	MSG_DROP_BLANK_SERVENT_ID,
	MSG_DROP_GUESS_MISSING_TOKEN,
	MSG_DROP_GUESS_INVALID_TOKEN,
	MSG_DROP_DHT_INVALID_TOKEN,
	MSG_DROP_DHT_TOO_MANY_STORE,
	MSG_DROP_DHT_UNPARSEABLE,
	
	MSG_DROP_REASON_COUNT /**< number of known reasons to drop a message */
} msg_drop_reason_t;

/*
 * Any change in the following enum needs to be reported to:
 *
 *	gnet_stats_general_to_string()	[core]
 *  general_type_str()				[ui/gtk]
 */

typedef enum {
	GNR_ROUTING_ERRORS = 0,
	GNR_ROUTING_TABLE_CHUNKS,
	GNR_ROUTING_TABLE_CAPACITY,
	GNR_ROUTING_TABLE_COUNT,
	GNR_ROUTING_TRANSIENT_AVOIDED,
	GNR_DUPS_WITH_HIGHER_TTL,
	GNR_SPAM_SHA1_HITS,
	GNR_SPAM_NAME_HITS,
	GNR_SPAM_FAKE_HITS,
	GNR_SPAM_DUP_HITS,
	GNR_SPAM_CAUGHT_HOSTILE_IP,
	GNR_SPAM_CAUGHT_HOSTILE_HELD,
	GNR_SPAM_IP_HELD,
	GNR_LOCAL_SEARCHES,
	GNR_LOCAL_HITS,
	GNR_LOCAL_PARTIAL_HITS,
	GNR_LOCAL_WHATS_NEW_HITS,
	GNR_LOCAL_QUERY_HITS,
	GNR_OOB_PROXIED_QUERY_HITS,
	GNR_OOB_QUERIES,
	GNR_OOB_QUERIES_STRIPPED,
	GNR_QUERY_OOB_PROXIED_DUPS,
	GNR_OOB_HITS_FOR_PROXIED_QUERIES,
	GNR_OOB_HITS_WITH_ALIEN_IP,
	GNR_OOB_HITS_IGNORED_ON_SPAMMER_HIT,
	GNR_UNCLAIMED_OOB_HITS,
	GNR_PARTIALLY_CLAIMED_OOB_HITS,
	GNR_SPURIOUS_OOB_HIT_CLAIM,
	GNR_UNREQUESTED_OOB_HITS,
	GNR_QUERY_HIT_FOR_UNTRACKED_QUERY,
	GNR_QUERY_TRACKED_MUIDS,
	GNR_QUERY_COMPACT_COUNT,
	GNR_QUERY_COMPACT_SIZE,
	GNR_QUERY_UTF8,
	GNR_QUERY_SHA1,
	GNR_QUERY_WHATS_NEW,
	GNR_QUERY_GUESS,
	GNR_QUERY_GUESS_02,
	GNR_GUESS_LINK_CACHE,
	GNR_GUESS_CACHED_QUERY_KEYS_HELD,
	GNR_GUESS_CACHED_02_HOSTS_HELD,
	GNR_GUESS_LOCAL_QUERIES,
	GNR_GUESS_LOCAL_RUNNING,
	GNR_GUESS_LOCAL_QUERY_HITS,
	GNR_GUESS_HOSTS_QUERIED,
	GNR_GUESS_HOSTS_ACKNOWLEDGED,
	GNR_BROADCASTED_PUSHES,
	GNR_PUSH_PROXY_UDP_RELAYED,
	GNR_PUSH_PROXY_TCP_RELAYED,
	GNR_PUSH_PROXY_BROADCASTED,
	GNR_PUSH_PROXY_ROUTE_NOT_PROXIED,
	GNR_PUSH_PROXY_FAILED,
	GNR_PUSH_RELAYED_VIA_LOCAL_ROUTE,
	GNR_PUSH_RELAYED_VIA_TABLE_ROUTE,
	GNR_LOCAL_DYN_QUERIES,
	GNR_LEAF_DYN_QUERIES,
	GNR_OOB_PROXIED_QUERIES,
	GNR_DYN_QUERIES_COMPLETED_FULL,
	GNR_DYN_QUERIES_COMPLETED_PARTIAL,
	GNR_DYN_QUERIES_COMPLETED_ZERO,
	GNR_DYN_QUERIES_LINGER_EXTRA,
	GNR_DYN_QUERIES_LINGER_RESULTS,
	GNR_DYN_QUERIES_LINGER_COMPLETED,
	GNR_GTKG_TOTAL_QUERIES,
	GNR_GTKG_REQUERIES,
	GNR_QUERIES_WITH_GGEP_H,
	GNR_GIV_CALLBACKS,
	GNR_GIV_DISCARDED,
	GNR_QUEUE_CALLBACKS,
	GNR_QUEUE_DISCARDED,
	GNR_UDP_BOGUS_SOURCE_IP,
	GNR_UDP_ALIEN_MESSAGE,
	GNR_UDP_UNPROCESSED_MESSAGE,
	GNR_UDP_TX_COMPRESSED,
	GNR_UDP_RX_COMPRESSED,
	GNR_UDP_LARGER_HENCE_NOT_COMPRESSED,
	GNR_CONSOLIDATED_SERVERS,
	GNR_DUP_DOWNLOADS_IN_CONSOLIDATION,
	GNR_DISCOVERED_SERVER_GUID,
	GNR_CHANGED_SERVER_GUID,
	GNR_GUID_COLLISIONS,
	GNR_OWN_GUID_COLLISIONS,
	GNR_BANNED_GUID_HELD,
	GNR_RECEIVED_KNOWN_FW_NODE_INFO,
	GNR_REVITALIZED_PUSH_ROUTES,
	GNR_COLLECTED_PUSH_PROXIES,
	GNR_ATTEMPTED_RESOURCE_SWITCHING,
	GNR_ATTEMPTED_RESOURCE_SWITCHING_AFTER_ERROR,
	GNR_SUCCESSFUL_RESOURCE_SWITCHING,
	GNR_SUCCESSFUL_PLAIN_RESOURCE_SWITCHING,
	GNR_SUCCESSFUL_RESOURCE_SWITCHING_AFTER_ERROR,
	GNR_QUEUED_AFTER_SWITCHING,
	GNR_SUNK_DATA,
	GNR_IGNORED_DATA,
	GNR_IGNORING_AFTER_MISMATCH,
	GNR_IGNORING_TO_PRESERVE_CONNECTION,
	GNR_IGNORING_DURING_AGGRESSIVE_SWARMING,
	GNR_IGNORING_REFUSED,
	GNR_CLIENT_RESOURCE_SWITCHING,
	GNR_CLIENT_PLAIN_RESOURCE_SWITCHING,
	GNR_CLIENT_FOLLOWUP_AFTER_ERROR,
	GNR_PARQ_SLOT_RESOURCE_SWITCHING,
	GNR_PARQ_RETRY_AFTER_VIOLATION,
	GNR_PARQ_RETRY_AFTER_KICK_OUT,
	GNR_PARQ_SLOT_LIMIT_OVERRIDES,
	GNR_PARQ_QUICK_SLOTS_GRANTED,
	GNR_PARQ_QUEUE_SENDING_ATTEMPTS,
	GNR_PARQ_QUEUE_SENT,
	GNR_PARQ_QUEUE_FOLLOW_UPS,
	GRN_SHA1_VERIFICATIONS,
	GRN_TTH_VERIFICATIONS,
	GNR_BITZI_TICKETS_HELD,
	GNR_QHIT_SEEDING_OF_ORPHAN,
	GNR_UPLOAD_SEEDING_OF_ORPHAN,
	GNR_DHT_ESTIMATED_SIZE,
	GNR_DHT_KBALL_THEORETICAL,
	GNR_DHT_KBALL_FURTHEST,
	GNR_DHT_KBALL_CLOSEST,
	GNR_DHT_ROUTING_BUCKETS,
	GNR_DHT_ROUTING_LEAVES,
	GNR_DHT_ROUTING_MAX_DEPTH,
	GNR_DHT_ROUTING_GOOD_NODES,
	GNR_DHT_ROUTING_STALE_NODES,
	GNR_DHT_ROUTING_PENDING_NODES,
	GNR_DHT_ROUTING_EVICTED_NODES,
	GNR_DHT_ROUTING_EVICTED_FIREWALLED_NODES,
	GNR_DHT_ROUTING_EVICTED_QUOTA_NODES,
	GNR_DHT_ROUTING_PROMOTED_PENDING_NODES,
	GNR_DHT_ROUTING_PINGED_PROMOTED_NODES,
	GNR_DHT_ROUTING_REJECTED_NODE_BUCKET_QUOTA,
	GNR_DHT_ROUTING_REJECTED_NODE_GLOBAL_QUOTA,
	GNR_DHT_COMPLETED_BUCKET_REFRESH,
	GNR_DHT_FORCED_BUCKET_REFRESH,
	GNR_DHT_FORCED_BUCKET_MERGE,
	GNR_DHT_DENIED_UNSPLITABLE_BUCKET_REFRESH,
	GNR_DHT_BUCKET_ALIVE_CHECK,
	GNR_DHT_ALIVE_PINGS_TO_GOOD_NODES,
	GNR_DHT_ALIVE_PINGS_TO_STALE_NODES,
	GNR_DHT_ALIVE_PINGS_TO_SHUTDOWNING_NODES,
	GNR_DHT_ALIVE_PINGS_AVOIDED,
	GNR_DHT_ALIVE_PINGS_SKIPPED,
	GNR_DHT_REVITALIZED_STALE_NODES,
	GNR_DHT_REJECTED_VALUE_ON_QUOTA,
	GNR_DHT_REJECTED_VALUE_ON_CREATOR,
	GNR_DHT_LOOKUP_REJECTED_NODE_ON_NET_QUOTA,
	GNR_DHT_LOOKUP_REJECTED_NODE_ON_PROXIMITY,
	GNR_DHT_LOOKUP_REJECTED_NODE_ON_DIVERGENCE,
	GNR_DHT_KEYS_HELD,
	GNR_DHT_CACHED_KEYS_HELD,
	GNR_DHT_VALUES_HELD,
	GNR_DHT_CACHED_KUID_TARGETS_HELD,
	GNR_DHT_CACHED_ROOTS_HELD,
	GNR_DHT_CACHED_ROOTS_EXACT_HITS,
	GNR_DHT_CACHED_ROOTS_APPROXIMATE_HITS,
	GNR_DHT_CACHED_ROOTS_MISSES,
	GNR_DHT_CACHED_ROOTS_KBALL_LOOKUPS,
	GNR_DHT_CACHED_ROOTS_CONTACT_REFRESHED,
	GNR_DHT_CACHED_TOKENS_HELD,
	GNR_DHT_CACHED_TOKENS_HITS,
	GNR_DHT_STABLE_NODES_HELD,
	GNR_DHT_FETCH_LOCAL_HITS,
	GNR_DHT_FETCH_LOCAL_CACHED_HITS,
	GNR_DHT_RETURNED_EXPANDED_VALUES,
	GNR_DHT_RETURNED_SECONDARY_KEYS,
	GNR_DHT_CLAIMED_SECONDARY_KEYS,
	GNR_DHT_RETURNED_EXPANDED_CACHED_VALUES,
	GNR_DHT_RETURNED_CACHED_SECONDARY_KEYS,
	GNR_DHT_CLAIMED_CACHED_SECONDARY_KEYS,
	GNR_DHT_PUBLISHED,
	GNR_DHT_REMOVED,
	GNR_DHT_STALE_REPLICATION,
	GNR_DHT_REPLICATION,
	GNR_DHT_REPUBLISH,
	GNR_DHT_SECONDARY_KEY_FETCH,
	GNR_DHT_DUP_VALUES,
	GNR_DHT_KUID_COLLISIONS,
	GNR_DHT_OWN_KUID_COLLISIONS,
	GNR_DHT_RPC_KUID_REPLY_MISMATCH,
	GNR_DHT_CACHING_ATTEMPTS,
	GNR_DHT_CACHING_SUCCESSFUL,
	GNR_DHT_CACHING_PARTIALLY_SUCCESSFUL,
	GNR_DHT_KEY_OFFLOADING_CHECKS,
	GNR_DHT_KEYS_SELECTED_FOR_OFFLOADING,
	GNR_DHT_KEY_OFFLOADING_ATTEMPTS,
	GNR_DHT_KEY_OFFLOADING_SUCCESSFUL,
	GNR_DHT_KEY_OFFLOADING_PARTIALLY_SUCCESSFUL,
	GNR_DHT_VALUES_OFFLOADED,
	GNR_DHT_PUBLISHING_ATTEMPTS,
	GNR_DHT_PUBLISHING_SUCCESSFUL,
	GNR_DHT_PUBLISHING_PARTIALLY_SUCCESSFUL,
	GNR_DHT_PUBLISHING_SATISFACTORY,
	GNR_DHT_REPUBLISHED_LATE,
	GNR_DHT_PUBLISHING_TO_SELF,
	GNR_DHT_PUBLISHING_BG_ATTEMPTS,
	GNR_DHT_PUBLISHING_BG_IMPROVEMENTS,
	GNR_DHT_PUBLISHING_BG_SUCCESSFUL,
	GNR_DHT_SHA1_DATA_TYPE_COLLISIONS,
	GNR_DHT_PASSIVELY_PROTECTED_LOOKUP_PATH,
	GNR_DHT_ACTIVELY_PROTECTED_LOOKUP_PATH,
	GNR_DHT_ALT_LOC_LOOKUPS,
	GNR_DHT_PUSH_PROXY_LOOKUPS,
	GNR_DHT_SUCCESSFUL_ALT_LOC_LOOKUPS,
	GNR_DHT_SUCCESSFUL_PUSH_PROXY_LOOKUPS,
	GNR_DHT_SUCCESSFUL_NODE_PUSH_ENTRY_LOOKUPS,
	GNR_DHT_SEEDING_OF_ORPHAN,

	GNR_TYPE_COUNT /* number of general stats */
} gnr_stats_t;

#define STATS_FLOWC_COLUMNS 10	/**< Type, 0..7, 8+ */
#define STATS_RECV_COLUMNS 10	/**< -"- */

typedef struct gnet_stat {
	guint64 drop_reason[MSG_DROP_REASON_COUNT][MSG_TYPE_COUNT];

	struct {
		guint64 received[MSG_TYPE_COUNT];
		guint64 expired[MSG_TYPE_COUNT];
		guint64 dropped[MSG_TYPE_COUNT];
		guint64 queued[MSG_TYPE_COUNT];
		guint64 relayed[MSG_TYPE_COUNT];
		guint64 gen_queued[MSG_TYPE_COUNT];
		guint64 generated[MSG_TYPE_COUNT];
		guint64 received_hops[STATS_RECV_COLUMNS][MSG_TYPE_COUNT];
		guint64 received_ttl[STATS_RECV_COLUMNS][MSG_TYPE_COUNT];
		guint64 flowc_hops[STATS_FLOWC_COLUMNS][MSG_TYPE_COUNT];
		guint64 flowc_ttl[STATS_FLOWC_COLUMNS][MSG_TYPE_COUNT];
	} pkg, byte;

	guint64 general[GNR_TYPE_COUNT];
} gnet_stats_t;

typedef enum {
	BW_GNET_IN,
	BW_GNET_OUT,
	BW_HTTP_IN,
	BW_HTTP_OUT,
	BW_LEAF_IN,
	BW_LEAF_OUT,
	BW_GNET_UDP_IN,
	BW_GNET_UDP_OUT,
	BW_DHT_IN,
	BW_DHT_OUT
} gnet_bw_source;

typedef struct gnet_bw_stats {
	gboolean enabled;
	guint32  current;
	guint32  average;
	guint32  limit;
} gnet_bw_stats_t;

/***
 *** General statistics
 ***/

#ifdef CORE_SOURCES

void gnet_stats_get(gnet_stats_t *stats);
void gnet_stats_tcp_get(gnet_stats_t *stats);
void gnet_stats_udp_get(gnet_stats_t *stats);
void gnet_get_bw_stats(gnet_bw_source type, gnet_bw_stats_t *stats);
const char *gnet_stats_drop_reason_to_string(msg_drop_reason_t reason);
const char *gnet_stats_general_to_string(gnr_stats_t type);

#endif /* CORE_SOURCES */

#endif /* _if_core_net_stats_h_ */

/* vi: set ts=4 sw=4 cindent: */
