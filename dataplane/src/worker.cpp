#include <string>
#include <thread>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_gre.h>
#include <rte_hash_crc.h>
#include <rte_icmp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "checksum.h"
#include "common.h"
#include "dataplane.h"
#include "icmp.h"
#include "metadata.h"
#include "worker.h"
#include "prepare.h"

//

cWorker::cWorker(cDataPlane* dataPlane) :
        dataPlane(dataPlane),
        coreId(-1),
        socketId(-1),
        mempool(nullptr),
        iteration(0),
        currentBaseId(0),
        localBaseId(0),
        nat64stateful_packet_id(0),
        nat64statelessPacketId(0),
        ring_highPriority(nullptr),
        ring_normalPriority(nullptr),
        ring_lowPriority(nullptr),
        ring_toFreePackets(nullptr),
        ring_log(nullptr),
		packetsToSWNPRemainder(dataPlane->config.SWNormalPriorityRateLimitPerWorker)
{
	memset(bursts, 0, sizeof(bursts));
	memset(counters, 0, sizeof(counters));
}

cWorker::~cWorker()
{
	if (mempool)
	{
		rte_mempool_free(mempool);
	}

	if (ring_highPriority)
	{
		rte_ring_free(ring_highPriority);
	}

	if (ring_normalPriority)
	{
		rte_ring_free(ring_normalPriority);
	}

	if (ring_lowPriority)
	{
		rte_ring_free(ring_lowPriority);
	}

	if (ring_toFreePackets)
	{
		rte_ring_free(ring_toFreePackets);
	}
	if (ring_log)
	{
		rte_ring_free(ring_log);
	}
}

eResult cWorker::init(const tCoreId& coreId,
                      const dataplane::base::permanently& basePermanently,
                      const dataplane::base::generation& base)
{
	YADECAP_LOG_DEBUG("rte_mempool_create(coreId: %u, socketId: %u)\n",
	                  coreId,
	                  rte_lcore_to_socket_id(coreId));

	this->coreId = coreId;
	this->socketId = rte_lcore_to_socket_id(coreId);
	this->basePermanently = basePermanently;
	this->bases[currentBaseId] = base;
	this->bases[currentBaseId ^ 1] = base;

	unsigned int elements_count = 2 * CONFIG_YADECAP_WORKER_PORTS_SIZE * dataPlane->getConfigValue(eConfigType::port_rx_queue_size) +
	                              2 * CONFIG_YADECAP_WORKER_PORTS_SIZE * dataPlane->getConfigValue(eConfigType::port_tx_queue_size) +
	                              2 * dataPlane->getConfigValue(eConfigType::ring_highPriority_size) +
	                              2 * dataPlane->getConfigValue(eConfigType::ring_normalPriority_size) +
	                              2 * dataPlane->getConfigValue(eConfigType::ring_lowPriority_size);

	YADECAP_LOG_DEBUG("elements_count: %u\n", elements_count);

	/// init mempool
	mempool = rte_mempool_create(("fp" + std::to_string(coreId)).data(),
	                             elements_count,
	                             CONFIG_YADECAP_MBUF_SIZE,
	                             0,
	                             sizeof(struct rte_pktmbuf_pool_private),
	                             rte_pktmbuf_pool_init,
	                             nullptr,
	                             rte_pktmbuf_init,
	                             nullptr,
	                             socketId,
	                             MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET);
	if (!mempool)
	{
		YADECAP_LOG_ERROR("rte_mempool_create(): %s [%u]\n", rte_strerror(rte_errno), rte_errno);
		return eResult::errorInitMempool;
	}

	/// init rings
	ring_highPriority = rte_ring_create(("r_hp_" + std::to_string(coreId)).c_str(),
	                                    dataPlane->getConfigValue(eConfigType::ring_highPriority_size),
	                                    socketId,
	                                    RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring_highPriority)
	{
		return eResult::errorInitRing;
	}

	ring_normalPriority = rte_ring_create(("r_np_" + std::to_string(coreId)).c_str(),
	                                      dataPlane->getConfigValue(eConfigType::ring_normalPriority_size),
	                                      socketId,
	                                      RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring_normalPriority)
	{
		return eResult::errorInitRing;
	}

	ring_lowPriority = rte_ring_create(("r_lp_" + std::to_string(coreId)).c_str(),
	                                   dataPlane->getConfigValue(eConfigType::ring_lowPriority_size),
	                                   socketId,
	                                   RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring_lowPriority)
	{
		return eResult::errorInitRing;
	}

	ring_toFreePackets = rte_ring_create(("r_tfp_" + std::to_string(coreId)).c_str(),
	                                     dataPlane->getConfigValue(eConfigType::ring_toFreePackets_size),
	                                     socketId,
	                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring_toFreePackets)
	{
		return eResult::errorInitRing;
	}

	ring_log = rte_ring_create(("r_log_" + std::to_string(coreId)).c_str(),
	                                     dataPlane->getConfigValue(eConfigType::ring_log_size),
	                                     socketId,
	                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring_log)
	{
		return eResult::errorInitRing;
	}

	if (coreId > 0xFF)
	{
		YADECAP_LOG_ERROR("invalid coreId: %u\n", coreId);
		return eResult::invalidCoreId;
	}

	return eResult::success;
}

void cWorker::start()
{
	eResult result = sanityCheck();
	if (result != eResult::success)
	{
		abort();
	}

	/// @todo: prepare()

	unsigned int mbufs_count_expect = 2 * CONFIG_YADECAP_WORKER_PORTS_SIZE * dataPlane->getConfigValue(eConfigType::port_rx_queue_size) +
	                                  2 * CONFIG_YADECAP_WORKER_PORTS_SIZE * dataPlane->getConfigValue(eConfigType::port_tx_queue_size) +
	                                  2 * dataPlane->getConfigValue(eConfigType::ring_highPriority_size) +
	                                  2 * dataPlane->getConfigValue(eConfigType::ring_normalPriority_size) +
	                                  2 * dataPlane->getConfigValue(eConfigType::ring_lowPriority_size);

	unsigned int mbufs_count = rte_mempool_avail_count(mempool);
	if (mbufs_count != mbufs_count_expect)
	{
		YADECAP_LOG_ERROR("mbufs_count: %u != %u\n",
		                  mbufs_count,
		                  mbufs_count_expect);
		abort();
	}

	std::vector<rte_mbuf*> mbufs;
	mbufs.resize(mbufs_count);

	int rc = rte_mempool_ops_dequeue_bulk(mempool, (void**)&mbufs[0], mbufs_count);
	if (rc)
	{
		YADECAP_LOG_ERROR("rte_mempool_ops_dequeue_bulk\n");
		abort();
	}

	rc = rte_mempool_ops_enqueue_bulk(mempool, (void**)&mbufs[0], mbufs_count);
	if (rc)
	{
		YADECAP_LOG_ERROR("rte_mempool_ops_enqueue_bulk\n");
		abort();
	}

	for (unsigned int worker_port_i = 0;
	     worker_port_i < basePermanently.workerPortsCount;
	     worker_port_i++)
	{
		const auto& portId = basePermanently.workerPorts[worker_port_i].inPortId;
		const auto& queueId = basePermanently.workerPorts[worker_port_i].inQueueId;

		rc = rte_eth_rx_queue_setup(portId,
		                            queueId,
		                            dataPlane->getConfigValue(eConfigType::port_rx_queue_size),
		                            rte_eth_dev_socket_id(portId),
		                            nullptr, ///< @todo
		                            mempool);
		if (rc < 0)
		{
			YADECAP_LOG_ERROR("rte_eth_rx_queue_setup() = %d\n", rc);
			abort();
		}
	}

	if (rte_mempool_default_cache(mempool, coreId))
	{
		YADECAP_LOG_ERROR("mempool cache not empty\n");
		abort();
	}

	rc = pthread_barrier_wait(&dataPlane->initPortBarrier);
	if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
	{
		pthread_barrier_destroy(&dataPlane->initPortBarrier);
	}
	else if (rc != 0)
	{
		YADECAP_LOG_ERROR("pthread_barrier_wait() = %d\n", rc);
		abort();
	}

	rc = pthread_barrier_wait(&dataPlane->runBarrier);
	if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
	{
		pthread_barrier_destroy(&dataPlane->runBarrier);
	}
	else if (rc != 0)
	{
		YADECAP_LOG_ERROR("pthread_barrier_wait() = %d\n", rc);
		abort();
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < mbufs_count;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = mbufs[mbuf_i];

		rte_prefetch0((void*)mbuf);
		rte_prefetch0((void*)YADECAP_METADATA(mbuf));
		rte_prefetch0(rte_pktmbuf_mtod(mbuf, void*));
	}

	rte_prefetch0((void*)&logicalPort_ingress_stack.mbufsCount);
	rte_prefetch0((void*)logicalPort_ingress_stack.mbufs);
	rte_prefetch0((void*)&route_stack4.mbufsCount);
	rte_prefetch0((void*)route_stack4.mbufs);
	rte_prefetch0((void*)&route_stack6.mbufsCount);
	rte_prefetch0((void*)route_stack6.mbufs);

	mainThread();
}

void cWorker::fillStatsNamesToAddrsTable(std::unordered_map<std::string, uint64_t*>& table)
{
	table["brokenPackets"] = &stats.brokenPackets;
	table["dropPackets"] = &stats.dropPackets;
	table["ring_highPriority_drops"] = &stats.ring_highPriority_drops;
	table["ring_normalPriority_drops"] = &stats.ring_normalPriority_drops;
	table["ring_lowPriority_drops"] = &stats.ring_lowPriority_drops;
	table["ring_highPriority_packets"] = &stats.ring_highPriority_packets;
	table["ring_normalPriority_packets"] = &stats.ring_normalPriority_packets;
	table["ring_lowPriority_packets"] = &stats.ring_lowPriority_packets;
	table["decap_packets"] = &stats.decap_packets;
	table["decap_fragments"] = &stats.decap_fragments;
	table["decap_unknownExtensions"] = &stats.decap_unknownExtensions;
	table["interface_lookupMisses"] = &stats.interface_lookupMisses;
	table["interface_hopLimits"] = &stats.interface_hopLimits;
	table["interface_neighbor_invalid"] = &stats.interface_neighbor_invalid;
	table["nat64stateless_ingressPackets"] = &stats.nat64stateless_ingressPackets;
	table["nat64stateless_ingressFragments"] = &stats.nat64stateless_ingressFragments;
	table["nat64stateless_ingressUnknownICMP"] = &stats.nat64stateless_ingressUnknownICMP;
	table["nat64stateless_egressPackets"] = &stats.nat64stateless_egressPackets;
	table["nat64stateless_egressFragments"] = &stats.nat64stateless_egressFragments;
	table["nat64stateless_egressUnknownICMP"] = &stats.nat64stateless_egressUnknownICMP;
	table["balancer_invalid_reals_count"] = &stats.balancer_invalid_reals_count;
	table["fwsync_multicast_egress_drops"] = &stats.fwsync_multicast_egress_drops;
	table["fwsync_multicast_egress_packets"] = &stats.fwsync_multicast_egress_packets;
	table["fwsync_multicast_egress_imm_packets"] = &stats.fwsync_multicast_egress_imm_packets;
	table["fwsync_no_config_drops"] = &stats.fwsync_no_config_drops;
	table["fwsync_unicast_egress_drops"] = &stats.fwsync_unicast_egress_drops;
	table["fwsync_unicast_egress_packets"] = &stats.fwsync_unicast_egress_packets;
	table["acl_ingress_dropPackets"] = &stats.acl_ingress_dropPackets;
	table["acl_egress_dropPackets"] = &stats.acl_egress_dropPackets;
	table["repeat_ttl"] = &stats.repeat_ttl;
	table["leakedMbufs"] = &stats.leakedMbufs;
	table["logs_packets"] = &stats.logs_packets;
	table["logs_drops"] = &stats.logs_drops;

	table["balancer_state_insert_failed"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_state_insert_failed];
	table["balancer_state_insert_done"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_state_insert_done];
	table["balancer_icmp_generated_echo_reply_ipv4"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_generated_echo_reply_ipv4];
	table["balancer_icmp_generated_echo_reply_ipv6"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_generated_echo_reply_ipv6];
	table["balancer_icmp_drop_icmpv4_payload_too_short_ip"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv4_payload_too_short_ip];
	table["balancer_icmp_drop_icmpv4_payload_too_short_port"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv4_payload_too_short_port];
	table["balancer_icmp_drop_icmpv6_payload_too_short_ip"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv6_payload_too_short_ip];
	table["balancer_icmp_drop_icmpv6_payload_too_short_port"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv6_payload_too_short_port];
	table["balancer_icmp_unmatching_src_from_original_ipv4"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_unmatching_src_from_original_ipv4];
	table["balancer_icmp_unmatching_src_from_original_ipv6"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_unmatching_src_from_original_ipv6];
	table["balancer_icmp_drop_real_disabled"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_real_disabled];
	table["balancer_icmp_no_balancer_src_ipv4"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_no_balancer_src_ipv4];
	table["balancer_icmp_no_balancer_src_ipv6"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_no_balancer_src_ipv6];
	table["balancer_icmp_drop_already_cloned"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_already_cloned];
	table["balancer_icmp_drop_no_unrdup_table_for_balancer_id"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_no_unrdup_table_for_balancer_id];
	table["balancer_icmp_drop_unrdup_vip_not_found"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_unrdup_vip_not_found];
	table["balancer_icmp_drop_no_vip_vport_proto_table_for_balancer_id"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_no_vip_vport_proto_table_for_balancer_id];
	table["balancer_icmp_drop_unexpected_transport_protocol"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_unexpected_transport_protocol];
	table["balancer_icmp_drop_unknown_service"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_unknown_service];
	table["balancer_icmp_failed_to_clone"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_failed_to_clone];
	table["balancer_icmp_clone_forwarded"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_clone_forwarded];
	table["balancer_icmp_sent_to_real"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_sent_to_real];
	table["balancer_icmp_out_rate_limit_reached"] = &counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_out_rate_limit_reached];
	table["slow_worker_normal_priority_rate_limit_exceeded"] = &counters[(uint32_t)common::globalBase::static_counter_type::slow_worker_normal_priority_rate_limit_exceeded];

	table["acl_ingress_v4_broken_packet"] = &counters[(uint32_t)common::globalBase::static_counter_type::acl_ingress_v4_broken_packet];
	table["acl_ingress_v6_broken_packet"] = &counters[(uint32_t)common::globalBase::static_counter_type::acl_ingress_v6_broken_packet];
	table["acl_egress_v4_broken_packet"] = &counters[(uint32_t)common::globalBase::static_counter_type::acl_egress_v4_broken_packet];
	table["acl_egress_v6_broken_packet"] = &counters[(uint32_t)common::globalBase::static_counter_type::acl_egress_v6_broken_packet];
}

eResult cWorker::sanityCheck()
{
	if (coreId != rte_lcore_id())
	{
		YADECAP_LOG_ERROR("invalid core id: %u != %u\n", coreId, rte_lcore_id());
		return eResult::invalidCoreId;
	}

	if (socketId != rte_socket_id())
	{
		YADECAP_LOG_ERROR("invalid socket id: %u != %u\n", socketId, rte_socket_id());
		return eResult::invalidSocketId;
	}

	return eResult::success;
}

YANET_NEVER_INLINE void cWorker::mainThread()
{
	for (;;)
	{
		localBaseId = currentBaseId;

		/// @todo: opt
		for (unsigned int worker_port_i = 0;
		     worker_port_i < basePermanently.workerPortsCount;
		     worker_port_i++)
		{
			toFreePackets_handle();
			physicalPort_ingress_handle(worker_port_i);

			if (unlikely(logicalPort_ingress_stack.mbufsCount == 0))
			{
				continue;
			}

			handlePackets();
		}

		iteration++;

#ifdef CONFIG_YADECAP_AUTOTEST
		std::this_thread::sleep_for(std::chrono::microseconds{1});
#endif // CONFIG_YADECAP_AUTOTEST
	}
}

inline void cWorker::calcHash(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	metadata->hash = 0;

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		metadata->hash = rte_hash_crc(&ipv4Header->next_proto_id, 1, metadata->hash);
		metadata->hash = rte_hash_crc(&ipv4Header->src_addr, 4 + 4, metadata->hash);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		metadata->hash = rte_hash_crc(&ipv6Header->proto, 1, metadata->hash);
		metadata->hash = rte_hash_crc(&ipv6Header->src_addr, 16 + 16, metadata->hash);
	}

	if (!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT))
	{
		if (metadata->transport_headerType == IPPROTO_ICMP)
		{
			icmp_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmp_header_t*, metadata->transport_headerOffset);

			if (icmpHeader->type == ICMP_ECHO ||
			    icmpHeader->type == ICMP_ECHOREPLY)
			{
				metadata->hash = rte_hash_crc(&icmpHeader->identifier, 2, metadata->hash);
			}
		}
		else if (metadata->transport_headerType == IPPROTO_ICMPV6)
		{
			icmpv6_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

			if (icmpHeader->type == ICMP6_ECHO_REQUEST ||
			    icmpHeader->type == ICMP6_ECHO_REPLY)
			{
				metadata->hash = rte_hash_crc(&icmpHeader->identifier, 2, metadata->hash);
			}
		}
		else if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);
			metadata->hash = rte_hash_crc(&tcpHeader->src_port, 2 + 2, metadata->hash);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);
			metadata->hash = rte_hash_crc(&udpHeader->src_port, 2 + 2, metadata->hash);
		}
	}
}

void cWorker::preparePacket(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	metadata->network_headerType = YANET_NETWORK_TYPE_UNKNOWN;
	metadata->network_flags = 0;
	metadata->transport_headerType = YANET_TRANSPORT_TYPE_UNKNOWN;
	metadata->transport_flags = 0;

	const rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, rte_ether_hdr*);

	if (ethernetHeader->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))
	{
		const rte_vlan_hdr* vlanHeader = rte_pktmbuf_mtod_offset(mbuf, rte_vlan_hdr*, sizeof(rte_ether_hdr));

		metadata->network_headerType = vlanHeader->eth_proto;
		metadata->network_headerOffset = sizeof(rte_ether_hdr) + sizeof(rte_vlan_hdr);
	}
	else
	{
		metadata->network_headerType = ethernetHeader->ether_type;
		metadata->network_headerOffset = sizeof(rte_ether_hdr);
	}

	uint16_t network_payload_length = 0;

	// will traverse through ipv4 options/ipv6 extensions and try to determine transport header type and offset
	if (!prepareL3(mbuf, metadata))
	{
		stats.brokenPackets++;
		return;
	}

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		const rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
		network_payload_length = rte_be_to_cpu_16(ipv4Header->total_length) - 4 * (ipv4Header->version_ihl & 0x0F);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		const rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
		network_payload_length = rte_be_to_cpu_16(ipv6Header->payload_len) - (metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv6_hdr));
	}
	else
	{
		return;
	}

	if ((!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT)) &&
	    network_payload_length < basePermanently.transportSizes[metadata->transport_headerType])
	{
		stats.brokenPackets++;
		metadata->transport_headerType = YANET_TRANSPORT_TYPE_UNKNOWN;
		return;
	}
}

inline void cWorker::handlePackets()
{
	const auto& base = bases[localBaseId & 1];
	const auto& globalbase = *base.globalBase;

	logicalPort_ingress_handle();

	acl_ingress_handle4();
	acl_ingress_handle6();

	if (globalbase.early_decap_enabled)
	{
		if (after_early_decap_stack4.mbufsCount > 0)
		{
			acl_ingress_stack4 = after_early_decap_stack4;
			after_early_decap_stack4.clear();
			acl_ingress_handle4();
		}

		if (after_early_decap_stack6.mbufsCount > 0)
		{
			acl_ingress_stack6 = after_early_decap_stack6;
			after_early_decap_stack6.clear();
			acl_ingress_handle6();
		}
	}

	if (globalbase.tun64_enabled)
	{
		tun64_ipv4_handle();
		tun64_ipv6_handle();
	}

	if (globalbase.decap_enabled)
	{
		decap_handle();
	}

	if (globalbase.nat64stateful_enabled)
	{
		nat64stateful_lan_handle();
		nat64stateful_wan_handle();
	}

	if (globalbase.nat64stateless_enabled)
	{
		nat64stateless_ingress_handle();
		nat64stateless_egress_handle();
	}

	if (globalbase.balancer_enabled)
	{
		balancer_handle();

		balancer_icmp_reply_handle(); // balancer replies instead of real (when client pings VS)
		balancer_icmp_forward_handle(); // forward icmp message to other balancers (if not sent to one of this balancer's reals)
	}

	route_handle4();
	route_handle6();

	if (globalbase.route_tunnel_enabled)
	{
		route_tunnel_handle4();
		route_tunnel_handle6();
	}

	if (globalbase.acl_egress_enabled)
	{
		acl_egress_handle4();
		acl_egress_handle6();
	}

	logicalPort_egress_handle();
	controlPlane_handle();
	physicalPort_egress_handle();
}

