#include "config.release.h"

#define CONFIG_YADECAP_AUTOTEST

#undef CONFIG_YADECAP_MBUFS_COUNT
#define CONFIG_YADECAP_MBUFS_COUNT (256)

#undef CONFIG_YADECAP_NAT64STATELESS_TRANSLATIONS_SIZE
#define CONFIG_YADECAP_NAT64STATELESS_TRANSLATIONS_SIZE (1024)

#undef YANET_CONFIG_ACL_COUNTERS_SIZE
#define YANET_CONFIG_ACL_COUNTERS_SIZE (3000)

#undef YANET_CONFIG_ACL_STATES4_HT_SIZE
#define YANET_CONFIG_ACL_STATES4_HT_SIZE (8 * 1024)

#undef YANET_CONFIG_ACL_STATES6_HT_SIZE
#define YANET_CONFIG_ACL_STATES6_HT_SIZE (8 * 1024)

#undef CONFIG_YADECAP_TUN64_HT_SIZE
#define CONFIG_YADECAP_TUN64_HT_SIZE (4 * 1024)

#undef CONFIG_YADECAP_TUN64_HT_EXTENDED_SIZE
#define CONFIG_YADECAP_TUN64_HT_EXTENDED_SIZE (1024)

#undef YANET_CONFIG_ROUTE_VALUES_SIZE
#define YANET_CONFIG_ROUTE_VALUES_SIZE (256)

#undef YANET_CONFIG_ROUTE_TUNNEL_VALUES_SIZE
#define YANET_CONFIG_ROUTE_TUNNEL_VALUES_SIZE (256)

#undef YANET_CONFIG_DREGRESS_VALUES_SIZE
#define YANET_CONFIG_DREGRESS_VALUES_SIZE (128)

#undef YANET_CONFIG_DREGRESS_HT_SIZE
#define YANET_CONFIG_DREGRESS_HT_SIZE (4 * 1024)

#undef YANET_CONFIG_DREGRESS_HT_EXTENDED_SIZE
#define YANET_CONFIG_DREGRESS_HT_EXTENDED_SIZE (1024)

#undef CONFIG_YADECAP_FW_STATEFUL6_HT_EXTENDED_SIZE
#define CONFIG_YADECAP_FW_STATEFUL6_HT_EXTENDED_SIZE (1024)

#undef YANET_CONFIG_COUNTERS_SIZE
#define YANET_CONFIG_COUNTERS_SIZE (1024)

#undef YANET_CONFIG_ROUTE_TUNNEL_WEIGHTS_SIZE
#define YANET_CONFIG_ROUTE_TUNNEL_WEIGHTS_SIZE (8 * 1024)

#undef YANET_CONFIG_BALANCER_SERVICES_SIZE
#define YANET_CONFIG_BALANCER_SERVICES_SIZE (1024)

#undef YANET_CONFIG_BALANCER_REALS_SIZE
#define YANET_CONFIG_BALANCER_REALS_SIZE (1024)

#undef YANET_CONFIG_BALANCER_WEIGHTS_SIZE
#define YANET_CONFIG_BALANCER_WEIGHTS_SIZE (8192)

#undef YANET_CONFIG_BALANCER_STATE_HT_SIZE
#define YANET_CONFIG_BALANCER_STATE_HT_SIZE (1024)

#undef YANET_CONFIG_RING_PRIORITY_RATIO
#define YANET_CONFIG_RING_PRIORITY_RATIO (4)

#undef YANET_CONFIG_BALANCER_WLC_RECONFIGURE
#define YANET_CONFIG_BALANCER_WLC_RECONFIGURE (1)

#undef YANET_CONFIG_BALANCER_WLC_DEFAULT_POWER
#define YANET_CONFIG_BALANCER_WLC_DEFAULT_POWER (10)

#undef YANET_CONFIG_NAT64STATEFUL_HT_SIZE
#define YANET_CONFIG_NAT64STATEFUL_HT_SIZE (64 * 1024)

#undef YANET_CONFIG_ACL_TREE_CHUNKS_BUCKET_SIZE
#define YANET_CONFIG_ACL_TREE_CHUNKS_BUCKET_SIZE (1024)
