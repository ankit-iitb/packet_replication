/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_fbk_hash.h>
#include <rte_ip.h>

#define RTE_LOGTYPE_IPv4_MULTICAST RTE_LOGTYPE_USER1

#define MAX_PORTS 16

#define	PKT_MBUF_DATA_SIZE	RTE_MBUF_DEFAULT_BUF_SIZE
#define	NB_PKT_MBUF	8192
#define NB_CLONE_MBUF 512

#define	HDR_MBUF_DATA_SIZE	(2 * RTE_PKTMBUF_HEADROOM)
#define	NB_HDR_MBUF	(NB_PKT_MBUF * MAX_PORTS)

/* allow max jumbo frame 9.5 KB */
#define	JUMBO_FRAME_MAX_SIZE	0x2600

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

/* Configure how many packets ahead to prefetch, when reading packets */
#define PREFETCH_OFFSET	3

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct ether_addr ports_eth_addr[MAX_PORTS];

/* mask of enabled ports */
static uint32_t enabled_port_mask = 0;

static uint8_t nb_ports = 0;

static int rx_queue_per_lcore = 1;

struct mbuf_table {
	uint16_t len;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
	uint64_t tx_tsc;
	uint16_t n_rx_queue;
	uint8_t rx_queue_list[MAX_RX_QUEUE_PER_LCORE];
	uint16_t tx_queue_id[MAX_PORTS];
	struct mbuf_table tx_mbufs[MAX_PORTS];
} __rte_cache_aligned;
static struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.max_rx_pkt_len = JUMBO_FRAME_MAX_SIZE,
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support enabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static struct rte_mempool *packet_pool, *header_pool, *clone_pool;

/* Send burst of packets on an output interface */
static void
send_burst(struct lcore_queue_conf *qconf, uint8_t port)
{
	struct rte_mbuf **m_table;
	uint16_t n, queueid;
	int ret;

	queueid = qconf->tx_queue_id[port];
	m_table = (struct rte_mbuf **)qconf->tx_mbufs[port].m_table;
	n = qconf->tx_mbufs[port].len;

	ret = rte_eth_tx_burst(port, queueid, m_table, n);
	while (unlikely (ret < n)) {
		printf("Tx'ed %d on port %d\n", ret, port);
		rte_pktmbuf_free(m_table[ret]);
		ret++;
	}

	qconf->tx_mbufs[port].len = 0;
}

/* Get number of bits set. */
static inline uint32_t
bitcnt(uint32_t v)
{
	uint32_t n;

	for (n = 0; v != 0; v &= v - 1, n++)
		;

	return n;
}

static inline void
mcast_out_pkt(struct rte_mbuf *pkt, struct rte_mbuf *hdr)
{
	/* prepend new header */
        char *eth_hdr = (char *)rte_pktmbuf_prepend(hdr, pkt->pkt_len);
        if(eth_hdr == NULL) {
                printf("panic\n");
        }

	/*Copy the content of old packet into new one*/
        char *b = rte_pktmbuf_mtod((struct rte_mbuf*)pkt, char *);
        rte_memcpy(eth_hdr, b, pkt->pkt_len);

        /* copy metadata from source packet*/
        hdr->port = pkt->port;
        hdr->vlan_tci = pkt->vlan_tci;
        hdr->vlan_tci_outer = pkt->vlan_tci_outer;
        hdr->tx_offload = pkt->tx_offload;
        hdr->hash = pkt->hash;

        hdr->ol_flags = pkt->ol_flags;

        __rte_mbuf_sanity_check(hdr, 1);
}

/*
 * Write new Ethernet header to the outgoing packet,
 * and put it into the outgoing queue for the given port.
 */
static inline void
mcast_send_pkt(struct rte_mbuf *pkt, struct ether_addr *dest_addr,
		struct lcore_queue_conf *qconf, uint8_t port)
{
	struct ether_hdr *ethdr;
	uint16_t len;