static_assert(CONFIG_YADECAP_PORTS_SIZE == 8, "(vlanId << 3) | metadata->fromPortId");
static_assert(CONFIG_YADECAP_LOGICALPORTS_SIZE == CONFIG_YADECAP_PORTS_SIZE * 4096, "base.globalBase->logicalPorts[(vlanId << 3) | metadata->fromPortId]");

inline void cWorker::physicalPort_ingress_handle(const unsigned int& worker_port_i)
{
	/// read packets from ports
	uint16_t rxSize = rte_eth_rx_burst(basePermanently.workerPorts[worker_port_i].inPortId,
	                                   basePermanently.workerPorts[worker_port_i].inQueueId,
	                                   logicalPort_ingress_stack.mbufs,
	                                   CONFIG_YADECAP_MBUFS_BURST_SIZE);

	/// init metadata
	for (unsigned int mbuf_i = 0;
	     mbuf_i < rxSize;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = logicalPort_ingress_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		metadata->fromPortId = basePermanently.workerPorts[worker_port_i].inPortId;
		metadata->repeat_ttl = YANET_CONFIG_REPEAT_TTL;
		metadata->flowLabel = 0;
		metadata->already_early_decapped = 0;
		metadata->aclId = 0;

		const rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, rte_ether_hdr*);

		if (ethernetHeader->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))
		{
			const rte_vlan_hdr* vlanHeader = rte_pktmbuf_mtod_offset(mbuf, rte_vlan_hdr*, sizeof(rte_ether_hdr));

			metadata->flow.data.logicalPortId = (rte_be_to_cpu_16(vlanHeader->vlan_tci & 0xFF0F) << 3) | metadata->fromPortId;
		}
		else
		{
			metadata->flow.data.logicalPortId = metadata->fromPortId;
		}
		metadata->in_logicalport_id = metadata->flow.data.logicalPortId;

		preparePacket(mbuf);

		if (basePermanently.globalBaseAtomic->physicalPort_flags[metadata->fromPortId] & YANET_PHYSICALPORT_FLAG_INGRESS_DUMP)
		{
			if (!rte_ring_full(ring_lowPriority))
			{
				rte_mbuf* mbuf_clone = rte_pktmbuf_alloc(mempool);
				if (mbuf_clone)
				{
					*YADECAP_METADATA(mbuf_clone) = *YADECAP_METADATA(mbuf);

					rte_memcpy(rte_pktmbuf_mtod(mbuf_clone, char*),
					           rte_pktmbuf_mtod(mbuf, char*),
					           mbuf->data_len);

					mbuf_clone->data_len = mbuf->data_len;
					mbuf_clone->pkt_len = mbuf->pkt_len;

					YADECAP_METADATA(mbuf_clone)->flow.type = common::globalBase::eFlowType::slowWorker_dump;
					YADECAP_METADATA(mbuf_clone)->flow.data.dump.type = common::globalBase::dump_type_e::physicalPort_ingress;
					YADECAP_METADATA(mbuf_clone)->flow.data.dump.id = metadata->fromPortId;
					slowWorker_entry_lowPriority(mbuf_clone);
				}
			}
		}
	}

	/// for calc usage
	bursts[rxSize]++;

	logicalPort_ingress_stack.mbufsCount = rxSize;
}

inline void cWorker::physicalPort_egress_handle()
{
	for (tPortId portId = 0;
	     portId < basePermanently.ports_count;
	     portId++)
	{
		if (unlikely(physicalPort_stack[portId].mbufsCount == 0))
		{
			continue;
		}

		uint16_t txSize = rte_eth_tx_burst(portId,
		                                   basePermanently.outQueueId,
		                                   physicalPort_stack[portId].mbufs,
		                                   physicalPort_stack[portId].mbufsCount);

		statsPorts[portId].physicalPort_egress_drops += physicalPort_stack[portId].mbufsCount - txSize;

		for (;
		     txSize < physicalPort_stack[portId].mbufsCount;
		     txSize++)
		{
			rte_pktmbuf_free(physicalPort_stack[portId].mbufs[txSize]);
		}

		physicalPort_stack[portId].clear();
	}
}

inline void cWorker::logicalPort_ingress_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(logicalPort_ingress_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < logicalPort_ingress_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = logicalPort_ingress_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& logicalPort = base.globalBase->logicalPorts[metadata->flow.data.logicalPortId];

		generic_rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);

		if (ethernetHeader->dst_addr.addr_bytes[0] & 1) ///< multicast
		{
			/// @todo: stats
			controlPlane(mbuf);
			continue;
		}

		if ((!(logicalPort.flags & YANET_LOGICALPORT_FLAG_PROMISCUOUSMODE)) &&
		    (!equal(ethernetHeader->dst_addr, logicalPort.etherAddress)))
		{
			/// @todo: stats
			controlPlane(mbuf);
			continue;
		}

		logicalPort_ingress_flow(mbuf, logicalPort.flow);
	}

	logicalPort_ingress_stack.clear();
}

inline void cWorker::early_decap(rte_mbuf *mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->already_early_decapped = 1;

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4OuterHeader = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		if (ipv4OuterHeader->next_proto_id == IPPROTO_IPIP)
		{
			rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
					rte_pktmbuf_mtod(mbuf, char*),
					metadata->network_headerOffset);
			rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

			uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2); // metadata->network_headerOffset - 2 == metadata->network_headerType?
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

			preparePacket(mbuf);
		}
		else if (ipv4OuterHeader->next_proto_id == IPPROTO_IPV6)
		{
			rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
					rte_pktmbuf_mtod(mbuf, char*),
					metadata->network_headerOffset);
			rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

			uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2); // metadata->network_headerOffset - 2 == metadata->network_headerType?
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

			preparePacket(mbuf);
		}
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		rte_ipv6_hdr* ipv6OuterHeader = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		if (ipv6OuterHeader->proto == IPPROTO_IPIP)
		{
			rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
					rte_pktmbuf_mtod(mbuf, char*),
					metadata->network_headerOffset);
			rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

			uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2); // metadata->network_headerOffset - 2 == metadata->network_headerType?
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

			preparePacket(mbuf);
		}
		else if (ipv6OuterHeader->proto == IPPROTO_IPV6)
		{
			rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
					rte_pktmbuf_mtod(mbuf, char*),
					metadata->network_headerOffset);
			rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

			uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2); // metadata->network_headerOffset - 2 == metadata->network_headerType?
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

			preparePacket(mbuf);
		}
	}
}

inline void cWorker::logicalPort_ingress_flow(rte_mbuf* mbuf,
                                              const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::acl_ingress)
	{
		acl_ingress_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::logicalPort_egress_entry(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->out_logicalport_id = metadata->flow.data.logicalPortId;

	logicalPort_egress_stack.insert(mbuf);
}

inline void cWorker::logicalPort_egress_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(logicalPort_egress_stack.mbufsCount == 0))
	{
		return;
	}

	if (base.globalBase->sampler_enabled)
	{
		sampler.add(logicalPort_egress_stack.mbufs, logicalPort_egress_stack.mbufsCount);
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < logicalPort_egress_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = logicalPort_egress_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& logicalPort = base.globalBase->logicalPorts[metadata->flow.data.logicalPortId];

		generic_rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);
		rte_ether_addr_copy(&logicalPort.etherAddress, &ethernetHeader->src_addr);

		if (logicalPort.vlanId)
		{
			if (ethernetHeader->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))
			{
				rte_vlan_hdr* vlanHeader = rte_pktmbuf_mtod_offset(mbuf, rte_vlan_hdr*, sizeof(rte_ether_hdr));
				vlanHeader->vlan_tci = logicalPort.vlanId;
			}
			else
			{
				rte_pktmbuf_prepend(mbuf, sizeof(rte_vlan_hdr));
				memmove(rte_pktmbuf_mtod(mbuf, char*),
				        rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_vlan_hdr)),
				        sizeof(rte_ether_hdr));

				ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);

				rte_vlan_hdr* vlanHeader = rte_pktmbuf_mtod_offset(mbuf, rte_vlan_hdr*, sizeof(rte_ether_hdr));
				vlanHeader->vlan_tci = logicalPort.vlanId;
				vlanHeader->eth_proto = ethernetHeader->ether_type;

				ethernetHeader->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
			}
		}
		else
		{
			if (ethernetHeader->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))
			{
				rte_vlan_hdr* vlanHeader = rte_pktmbuf_mtod_offset(mbuf, rte_vlan_hdr*, sizeof(rte_ether_hdr));

				ethernetHeader->ether_type = vlanHeader->eth_proto;

				memmove(rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_vlan_hdr)),
				        rte_pktmbuf_mtod(mbuf, char*),
				        sizeof(rte_ether_hdr));
				rte_pktmbuf_adj(mbuf, sizeof(rte_vlan_hdr));
			}
		}

		if (basePermanently.globalBaseAtomic->physicalPort_flags[logicalPort.portId] & YANET_PHYSICALPORT_FLAG_EGRESS_DUMP)
		{
			if (!rte_ring_full(ring_lowPriority))
			{
				rte_mbuf* mbuf_clone = rte_pktmbuf_alloc(mempool);
				if (mbuf_clone)
				{
					*YADECAP_METADATA(mbuf_clone) = *YADECAP_METADATA(mbuf);

					rte_memcpy(rte_pktmbuf_mtod(mbuf_clone, char*),
					           rte_pktmbuf_mtod(mbuf, char*),
					           mbuf->data_len);

					mbuf_clone->data_len = mbuf->data_len;
					mbuf_clone->pkt_len = mbuf->pkt_len;

					YADECAP_METADATA(mbuf_clone)->flow.type = common::globalBase::eFlowType::slowWorker_dump;
					YADECAP_METADATA(mbuf_clone)->flow.data.dump.type = common::globalBase::dump_type_e::physicalPort_egress;
					YADECAP_METADATA(mbuf_clone)->flow.data.dump.id = logicalPort.portId;
					slowWorker_entry_lowPriority(mbuf_clone);
				}
			}
		}
		if (rte_mbuf_refcnt_read(mbuf) < 1)
		{
			stats.leakedMbufs++;

#ifdef CONFIG_YADECAP_AUTOTEST
			YADECAP_LOG_ERROR("mbuf[%p] is broken\n", mbuf);
			std::abort();
#endif
		}

		physicalPort_stack[logicalPort.portId].insert(mbuf);
	}

	logicalPort_egress_stack.clear();
}

inline void cWorker::after_early_decap_entry(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		after_early_decap_stack4.insert(mbuf);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		after_early_decap_stack6.insert(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::acl_ingress_entry(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		acl_ingress_stack4.insert(mbuf);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		acl_ingress_stack6.insert(mbuf);
	}
	else
	{
		controlPlane(mbuf);
	}
}

inline void cWorker::acl_ingress_handle4()
{
	const auto& base = bases[localBaseId & 1];
	const auto& acl = base.globalBase->acl;

	if (unlikely(acl_ingress_stack4.mbufsCount == 0))
	{
		return;
	}

	uint32_t mask = 0xFFFFFFFFu >> (8 * sizeof(uint32_t) - acl_ingress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
		key_acl.ipv4_sources[mbuf_i].address = ipv4Header->src_addr;
		key_acl.ipv4_destinations[mbuf_i].address = ipv4Header->dst_addr;
	}

	acl.network.ipv4.source->lookup(key_acl.ipv4_sources,
	                                value_acl.ipv4_sources,
	                                acl_ingress_stack4.mbufsCount);

	acl.network.ipv4.destination->lookup(key_acl.ipv4_destinations,
	                                     value_acl.ipv4_destinations,
	                                     acl_ingress_stack4.mbufsCount);

	acl.network_table->lookup(value_acl.ipv4_sources,
	                          value_acl.ipv4_destinations,
	                          value_acl.networks,
	                          acl_ingress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& network_value = value_acl.networks[mbuf_i];
		auto& transport_key = key_acl.transports[mbuf_i];

		const auto& transport_layer = acl.transport_layers[network_value & acl.transport_layers_mask];

		transport_key.network_id = network_value;
		transport_key.protocol = transport_layer.protocol.array[metadata->transport_headerType];
		transport_key.group1 = 0;
		transport_key.group2 = 0;
		transport_key.group3 = 0;

		if (!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT))
		{
			if (metadata->transport_headerType == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcp_header = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.tcp.source.array[rte_be_to_cpu_16(tcp_header->src_port)];
				transport_key.group2 = transport_layer.tcp.destination.array[rte_be_to_cpu_16(tcp_header->dst_port)];
				transport_key.group3 = transport_layer.tcp.flags.array[tcp_header->tcp_flags];
			}
			else if (metadata->transport_headerType == IPPROTO_UDP)
			{
				rte_udp_hdr* udp_header = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.udp.source.array[rte_be_to_cpu_16(udp_header->src_port)];
				transport_key.group2 = transport_layer.udp.destination.array[rte_be_to_cpu_16(udp_header->dst_port)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMP)
			{
				icmp_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmp_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMPV6)
			{
				icmpv6_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == YANET_TRANSPORT_TYPE_UNKNOWN)
			{
				mask ^= (1u << mbuf_i);
			}
		}

		transport_key.network_flags = acl.network_flags.array[metadata->network_flags];
	}

	acl.transport_table->lookup(hashes,
	                            key_acl.transports,
	                            value_acl.transports,
	                            acl_ingress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& transport_value = value_acl.transports[mbuf_i];
		auto& total_key = key_acl.totals[mbuf_i];

		total_key.acl_id = metadata->flow.data.aclId;
		total_key.transport_id = transport_value;
	}

	acl.total_table->lookup(hashes,
	                        key_acl.totals,
	                        value_acl.totals,
	                        acl_ingress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (!(mask & (1u << mbuf_i)))
		{
			counters[(uint32_t)common::globalBase::static_counter_type::acl_ingress_v4_broken_packet]++;
			drop(mbuf);
			continue;
		}

		auto total_value = value_acl.totals[mbuf_i];
		if (total_value & 0x80000000u)
		{
			total_value = 0; ///< default
		}

		const auto& value = acl.values[total_value];

		if (value.flow.type == common::globalBase::eFlowType::drop)
		{
			// Try to match against stateful dynamic rules. If so - a packet will be handled.
			if (acl_try_keepstate(mbuf))
			{
				continue;
			}
		}

		aclCounters[value.flow.counter_id]++;

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::log)
		{
			acl_log(mbuf, value.flow, metadata->flow.data.aclId);
		}

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::keepstate)
		{
			acl_create_keepstate(mbuf, metadata->flow.data.aclId, value.flow);
		}

		acl_ingress_flow(mbuf, value.flow);
	}

	acl_ingress_stack4.clear();
}

inline void cWorker::acl_ingress_handle6()
{
	const auto& base = bases[localBaseId & 1];
	const auto& acl = base.globalBase->acl;

	if (unlikely(acl_ingress_stack6.mbufsCount == 0))
	{
		return;
	}

	uint32_t mask = 0xFFFFFFFFu >> (8 * sizeof(uint32_t) - acl_ingress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
		rte_memcpy(key_acl.ipv6_sources[mbuf_i].bytes, ipv6Header->src_addr, 16);
		rte_memcpy(key_acl.ipv6_destinations[mbuf_i].bytes, ipv6Header->dst_addr, 16);
	}

	acl.network.ipv6.source->lookup(key_acl.ipv6_sources,
	                                value_acl.ipv6_sources,
	                                acl_ingress_stack6.mbufsCount);

	{
		uint32_t mask = acl.network.ipv6.destination_ht->lookup(hashes,
		                                                        key_acl.ipv6_destinations,
		                                                        value_acl.ipv6_destinations,
		                                                        acl_ingress_stack6.mbufsCount);
		acl.network.ipv6.destination->lookup(mask,
		                                     key_acl.ipv6_destinations,
		                                     value_acl.ipv6_destinations,
		                                     acl_ingress_stack6.mbufsCount);
	}

	acl.network_table->lookup(value_acl.ipv6_sources,
	                          value_acl.ipv6_destinations,
	                          value_acl.networks,
	                          acl_ingress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& network_value = value_acl.networks[mbuf_i];
		auto& transport_key = key_acl.transports[mbuf_i];

		const auto& transport_layer = acl.transport_layers[network_value & acl.transport_layers_mask];

		transport_key.network_id = network_value;
		transport_key.protocol = transport_layer.protocol.array[metadata->transport_headerType];
		transport_key.group1 = 0;
		transport_key.group2 = 0;
		transport_key.group3 = 0;

		if (!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT))
		{
			if (metadata->transport_headerType == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcp_header = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.tcp.source.array[rte_be_to_cpu_16(tcp_header->src_port)];
				transport_key.group2 = transport_layer.tcp.destination.array[rte_be_to_cpu_16(tcp_header->dst_port)];
				transport_key.group3 = transport_layer.tcp.flags.array[tcp_header->tcp_flags];
			}
			else if (metadata->transport_headerType == IPPROTO_UDP)
			{
				rte_udp_hdr* udp_header = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.udp.source.array[rte_be_to_cpu_16(udp_header->src_port)];
				transport_key.group2 = transport_layer.udp.destination.array[rte_be_to_cpu_16(udp_header->dst_port)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMP)
			{
				icmp_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmp_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMPV6)
			{
				icmpv6_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == YANET_TRANSPORT_TYPE_UNKNOWN)
			{
				mask ^= (1u << mbuf_i);
			}
		}

		transport_key.network_flags = acl.network_flags.array[metadata->network_flags];
	}

	acl.transport_table->lookup(hashes,
	                            key_acl.transports,
	                            value_acl.transports,
	                            acl_ingress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& transport_value = value_acl.transports[mbuf_i];
		auto& total_key = key_acl.totals[mbuf_i];

		total_key.acl_id = metadata->flow.data.aclId;
		total_key.transport_id = transport_value;
	}

	acl.total_table->lookup(hashes,
	                        key_acl.totals,
	                        value_acl.totals,
	                        acl_ingress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_ingress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_ingress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (!(mask & (1u << mbuf_i)))
		{
			counters[(uint32_t)common::globalBase::static_counter_type::acl_ingress_v6_broken_packet]++;
			drop(mbuf);
			continue;
		}

		auto total_value = value_acl.totals[mbuf_i];
		if (total_value & 0x80000000u)
		{
			total_value = 0; ///< default
		}

		const auto& value = acl.values[total_value];

		if (value.flow.type == common::globalBase::eFlowType::drop)
		{
			// Try to match against stateful dynamic rules. If so - a packet will be handled.
			if (acl_try_keepstate(mbuf))
			{
				continue;
			}
		}

		aclCounters[value.flow.counter_id]++;

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::log)
		{
			acl_log(mbuf, value.flow, metadata->flow.data.aclId);
		}

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::keepstate)
		{
			acl_create_keepstate(mbuf, metadata->flow.data.aclId, value.flow);
		}

		acl_ingress_flow(mbuf, value.flow);
	}

	acl_ingress_stack6.clear();
}

