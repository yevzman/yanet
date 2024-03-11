#include "config.release.h"

#undef YANET_CONFIG_ACL_NETWORK_DESTINATION_HT_SIZE
#define YANET_CONFIG_ACL_NETWORK_DESTINATION_HT_SIZE (1024 * 1024)

#undef YANET_CONFIG_ACL_NETWORK_SOURCE_LPM6_CHUNKS_SIZE
#define YANET_CONFIG_ACL_NETWORK_SOURCE_LPM6_CHUNKS_SIZE (28 * 8 * 1024)

#undef YANET_CONFIG_ACL_NETWORK_DESTINATION_LPM6_CHUNKS_SIZE
#define YANET_CONFIG_ACL_NETWORK_DESTINATION_LPM6_CHUNKS_SIZE (4 * 1024)

#undef YANET_CONFIG_ACL_NETWORK_TABLE_SIZE
#define YANET_CONFIG_ACL_NETWORK_TABLE_SIZE (64 * 1024 * 1024)

#undef YANET_CONFIG_ACL_TRANSPORT_LAYERS_SIZE
#define YANET_CONFIG_ACL_TRANSPORT_LAYERS_SIZE (128)

#undef YANET_CONFIG_ACL_TRANSPORT_HT_SIZE
#define YANET_CONFIG_ACL_TRANSPORT_HT_SIZE (4 * 1024 * 1024)

#undef YANET_CONFIG_ACL_TOTAL_HT_SIZE
#define YANET_CONFIG_ACL_TOTAL_HT_SIZE (2 * 1024 * 1024)

#undef YANET_CONFIG_ACL_STATES4_HT_SIZE
#define YANET_CONFIG_ACL_STATES4_HT_SIZE (128 * 1024)

#undef YANET_CONFIG_ACL_STATES6_HT_SIZE
#define YANET_CONFIG_ACL_STATES6_HT_SIZE (128 * 1024)

#undef CONFIG_YADECAP_TUN64_HT_SIZE
#define CONFIG_YADECAP_TUN64_HT_SIZE (4 * 1024)

#undef YANET_CONFIG_ROUTE_TUNNEL_LPM4_EXTENDED_SIZE
#define YANET_CONFIG_ROUTE_TUNNEL_LPM4_EXTENDED_SIZE (512)

#undef YANET_CONFIG_ROUTE_TUNNEL_LPM6_EXTENDED_SIZE
#define YANET_CONFIG_ROUTE_TUNNEL_LPM6_EXTENDED_SIZE (512)

#undef YANET_CONFIG_BALANCER_WEIGHTS_SIZE
#define YANET_CONFIG_BALANCER_WEIGHTS_SIZE (32 * 1024 * 1024)

#undef YANET_CONFIG_BALANCER_STATE_HT_SIZE
#define YANET_CONFIG_BALANCER_STATE_HT_SIZE (64 * 1024 * 1024)

#undef YANET_CONFIG_NAT64STATEFUL_HT_SIZE
#define YANET_CONFIG_NAT64STATEFUL_HT_SIZE (64 * 1024)