	/* point to ethernet header in packet. */
	ethdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);

	ether_addr_copy(dest_addr, &ethdr->d_addr);
	ether_addr_copy(dest_addr, &ethdr->s_addr);
	ethdr->ether_type = rte_be_to_cpu_16(ETHER_TYPE_IPv4);

	/* Put new packet into the output queue */
	len = qconf->tx_mbufs[port].len;
	qconf->tx_mbufs[port].m_table[len] = pkt;
	qconf->tx_mbufs[port].len = ++len;

	/* Transmit packets */
	if (unlikely(MAX_PKT_BURST == len))
		send_burst(qconf, port);
}

/* Multicast forward of the input packet */
static inline void
mcast_forward(struct rte_mbuf *m, struct lcore_queue_conf *qconf)
{
	struct ether_addr s_addr = {{160,54,159,32,56,74}};

	uint32_t port_mask = 1; //TODO: replication factor, pass it from the terminal
	uint32_t port_num = bitcnt(port_mask);
	struct rte_mbuf *hdr[port_num];
	int ret = rte_pktmbuf_alloc_bulk(header_pool, hdr, port_num);
	if(ret == -ENOENT) {
		printf("Ret %d\n", ret);
		return;
	}

	for (int i = 0; i < (int )port_num; i++) {
		mcast_out_pkt(m, hdr[i]);
		mcast_send_pkt(hdr[i], &s_addr, qconf, 3); //Forward on port 3
	}
	mcast_send_pkt(m, &s_addr, qconf, 0); //Forward on port 0
}

/* Send burst of outgoing packet, if timeout expires. */
static inline void
send_timeout_burst(struct lcore_queue_conf *qconf)
{
	uint64_t cur_tsc;
	uint8_t portid;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;

	cur_tsc = rte_rdtsc();
	if (likely (cur_tsc < qconf->tx_tsc + drain_tsc))
		return;

	for (portid = 0; portid < MAX_PORTS; portid++) {
		if (qconf->tx_mbufs[portid].len != 0)
			send_burst(qconf, portid);
	}
	qconf->tx_tsc = cur_tsc;
}

/* main processing loop */
static int
main_loop(__rte_unused void *dummy)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	int i, j, nb_rx;
	uint8_t portid;
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];


	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, IPv4_MULTICAST, "lcore %u has nothing to do\n",
		    lcore_id);
		return 0;
	}

	RTE_LOG(INFO, IPv4_MULTICAST, "entering main loop on lcore %u\n",
	    lcore_id);

	for (i = 0; i < qconf->n_rx_queue; i++) {

		portid = qconf->rx_queue_list[i];
		RTE_LOG(INFO, IPv4_MULTICAST, " -- lcoreid=%u portid=%d\n",
		    lcore_id, (int) portid);
	}

	while (1) {
	
		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_queue; i++) {

			portid = qconf->rx_queue_list[i];
			nb_rx = rte_eth_rx_burst(portid, 0, pkts_burst,
						 MAX_PKT_BURST);

			/* Prefetch first packets */
			for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++) {
				rte_prefetch0(rte_pktmbuf_mtod(
						pkts_burst[j], void *));
			}

			/* Prefetch and forward already prefetched packets */
			for (j = 0; j < (nb_rx - PREFETCH_OFFSET); j++) {
				rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[
						j + PREFETCH_OFFSET], void *));
				mcast_forward(pkts_burst[j], qconf);
			}

			/* Forward remaining prefetched packets */
			for (; j < nb_rx; j++) {
				mcast_forward(pkts_burst[j], qconf);
			}
		}

		/* Send out packets from TX queues */
		send_timeout_burst(qconf);
	}
}

/* display usage */
static void
print_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
	    "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	    "  -q NQ: number of queue (=ports) per lcore (default is 1)\n",
	    prgname);
}

static uint32_t
parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return (uint32_t)pm;
}

static int
parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse numerical string */
	errno = 0;
	n = strtoul(q_arg, &end, 0);
	if (errno != 0 || end == NULL || *end != '\0' ||
			n == 0 || n >= MAX_RX_QUEUE_PER_LCORE)
		return -1;

	return n;
}

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:q:",
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			enabled_port_mask = parse_portmask(optarg);
			if (enabled_port_mask == 0) {
				printf("invalid portmask\n");
				print_usage(prgname);
				return -1;
			}
			break;

		/* nqueue */
		case 'q':
			rx_queue_per_lcore = parse_nqueue(optarg);
			if (rx_queue_per_lcore < 0) {
				printf("invalid queue number\n");
				print_usage(prgname);
				return -1;
			}
			break;

		default:
			print_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