inline void cWorker::acl_ingress_flow(rte_mbuf* mbuf,
                                      const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::tun64_ipv4_checked)
	{
		tun64_ipv4_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::tun64_ipv6_checked)
	{
		tun64_ipv6_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::decap_checked)
	{
		decap_entry_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateful_lan)
	{
		nat64stateful_lan_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateful_wan)
	{
		nat64stateful_wan_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_ingress_checked)
	{
		nat64stateless_ingress_entry_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_ingress_icmp)
	{
		nat64stateless_ingress_entry_icmp(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_ingress_fragmentation)
	{
		nat64stateless_ingress_entry_fragmentation(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_checked)
	{
		nat64stateless_egress_entry_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_icmp)
	{
		nat64stateless_egress_entry_icmp(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_fragmentation)
	{
		nat64stateless_egress_entry_fragmentation(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_farm)
	{
		nat64stateless_egress_entry_farm(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::balancer)
	{
		balancer_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::balancer_icmp_reply)
	{
		balancer_icmp_reply_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::balancer_icmp_forward)
	{
		balancer_icmp_forward_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::balancer_fragment)
	{
		balancer_fragment_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::dregress)
	{
		dregress_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_local)
	{
		route_entry_local(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::after_early_decap && !metadata->already_early_decapped)
	{
		early_decap(mbuf);
		after_early_decap_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		stats.acl_ingress_dropPackets++; ///< @todo
		drop(mbuf);
	}
}

inline void cWorker::tun64_ipv4_checked(rte_mbuf* mbuf)
{
	tun64_stack4.insert(mbuf);
}

inline void cWorker::tun64_ipv6_checked(rte_mbuf* mbuf)
{
	tun64_stack6.insert(mbuf);
}

inline void cWorker::tun64_ipv4_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(tun64_stack4.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < tun64_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = tun64_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, const rte_ipv4_hdr*, metadata->network_headerOffset);

		tun64_keys[mbuf_i] = {metadata->flow.data.tun64Id, rte_be_to_cpu_32(ipv4Header->dst_addr)};
	}

	base.globalBase->tun64mappingsTable.lookup(tun64_keys, tun64_values, tun64_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < tun64_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = tun64_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& tunnel = base.globalBase->tun64tunnels[metadata->flow.data.tun64Id];
		const rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, const rte_ipv4_hdr*, metadata->network_headerOffset);

		if (tun64_values[mbuf_i] == nullptr)
		{
			counters[tunnel.flow.counter_id + 2]++; ///< common::tun64::stats_t.encap_dropped
			drop(mbuf);
			continue;
		}

		rte_pktmbuf_prepend(mbuf, sizeof(rte_ipv6_hdr));
		rte_memcpy(rte_pktmbuf_mtod(mbuf, char*),
		           rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_ipv6_hdr)),
		           metadata->network_headerOffset);

		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		ipv6Header->vtc_flow = rte_cpu_to_be_32((0x6 << 28) | (ipv4Header->type_of_service << 20));
		ipv6Header->payload_len = ipv4Header->total_length;
		ipv6Header->proto = IPPROTO_IPIP;
		ipv6Header->hop_limits = 64;
		rte_memcpy(ipv6Header->dst_addr, tun64_values[mbuf_i]->ipv6AddressDestination.bytes, 16);
		rte_memcpy(ipv6Header->src_addr, tunnel.ipv6AddressSource.bytes, 16);

		if (tunnel.srcRndEnabled)
		{
			((uint32_t *)ipv6Header->src_addr)[2] = ipv4Header->src_addr;
		}

		counters[tun64_values[mbuf_i]->counter_id]++;	///< common::tun64mapping::stats_t.encap_packets
		counters[tun64_values[mbuf_i]->counter_id + 1] += mbuf->pkt_len; ///< common::tun64mapping::stats_t.encap_bytes
		counters[tunnel.flow.counter_id]++; ///< common::tun64::stats_t.encap_packets
		counters[tunnel.flow.counter_id + 1] += mbuf->pkt_len; ///< common::tun64::stats_t.encap_bytes

		preparePacket(mbuf);
		tun64_flow(mbuf, tunnel.flow);
	}

	tun64_stack4.clear();
}

inline void cWorker::tun64_ipv6_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(tun64_stack6.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < tun64_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = tun64_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
	                   rte_pktmbuf_mtod(mbuf, char*),
			   metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
		const auto& tunnel = base.globalBase->tun64tunnels[metadata->flow.data.tun64Id];
		if (tunnel.ipv4DSCPFlags & YADECAP_GB_DSCP_FLAG_ALWAYS_MARK)
		{
			uint16_t sum = ~rte_be_to_cpu_16(ipv4Header->hdr_checksum);
			sum = csum_minus(sum, ipv4Header->type_of_service & 0xFC);
			sum = csum_plus(sum, tunnel.ipv4DSCPFlags & 0xFC);

			ipv4Header->hdr_checksum = ~rte_cpu_to_be_16(sum);

			ipv4Header->type_of_service &= 0x3; ///< ECN
			ipv4Header->type_of_service |= tunnel.ipv4DSCPFlags & 0xFC;
		}
		else if (tunnel.ipv4DSCPFlags & YADECAP_GB_DSCP_FLAG_MARK)
		{
			if (!(ipv4Header->type_of_service & 0xFC)) ///< DSCP == 0
			{
				uint16_t sum = ~rte_be_to_cpu_16(ipv4Header->hdr_checksum);
				sum = csum_plus(sum, tunnel.ipv4DSCPFlags & 0xFC);

				ipv4Header->hdr_checksum = ~rte_cpu_to_be_16(sum + (sum >> 16));

				ipv4Header->type_of_service |= tunnel.ipv4DSCPFlags & 0xFC;
			}
		}

		tun64_keys[mbuf_i] = {metadata->flow.data.tun64Id, rte_be_to_cpu_32(ipv4Header->src_addr)};
	}

	base.globalBase->tun64mappingsTable.lookup(tun64_keys, tun64_values, tun64_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < tun64_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = tun64_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& tunnel = base.globalBase->tun64tunnels[metadata->flow.data.tun64Id];

		if (tun64_values[mbuf_i] == nullptr)
		{
			counters[tunnel.flow.counter_id + 5]++; ///< common::tun64::stats_t.decap_unknown
		}
		else
		{
			counters[tun64_values[mbuf_i]->counter_id + 2]++;	///< common::tun64mapping::stats_t.decap_packets
			counters[tun64_values[mbuf_i]->counter_id + 3] += mbuf->pkt_len; ///< common::tun64mapping::stats_t.decap_bytes
		}

		counters[tunnel.flow.counter_id + 3]++; ///< common::tun64::stats_t.decap_packets
		counters[tunnel.flow.counter_id + 4] += mbuf->pkt_len; ///< common::tun64::stats_t.decap_bytes

		preparePacket(mbuf);
		tun64_flow(mbuf, tunnel.flow);
	}

	tun64_stack6.clear();
}

inline void cWorker::tun64_flow(rte_mbuf* mbuf,
                                const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::decap_entry_checked(rte_mbuf* mbuf)
{
	decap_stack.insert(mbuf);
}

inline void cWorker::decap_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(decap_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < decap_stack.mbufsCount;
	     mbuf_i++)
	{
		/// @todo: func
		rte_mbuf* mbuf = decap_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& decap = base.globalBase->decaps[metadata->flow.data.decapId];

		{
			rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
			metadata->flowLabel = rte_be_to_cpu_32(ipv6Header->vtc_flow) & 0x000FFFFF;
		}

		if (!decap_cut(mbuf))
		{
			controlPlane(mbuf);
			continue;
		}

		if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		{
			if (decap.ipv4DSCPFlags & YADECAP_GB_DSCP_FLAG_ALWAYS_MARK)
			{
				rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

				uint16_t sum = ~rte_be_to_cpu_16(ipv4Header->hdr_checksum);
				 // removing previous value of dscp from checksum to replace its with value from config
				sum = csum_minus(sum, ipv4Header->type_of_service & 0xFC);
				sum = csum_plus(sum, decap.ipv4DSCPFlags & 0xFC);

				ipv4Header->hdr_checksum = ~rte_cpu_to_be_16(sum);

				ipv4Header->type_of_service &= 0x3; ///< ECN
				ipv4Header->type_of_service |= decap.ipv4DSCPFlags & 0xFC;
			}
			else if (decap.ipv4DSCPFlags & YADECAP_GB_DSCP_FLAG_MARK)
			{
				rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

				if (!(ipv4Header->type_of_service & 0xFC)) ///< DSCP == 0
				{
					uint16_t sum = ~rte_be_to_cpu_16(ipv4Header->hdr_checksum);
					// DSCP is equal to 0 anyway (condition above), nothing to remove from checksum
					sum = csum_plus(sum, decap.ipv4DSCPFlags & 0xFC);

					ipv4Header->hdr_checksum = ~rte_cpu_to_be_16(sum);

					ipv4Header->type_of_service |= decap.ipv4DSCPFlags & 0xFC;
				}
			}
		}

		stats.decap_packets++;
		decap_flow(mbuf, decap.flow);
	}

	decap_stack.clear();
}

inline void cWorker::decap_flow(rte_mbuf* mbuf,
                                const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline bool cWorker::decap_cut(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	const auto& base = bases[localBaseId & 1];
	const auto& decap = base.globalBase->decaps[metadata->flow.data.decapId];

	if (metadata->transport_headerType == IPPROTO_GRE)
	{
		const rte_gre_hdr* greHeader = rte_pktmbuf_mtod_offset(mbuf, rte_gre_hdr*, metadata->transport_headerOffset);
		if (((*(uint32_t*)greHeader) & 0xFFFFFF4F) != 0x00080000) ///< |X|0|X|X|0|0|0x0800|. @todo: ACL_GRE
		{
			stats.decap_unknownExtensions++;
			return false;
		}

		metadata->transport_headerType = IPPROTO_IPIP;
		metadata->transport_headerOffset += 4u * ((__builtin_popcount((*(uint8_t*)greHeader) >> 4u)) + 1);
	}

	if (metadata->transport_headerType == IPPROTO_IPIP)
	{
		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
		           rte_pktmbuf_mtod(mbuf, char*),
		           metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		/// @todo: opt
		preparePacket(mbuf);

		return true;
	}
	else if (metadata->transport_headerType == IPPROTO_IPV6 &&
	         decap.flag_ipv6_enabled)
	{
		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
		           rte_pktmbuf_mtod(mbuf, char*),
		           metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

		/// @todo: opt
		preparePacket(mbuf);

		return true;
	}

	stats.decap_unknownExtensions++;
	return false;
}

inline void cWorker::route_entry(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		route_stack4.insert(mbuf);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		route_stack6.insert(mbuf);
	}
	else
	{
		controlPlane(mbuf);
	}
}

inline void cWorker::route_entry_local(rte_mbuf* mbuf)
{
	slowWorker_entry_highPriority(mbuf, common::globalBase::eFlowType::slowWorker_kni_local);
}

inline void cWorker::route_handle4()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(route_stack4.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		route_ipv4_keys[mbuf_i] = ipv4Header->dst_addr;

		calcHash(mbuf);
	}

	base.globalBase->route_lpm4.lookup(route_ipv4_keys, route_ipv4_values, route_stack4.mbufsCount);
	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (route_ipv4_values[mbuf_i] == dataplane::lpmValueIdInvalid)
		{
			stats.interface_lookupMisses++;
			rte_pktmbuf_free(mbuf);
			continue;
		}

		const auto& route_value = base.globalBase->route_values[route_ipv4_values[mbuf_i]];
		if (route_value.type == common::globalBase::eNexthopType::interface)
		{
			const auto& nexthop = route_value.interface.nexthops[metadata->hash % route_value.interface.ecmpCount];
			const auto& targetInterface = base.globalBase->interfaces[nexthop.interfaceId];

			rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
			if (ipv4Header->time_to_live <= 1)
			{
				stats.interface_hopLimits++;
				drop(mbuf);
				continue;
			}

			if (targetInterface.neighbor_ether_address_v4.addr_bytes[0] == 1)
			{
				stats.interface_neighbor_invalid++;
				drop(mbuf);
				continue;
			}

			generic_rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);
			rte_ether_addr_copy(&targetInterface.neighbor_ether_address_v4, &ethernetHeader->dst_addr);

			route_nexthop(mbuf, nexthop);

			ipv4Header->time_to_live--;
			unsigned int sum = ((~rte_be_to_cpu_16(ipv4Header->hdr_checksum)) & 0xFFFF) + 0xFEFF;
			ipv4Header->hdr_checksum = ~rte_cpu_to_be_16(sum + (sum >> 16));

			route_flow(mbuf, targetInterface.flow, targetInterface.aclId);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::controlPlane)
		{
			controlPlane(mbuf);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::repeat)
		{
			metadata->repeat_ttl--;
			if (metadata->repeat_ttl == 0)
			{
				stats.repeat_ttl++;
				rte_pktmbuf_free(mbuf);
			}
			else
			{
				slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_repeat);
			}
			continue;
		}
		else
		{
			drop(mbuf);
			continue;
		}
	}

	route_stack4.clear();
}

inline void cWorker::route_handle6()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(route_stack6.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		rte_memcpy(route_ipv6_keys[mbuf_i].bytes, ipv6Header->dst_addr, 16);

		calcHash(mbuf);
	}

	base.globalBase->route_lpm6.lookup(route_ipv6_keys, route_ipv6_values, route_stack6.mbufsCount);
	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (route_ipv6_values[mbuf_i] == dataplane::lpmValueIdInvalid)
		{
			stats.interface_lookupMisses++;
			rte_pktmbuf_free(mbuf);
			continue;
		}

		const auto& route_value = base.globalBase->route_values[route_ipv6_values[mbuf_i]];
		if (route_value.type == common::globalBase::eNexthopType::interface)
		{
			const auto& nexthop = route_value.interface.nexthops[metadata->hash % route_value.interface.ecmpCount];
			const auto& targetInterface = base.globalBase->interfaces[nexthop.interfaceId];

			rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
			if (ipv6Header->hop_limits <= 1)
			{
				stats.interface_hopLimits++;
				drop(mbuf);
				continue;
			}

			if (targetInterface.neighbor_ether_address_v6.addr_bytes[0] == 1)
			{
				stats.interface_neighbor_invalid++;
				drop(mbuf);
				continue;
			}

			generic_rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);
			rte_ether_addr_copy(&targetInterface.neighbor_ether_address_v6, &ethernetHeader->dst_addr);

			route_nexthop(mbuf, nexthop);

			ipv6Header->hop_limits--;

			route_flow(mbuf, targetInterface.flow, targetInterface.aclId);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::controlPlane)
		{
			controlPlane(mbuf);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::repeat)
		{
			metadata->repeat_ttl--;
			if (metadata->repeat_ttl == 0)
			{
				stats.repeat_ttl++;
				rte_pktmbuf_free(mbuf);
			}
			else
			{
				slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_repeat);
			}
			continue;
		}
		else
		{
			drop(mbuf);
			continue;
		}
	}

	route_stack6.clear();
}

inline void cWorker::route_flow(rte_mbuf* mbuf,
                                const common::globalBase::tFlow& flow,
                                tAclId aclId)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::logicalPort_egress)
	{
		logicalPort_egress_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::acl_egress)
	{
		acl_egress_entry(mbuf, aclId);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::route_nexthop(rte_mbuf* mbuf,
                                   const dataplane::globalBase::nexthop& nexthop)
{
	/// @todo: fix offset

	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (nexthop.labelExpTransport >> 24)
	{
		/// labelled

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(YADECAP_ETHER_TYPE_MPLS);

		metadata->network_headerType = rte_cpu_to_be_16(YADECAP_ETHER_TYPE_MPLS);

		if (nexthop.labelExpService >> 24)
		{
			rte_pktmbuf_prepend(mbuf, YADECAP_MPLS_HEADER_SIZE * 2);
			memmove(rte_pktmbuf_mtod(mbuf, char*),
			        rte_pktmbuf_mtod_offset(mbuf, char*, YADECAP_MPLS_HEADER_SIZE * 2),
			        metadata->network_headerOffset);

			uint32_t* mplsHeaderTransport = rte_pktmbuf_mtod_offset(mbuf, uint32_t*, metadata->network_headerOffset);
			*mplsHeaderTransport = nexthop.labelExpTransport;

			uint32_t* mplsHeaderService = rte_pktmbuf_mtod_offset(mbuf, uint32_t*, metadata->network_headerOffset + YADECAP_MPLS_HEADER_SIZE);
			*mplsHeaderService = nexthop.labelExpService;
		}
		else
		{
			rte_pktmbuf_prepend(mbuf, YADECAP_MPLS_HEADER_SIZE);
			memmove(rte_pktmbuf_mtod(mbuf, char*),
			        rte_pktmbuf_mtod_offset(mbuf, char*, YADECAP_MPLS_HEADER_SIZE),
			        metadata->network_headerOffset);

			uint32_t* mplsHeaderTransport = rte_pktmbuf_mtod_offset(mbuf, uint32_t*, metadata->network_headerOffset);
			*mplsHeaderTransport = nexthop.labelExpTransport;
		}
	}
}

inline void cWorker::route_tunnel_entry(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		route_tunnel_stack4.insert(mbuf);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		route_tunnel_stack6.insert(mbuf);
	}
	else
	{
		controlPlane(mbuf);
	}
}

inline void cWorker::route_tunnel_handle4()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(route_tunnel_stack4.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_tunnel_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_tunnel_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		route_ipv4_keys[mbuf_i] = ipv4Header->dst_addr;

		calcHash(mbuf);
		metadata->hash = rte_hash_crc(&metadata->flowLabel, 4, metadata->hash);
	}

	base.globalBase->route_tunnel_lpm4.lookup(route_ipv4_keys, route_ipv4_values, route_tunnel_stack4.mbufsCount);
	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_tunnel_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_tunnel_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (route_ipv4_values[mbuf_i] == dataplane::lpmValueIdInvalid)
		{
			stats.interface_lookupMisses++;
			rte_pktmbuf_free(mbuf);
			continue;
		}

		const auto& route_value = base.globalBase->route_tunnel_values[route_ipv4_values[mbuf_i]];
		if (route_value.type == common::globalBase::eNexthopType::interface)
		{
			const auto nexthop_i = base.globalBase->route_tunnel_weights[route_value.interface.weight_start + (metadata->hash % route_value.interface.weight_size)];

			const auto& nexthop = route_value.interface.nexthops[nexthop_i];
			const auto& targetInterface = base.globalBase->interfaces[nexthop.interface_id];

			rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
			if (ipv4Header->time_to_live <= 1)
			{
				stats.interface_hopLimits++;
				drop(mbuf);
				continue;
			}

			if (targetInterface.neighbor_ether_address_v4.addr_bytes[0] == 1)
			{
				stats.interface_neighbor_invalid++;
				drop(mbuf);
				continue;
			}

			/// counters[nexthop.counter_id]++;
			counters[nexthop.atomic1 >> 8]++;
			counters[(nexthop.atomic1 >> 8) + 1] += mbuf->pkt_len;

			generic_rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);
			rte_ether_addr_copy(&targetInterface.neighbor_ether_address_v4, &ethernetHeader->dst_addr);

			route_tunnel_nexthop(mbuf, nexthop);

			ipv4Header->time_to_live--;
			unsigned int sum = ((~rte_be_to_cpu_16(ipv4Header->hdr_checksum)) & 0xFFFF) + 0xFEFF;
			ipv4Header->hdr_checksum = ~rte_cpu_to_be_16(sum + (sum >> 16));

			route_flow(mbuf, targetInterface.flow, targetInterface.aclId);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::controlPlane)
		{
			controlPlane(mbuf);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::repeat)
		{
			metadata->repeat_ttl--;
			if (metadata->repeat_ttl == 0)
			{
				stats.repeat_ttl++;
				rte_pktmbuf_free(mbuf);
			}
			else
			{
				slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_repeat);
			}
			continue;
		}
		else
		{
			drop(mbuf);
			continue;
		}
	}

	route_tunnel_stack4.clear();
}

inline void cWorker::route_tunnel_handle6()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(route_tunnel_stack6.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_tunnel_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_tunnel_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		rte_memcpy(route_ipv6_keys[mbuf_i].bytes, ipv6Header->dst_addr, 16);

		calcHash(mbuf);
		metadata->hash = rte_hash_crc(&metadata->flowLabel, 4, metadata->hash);
	}

	base.globalBase->route_tunnel_lpm6.lookup(route_ipv6_keys, route_ipv6_values, route_tunnel_stack6.mbufsCount);
	for (unsigned int mbuf_i = 0;
	     mbuf_i < route_tunnel_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = route_tunnel_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (route_ipv6_values[mbuf_i] == dataplane::lpmValueIdInvalid)
		{
			stats.interface_lookupMisses++;
			rte_pktmbuf_free(mbuf);
			continue;
		}

		const auto& route_value = base.globalBase->route_tunnel_values[route_ipv6_values[mbuf_i]];
		if (route_value.type == common::globalBase::eNexthopType::interface)
		{
			const auto nexthop_i = base.globalBase->route_tunnel_weights[route_value.interface.weight_start + (metadata->hash % route_value.interface.weight_size)];

			const auto& nexthop = route_value.interface.nexthops[nexthop_i];
			const auto& targetInterface = base.globalBase->interfaces[nexthop.interface_id];

			rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
			if (ipv6Header->hop_limits <= 1)
			{
				stats.interface_hopLimits++;
				drop(mbuf);
				continue;
			}

			if (targetInterface.neighbor_ether_address_v6.addr_bytes[0] == 1)
			{
				stats.interface_neighbor_invalid++;
				drop(mbuf);
				continue;
			}

			/// counters[nexthop.counter_id]++;
			counters[nexthop.atomic1 >> 8]++;
			counters[(nexthop.atomic1 >> 8) + 1] += mbuf->pkt_len;

			generic_rte_ether_hdr* ethernetHeader = rte_pktmbuf_mtod(mbuf, generic_rte_ether_hdr*);
			rte_ether_addr_copy(&targetInterface.neighbor_ether_address_v6, &ethernetHeader->dst_addr);

			route_tunnel_nexthop(mbuf, nexthop);

			ipv6Header->hop_limits--;

			route_flow(mbuf, targetInterface.flow, targetInterface.aclId);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::controlPlane)
		{
			controlPlane(mbuf);
			continue;
		}
		else if (route_value.type == common::globalBase::eNexthopType::repeat)
		{
			metadata->repeat_ttl--;
			if (metadata->repeat_ttl == 0)
			{
				stats.repeat_ttl++;
				rte_pktmbuf_free(mbuf);
			}
			else
			{
				slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_repeat);
			}
			continue;
		}
		else
		{
			drop(mbuf);
			continue;
		}
	}

	route_tunnel_stack6.clear();
}

inline void cWorker::route_tunnel_nexthop(rte_mbuf* mbuf,
                                          const dataplane::globalBase::nexthop_tunnel_t& nexthop)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	const auto& base = bases[localBaseId & 1];
	const auto& route = base.globalBase->routes[metadata->flow.data.routeId];

	if (nexthop.label == 3) ///< @todo: DEFINE
	{
		/// fallback to default
		return;
	}

	uint16_t payload_length;
	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4HeaderInner = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		/// @todo: mpls_header_t
		rte_pktmbuf_prepend(mbuf, sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE);
		rte_memcpy(rte_pktmbuf_mtod(mbuf, char*),
		           rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE),
		           metadata->network_headerOffset);

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		ipv4Header->version_ihl = 0x45;
		ipv4Header->type_of_service = ipv4HeaderInner->type_of_service;
		ipv4Header->total_length = rte_cpu_to_be_16((sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE) + rte_be_to_cpu_16(ipv4HeaderInner->total_length));
		ipv4Header->packet_id = ipv4HeaderInner->packet_id;
		ipv4Header->fragment_offset = 0;
		ipv4Header->time_to_live = 64;
		ipv4Header->next_proto_id = IPPROTO_UDP;
		ipv4Header->hdr_checksum = 0;
		ipv4Header->src_addr = route.ipv4AddressSource.address;
		ipv4Header->dst_addr = nexthop.nexthop_address.mapped_ipv4_address.address;

		yanet_ipv4_checksum(ipv4Header);

		metadata->transport_headerOffset = metadata->network_headerOffset + sizeof(rte_ipv4_hdr);

		payload_length = rte_be_to_cpu_16(ipv4HeaderInner->total_length);
	}
	else
	{
		rte_ipv6_hdr* ipv6HeaderInner = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		/// @todo: mpls_header_t
		rte_pktmbuf_prepend(mbuf, sizeof(rte_ipv6_hdr) + sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE);
		rte_memcpy(rte_pktmbuf_mtod(mbuf, char*),
		           rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_ipv6_hdr) + sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE),
		           metadata->network_headerOffset);

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		ipv6Header->vtc_flow = ipv6HeaderInner->vtc_flow & rte_cpu_to_be_32(0xFFF00000);
		ipv6Header->payload_len = rte_cpu_to_be_16(sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE + sizeof(rte_ipv6_hdr) + rte_be_to_cpu_16(ipv6HeaderInner->payload_len));
		ipv6Header->proto = IPPROTO_UDP;
		ipv6Header->hop_limits = 64;
		rte_memcpy(ipv6Header->src_addr, route.ipv6AddressSource.bytes, 16);
		rte_memcpy(ipv6Header->dst_addr, nexthop.nexthop_address.bytes, 16);

		metadata->transport_headerOffset = metadata->network_headerOffset + sizeof(rte_ipv6_hdr);

		payload_length = sizeof(rte_ipv6_hdr) + rte_be_to_cpu_16(ipv6HeaderInner->payload_len);
	}

	{
		rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

		udpHeader->src_port = rte_cpu_to_be_16(metadata->hash | 0xC000u);
		udpHeader->dst_port = route.udpDestinationPort;
		udpHeader->dgram_len = rte_cpu_to_be_16(sizeof(rte_udp_hdr) + YADECAP_MPLS_HEADER_SIZE + payload_length);
		udpHeader->dgram_cksum = 0;
	}

	{
		uint32_t* mplsHeaderTransport = rte_pktmbuf_mtod_offset(mbuf, uint32_t*, metadata->transport_headerOffset + sizeof(rte_udp_hdr));

		*mplsHeaderTransport = rte_cpu_to_be_32(((nexthop.label & 0xFFFFF) << 12) | (1 << 8)) | ((uint8_t)255 << 24);
	}

	/// @todo: opt
	preparePacket(mbuf);
}

static inline void ipv4_to_ipv6(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

	uint16_t ipv6Header_size = sizeof(rte_ipv6_hdr) + ((metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT) ? sizeof(ipv6_extension_fragment_t) : 0);
	uint16_t ipv4Header_size = (metadata->transport_headerOffset - metadata->network_headerOffset);

	uint16_t packetId = ipv4Header->packet_id;
	uint16_t fragmentOffset = ipv4Header->fragment_offset;

	ipv6_header_without_addresses_t ipv6HeaderShort;
	ipv6HeaderShort.vtc_flow = rte_cpu_to_be_32((0x6 << 28) | (ipv4Header->type_of_service << 20)); ///< @todo: flow label
	ipv6HeaderShort.payload_len = rte_cpu_to_be_16(rte_be_to_cpu_16(ipv4Header->total_length) - 4 * (ipv4Header->version_ihl & 0x0F) + (ipv6Header_size - sizeof(rte_ipv6_hdr)));
	ipv6HeaderShort.proto = metadata->transport_headerType;
	ipv6HeaderShort.hop_limits = ipv4Header->time_to_live;

	if (ipv6Header_size >= ipv4Header_size)
	{
		rte_pktmbuf_prepend(mbuf, ipv6Header_size - ipv4Header_size);
		memmove(rte_pktmbuf_mtod(mbuf, char*),
		        rte_pktmbuf_mtod_offset(mbuf, char*, ipv6Header_size - ipv4Header_size),
		        metadata->network_headerOffset);
	}
	else
	{
		memmove(rte_pktmbuf_mtod_offset(mbuf, char*, ipv4Header_size - ipv6Header_size),
		        rte_pktmbuf_mtod(mbuf, char*),
		        metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, ipv4Header_size - ipv6Header_size);
	}

	metadata->network_fragmentHeaderOffset = metadata->network_headerOffset + sizeof(rte_ipv6_hdr);
	metadata->transport_headerOffset = metadata->network_headerOffset + ipv6Header_size;

	/// @todo: check for ethernetHeader or vlanHeader
	uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
	*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

	rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

	rte_memcpy(ipv6Header, &ipv6HeaderShort, sizeof(ipv6_header_without_addresses_t));

	if (metadata->transport_headerType == IPPROTO_ICMP)
	{
		ipv6Header->proto = IPPROTO_ICMPV6;
		metadata->transport_headerType = IPPROTO_ICMPV6;
	}

	if (metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT)
	{
		ipv6_extension_fragment_t* extension = rte_pktmbuf_mtod_offset(mbuf, ipv6_extension_fragment_t*, metadata->network_fragmentHeaderOffset);

		extension->nextHeader = metadata->transport_headerType;
		extension->reserved = 0;
		extension->offsetFlagM = rte_cpu_to_be_16(rte_be_to_cpu_16(fragmentOffset) << 3);
		extension->offsetFlagM |= (fragmentOffset & 0x0020) << 3;
		extension->identification = packetId;

		ipv6Header->proto = IPPROTO_FRAGMENT;

		metadata->network_flags |= YANET_NETWORK_FLAG_HAS_EXTENSION;
	}
}

inline uint32_t nat64stateful_hash(const ipv6_address_t& ipv6_source)
{
	uint32_t result = 0;

	unsigned int offset = 0;

	for (unsigned int i = 0;
	     i < sizeof(ipv6_address_t) / 8;
	     i++)
	{
		result = rte_hash_crc_8byte(*(((const uint64_t*)&ipv6_source) + offset / 8), result);
		offset += 8;
	}

	return result;
}

inline void cWorker::nat64stateful_lan_entry(rte_mbuf* mbuf)
{
	nat64stateful_lan_stack.insert(mbuf);
}

inline void cWorker::nat64stateful_lan_handle()
{
	const auto& base = bases[localBaseId & 1];
	auto* nat64stateful_lan_state = basePermanently.globalBaseAtomic->nat64stateful_lan_state;
	auto* nat64stateful_wan_state = basePermanently.globalBaseAtomic->nat64stateful_wan_state;

	if (unlikely(nat64stateful_lan_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < nat64stateful_lan_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = nat64stateful_lan_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_ipv6_hdr* ipv6_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		auto& key = nat64stateful_lan_keys[mbuf_i];
		key.nat64stateful_id = metadata->flow.data.nat64stateful_id;

		rte_memcpy(key.ipv6_source.bytes, ipv6_header->src_addr, 16);
		rte_memcpy(key.ipv6_destination.bytes, ipv6_header->dst_addr, 16);

		key.proto = metadata->transport_headerType;
		if (key.proto == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcp_header = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			key.port_source = tcp_header->src_port;
			key.port_destination = tcp_header->dst_port;
		}
		else if (key.proto == IPPROTO_UDP)
		{
			rte_udp_hdr* udp_header = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			key.port_source = udp_header->src_port;
			key.port_destination = udp_header->dst_port;
		}
		else if (key.proto == IPPROTO_ICMPV6)
		{
			icmpv6_header_t* icmpv6_header = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

			key.port_source = icmpv6_header->identifier;
			key.port_destination = icmpv6_header->identifier;
		}
		else
		{
			key.port_source = 0;
			key.port_destination = 0;
		}
	}

	dataplane::globalBase::nat64stateful_lan_value value;
	for (unsigned int mbuf_i = 0;
	     mbuf_i < nat64stateful_lan_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = nat64stateful_lan_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& key = nat64stateful_lan_keys[mbuf_i];

		const auto& nat64stateful = base.globalBase->nat64statefuls[metadata->flow.data.nat64stateful_id];

		dataplane::globalBase::nat64stateful_lan_value* value_lookup;
		dataplane::spinlock_nonrecursive_t* locker;
		const uint32_t hash = nat64stateful_lan_state->lookup(key, value_lookup, locker);
		if (value_lookup)
		{
			value_lookup->timestamp_last_packet = basePermanently.globalBaseAtomic->currentTime;
			value = *value_lookup;
			locker->unlock();
		}
		else
		{
			locker->unlock();

			if (!nat64stateful.pool_size)
			{
				counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::pool_is_empty]++;
				drop(mbuf);
				continue;
			}

			uint32_t client_hash = nat64stateful_hash(key.ipv6_source);

			uint16_t port_step = rte_cpu_to_be_16((client_hash >> 17) / YANET_CONFIG_NAT64STATEFUL_INSERT_TRIES);
			if ((port_step & 0x00F8) == 0) ///< 0-1023. @todo: config
			{
				port_step += 0x0004; ///< + 1024
			}

			dataplane::globalBase::nat64stateful_wan_key wan_key;
			wan_key.nat64stateful_id = key.nat64stateful_id;
			wan_key.proto = key.proto;
			wan_key.ipv4_source.address = *(uint32_t*)&key.ipv6_destination.bytes[12]; ///< @todo [12] -> [any]
			wan_key.ipv4_destination = base.globalBase->nat64stateful_pool[nat64stateful.pool_start + client_hash % nat64stateful.pool_size];
			wan_key.port_source = key.port_destination;
			wan_key.port_destination = key.port_source;

			uint32_t wan_hash;
			dataplane::globalBase::nat64stateful_wan_value* wan_value_lookup;
			dataplane::spinlock_nonrecursive_t* wan_locker;
			for (unsigned int try_i = 0;
			     try_i < YANET_CONFIG_NAT64STATEFUL_INSERT_TRIES;
			     try_i++)
			{
				wan_key.port_destination &= basePermanently.nat64stateful_numa_mask;
				wan_key.port_destination ^= basePermanently.nat64stateful_numa_id;

				if ((wan_key.port_destination & 0x00F8) == 0) ///< 0-1023. @todo: config
				{
					wan_key.port_destination += 0x0004; ///< + 1024
				}

				wan_hash = nat64stateful_wan_state->lookup(wan_key, wan_value_lookup, wan_locker);
				if (!wan_value_lookup)
				{
					/// success
					break;
				}
				wan_locker->unlock();

				counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::tries]++;

				wan_key.port_destination += port_step;
			}

			if (wan_value_lookup)
			{
				/// unluck. state not created

				counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::tries_failed]++;
				drop(mbuf);
				continue;
			}

			{
				dataplane::globalBase::nat64stateful_wan_value wan_value;
				memcpy(wan_value.ipv6_source.bytes, key.ipv6_destination.bytes, 12);
				wan_value.port_destination = key.port_source;
				wan_value.timestamp_last_packet = basePermanently.globalBaseAtomic->currentTime;
				wan_value.ipv6_destination = key.ipv6_source;

				/// counter_id:
				///   0 - insert failed
				///   1 - insert done
				uint32_t counter_id = nat64stateful_wan_state->insert(wan_hash, wan_key, wan_value);
				wan_locker->unlock();

				counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::wan_state_insert + counter_id]++;

				/// @todo: create cross-numa state over slowworker?
				for (unsigned int numa_i = 0;
				     numa_i < YANET_CONFIG_NUMA_SIZE;
				     numa_i++)
				{
					auto* globalbase_atomic = basePermanently.globalBaseAtomics[numa_i];
					if (globalbase_atomic == basePermanently.globalBaseAtomic)
					{
						continue;
					}
					else if (globalbase_atomic == nullptr)
					{
						break;
					}

					/// counter_id:
					///   0 - insert failed
					///   1 - insert done
					uint32_t counter_id = globalbase_atomic->nat64stateful_wan_state->insert_or_update(wan_key, wan_value);
					counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::wan_state_cross_numa_insert + counter_id]++;
				}
			}

			{
				value.ipv4_source = wan_key.ipv4_destination;
				value.port_source = wan_key.port_destination;
				value.timestamp_last_packet = basePermanently.globalBaseAtomic->currentTime;

				locker->lock();

				/// counter_id:
				///   0 - insert failed
				///   1 - insert done
				uint32_t counter_id = nat64stateful_lan_state->insert(hash, key, value);
				locker->unlock();

				counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::lan_state_insert + counter_id]++;

				/// @todo: create cross-numa state over slowworker?
				for (unsigned int numa_i = 0;
				     numa_i < YANET_CONFIG_NUMA_SIZE;
				     numa_i++)
				{
					auto* globalbase_atomic = basePermanently.globalBaseAtomics[numa_i];
					if (globalbase_atomic == basePermanently.globalBaseAtomic)
					{
						continue;
					}
					else if (globalbase_atomic == nullptr)
					{
						break;
					}

					/// counter_id:
					///   0 - insert failed
					///   1 - insert done
					uint32_t counter_id = globalbase_atomic->nat64stateful_lan_state->insert_or_update(key, value);
					counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::lan_state_cross_numa_insert + counter_id]++;
				}
			}
		}

		nat64stateful_lan_translation(mbuf, value);

		if (nat64stateful.ipv4_dscp_flags & YADECAP_GB_DSCP_FLAG_ALWAYS_MARK)
		{
			rte_ipv4_hdr* ipv4_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

			uint16_t sum = ~rte_be_to_cpu_16(ipv4_header->hdr_checksum);
			sum = csum_minus(sum, ipv4_header->type_of_service & 0xFC);
			sum = csum_plus(sum, nat64stateful.ipv4_dscp_flags & 0xFC);

			ipv4_header->hdr_checksum = ~rte_cpu_to_be_16(sum);

			ipv4_header->type_of_service &= 0x3; ///< ECN
			ipv4_header->type_of_service |= nat64stateful.ipv4_dscp_flags & 0xFC;
		}
		else if (nat64stateful.ipv4_dscp_flags & YADECAP_GB_DSCP_FLAG_MARK)
		{
			rte_ipv4_hdr* ipv4_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

			if (!(ipv4_header->type_of_service & 0xFC)) ///< DSCP == 0
			{
				uint16_t sum = ~rte_be_to_cpu_16(ipv4_header->hdr_checksum);
				sum = csum_plus(sum, nat64stateful.ipv4_dscp_flags & 0xFC);

				ipv4_header->hdr_checksum = ~rte_cpu_to_be_16(sum);

				ipv4_header->type_of_service |= nat64stateful.ipv4_dscp_flags & 0xFC;
			}
		}

		counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::lan_packets]++;
		counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::lan_bytes] += mbuf->pkt_len;
		nat64stateful_lan_flow(mbuf, nat64stateful.flow);
	}

	nat64stateful_lan_stack.clear();
}

inline void cWorker::nat64stateful_lan_translation(rte_mbuf* mbuf,
                                                   const dataplane::globalBase::nat64stateful_lan_value& value)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

#ifdef CONFIG_YADECAP_AUTOTEST
#else // CONFIG_YADECAP_AUTOTEST
	nat64stateful_packet_id++;
#endif // CONFIG_YADECAP_AUTOTEST

	rte_ipv6_hdr* ipv6_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
	rte_ipv4_hdr* ipv4_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->transport_headerOffset - sizeof(rte_ipv4_hdr));

	uint32_t ipv4_address_destination = *(uint32_t*)&ipv6_header->dst_addr[12]; ///< @todo: [12] -> [any]
	uint16_t payload_length = rte_be_to_cpu_16(ipv6_header->payload_len);

	uint16_t checksum_before = yanet_checksum(&ipv6_header->src_addr[0], 32);

	ipv4_header->version_ihl = 0x45;
	ipv4_header->type_of_service = (rte_be_to_cpu_32(ipv6_header->vtc_flow) >> 20) & 0xFF;

	/// @todo: ipv4_dscp_flags

	ipv4_header->total_length = rte_cpu_to_be_16(payload_length + sizeof(rte_ipv4_hdr) - (metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv6_hdr)));
	ipv4_header->packet_id = nat64stateful_packet_id;
	ipv4_header->fragment_offset = 0;
	ipv4_header->time_to_live = ipv6_header->hop_limits;
	ipv4_header->next_proto_id = metadata->transport_headerType;
	ipv4_header->src_addr = value.ipv4_source.address;
	ipv4_header->dst_addr = ipv4_address_destination;

	if (metadata->transport_headerType == IPPROTO_ICMPV6)
	{
		ipv4_header->next_proto_id = IPPROTO_ICMP;
	}

	yanet_ipv4_checksum(ipv4_header);

	uint16_t checksum_after = yanet_checksum(&ipv4_header->src_addr, 8);

	if (metadata->transport_headerType == IPPROTO_TCP)
	{
		rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

		checksum_before = csum_plus(checksum_before, tcpHeader->src_port);
		tcpHeader->src_port = value.port_source;
		checksum_after = csum_plus(checksum_after, tcpHeader->src_port);

		yanet_tcp_checksum_v6_to_v4(tcpHeader, checksum_before, checksum_after);
	}
	else if (metadata->transport_headerType == IPPROTO_UDP)
	{
		rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

		checksum_before = csum_plus(checksum_before, udpHeader->src_port);
		udpHeader->src_port = value.port_source;
		checksum_after = csum_plus(checksum_after, udpHeader->src_port);

		yanet_udp_checksum_v6_to_v4(udpHeader, checksum_before, checksum_after);
	}
	else if (metadata->transport_headerType == IPPROTO_ICMPV6)
	{
		icmpv6_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

		checksum_before = csum_plus(checksum_before, rte_cpu_to_be_16(IPPROTO_ICMPV6));
		checksum_before = csum_plus(checksum_before, rte_cpu_to_be_16(payload_length));
		checksum_before = csum_plus(checksum_before, icmpHeader->typeCode);
		checksum_before = csum_plus(checksum_before, icmpHeader->identifier);

		if (icmpHeader->type == ICMP6_ECHO_REQUEST)
		{
			icmpHeader->type = ICMP_ECHO;
		}
		icmpHeader->identifier = value.port_source;

		checksum_after = csum_plus(0, icmpHeader->typeCode);
		checksum_after = csum_plus(checksum_after, icmpHeader->identifier);

		yanet_icmp_checksum_v6_to_v4(icmpHeader, checksum_before, checksum_after);
	}

	{
		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv4_hdr)),
		           rte_pktmbuf_mtod(mbuf, char*),
		           metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv4_hdr));

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	}

	preparePacket(mbuf);
}