static void
print_ethaddr(const char *name, struct ether_addr *eth_addr)
{
	char buf[ETHER_ADDR_FMT_SIZE];
	ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s%s", name, buf);
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
							(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

int
main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	int ret;
	uint16_t queueid;
	unsigned lcore_id = 0, rx_lcore_id = 0;
	uint32_t n_tx_queue, nb_lcores;
	uint8_t portid;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= ret;
	argv += ret;

	/* parse application arguments (after the EAL ones) */
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid IPV4_MULTICAST parameters\n");

	/* create the mbuf pools */
	packet_pool = rte_pktmbuf_pool_create("packet_pool", NB_PKT_MBUF, 32,
		0, PKT_MBUF_DATA_SIZE, rte_socket_id());

	if (packet_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init packet mbuf pool\n");

	header_pool = rte_pktmbuf_pool_create("header_pool", NB_HDR_MBUF, 32,
		0, HDR_MBUF_DATA_SIZE, rte_socket_id());

	if (header_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init header mbuf pool\n");

	clone_pool = rte_pktmbuf_pool_create("clone_pool", NB_CLONE_MBUF, 31,
		0, 0, rte_socket_id());

	if (clone_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init clone mbuf pool\n");

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No physical ports!\n");
	if (nb_ports > MAX_PORTS)
		nb_ports = MAX_PORTS;

	nb_lcores = rte_lcore_count();

	/* initialize all ports */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %d\n", portid);
			continue;
		}

		qconf = &lcore_queue_conf[rx_lcore_id];

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       qconf->n_rx_queue == (unsigned)rx_queue_per_lcore) {

			rx_lcore_id ++;
			qconf = &lcore_queue_conf[rx_lcore_id];

			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}
		qconf->rx_queue_list[qconf->n_rx_queue] = portid;
		qconf->n_rx_queue++;

		/* init port */
		printf("Initializing port %d on lcore %u... ", portid,
		       rx_lcore_id);
		fflush(stdout);

		n_tx_queue = nb_lcores;
		if (n_tx_queue > MAX_TX_QUEUE_PER_PORT)
			n_tx_queue = MAX_TX_QUEUE_PER_PORT;
		ret = rte_eth_dev_configure(portid, 1, (uint16_t)n_tx_queue,
					    &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%d\n",
				  ret, portid);

		rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		print_ethaddr(" Address:", &ports_eth_addr[portid]);
		printf(", ");

		/* init one RX queue */
		queueid = 0;
		printf("rxq=%hu ", queueid);
		fflush(stdout);
		ret = rte_eth_rx_queue_setup(portid, queueid, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     NULL,
					     packet_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: err=%d, port=%d\n",
				  ret, portid);

		/* init one TX queue per couple (lcore,port) */
		queueid = 0;

		RTE_LCORE_FOREACH(lcore_id) {
			if (rte_lcore_is_enabled(lcore_id) == 0)
				continue;
			printf("txq=%u,%hu ", lcore_id, queueid);
			fflush(stdout);

			rte_eth_dev_info_get(portid, &dev_info);
			txconf = &dev_info.default_txconf;
			txconf->txq_flags = 0;
			txconf->tx_rs_thresh = 8;
			ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
						     rte_lcore_to_socket_id(lcore_id), txconf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: err=%d, "
					  "port=%d\n", ret, portid);

			qconf = &lcore_queue_conf[lcore_id];
			qconf->tx_queue_id[portid] = queueid;
			queueid++;
		}

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start: err=%d, port=%d\n",
				  ret, portid);
		rte_eth_promiscuous_enable(portid);	//TODO:
		printf("done:\n");
	}

	check_all_ports_link_status(nb_ports, enabled_port_mask);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;
	}

	return 0;
}