inline void cWorker::nat64stateful_lan_flow(rte_mbuf* mbuf,
                                            const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::nat64stateful_wan_entry(rte_mbuf* mbuf)
{
	nat64stateful_wan_stack.insert(mbuf);
}

inline void cWorker::nat64stateful_wan_handle()
{
	const auto& base = bases[localBaseId & 1];
	auto* nat64stateful_wan_state = basePermanently.globalBaseAtomic->nat64stateful_wan_state;

	if (unlikely(nat64stateful_wan_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < nat64stateful_wan_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = nat64stateful_wan_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_ipv4_hdr* ipv4_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		auto& key = nat64stateful_wan_keys[mbuf_i];
		key.nat64stateful_id = metadata->flow.data.nat64stateful_id;

		key.ipv4_source.address = ipv4_header->src_addr;
		key.ipv4_destination.address = ipv4_header->dst_addr;

		key.proto = metadata->transport_headerType;
		if (key.proto == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcp_header = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			key.port_source = tcp_header->src_port;
			key.port_destination = tcp_header->dst_port;
		}
		else if (key.proto == IPPROTO_UDP)
		{
			rte_udp_hdr* udp_header = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			key.port_source = udp_header->src_port;
			key.port_destination = udp_header->dst_port;
		}
		else if (key.proto == IPPROTO_ICMP)
		{
			icmpv4_header_t* icmpv4_header = rte_pktmbuf_mtod_offset(mbuf, icmpv4_header_t*, metadata->transport_headerOffset);

			key.proto = IPPROTO_ICMPV6; ///< for correct lookup
			key.port_source = icmpv4_header->identifier;
			key.port_destination = icmpv4_header->identifier;
		}
		else
		{
			key.port_source = 0;
			key.port_destination = 0;
		}
	}

	dataplane::globalBase::nat64stateful_wan_value value;
	for (unsigned int mbuf_i = 0;
	     mbuf_i < nat64stateful_wan_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = nat64stateful_wan_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& key = nat64stateful_wan_keys[mbuf_i];

		const auto& nat64stateful = base.globalBase->nat64statefuls[metadata->flow.data.nat64stateful_id];

		dataplane::globalBase::nat64stateful_wan_value* value_lookup;
		dataplane::spinlock_nonrecursive_t* locker;
		nat64stateful_wan_state->lookup(key, value_lookup, locker);
		if (!value_lookup)
		{
			locker->unlock();

			counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::wan_state_not_found]++;
			drop(mbuf);
			continue;
		}

		value_lookup->timestamp_last_packet = basePermanently.globalBaseAtomic->currentTime;
		value = *value_lookup;
		locker->unlock();

		nat64stateful_wan_translation(mbuf, value);

		counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::wan_packets]++;
		counters[nat64stateful.counter_id + (tCounterId)nat64stateful::module_counter::wan_bytes] += mbuf->pkt_len;
		nat64stateful_wan_flow(mbuf, nat64stateful.flow);
	}

	nat64stateful_wan_stack.clear();
}

inline void cWorker::nat64stateful_wan_translation(rte_mbuf* mbuf,
                                                   const dataplane::globalBase::nat64stateful_wan_value& value)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	rte_ipv4_hdr* ipv4_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

	uint16_t checksum_before = yanet_checksum(&ipv4_header->src_addr, 8);

	uint32_t ipv4_address_source = ipv4_header->src_addr;

	ipv4_to_ipv6(mbuf);

	rte_ipv6_hdr* ipv6_header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

	rte_memcpy(&ipv6_header->src_addr[0], value.ipv6_source.bytes, 12);
	rte_memcpy(&ipv6_header->src_addr[12], &ipv4_address_source, 4); ///< @todo: [12] -> [any]
	rte_memcpy(ipv6_header->dst_addr, value.ipv6_destination.bytes, 16);

	uint16_t checksum_after = yanet_checksum(&ipv6_header->src_addr[0], 32);

	if (metadata->transport_headerType == IPPROTO_TCP)
	{
		rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

		checksum_before = csum_plus(checksum_before, tcpHeader->dst_port);
		tcpHeader->dst_port = value.port_destination;
		checksum_after = csum_plus(checksum_after, tcpHeader->dst_port);

		yanet_tcp_checksum_v4_to_v6(tcpHeader, checksum_before, checksum_after);
	}
	else if (metadata->transport_headerType == IPPROTO_UDP)
	{
		rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

		checksum_before = csum_plus(checksum_before, udpHeader->dst_port);
		udpHeader->dst_port = value.port_destination;
		checksum_after = csum_plus(checksum_after, udpHeader->dst_port);

		yanet_udp_checksum_v4_to_v6(udpHeader, checksum_before, checksum_after);
	}
	else if (metadata->transport_headerType == IPPROTO_ICMPV6)
	{
		icmpv4_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmpv4_header_t*, metadata->transport_headerOffset);

		checksum_before = csum_plus(0, icmpHeader->typeCode);
		checksum_before = csum_plus(checksum_before, icmpHeader->identifier);

		if (icmpHeader->type == ICMP_ECHOREPLY)
		{
			icmpHeader->type = ICMP6_ECHO_REPLY;
		}
		icmpHeader->identifier = value.port_destination;

		checksum_after = csum_plus(checksum_after, rte_cpu_to_be_16(IPPROTO_ICMPV6));
		checksum_after = csum_plus(checksum_after, ipv6_header->payload_len);
		checksum_after = csum_plus(checksum_after, icmpHeader->typeCode);
		checksum_after = csum_plus(checksum_after, icmpHeader->identifier);

		yanet_icmp_checksum_v4_to_v6(icmpHeader, checksum_before, checksum_after);
	}

	preparePacket(mbuf);
}

inline void cWorker::nat64stateful_wan_flow(rte_mbuf* mbuf,
                                            const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::nat64stateless_ingress_entry_checked(rte_mbuf* mbuf)
{
	nat64stateless_ingress_stack.insert(mbuf);
}

inline void cWorker::nat64stateless_ingress_entry_icmp(rte_mbuf* mbuf)
{
	slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_nat64stateless_ingress_icmp);
}

inline void cWorker::nat64stateless_ingress_entry_fragmentation(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->transport_headerType == IPPROTO_ICMPV6)
	{
		// icmp checksum use payload length which is calculated from first and last fragments
		slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_nat64stateless_ingress_fragmentation);
	}
	else
	{
		metadata->flow.type = common::globalBase::eFlowType::nat64stateless_ingress_checked;
		nat64stateless_ingress_entry_checked(mbuf);
	}
}

inline void cWorker::nat64stateless_ingress_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(nat64stateless_ingress_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < nat64stateless_ingress_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = nat64stateless_ingress_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& nat64stateless = base.globalBase->nat64statelesses[metadata->flow.data.nat64stateless.id];

		///                                                                  [metadata->flow.data.nat64stateless.translationId];
		const auto& translation = base.globalBase->nat64statelessTranslations[metadata->flow.data.atomic >> 8];

		nat64stateless_ingress_translation(mbuf, nat64stateless, translation);

		stats.nat64stateless_ingressPackets++;
		nat64stateless_ingress_flow(mbuf, nat64stateless.flow);
	}

	nat64stateless_ingress_stack.clear();
}

inline void cWorker::nat64stateless_ingress_flow(rte_mbuf* mbuf,
                                                 const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::nat64stateless_ingress_translation(rte_mbuf* mbuf,
                                                        const dataplane::globalBase::tNat64stateless& nat64stateless,
                                                        const dataplane::globalBase::nat64stateless_translation_t& translation)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

#ifdef CONFIG_YADECAP_AUTOTEST
#else // CONFIG_YADECAP_AUTOTEST
	nat64statelessPacketId++;
#endif // CONFIG_YADECAP_AUTOTEST

	rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
	rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->transport_headerOffset - sizeof(rte_ipv4_hdr));

	uint16_t packetId = nat64statelessPacketId;
	uint16_t fragmentOffset = 0; ///< @todo: rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG)
	uint32_t ipv4AddressDestination = *(uint32_t*)&ipv6Header->dst_addr[12];
	uint16_t payloadLength = rte_be_to_cpu_16(ipv6Header->payload_len);

	if (metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT)
	{
		ipv6_extension_fragment_t* extension = rte_pktmbuf_mtod_offset(mbuf, ipv6_extension_fragment_t*, metadata->network_fragmentHeaderOffset);

		packetId = rte_hash_crc(&extension->identification, sizeof(extension->identification), 0) & 0xFFFF;
		fragmentOffset = rte_cpu_to_be_16(rte_be_to_cpu_16(extension->offsetFlagM) >> 3);
		fragmentOffset |= (extension->offsetFlagM & 0x0100) >> 3;

		stats.nat64stateless_ingressFragments++;
	}

	uint16_t checksum_before = yanet_checksum(&ipv6Header->src_addr[0], 32);

	ipv4Header->version_ihl = 0x45;
	ipv4Header->type_of_service = (rte_be_to_cpu_32(ipv6Header->vtc_flow) >> 20) & 0xFF;

	if (nat64stateless.ipv4DSCPFlags & YADECAP_GB_DSCP_FLAG_ALWAYS_MARK)
	{
		ipv4Header->type_of_service &= 0x3; ///< ECN
		ipv4Header->type_of_service |= nat64stateless.ipv4DSCPFlags & 0xFC;
	}
	else if (nat64stateless.ipv4DSCPFlags & YADECAP_GB_DSCP_FLAG_MARK)
	{
		if (!(ipv4Header->type_of_service & 0xFC)) ///< DSCP == 0
		{
			ipv4Header->type_of_service |= nat64stateless.ipv4DSCPFlags & 0xFC;
		}
	}

	ipv4Header->total_length = rte_cpu_to_be_16(payloadLength + 20 - (metadata->transport_headerOffset - metadata->network_headerOffset - 40));
	ipv4Header->packet_id = packetId;
	ipv4Header->fragment_offset = fragmentOffset;
	ipv4Header->time_to_live = ipv6Header->hop_limits;
	ipv4Header->next_proto_id = metadata->transport_headerType;
	ipv4Header->src_addr = translation.ipv4Address.address;
	ipv4Header->dst_addr = ipv4AddressDestination;

	if (metadata->transport_headerType == IPPROTO_ICMPV6)
	{
		ipv4Header->next_proto_id = IPPROTO_ICMP;
		metadata->transport_headerType = IPPROTO_ICMP;
	}

	yanet_ipv4_checksum(ipv4Header);

	uint16_t checksum_after = yanet_checksum(&ipv4Header->src_addr, 8);

	if ((fragmentOffset & 0xFF1F) == 0) ///< @todo: YANET_NETWORK_FLAG_FIRST_FRAGMENT
	{
		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			checksum_before = csum_plus(checksum_before, tcpHeader->src_port);
			tcpHeader->src_port = rte_cpu_to_be_16(rte_be_to_cpu_16(tcpHeader->src_port) + translation.diffPort);
			checksum_after = csum_plus(checksum_after, tcpHeader->src_port);

			yanet_tcp_checksum_v6_to_v4(tcpHeader, checksum_before, checksum_after);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			checksum_before = csum_plus(checksum_before, udpHeader->src_port);
			udpHeader->src_port = rte_cpu_to_be_16(rte_be_to_cpu_16(udpHeader->src_port) + translation.diffPort);
			checksum_after = csum_plus(checksum_after, udpHeader->src_port);

			yanet_udp_checksum_v6_to_v4(udpHeader, checksum_before, checksum_after);
		}
		else if (metadata->transport_headerType == IPPROTO_ICMP)
		{
			icmpv6_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

			checksum_before = csum_plus(checksum_before, rte_cpu_to_be_16(IPPROTO_ICMPV6));

			if (metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT)
			{
				checksum_before = csum_plus(checksum_before, rte_cpu_to_be_16(metadata->payload_length));
			}
			else
			{
				checksum_before = csum_plus(checksum_before, rte_cpu_to_be_16(payloadLength));
			}

			checksum_before = csum_plus(checksum_before, icmpHeader->typeCode);
			checksum_before = csum_plus(checksum_before, icmpHeader->identifier);

			if (icmpHeader->type == ICMP6_ECHO_REQUEST)
			{
				icmpHeader->type = ICMP_ECHO;
			}
			else if (icmpHeader->type == ICMP6_ECHO_REPLY)
			{
				icmpHeader->type = ICMP_ECHOREPLY;
			}
			icmpHeader->identifier = rte_cpu_to_be_16(rte_be_to_cpu_16(icmpHeader->identifier) + translation.diffPort);

			checksum_after = csum_plus(0, icmpHeader->typeCode);
			checksum_after = csum_plus(checksum_after, icmpHeader->identifier);

			yanet_icmp_checksum_v6_to_v4(icmpHeader, checksum_before, checksum_after);
		}
	}

	{
		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv4_hdr)),
		           rte_pktmbuf_mtod(mbuf, char*),
		           metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv4_hdr));

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	}

	/// @todo: opt
	preparePacket(mbuf);
}

inline void cWorker::nat64stateless_egress_entry_checked(rte_mbuf* mbuf)
{
	nat64stateless_egress_stack.insert(mbuf);
}

inline void cWorker::nat64stateless_egress_entry_icmp(rte_mbuf* mbuf)
{
	slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_nat64stateless_egress_icmp);
}

inline void cWorker::nat64stateless_egress_entry_fragmentation(rte_mbuf* mbuf)
{
	slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_nat64stateless_egress_fragmentation);
}

inline void cWorker::nat64stateless_egress_entry_farm(rte_mbuf* mbuf)
{
	slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_nat64stateless_egress_farm);
}

inline void cWorker::nat64stateless_egress_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(nat64stateless_egress_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < nat64stateless_egress_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = nat64stateless_egress_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& nat64stateless = base.globalBase->nat64statelesses[metadata->flow.data.nat64stateless.id];

		///                                                                  [metadata->flow.data.nat64stateless.translationId];
		const auto& translation = base.globalBase->nat64statelessTranslations[metadata->flow.data.atomic >> 8];

		nat64stateless_egress_translation(mbuf, translation);

		stats.nat64stateless_egressPackets++;
		nat64stateless_egress_flow(mbuf, nat64stateless.flow);
	}

	nat64stateless_egress_stack.clear();
}

inline void cWorker::nat64stateless_egress_flow(rte_mbuf* mbuf,
                                                const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

static inline void ipv6_to_ipv4(rte_mbuf* mbuf, uint16_t packetId)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
	rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->transport_headerOffset - sizeof(rte_ipv4_hdr));

	uint16_t fragmentOffset = 0; ///< @todo: rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG)
	uint16_t payloadLength = rte_be_to_cpu_16(ipv6Header->payload_len);
	uint32_t dstAddr = *(uint32_t*)&ipv6Header->dst_addr[12];

	if (metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT)
	{
		ipv6_extension_fragment_t* extension = rte_pktmbuf_mtod_offset(mbuf, ipv6_extension_fragment_t*, metadata->network_fragmentHeaderOffset);

		packetId = extension->identification & 0xFFFF;
		fragmentOffset = rte_cpu_to_be_16(rte_be_to_cpu_16(extension->offsetFlagM) >> 3);
		fragmentOffset |= (extension->offsetFlagM & 0x0100) >> 3;
	}

	ipv4Header->version_ihl = 0x45;
	ipv4Header->type_of_service = (rte_be_to_cpu_32(ipv6Header->vtc_flow) >> 20) & 0xFF;
	ipv4Header->total_length = rte_cpu_to_be_16(payloadLength + 20 - (metadata->transport_headerOffset - metadata->network_headerOffset - 40));
	ipv4Header->packet_id = packetId;
	ipv4Header->fragment_offset = fragmentOffset;
	ipv4Header->time_to_live = ipv6Header->hop_limits;
	ipv4Header->next_proto_id = metadata->transport_headerType;
	ipv4Header->src_addr = *(uint32_t*)&ipv6Header->src_addr[12];
	ipv4Header->dst_addr = dstAddr;

	if (metadata->transport_headerType == IPPROTO_ICMPV6)
	{
		ipv4Header->next_proto_id = IPPROTO_ICMP;
		metadata->transport_headerType = IPPROTO_ICMP;
	}

	yanet_ipv4_checksum(ipv4Header);

	{
		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv4_hdr)),
		           rte_pktmbuf_mtod(mbuf, char*),
		           metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset - sizeof(rte_ipv4_hdr));

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	}
}

inline void cWorker::nat64stateless_egress_translation(rte_mbuf* mbuf,
                                                       const dataplane::globalBase::nat64stateless_translation_t& translation)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

	uint16_t checksum_before = yanet_checksum(&ipv4Header->src_addr, 8);

	uint32_t ipv4AddressSource = ipv4Header->src_addr;

	ipv4_to_ipv6(mbuf);

	rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

	if (metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT)
	{
		stats.nat64stateless_egressFragments++;
	}

	rte_memcpy(&ipv6Header->src_addr[0], translation.ipv6DestinationAddress.bytes, 12);
	rte_memcpy(&ipv6Header->src_addr[12], &ipv4AddressSource, 4);
	rte_memcpy(ipv6Header->dst_addr, translation.ipv6Address.bytes, 16);

	uint16_t checksum_after = yanet_checksum(&ipv6Header->src_addr[0], 32);

	if (!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT))
	{
		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			checksum_before = csum_plus(checksum_before, tcpHeader->dst_port);
			tcpHeader->dst_port = rte_cpu_to_be_16(rte_be_to_cpu_16(tcpHeader->dst_port) - translation.diffPort);
			checksum_after = csum_plus(checksum_after, tcpHeader->dst_port);

			yanet_tcp_checksum_v4_to_v6(tcpHeader, checksum_before, checksum_after);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			checksum_before = csum_plus(checksum_before, udpHeader->dst_port);
			udpHeader->dst_port = rte_cpu_to_be_16(rte_be_to_cpu_16(udpHeader->dst_port) - translation.diffPort);
			checksum_after = csum_plus(checksum_after, udpHeader->dst_port);

			yanet_udp_checksum_v4_to_v6(udpHeader, checksum_before, checksum_after);
		}
		else if (metadata->transport_headerType == IPPROTO_ICMPV6)
		{
			icmpv4_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmpv4_header_t*, metadata->transport_headerOffset);

			checksum_before = csum_plus(0, icmpHeader->typeCode);
			checksum_before = csum_plus(checksum_before, icmpHeader->identifier);

			if (icmpHeader->type == ICMP_ECHO)
			{
				icmpHeader->type = ICMP6_ECHO_REQUEST;
			}
			else if (icmpHeader->type == ICMP_ECHOREPLY)
			{
				icmpHeader->type = ICMP6_ECHO_REPLY;
			}
			icmpHeader->identifier = rte_cpu_to_be_16(rte_be_to_cpu_16(icmpHeader->identifier) - translation.diffPort);

			checksum_after = csum_plus(checksum_after, rte_cpu_to_be_16(IPPROTO_ICMPV6));

			if (metadata->network_flags & YANET_NETWORK_FLAG_FRAGMENT)
			{
				checksum_after = csum_plus(checksum_after, rte_cpu_to_be_16(metadata->payload_length));
			}
			else
			{
				checksum_after = csum_plus(checksum_after, ipv6Header->payload_len);
			}

			checksum_after = csum_plus(checksum_after, icmpHeader->typeCode);
			checksum_after = csum_plus(checksum_after, icmpHeader->identifier);

			yanet_icmp_checksum_v4_to_v6(icmpHeader, checksum_before, checksum_after);
		}
	}

	/// @todo: opt
	preparePacket(mbuf);
}

inline void cWorker::balancer_entry(rte_mbuf* mbuf)
{
	balancer_stack.insert(mbuf);
}

inline void cWorker::balancer_icmp_reply_entry(rte_mbuf* mbuf)
{
	balancer_icmp_reply_stack.insert(mbuf);
}

inline void cWorker::balancer_icmp_forward_entry(rte_mbuf* mbuf)
{
	balancer_icmp_forward_stack.insert(mbuf);
}

inline void cWorker::balancer_fragment_entry(rte_mbuf* mbuf)
{
	counters[(uint32_t)common::globalBase::static_counter_type::balancer_fragment_drops]++;
	drop(mbuf);
}

inline void balancer_set_mss(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->transport_headerType == IPPROTO_TCP)
	{
		rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

		if ((tcpHeader->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_RST_FLAG)) != RTE_TCP_SYN_FLAG)
		{
			return;
		}

		uint16_t tcpDataOffset = (tcpHeader->data_off >> 4) * 4;
		if (tcpDataOffset < sizeof(rte_tcp_hdr) ||
		    metadata->transport_headerOffset + tcpDataOffset > rte_pktmbuf_pkt_len(mbuf))
		{
			/// data offset is out of bounds of the packet, nothing to do here
			return;
		}

		/// option lookup
		uint16_t tcpOptionOffset = sizeof(rte_tcp_hdr);
		while (tcpOptionOffset + TCP_OPTION_MSS_LEN <= tcpDataOffset)
		{
			const tcp_option_t* option = rte_pktmbuf_mtod_offset(mbuf, tcp_option_t*, metadata->transport_headerOffset + tcpOptionOffset);

			if (option->kind == TCP_OPTION_KIND_MSS)
			{
				/// mss could not be increased so check the value first
				uint16_t old_mss = rte_be_to_cpu_16(*(uint16_t*)option->data);
				if (old_mss <= YANET_BALANCER_FIX_MSS_SIZE)
				{
					return;
				}
				uint16_t cksum = ~tcpHeader->cksum;
				cksum = csum_minus(cksum, *(uint16_t*)option->data);
				*(uint16_t*)option->data = rte_cpu_to_be_16(YANET_BALANCER_FIX_MSS_SIZE);
				cksum = csum_plus(cksum, *(uint16_t*)option->data);
				tcpHeader->cksum = (cksum == 0xffff) ? cksum : ~cksum;
				return;
			}
			else if (option->kind == TCP_OPTION_KIND_EOL ||
			         option->kind == TCP_OPTION_KIND_NOP)
			{
				tcpOptionOffset++;
			}
			else
			{
				if (option->len == 0)
				{
					/// packet header is broken
					return;
				}
				tcpOptionOffset += option->len;
			}
		}

		/// try to insert option
		if (tcpDataOffset > (0x0f << 2) - TCP_OPTION_MSS_LEN)
		{
			/// no space to insert the option
			return;
		}

		/// insert option just after regular tcp header
		rte_pktmbuf_prepend(mbuf, TCP_OPTION_MSS_LEN);
		memmove(rte_pktmbuf_mtod(mbuf, char*),
		        rte_pktmbuf_mtod_offset(mbuf, char*, TCP_OPTION_MSS_LEN),
		        metadata->transport_headerOffset + sizeof(rte_tcp_hdr));
		tcp_option_t* option = rte_pktmbuf_mtod_offset(mbuf, tcp_option_t*, metadata->transport_headerOffset + sizeof(rte_tcp_hdr));
		option->kind = TCP_OPTION_KIND_MSS;
		option->len = TCP_OPTION_MSS_LEN;
		*(uint16_t*)option->data = rte_cpu_to_be_16(YANET_BALANCER_DEFAULT_MSS_SIZE);

		/// adjust tcp and ip lengths and update checksums
		tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);
		tcpHeader->data_off += 0x1 << 4;
		uint16_t cksum = ~tcpHeader->cksum;
		/// data_off is the leading byte of corresponding 2-byte sequence inside a tcp header so there is no rte_cpu_to_be_16
		cksum = csum_plus(cksum, 0x1 << 4);
		cksum = csum_plus(cksum, *(uint16_t*)option);
		cksum = csum_plus(cksum, *(uint16_t*)option->data);
		cksum = csum_plus(cksum, rte_cpu_to_be_16(TCP_OPTION_MSS_LEN));
		tcpHeader->cksum = (cksum == 0xffff) ? cksum : ~cksum;

		if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		{
			rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
			ipv4Header->total_length = rte_cpu_to_be_16(rte_be_to_cpu_16(ipv4Header->total_length) + TCP_OPTION_MSS_LEN);

			cksum = ~ipv4Header->hdr_checksum;
			cksum = csum_plus(cksum, rte_cpu_to_be_16(TCP_OPTION_MSS_LEN));
			ipv4Header->hdr_checksum = (cksum == 0xffff) ? cksum : ~cksum;
		}
		else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
		{
			rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
			ipv6Header->payload_len = rte_cpu_to_be_16(rte_be_to_cpu_16(ipv6Header->payload_len) + TCP_OPTION_MSS_LEN);
		}
	}
}

inline void cWorker::balancer_handle()
{
	const auto& base = bases[localBaseId & 1];
	const auto *ring =
		base.globalBase->balancer_service_rings + base.globalBase->balancer_service_ring_id;

	if (unlikely(balancer_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < balancer_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = balancer_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		calcHash(mbuf);
		metadata->hash = rte_hash_crc(&metadata->flowLabel, 4, metadata->hash);

		auto& key = balancer_keys[mbuf_i];
		if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		{
			rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

			key.balancer_id = metadata->flow.data.balancer.id;
			key.protocol = metadata->transport_headerType;
			key.addr_type = 4;
			memset(key.ip_source.nap, 0, sizeof(key.ip_source.nap));
			key.ip_source.mapped_ipv4_address.address = ipv4Header->src_addr;
			memset(key.ip_destination.nap, 0, sizeof(key.ip_destination.nap));
			key.ip_destination.mapped_ipv4_address.address = ipv4Header->dst_addr;
			key.port_source = 0;
			key.port_destination = 0;
			if (metadata->transport_headerType == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

				key.port_source = tcpHeader->src_port;
				key.port_destination = tcpHeader->dst_port;
			}
			else if (metadata->transport_headerType == IPPROTO_UDP)
			{
				rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

				key.port_source = udpHeader->src_port;
				key.port_destination = udpHeader->dst_port;
			}
			else
			{
				/// @todo: udp-lite, ip

				key.balancer_id = YANET_BALANCER_ID_INVALID;
			}
		}
		else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
		{
			rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

			key.balancer_id = metadata->flow.data.balancer.id;
			key.protocol = metadata->transport_headerType;
			key.addr_type = 6;
			memcpy(key.ip_source.bytes, ipv6Header->src_addr, 16);
			memcpy(key.ip_destination.bytes, ipv6Header->dst_addr, 16);
			key.port_source = 0;
			key.port_destination = 0;
			if (metadata->transport_headerType == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

				key.port_source = tcpHeader->src_port;
				key.port_destination = tcpHeader->dst_port;
			}
			else if (metadata->transport_headerType == IPPROTO_UDP)
			{
				rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

				key.port_source = udpHeader->src_port;
				key.port_destination = udpHeader->dst_port;
			}
			else
			{
				/// @todo: udp-lite, ip

				key.balancer_id = YANET_BALANCER_ID_INVALID;
			}
		}
		else
		{
			key.balancer_id = YANET_BALANCER_ID_INVALID;
		}
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < balancer_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = balancer_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& key = balancer_keys[mbuf_i];

		const auto& balancer = base.globalBase->balancers[metadata->flow.data.balancer.id];

		///                                                     [metadata->flow.data.balancer.service_id];
		const balancer_service_id_t service_id = metadata->flow.data.atomic >> 8;
		const auto& service = base.globalBase->balancer_services[service_id];

		if (service.flags & YANET_BALANCER_FIX_MSS_FLAG)
		{
			balancer_set_mss(mbuf);
		}

		/// @todo: BALANCER TCP SYN

		dataplane::globalBase::balancer_state_value_t* value;
		dataplane::spinlock_nonrecursive_t* locker;
		const uint32_t hash = basePermanently.globalBaseAtomic->balancer_state.lookup(key, value, locker);
		bool rescheduleReal = false;
		if (value)
		{
			const auto& real_unordered = base.globalBase->balancer_reals[value->real_unordered_id];
			const auto& real_state = base.globalBase->balancer_real_states[value->real_unordered_id];
			if (real_state.flags & YANET_BALANCER_FLAG_ENABLED)
			{
				value->timestamp_last_packet = basePermanently.globalBaseAtomic->currentTime;
				locker->unlock();

				balancer_tunnel(mbuf, real_unordered, real_unordered.counter_id);

				counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::packets]++;
				counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::bytes] += mbuf->pkt_len;
				balancer_flow(mbuf, balancer.flow);
				continue;
			}
			else
			{
				/// check if packet should be rescheduled (UDP or TCP SYN)
				if (metadata->transport_headerType == IPPROTO_UDP)
				{
					rescheduleReal = true;
				}
				else if (metadata->transport_headerType == IPPROTO_TCP)
				{
					rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

					if ((tcpHeader->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_RST_FLAG)) == RTE_TCP_SYN_FLAG)
					{
						rescheduleReal = true;
					}
				}

				if (!rescheduleReal)
				{
					locker->unlock();

					counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::real_disabled_packets]++;
					counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::real_disabled_bytes] += mbuf->pkt_len;
					drop(mbuf);
					continue;
				}
			}
		}

		if (!value || rescheduleReal)
		{
			auto *range = ring->ranges + service_id;
			if (!range->size)
			{
				locker->unlock();

				stats.balancer_invalid_reals_count++;
				drop(mbuf);
				continue;
			}

			const auto& real_id = ring->reals[range->start + (metadata->hash % range->size)];
			const auto& real_unordered = base.globalBase->balancer_reals[real_id];

			if (!value)
			{
				dataplane::globalBase::balancer_state_value_t value;
				value.real_unordered_id = real_id;
				value.timestamp_create = basePermanently.globalBaseAtomic->currentTime;
				value.timestamp_last_packet = value.timestamp_create;
				value.timestamp_gc = value.timestamp_last_packet - 1; ///< touch gc

				/// counter_id:
				///   0 - insert failed
				///   1 - insert done
				uint32_t counter_id = basePermanently.globalBaseAtomic->balancer_state.insert(hash, key, value);
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_state + counter_id]++;
				if (counter_id)
				{
					++counters[real_unordered.counter_id + (tCounterId)balancer::real_counter::sessions_created];
				}
			}
			else
			{
				const auto& old_real = base.globalBase->balancer_reals[value->real_unordered_id];
				++counters[old_real.counter_id + (tCounterId)balancer::real_counter::sessions_destroyed];
				++counters[real_unordered.counter_id + (tCounterId)balancer::real_counter::sessions_created];
				value->real_unordered_id = real_id;
				value->timestamp_create = basePermanently.globalBaseAtomic->currentTime;
				value->timestamp_last_packet = value->timestamp_create;
				value->timestamp_gc = value->timestamp_last_packet - 1; ///< touch gc
			}

			locker->unlock();

			balancer_tunnel(mbuf, real_unordered, real_unordered.counter_id);
			counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::packets]++;
			counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::bytes] += mbuf->pkt_len;
			balancer_flow(mbuf, balancer.flow);
			continue;
		}

		locker->unlock();
	}

	balancer_stack.clear();
}

inline void cWorker::balancer_tunnel(rte_mbuf* mbuf,
                                     const dataplane::globalBase::balancer_real_t& real,
                                     const tCounterId& real_counter_id)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	const auto& base = bases[localBaseId & 1];
	const auto& balancer = base.globalBase->balancers[metadata->flow.data.balancer.id];

	rte_ipv4_hdr* ipv4HeaderInner = nullptr;
	rte_ipv6_hdr* ipv6HeaderInner = nullptr;

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		ipv4HeaderInner = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
	}
	else
	{
		ipv6HeaderInner = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
	}

	if (real.flags & YANET_BALANCER_FLAG_DST_IPV6)
	{
		/// @todo: mpls_header_t
		rte_pktmbuf_prepend(mbuf, sizeof(rte_ipv6_hdr));
		rte_memcpy(rte_pktmbuf_mtod(mbuf, char*),
		           rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_ipv6_hdr)),
		           metadata->network_headerOffset);

		{
			/// @todo: check for ethernetHeader or vlanHeader
			uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);
		}

		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		if (ipv4HeaderInner)
		{
			ipv6Header->vtc_flow = rte_cpu_to_be_32((0x6 << 28) | (ipv4HeaderInner->type_of_service << 20)); ///< @todo: flow label
			ipv6Header->payload_len = ipv4HeaderInner->total_length;
			ipv6Header->proto = IPPROTO_IPIP;
			ipv6Header->hop_limits = ipv4HeaderInner->time_to_live;

			rte_memcpy(ipv6Header->src_addr, balancer.source_ipv6.bytes, 16);
			((uint32_t *)ipv6Header->src_addr)[2] = ipv4HeaderInner->src_addr;
			rte_memcpy(ipv6Header->dst_addr, real.destination.bytes, 16);
		}
		else
		{
			ipv6Header->vtc_flow = ipv6HeaderInner->vtc_flow;
			ipv6Header->payload_len = rte_cpu_to_be_16(sizeof(rte_ipv6_hdr) + rte_be_to_cpu_16(ipv6HeaderInner->payload_len));
			ipv6Header->proto = IPPROTO_IPV6;
			ipv6Header->hop_limits = ipv6HeaderInner->hop_limits;

			rte_memcpy(ipv6Header->src_addr, balancer.source_ipv6.bytes, 16);
			((uint32_t *)ipv6Header->src_addr)[2] = ((uint32_t *)ipv6HeaderInner->src_addr)[2] ^ ((uint32_t *)ipv6HeaderInner->src_addr)[3];
			rte_memcpy(ipv6Header->dst_addr, real.destination.bytes, 16);
		}
	}
	else
	{
		rte_pktmbuf_prepend(mbuf, sizeof(rte_ipv4_hdr));
		rte_memcpy(rte_pktmbuf_mtod(mbuf, char*),
		           rte_pktmbuf_mtod_offset(mbuf, char*, sizeof(rte_ipv4_hdr)),
		           metadata->network_headerOffset);

		{
			/// @todo: check for ethernetHeader or vlanHeader
			uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
		}

		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		ipv4Header->version_ihl = 0x45;
		if (ipv4HeaderInner)
		{
			ipv4Header->type_of_service = ipv4HeaderInner->type_of_service;
			ipv4Header->total_length = rte_cpu_to_be_16(sizeof(rte_ipv4_hdr) + rte_be_to_cpu_16(ipv4HeaderInner->total_length));

			ipv4Header->packet_id = ipv4HeaderInner->packet_id;
			ipv4Header->fragment_offset = ipv4HeaderInner->fragment_offset;
			ipv4Header->time_to_live = ipv4HeaderInner->time_to_live;
			ipv4Header->next_proto_id = IPPROTO_IPIP;
		}
		else
		{
			ipv4Header->type_of_service = (rte_be_to_cpu_32(ipv6HeaderInner->vtc_flow) >> 20) & 0xFF;
			ipv4Header->total_length = rte_cpu_to_be_16(sizeof(rte_ipv4_hdr) + sizeof(rte_ipv6_hdr) + rte_be_to_cpu_16(ipv6HeaderInner->payload_len));

			ipv4Header->packet_id = rte_cpu_to_be_16(0x01);
			ipv4Header->fragment_offset = 0;
			ipv4Header->time_to_live = ipv6HeaderInner->hop_limits;
			ipv4Header->next_proto_id = IPPROTO_IPV6;
		}

		ipv4Header->src_addr = balancer.source_ipv4.address;
		ipv4Header->dst_addr = real.destination.mapped_ipv4_address.address;

		yanet_ipv4_checksum(ipv4Header);
	}

	counters[real_counter_id + (tCounterId)balancer::real_counter::packets]++;
	counters[real_counter_id + (tCounterId)balancer::real_counter::bytes] += mbuf->pkt_len;

	/// @todo: opt
	preparePacket(mbuf);
}

inline void cWorker::balancer_flow(rte_mbuf* mbuf,
                                   const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

inline void cWorker::balancer_icmp_reply_handle()
{
	const auto& base = bases[localBaseId & 1];

	if (unlikely(balancer_icmp_reply_stack.mbufsCount == 0))
	{
		return;
	}

	for (unsigned int mbuf_i = 0;
	     mbuf_i < balancer_icmp_reply_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = balancer_icmp_reply_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& balancer = base.globalBase->balancers[metadata->flow.data.balancer.id];

		if (metadata->transport_headerType == IPPROTO_ICMP)
		{
			icmpv4_header_t* icmpHeader = rte_pktmbuf_mtod_offset(mbuf, icmpv4_header_t*, metadata->transport_headerOffset);

			icmpHeader->type = ICMP_ECHOREPLY;
			icmpHeader->code = 0;

			rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

			uint32_t tmp_for_swap = ipv4Header->src_addr;
			ipv4Header->src_addr = ipv4Header->dst_addr;
			ipv4Header->dst_addr = tmp_for_swap;

			// it is a reply, ttl starts anew, route_handle() will decrease it and modify checksum accordingly
			ipv4Header->time_to_live = 65;

			yanet_ipv4_checksum(ipv4Header);

			uint16_t icmp_checksum = ~icmpHeader->checksum;
			icmp_checksum = csum_minus(icmp_checksum, ICMP_ECHO);
			icmp_checksum = csum_plus(icmp_checksum, ICMP_ECHOREPLY);
			icmpHeader->checksum = ~icmp_checksum;

			counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_generated_echo_reply_ipv4]++;

			balancer_flow(mbuf, balancer.flow);
		}
		else if (metadata->transport_headerType == IPPROTO_ICMPV6)
		{
			icmpv6_header_t* icmpv6Header = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

			icmpv6Header->type = ICMP6_ECHO_REPLY;
			icmpv6Header->code = 0;

			rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

			uint8_t tmp_for_swap[sizeof(ipv6Header->src_addr)];
			memcpy(tmp_for_swap,ipv6Header->src_addr, sizeof(tmp_for_swap));
			memcpy(ipv6Header->src_addr, ipv6Header->dst_addr, sizeof(ipv6Header->src_addr));
			memcpy(ipv6Header->dst_addr, tmp_for_swap, sizeof(tmp_for_swap));

			ipv6Header->hop_limits = 65; // it is a reply, hop_limits start anew

			uint16_t icmpv6_checksum = ~icmpv6Header->checksum;
			icmpv6_checksum = csum_minus(icmpv6_checksum, ICMP6_ECHO_REQUEST);
			icmpv6_checksum = csum_plus(icmpv6_checksum, ICMP6_ECHO_REPLY);
			icmpv6Header->checksum = ~icmpv6_checksum;

			counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_generated_echo_reply_ipv6]++;

			balancer_flow(mbuf, balancer.flow);
		}
		else
		{
			drop(mbuf);
		}
	}

	balancer_icmp_reply_stack.clear();
}

inline void cWorker::balancer_icmp_forward_handle()
{
	const auto& base = bases[localBaseId & 1];

	/* maximum 32 (CONFIG_YADECAP_MBUFS_BURST_SIZE) mbufs in balancer_icmp_forward_stack (which is in fact an array),
	   therefore each bit in uint32_t number may represent an index in this array */
	uint32_t drop_mask = 0;

	if (unlikely(balancer_icmp_forward_stack.mbufsCount == 0))
	{
		return;
	}

	for (uint32_t mbuf_i = 0;
	     mbuf_i < balancer_icmp_forward_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = balancer_icmp_forward_stack.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		auto& key = balancer_keys[mbuf_i];

		if (metadata->transport_headerType == IPPROTO_ICMP)
		{
			rte_ipv4_hdr* outerIpv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

			dataplane::metadata inner_metadata;
			inner_metadata.network_headerType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			inner_metadata.network_headerOffset = metadata->transport_headerOffset + sizeof(icmpv4_header_t);

			if (!prepareL3(mbuf, &inner_metadata))
			{
				drop_mask |= (1 << mbuf_i);
				// icmp payload is too short to determine original src/dst ips
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv4_payload_too_short_ip]++;
				continue;
			}

			rte_ipv4_hdr* innerIpv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->transport_headerOffset + sizeof(icmpv4_header_t));

			if (innerIpv4Header->src_addr != outerIpv4Header->dst_addr)
			{
				drop_mask |= (1 << mbuf_i);
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_unmatching_src_from_original_ipv4]++;
				continue;
			}

			if (mbuf->pkt_len < inner_metadata.transport_headerOffset + 2 * sizeof(rte_be16_t))
			{
				drop_mask |= (1 << mbuf_i);
				// icmp payload is too short to determine original src/dst ports
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv4_payload_too_short_port]++;
				continue;
			}

			key.balancer_id = metadata->flow.data.balancer.id; // filled previously by metadata->flow = flow;

			key.protocol = inner_metadata.transport_headerType;
			key.addr_type = 4;

			memset(key.ip_source.nap, 0, sizeof(key.ip_source.nap)); // swapped src and dst to determine key
			key.ip_source.mapped_ipv4_address.address = innerIpv4Header->dst_addr;
			memset(key.ip_destination.nap, 0, sizeof(key.ip_destination.nap));
			key.ip_destination.mapped_ipv4_address.address = innerIpv4Header->src_addr;

			key.port_source = 0;
			key.port_destination = 0;

			if (key.protocol == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, inner_metadata.transport_headerOffset);

				key.port_source = tcpHeader->dst_port; // swapped src and dst to determine key
				key.port_destination = tcpHeader->src_port;
			}
			else if (key.protocol == IPPROTO_UDP)
			{
				rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, inner_metadata.transport_headerOffset);

				key.port_source = udpHeader->dst_port; // swapped src and dst to determine key
				key.port_destination = udpHeader->src_port;
			}
			else
			{
				/// @todo: udp-lite, ip
				key.balancer_id = YANET_BALANCER_ID_INVALID;
			}
		}
		else if (metadata->transport_headerType == IPPROTO_ICMPV6)
		{
			rte_ipv6_hdr* outerIpv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

			dataplane::metadata inner_metadata;
			inner_metadata.network_headerType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);
			inner_metadata.network_headerOffset = metadata->transport_headerOffset + sizeof(icmpv6_header_t);

			if (!prepareL3(mbuf, &inner_metadata))
			{
				drop_mask |= (1 << mbuf_i);
				// icmpv6 payload is too short to determine original src/dst ips
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv6_payload_too_short_ip]++;
				continue;
			}

			rte_ipv6_hdr* innerIpv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->transport_headerOffset + sizeof(icmpv6_header_t));

			if (memcmp(innerIpv6Header->src_addr, outerIpv6Header->dst_addr, 16))
			{
				drop_mask |= (1 << mbuf_i);
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_unmatching_src_from_original_ipv6]++;
				continue;
			}

			if (mbuf->pkt_len < inner_metadata.transport_headerOffset + 2 * sizeof(rte_be16_t))
			{
				drop_mask |= (1 << mbuf_i);
				// icmpv6 payload is too short to determine original src/dst ports
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_icmpv6_payload_too_short_port]++;
				continue;
			}

			key.balancer_id = metadata->flow.data.balancer.id; // filled previously by metadata->flow = flow;

			key.protocol = inner_metadata.transport_headerType;
			key.addr_type = 6;

			memcpy(key.ip_source.bytes, innerIpv6Header->dst_addr, 16); // swapped src and dst to determine key
			memcpy(key.ip_destination.bytes, innerIpv6Header->src_addr, 16);

			key.port_source = 0;
			key.port_destination = 0;

			if (key.protocol == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, inner_metadata.transport_headerOffset);

				key.port_source = tcpHeader->dst_port; // swapped src and dst to determine key
				key.port_destination = tcpHeader->src_port;
			}
			else if (key.protocol == IPPROTO_UDP)
			{
				rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, inner_metadata.transport_headerOffset);

				key.port_source = udpHeader->dst_port; // swapped src and dst to determine key
				key.port_destination = udpHeader->src_port;
			}
			else
			{
				/// @todo: udp-lite, ip
				key.balancer_id = YANET_BALANCER_ID_INVALID;
			}
		}
		else
		{
			key.balancer_id = YANET_BALANCER_ID_INVALID;
		}
	}

	for (uint32_t mbuf_i = 0;
		mbuf_i < balancer_icmp_forward_stack.mbufsCount;
		mbuf_i++)
	{
		rte_mbuf* mbuf = balancer_icmp_forward_stack.mbufs[mbuf_i];

		if (drop_mask & (1 << mbuf_i))
		{
			drop(mbuf);
			continue;
		}

		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
		const auto& key = balancer_keys[mbuf_i];

		const auto& balancer = base.globalBase->balancers[metadata->flow.data.balancer.id];

		///                                                     [metadata->flow.data.balancer.service_id];
		const balancer_service_id_t service_id = metadata->flow.data.atomic >> 8;
		const auto& service = base.globalBase->balancer_services[service_id];

		dataplane::globalBase::balancer_state_value_t* value;
		dataplane::spinlock_nonrecursive_t* locker;
		basePermanently.globalBaseAtomic->balancer_state.lookup(key, value, locker);

		if (value)
		{
			// destination of this icmp is actually one of this balancer's reals, just forward it

			const auto& real_unordered = base.globalBase->balancer_reals[value->real_unordered_id];
			const auto& real_state = base.globalBase->balancer_real_states[value->real_unordered_id];
			if (real_state.flags & YANET_BALANCER_FLAG_ENABLED)
			{
				value->timestamp_last_packet = basePermanently.globalBaseAtomic->currentTime;
				locker->unlock();

				balancer_tunnel(mbuf, real_unordered, real_unordered.counter_id);
				counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::packets]++;
				counters[(service.atomic1 >> 8) + (tCounterId)balancer::service_counter::bytes] += mbuf->pkt_len;

				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_sent_to_real]++;

				balancer_flow(mbuf, balancer.flow);
			}
			else
			{
				// real is disabled, drop packet
				locker->unlock();
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_real_disabled]++;
				drop(mbuf);
			}
		}
		else
		{
			locker->unlock();

			// packet will be cloned and sent to other balancers unless it is a clone already
			if (!metadata->already_early_decapped)
			{
				// cloned packets have outer ip headers (added by neighbor balancer)
				slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_balancer_icmp_forward);
			}
			else
			{
				// already cloned packet, real not found, drop
				drop(mbuf);
				counters[(uint32_t)common::globalBase::static_counter_type::balancer_icmp_drop_already_cloned]++;
			}
		}
	}

	balancer_icmp_forward_stack.clear();
}

inline bool cWorker::acl_try_keepstate(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		dataplane::globalBase::fw4_state_key_t key;
		key.proto = metadata->transport_headerType;
		key.__nap = 0;
		key.src_addr.address = ipv4Header->src_addr;
		key.dst_addr.address = ipv4Header->dst_addr;

		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(tcpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(tcpHeader->dst_port);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(udpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(udpHeader->dst_port);
		}
		else // todo: sctp, ddcp, udp-lite
		{
			key.src_port = 0;
			key.dst_port = 0;
		}

		dataplane::globalBase::fw_state_value_t* value;
		dataplane::spinlock_nonrecursive_t* locker;
		basePermanently.globalBaseAtomic->fw4_state->lookup(key, value, locker);

		return acl_try_keepstate(mbuf, value, locker);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		dataplane::globalBase::fw6_state_key_t key;
		key.proto = metadata->transport_headerType;
		key.__nap = 0;
		rte_memcpy(key.src_addr.bytes, ipv6Header->src_addr, 16);
		rte_memcpy(key.dst_addr.bytes, ipv6Header->dst_addr, 16);

		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(tcpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(tcpHeader->dst_port);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(udpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(udpHeader->dst_port);
		}
		else // todo: sctp, ddcp, udp-lite
		{
			key.src_port = 0;
			key.dst_port = 0;
		}

		dataplane::globalBase::fw_state_value_t* value;
		dataplane::spinlock_nonrecursive_t* locker;
		basePermanently.globalBaseAtomic->fw6_state->lookup(key, value, locker);

		return acl_try_keepstate(mbuf, value, locker);
	}

	return false;
}

inline bool cWorker::acl_try_keepstate(rte_mbuf* mbuf,
                                       dataplane::globalBase::fw_state_value_t* value,
                                       dataplane::spinlock_nonrecursive_t* locker)
{
	// Checking both value and locker for non-being-nullptr seems redundant.
	if (value == nullptr)
	{
		// No record found, the caller should continue as usual.
		locker->unlock();
		return false;
	}

	uint8_t flags = 0;
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	if (metadata->transport_headerType == IPPROTO_TCP)
	{
		rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);
		flags = common::fwstate::from_tcp_flags(tcpHeader->tcp_flags);
	}

	// Copy the flow to prevent concurrent usage. In the other thread there can be garbage collector active.
	common::globalBase::tFlow flow = value->flow;
	value->last_seen = basePermanently.globalBaseAtomic->currentTime;
	value->packets_since_last_sync++;
	value->packets_backward++;
	value->tcp.dst_flags |= flags;
	locker->unlock();

	// Handle the packet according its flow.
	acl_ingress_flow(mbuf, flow);
	return true;
}

inline void cWorker::acl_create_keepstate(rte_mbuf* mbuf, tAclId aclId, const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		dataplane::globalBase::fw4_state_key_t key;
		key.proto = metadata->transport_headerType;
		key.__nap = 0;

		// Swap src and dst addresses.
		key.dst_addr.address = ipv4Header->src_addr;
		key.src_addr.address = ipv4Header->dst_addr;

		uint8_t flags = 0;

		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			// Swap src and dst ports.
			key.dst_port = rte_be_to_cpu_16(tcpHeader->src_port);
			key.src_port = rte_be_to_cpu_16(tcpHeader->dst_port);

			flags = common::fwstate::from_tcp_flags(tcpHeader->tcp_flags);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			// Swap src and dst ports.
			key.dst_port = rte_be_to_cpu_16(udpHeader->src_port);
			key.src_port = rte_be_to_cpu_16(udpHeader->dst_port);
		}
		else // todo: sctp, ddcp, udp-lite
		{
			// Protocols without ports, like ICMP, ESP etc.
			key.dst_port = 0;
			key.src_port = 0;
		}

		dataplane::globalBase::fw_state_value_t value;
		value.type = static_cast<dataplane::globalBase::fw_state_type>(metadata->transport_headerType);
		value.owner = dataplane::globalBase::fw_state_owner_e::internal;
		value.last_seen = basePermanently.globalBaseAtomic->currentTime;
		value.flow = flow;
		value.acl_id = aclId;
		value.last_sync = basePermanently.globalBaseAtomic->currentTime;
		value.packets_since_last_sync = 0;
		value.packets_backward = 0;
		value.packets_forward = 0;
		value.tcp.src_flags = flags;
		value.tcp.dst_flags = 0;

		bool emit = false;
		for (unsigned int idx = 0; idx < YANET_CONFIG_NUMA_SIZE; ++idx)
		{
			dataplane::globalBase::atomic* atomic = basePermanently.globalBaseAtomics[idx];
			if (atomic == nullptr)
			{
				break;
			}

			dataplane::globalBase::fw_state_value_t* lookup_value;
			dataplane::spinlock_nonrecursive_t* locker;
			const uint32_t hash = atomic->fw4_state->lookup(key, lookup_value, locker);
			if (lookup_value)
			{
				lookup_value->last_seen = basePermanently.globalBaseAtomic->currentTime;
				lookup_value->packets_since_last_sync++;
				lookup_value->packets_forward++;
				lookup_value->tcp.src_flags |= flags;
				value.tcp = lookup_value->tcp; // to flags in the emit state
			}
			else
			{
				if (atomic->fw4_state->insert(hash, key, value))
				{
					emit = true;
				}
			}
			locker->unlock();
		}

		const auto& base = bases[localBaseId & 1];
		if (base.globalBase->fw_state_sync_configs[aclId].flows_size == 0)
		{
			// No fw state synchronization configured.
			stats.fwsync_no_config_drops++;
			return;
		}

		if (emit)
		{
			auto frame = dataplane::globalBase::fw_state_sync_frame_t::from_state_key(key);
			frame.flags = value.tcp.pack();
			acl_state_emit(aclId, frame);
		}
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		dataplane::globalBase::fw6_state_key_t key;
		key.proto = metadata->transport_headerType;
		key.__nap = 0;

		uint8_t flags = 0;

		// Swap src and dst addresses.
		rte_memcpy(key.dst_addr.bytes, ipv6Header->src_addr, 16);
		rte_memcpy(key.src_addr.bytes, ipv6Header->dst_addr, 16);

		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			// Swap src and dst ports.
			key.dst_port = rte_be_to_cpu_16(tcpHeader->src_port);
			key.src_port = rte_be_to_cpu_16(tcpHeader->dst_port);

			flags = common::fwstate::from_tcp_flags(tcpHeader->tcp_flags);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			// Swap src and dst ports.
			key.dst_port = rte_be_to_cpu_16(udpHeader->src_port);
			key.src_port = rte_be_to_cpu_16(udpHeader->dst_port);
		}
		else // todo: sctp, ddcp, udp-lite
		{
			// Protocols without ports, like ICMP, ESP etc.
			key.dst_port = 0;
			key.src_port = 0;
		}

		dataplane::globalBase::fw_state_value_t value;
		value.type = static_cast<dataplane::globalBase::fw_state_type>(metadata->transport_headerType);
		value.owner = dataplane::globalBase::fw_state_owner_e::internal;
		value.last_seen = basePermanently.globalBaseAtomic->currentTime;
		value.flow = flow;
		value.acl_id = aclId;
		value.last_sync = basePermanently.globalBaseAtomic->currentTime;
		value.packets_since_last_sync = 0;
		value.packets_backward = 0;
		value.packets_forward = 0;
		value.tcp.src_flags = flags;
		value.tcp.dst_flags = 0;

		bool emit = false;
		for (unsigned int idx = 0; idx < YANET_CONFIG_NUMA_SIZE; ++idx)
		{
			dataplane::globalBase::atomic* atomic = basePermanently.globalBaseAtomics[idx];
			if (atomic == nullptr)
			{
				break;
			}

			dataplane::globalBase::fw_state_value_t* lookup_value;
			dataplane::spinlock_nonrecursive_t* locker;
			const uint32_t hash = atomic->fw6_state->lookup(key, lookup_value, locker);
			if (lookup_value)
			{
				lookup_value->last_seen = basePermanently.globalBaseAtomic->currentTime;
				lookup_value->packets_since_last_sync++;
				lookup_value->packets_forward++;
				lookup_value->tcp.src_flags |= flags;
				value.tcp = lookup_value->tcp; // to flags in the emit state
			}
			else
			{
				if (atomic->fw6_state->insert(hash, key, value))
				{
					emit = true;
				}
			}
			locker->unlock();
		}

		const auto& base = bases[localBaseId & 1];
		if (base.globalBase->fw_state_sync_configs[aclId].flows_size == 0)
		{
			// No fw state synchronization configured.
			stats.fwsync_no_config_drops++;
			return;
		}

		if (emit)
		{
			auto frame = dataplane::globalBase::fw_state_sync_frame_t::from_state_key(key);
			frame.flags = value.tcp.pack();
			acl_state_emit(aclId, frame);
		}
	}
}

inline void cWorker::acl_state_emit(tAclId aclId, const dataplane::globalBase::fw_state_sync_frame_t& frame)
{
	rte_mbuf* mbuf = rte_pktmbuf_alloc(mempool);
	if (mbuf == nullptr)
	{
		stats.fwsync_multicast_egress_drops++;
		return;
	}

	/// @todo: init metadata

	constexpr uint16_t payload_offset = sizeof(rte_ether_hdr) + sizeof(rte_vlan_hdr) + sizeof(rte_ipv6_hdr) + sizeof(rte_udp_hdr);
	rte_pktmbuf_append(mbuf, payload_offset + sizeof(dataplane::globalBase::fw_state_sync_frame_t));

	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow.data.aclId = aclId;

	// We're only filling the payload here.
	// Other headers will be set in the slow worker before emitting.
	void* payload = rte_pktmbuf_mtod_offset(mbuf, void*, payload_offset);
	rte_memcpy(payload, (void*)&frame, sizeof(dataplane::globalBase::fw_state_sync_frame_t));

	// Push packet to the ring through stack.
	metadata->flow.type = common::globalBase::eFlowType::slowWorker_fw_sync;
	controlPlane_stack.insert(mbuf);
	stats.fwsync_multicast_egress_imm_packets++;
}

inline void cWorker::acl_egress_entry(rte_mbuf* mbuf, tAclId aclId)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	metadata->aclId = aclId;
	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		acl_egress_stack4.insert(mbuf);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		acl_egress_stack6.insert(mbuf);
	}
	else
	{
		controlPlane(mbuf);
	}
}

inline void cWorker::acl_egress_handle4()
{
	const auto& base = bases[localBaseId & 1];
	const auto& acl = base.globalBase->acl;

	if (unlikely(acl_egress_stack4.mbufsCount == 0))
	{
		return;
	}

	uint32_t mask = 0xFFFFFFFFu >> (8 * sizeof(uint32_t) - acl_egress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
		key_acl.ipv4_sources[mbuf_i].address = ipv4Header->src_addr;
		key_acl.ipv4_destinations[mbuf_i].address = ipv4Header->dst_addr;
	}

	acl.network.ipv4.source->lookup(key_acl.ipv4_sources,
	                                value_acl.ipv4_sources,
	                                acl_egress_stack4.mbufsCount);

	acl.network.ipv4.destination->lookup(key_acl.ipv4_destinations,
	                                     value_acl.ipv4_destinations,
	                                     acl_egress_stack4.mbufsCount);

	acl.network_table->lookup(value_acl.ipv4_sources,
	                          value_acl.ipv4_destinations,
	                          value_acl.networks,
	                          acl_egress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& network_value = value_acl.networks[mbuf_i];
		auto& transport_key = key_acl.transports[mbuf_i];

		const auto& transport_layer = acl.transport_layers[network_value & acl.transport_layers_mask];

		transport_key.network_id = network_value;
		transport_key.protocol = transport_layer.protocol.array[metadata->transport_headerType];
		transport_key.group1 = 0;
		transport_key.group2 = 0;
		transport_key.group3 = 0;

		if (!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT))
		{
			if (metadata->transport_headerType == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcp_header = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.tcp.source.array[rte_be_to_cpu_16(tcp_header->src_port)];
				transport_key.group2 = transport_layer.tcp.destination.array[rte_be_to_cpu_16(tcp_header->dst_port)];
				transport_key.group3 = transport_layer.tcp.flags.array[tcp_header->tcp_flags];
			}
			else if (metadata->transport_headerType == IPPROTO_UDP)
			{
				rte_udp_hdr* udp_header = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.udp.source.array[rte_be_to_cpu_16(udp_header->src_port)];
				transport_key.group2 = transport_layer.udp.destination.array[rte_be_to_cpu_16(udp_header->dst_port)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMP)
			{
				icmp_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmp_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMPV6)
			{
				icmpv6_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == YANET_TRANSPORT_TYPE_UNKNOWN)
			{
				mask ^= (1u << mbuf_i);
			}
		}

		transport_key.network_flags = acl.network_flags.array[metadata->network_flags];
	}

	acl.transport_table->lookup(hashes,
	                            key_acl.transports,
	                            value_acl.transports,
	                            acl_egress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& transport_value = value_acl.transports[mbuf_i];
		auto& total_key = key_acl.totals[mbuf_i];

		total_key.acl_id = metadata->aclId;
		total_key.transport_id = transport_value;
	}

	acl.total_table->lookup(hashes,
	                        key_acl.totals,
	                        value_acl.totals,
	                        acl_egress_stack4.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack4.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack4.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (!(mask & (1u << mbuf_i)))
		{
			counters[(uint32_t)common::globalBase::static_counter_type::acl_egress_v4_broken_packet]++;
			drop(mbuf);
			continue;
		}

		auto total_value = value_acl.totals[mbuf_i];
		if (total_value & 0x80000000u)
		{
			total_value = 0; ///< default
		}

		const auto& value = acl.values[total_value];

		if (value.flow.type == common::globalBase::eFlowType::drop)
		{
			// Try to match against stateful dynamic rules. If so - a packet will be handled.
			if (acl_egress_try_keepstate(mbuf))
			{
				continue;
			}
		}

		aclCounters[value.flow.counter_id]++;

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::log)
		{
			acl_log(mbuf, value.flow, metadata->aclId);
		}

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::keepstate)
		{
			acl_create_keepstate(mbuf, metadata->aclId, value.flow);
		}

		acl_egress_flow(mbuf, value.flow);
	}

	acl_egress_stack4.clear();
}

inline void cWorker::acl_egress_handle6()
{
	const auto& base = bases[localBaseId & 1];
	const auto& acl = base.globalBase->acl;

	if (unlikely(acl_egress_stack6.mbufsCount == 0))
	{
		return;
	}

	uint32_t mask = 0xFFFFFFFFu >> (8 * sizeof(uint32_t) - acl_egress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
		rte_memcpy(key_acl.ipv6_sources[mbuf_i].bytes, ipv6Header->src_addr, 16);
		rte_memcpy(key_acl.ipv6_destinations[mbuf_i].bytes, ipv6Header->dst_addr, 16);
	}

	acl.network.ipv6.source->lookup(key_acl.ipv6_sources,
	                                value_acl.ipv6_sources,
	                                acl_egress_stack6.mbufsCount);

	acl.network.ipv6.destination->lookup(key_acl.ipv6_destinations,
	                                     value_acl.ipv6_destinations,
	                                     acl_egress_stack6.mbufsCount);

	acl.network_table->lookup(value_acl.ipv6_sources,
	                          value_acl.ipv6_destinations,
	                          value_acl.networks,
	                          acl_egress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& network_value = value_acl.networks[mbuf_i];
		auto& transport_key = key_acl.transports[mbuf_i];

		const auto& transport_layer = acl.transport_layers[network_value & acl.transport_layers_mask];

		transport_key.network_id = network_value;
		transport_key.protocol = transport_layer.protocol.array[metadata->transport_headerType];
		transport_key.group1 = 0;
		transport_key.group2 = 0;
		transport_key.group3 = 0;

		if (!(metadata->network_flags & YANET_NETWORK_FLAG_NOT_FIRST_FRAGMENT))
		{
			if (metadata->transport_headerType == IPPROTO_TCP)
			{
				rte_tcp_hdr* tcp_header = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.tcp.source.array[rte_be_to_cpu_16(tcp_header->src_port)];
				transport_key.group2 = transport_layer.tcp.destination.array[rte_be_to_cpu_16(tcp_header->dst_port)];
				transport_key.group3 = transport_layer.tcp.flags.array[tcp_header->tcp_flags];
			}
			else if (metadata->transport_headerType == IPPROTO_UDP)
			{
				rte_udp_hdr* udp_header = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.udp.source.array[rte_be_to_cpu_16(udp_header->src_port)];
				transport_key.group2 = transport_layer.udp.destination.array[rte_be_to_cpu_16(udp_header->dst_port)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMP)
			{
				icmp_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmp_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == IPPROTO_ICMPV6)
			{
				icmpv6_header_t* icmp_header = rte_pktmbuf_mtod_offset(mbuf, icmpv6_header_t*, metadata->transport_headerOffset);

				transport_key.group1 = transport_layer.icmp.type_code.array[rte_be_to_cpu_16(icmp_header->typeCode)];
				transport_key.group2 = transport_layer.icmp.identifier.array[rte_be_to_cpu_16(icmp_header->identifier)];
			}
			else if (metadata->transport_headerType == YANET_TRANSPORT_TYPE_UNKNOWN)
			{
				mask ^= (1u << mbuf_i);
			}
		}

		transport_key.network_flags = acl.network_flags.array[metadata->network_flags];
	}

	acl.transport_table->lookup(hashes,
	                            key_acl.transports,
	                            value_acl.transports,
	                            acl_egress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		const auto& transport_value = value_acl.transports[mbuf_i];
		auto& total_key = key_acl.totals[mbuf_i];

		total_key.acl_id = metadata->aclId;
		total_key.transport_id = transport_value;
	}

	acl.total_table->lookup(hashes,
	                        key_acl.totals,
	                        value_acl.totals,
	                        acl_egress_stack6.mbufsCount);

	for (unsigned int mbuf_i = 0;
	     mbuf_i < acl_egress_stack6.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = acl_egress_stack6.mbufs[mbuf_i];
		dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

		if (!(mask & (1u << mbuf_i)))
		{
			counters[(uint32_t)common::globalBase::static_counter_type::acl_egress_v6_broken_packet]++;
			drop(mbuf);
			continue;
		}

		auto total_value = value_acl.totals[mbuf_i];
		if (total_value & 0x80000000u)
		{
			total_value = 0; ///< default
		}

		const auto& value = acl.values[total_value];

		if (value.flow.type == common::globalBase::eFlowType::drop)
		{
			// Try to match against stateful dynamic rules. If so - a packet will be handled.
			if (acl_egress_try_keepstate(mbuf))
			{
				continue;
			}
		}

		aclCounters[value.flow.counter_id]++;

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::log)
		{
			acl_log(mbuf, value.flow, metadata->aclId);
		}

		if (value.flow.flags & (uint8_t)common::globalBase::eFlowFlags::keepstate)
		{
			acl_create_keepstate(mbuf, metadata->aclId, value.flow);
		}

		acl_egress_flow(mbuf, value.flow);
	}

	acl_egress_stack6.clear();
}

void cWorker::acl_log(rte_mbuf* mbuf, const common::globalBase::tFlow& flow, tAclId aclId)
{
	if (rte_ring_full(ring_log))
	{
		stats.logs_drops++;
		return;
	}

	const auto& base = bases[localBaseId & 1];
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	samples::sample_t* sample;
	if (rte_mempool_get(dataPlane->mempool_log, (void**)&sample) != 0)
	{
		stats.logs_drops++;
		return;
	}

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);
		sample->ipv4_src_addr.address = ipv4Header->src_addr;
		sample->ipv4_dst_addr.address = ipv4Header->dst_addr;
		sample->is_ipv6 = 0;
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
		rte_memcpy(sample->ipv6_src_addr.bytes, ipv6Header->src_addr, 16);
		rte_memcpy(sample->ipv6_dst_addr.bytes, ipv6Header->dst_addr, 16);
		sample->is_ipv6 = 1;
	}
	else
	{
		stats.logs_drops++;
		rte_mempool_put(dataPlane->mempool_log, sample);
		return;
	}

	sample->action = flow.type;
	sample->flags = flow.flags;
	sample->counter_id = flow.counter_id;
	sample->serial = base.globalBase->serial;
	sample->acl_id = aclId;
	sample->proto = metadata->transport_headerType;

	if (metadata->transport_headerType == IPPROTO_TCP)
	{
		rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

		sample->src_port = rte_be_to_cpu_16(tcpHeader->src_port);
		sample->dst_port = rte_be_to_cpu_16(tcpHeader->dst_port);
	}
	else if (metadata->transport_headerType == IPPROTO_UDP)
	{
		rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

		sample->src_port = rte_be_to_cpu_16(udpHeader->src_port);
		sample->dst_port = rte_be_to_cpu_16(udpHeader->dst_port);
	}
	else
	{
		sample->src_port = 0;
		sample->dst_port = 0;
	}


	if (rte_ring_enqueue(ring_log, sample) != 0)
	{
		stats.logs_drops++;
		rte_mempool_put(dataPlane->mempool_log, sample);
		return;
	}
	stats.logs_packets++;
}

inline bool cWorker::acl_egress_try_keepstate(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

		dataplane::globalBase::fw4_state_key_t key;
		key.proto = metadata->transport_headerType;
		key.__nap = 0;
		key.src_addr.address = ipv4Header->src_addr;
		key.dst_addr.address = ipv4Header->dst_addr;

		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(tcpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(tcpHeader->dst_port);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(udpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(udpHeader->dst_port);
		}
		else // todo: sctp, ddcp, udp-lite
		{
			key.src_port = 0;
			key.dst_port = 0;
		}

		dataplane::globalBase::fw_state_value_t* value;
		dataplane::spinlock_nonrecursive_t* locker;
		basePermanently.globalBaseAtomic->fw4_state->lookup(key, value, locker);

		return acl_egress_try_keepstate(mbuf, value, locker);
	}
	else if (metadata->network_headerType == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
	{
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

		dataplane::globalBase::fw6_state_key_t key;
		key.proto = metadata->transport_headerType;
		key.__nap = 0;
		memcpy(key.src_addr.bytes, ipv6Header->src_addr, 16);
		memcpy(key.dst_addr.bytes, ipv6Header->dst_addr, 16);

		if (metadata->transport_headerType == IPPROTO_TCP)
		{
			rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(tcpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(tcpHeader->dst_port);
		}
		else if (metadata->transport_headerType == IPPROTO_UDP)
		{
			rte_udp_hdr* udpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr*, metadata->transport_headerOffset);

			key.src_port = rte_be_to_cpu_16(udpHeader->src_port);
			key.dst_port = rte_be_to_cpu_16(udpHeader->dst_port);
		}
		else // todo: sctp, ddcp, udp-lite
		{
			key.src_port = 0;
			key.dst_port = 0;
		}

		dataplane::globalBase::fw_state_value_t* value;
		dataplane::spinlock_nonrecursive_t* locker;
		basePermanently.globalBaseAtomic->fw6_state->lookup(key, value, locker);

		return acl_egress_try_keepstate(mbuf, value, locker);
	}

	return false;
}

inline bool cWorker::acl_egress_try_keepstate(rte_mbuf* mbuf,
                                              dataplane::globalBase::fw_state_value_t* value,
                                              dataplane::spinlock_nonrecursive_t* locker)
{
	// Checking both value and locker for non-being-nullptr seems redundant.
	if (value == nullptr)
	{
		// No record found, the caller should continue as usual.
		locker->unlock();
		return false;
	}

	uint8_t flags = 0;
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	if (metadata->transport_headerType == IPPROTO_TCP)
	{
		rte_tcp_hdr* tcpHeader = rte_pktmbuf_mtod_offset(mbuf, rte_tcp_hdr*, metadata->transport_headerOffset);
		flags = common::fwstate::from_tcp_flags(tcpHeader->tcp_flags);
	}

	// Copy the flow to prevent concurrent usage. In the other thread there can be garbage collector active.
	common::globalBase::tFlow flow = value->flow;
	value->last_seen = basePermanently.globalBaseAtomic->currentTime;
	value->packets_since_last_sync++;
	value->packets_backward++;
	value->tcp.dst_flags |= flags;
	locker->unlock();

	// Handle the packet according its flow.
	acl_egress_flow(mbuf, flow);
	return true;
}

inline void cWorker::acl_egress_flow(rte_mbuf* mbuf, const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		stats.acl_egress_dropPackets++;
		controlPlane(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::drop)
	{
		stats.acl_egress_dropPackets++;
		drop(mbuf);
	}
	else if (metadata->flow.type == common::globalBase::eFlowType::logicalPort_egress || metadata->flow.type == common::globalBase::eFlowType::acl_egress)
	{
		logicalPort_egress_entry(mbuf);
	}
	else
	{
		// should not happen
		drop(mbuf);
	}
}

inline void cWorker::dregress_entry(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	{
		rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);
		metadata->flowLabel = rte_be_to_cpu_32(ipv6Header->vtc_flow) & 0x000FFFFF;
	}

	{
		rte_memcpy(rte_pktmbuf_mtod_offset(mbuf, char*, metadata->transport_headerOffset - metadata->network_headerOffset),
		           rte_pktmbuf_mtod(mbuf, char*),
		           metadata->network_headerOffset);
		rte_pktmbuf_adj(mbuf, metadata->transport_headerOffset - metadata->network_headerOffset);

		/// @todo: check for ethernetHeader or vlanHeader
		uint16_t* nextHeaderType = rte_pktmbuf_mtod_offset(mbuf, uint16_t*, metadata->network_headerOffset - 2);
		if (metadata->transport_headerType == IPPROTO_IPIP)
		{
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
		}
		else
		{
			*nextHeaderType = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);
		}
	}

	/// @todo: opt
	preparePacket(mbuf);
	calcHash(mbuf);

	slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_dregress);
}

inline void cWorker::controlPlane(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow.type = common::globalBase::eFlowType::slowWorker_kni;

	controlPlane_stack.insert(mbuf);
}

inline void cWorker::controlPlane_handle()
{
	if (unlikely(controlPlane_stack.mbufsCount == 0))
	{
		return;
	}

	unsigned count = rte_ring_sp_enqueue_burst(ring_normalPriority,
	                                           (void**)controlPlane_stack.mbufs,
	                                           controlPlane_stack.mbufsCount,
	                                           nullptr);
	for (unsigned int mbuf_i = count;
	     mbuf_i < controlPlane_stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = controlPlane_stack.mbufs[mbuf_i];
		stats.ring_normalPriority_drops++;
		rte_pktmbuf_free(mbuf);
	}

	stats.ring_normalPriority_packets += count;

	controlPlane_stack.clear();
}

inline void cWorker::drop(rte_mbuf* mbuf)
{
	stats.dropPackets++;
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	if (basePermanently.globalBaseAtomic->physicalPort_flags[metadata->fromPortId] & YANET_PHYSICALPORT_FLAG_DROP_DUMP)
	{
		if (!rte_ring_full(ring_lowPriority))
		{
			rte_mbuf* mbuf_clone = rte_pktmbuf_alloc(mempool);
			if (mbuf_clone)
			{
				*YADECAP_METADATA(mbuf_clone) = *YADECAP_METADATA(mbuf);

				rte_memcpy(rte_pktmbuf_mtod(mbuf_clone, char*),
				           rte_pktmbuf_mtod(mbuf, char*),
				           mbuf->data_len);

				mbuf_clone->data_len = mbuf->data_len;
				mbuf_clone->pkt_len = mbuf->pkt_len;

				YADECAP_METADATA(mbuf_clone)->flow.type = common::globalBase::eFlowType::slowWorker_dump;
				YADECAP_METADATA(mbuf_clone)->flow.data.dump.type = common::globalBase::dump_type_e::physicalPort_drop;
				YADECAP_METADATA(mbuf_clone)->flow.data.dump.id = metadata->fromPortId;
				slowWorker_entry_lowPriority(mbuf_clone);
			}
		}
	}

	rte_pktmbuf_free(mbuf);
}

inline void cWorker::toFreePackets_handle()
{
	stack.mbufsCount = rte_ring_sc_dequeue_burst(ring_toFreePackets,
	                                             (void**)stack.mbufs,
	                                             CONFIG_YADECAP_MBUFS_BURST_SIZE,
	                                             nullptr);
	for (unsigned int mbuf_i = 0;
	     mbuf_i < stack.mbufsCount;
	     mbuf_i++)
	{
		rte_mbuf* mbuf = stack.mbufs[mbuf_i];
		rte_pktmbuf_free(mbuf);
	}
}

inline void cWorker::slowWorker_entry_highPriority(rte_mbuf* mbuf,
                                                   const common::globalBase::eFlowType& flowType)
{
	/// @todo: worker::tStack

	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow.type = flowType;

	if (rte_ring_sp_enqueue(ring_highPriority, (void*)mbuf))
	{
		stats.ring_highPriority_drops++;
		rte_pktmbuf_free(mbuf);
	}
	else
	{
		stats.ring_highPriority_packets++;
	}
}

inline void cWorker::slowWorker_entry_normalPriority(rte_mbuf* mbuf,
                                                     const common::globalBase::eFlowType& flowType)
{
	/// @todo: worker::tStack

	if (basePermanently.SWNormalPriorityRateLimitPerWorker != 0 && __atomic_fetch_sub(&packetsToSWNPRemainder, 1, __ATOMIC_RELAXED) <= 0)
	{
		rte_pktmbuf_free(mbuf);
		counters[(uint32_t)common::globalBase::static_counter_type::slow_worker_normal_priority_rate_limit_exceeded]++;

		return;
	}

	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow.type = flowType;

	if (rte_ring_sp_enqueue(ring_normalPriority, (void*)mbuf))
	{
		stats.ring_normalPriority_drops++;
		rte_pktmbuf_free(mbuf);
	}
	else
	{
		stats.ring_normalPriority_packets++;
	}
}

inline void cWorker::slowWorker_entry_lowPriority(rte_mbuf* mbuf)
{
	/// @todo: worker::tStack

	if (rte_ring_sp_enqueue(ring_lowPriority, (void*)mbuf))
	{
		stats.ring_lowPriority_drops++;
		rte_pktmbuf_free(mbuf);
	}
	else
	{
		stats.ring_lowPriority_packets++;
	}
}

YANET_NEVER_INLINE void cWorker::slowWorkerBeforeHandlePackets()
{
	localBaseId = currentBaseId;
}

YANET_NEVER_INLINE void cWorker::slowWorkerHandlePackets()
{
	handlePackets();
	toFreePackets_handle();
}

YANET_NEVER_INLINE void cWorker::slowWorkerAfterHandlePackets()
{
	iteration++;
}

YANET_NEVER_INLINE void cWorker::slowWorkerFlow(rte_mbuf* mbuf,
                                                const common::globalBase::tFlow& flow)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);
	metadata->flow = flow;

	if (flow.type == common::globalBase::eFlowType::acl_ingress)
	{
		acl_ingress_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::tun64_ipv4_checked)
	{
		tun64_ipv4_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::tun64_ipv6_checked)
	{
		tun64_ipv6_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::decap_checked)
	{
		decap_entry_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_ingress_checked)
	{
		nat64stateless_ingress_entry_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_ingress_icmp)
	{
		nat64stateless_ingress_entry_icmp(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_ingress_fragmentation)
	{
		nat64stateless_ingress_entry_fragmentation(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_checked)
	{
		nat64stateless_egress_entry_checked(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_icmp)
	{
		nat64stateless_egress_entry_icmp(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_fragmentation)
	{
		nat64stateless_egress_entry_fragmentation(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::nat64stateless_egress_farm)
	{
		slowWorkerFarmHandleFragment(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route)
	{
		route_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::route_tunnel)
	{
		route_tunnel_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::logicalPort_egress)
	{
		logicalPort_egress_entry(mbuf);
	}
	else if (flow.type == common::globalBase::eFlowType::controlPlane)
	{
		controlPlane(mbuf);
	}
	else
	{
		drop(mbuf);
	}
}

YANET_NEVER_INLINE void cWorker::slowWorkerTranslation(rte_mbuf* mbuf,
                                                       const dataplane::globalBase::tNat64stateless& nat64stateless,
                                                       const dataplane::globalBase::nat64stateless_translation_t& translation,
                                                       bool direction)
{
	if (direction)
	{
		nat64stateless_ingress_translation(mbuf, nat64stateless, translation);
	}
	else
	{
		nat64stateless_egress_translation(mbuf, translation);
	}
}

YANET_NEVER_INLINE void cWorker::slowWorkerHandleFragment(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	const auto& base = bases[localBaseId & 1];
	const auto& nat64stateless = base.globalBase->nat64statelesses[metadata->flow.data.nat64stateless.id];

	rte_ipv4_hdr* ipv4Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr*, metadata->network_headerOffset);

	uint32_t ipv4AddressSrc = ipv4Header->src_addr;
	uint32_t ipv4AddressDst = ipv4Header->dst_addr;

	ipv4_to_ipv6(mbuf);

	rte_ipv6_hdr* ipv6Header = rte_pktmbuf_mtod_offset(mbuf, rte_ipv6_hdr*, metadata->network_headerOffset);

	rte_memcpy(&ipv6Header->src_addr[0], nat64stateless.defrag_source_prefix.bytes, 12);
	rte_memcpy(&ipv6Header->src_addr[12], &ipv4AddressSrc, 4);
	rte_memcpy(ipv6Header->dst_addr, nat64stateless.defrag_farm_prefix.bytes, 12);
	rte_memcpy(&ipv6Header->dst_addr[12], &ipv4AddressDst, 4);

	preparePacket(mbuf);
}

YANET_NEVER_INLINE void cWorker::slowWorkerFarmHandleFragment(rte_mbuf* mbuf)
{
	dataplane::metadata* metadata = YADECAP_METADATA(mbuf);

	metadata->repeat_ttl--;
	if (metadata->repeat_ttl == 0)
	{
		stats.repeat_ttl++;
		rte_pktmbuf_free(mbuf);
		return;
	}

	ipv6_to_ipv4(mbuf, nat64statelessPacketId);

	preparePacket(mbuf);
	slowWorker_entry_normalPriority(mbuf, common::globalBase::eFlowType::slowWorker_repeat);
}
