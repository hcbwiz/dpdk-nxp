/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019-2021 NXP
 * Code was mostly borrowed from examples/l3fwd/main.c
 * See examples/l3fwd/main.c for additional Copyrights.
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
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_vect.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>
#include <rte_cpuflags.h>
#include <rte_eventdev.h>
#include <rte_event_eth_rx_adapter.h>
#include <rte_string_fns.h>
#include <rte_spinlock.h>
#include <rte_malloc.h>

#include <cmdline_parse.h>
#include <cmdline_parse_etheraddr.h>

#include "port_fwd.h"

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024

#define MAX_TX_QUEUE_PER_PORT RTE_MAX_ETHPORTS
#define MAX_RX_QUEUE_PER_PORT 128

#define MAX_LCORE_PARAMS 1024

/* Static global variables used within this file. */
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

uint32_t max_pkt_burst = MAX_PKT_BURST;
uint32_t max_tx_burst = MAX_TX_BURST;
uint32_t max_rx_burst = MAX_PKT_BURST;

static int s_proc_2nd_standalone;
static int s_pktgen_tool;
static struct rte_mempool *pktmbuf_tx_pool;

/* Global variables. */

static int per_port_pool;

static volatile bool force_quit;

/* mask of enabled ports */
static uint32_t enabled_port_mask;
static uint16_t enabled_port_num;

struct lcore_params {
	uint16_t port_id;
	uint8_t queue_id;
	uint8_t lcore_id;
} __rte_cache_aligned;

static struct lcore_params lcore_params_array[MAX_LCORE_PARAMS];
static struct lcore_params lcore_params_array_default[] = {
	{0, 0, 2},
	{0, 1, 2},
	{0, 2, 2},
	{1, 0, 2},
	{1, 1, 2},
	{1, 2, 2},
	{2, 0, 2},
	{3, 0, 3},
	{3, 1, 3},
};

static struct lcore_params *lcore_params = lcore_params_array_default;
static uint16_t nb_lcore_params = sizeof(lcore_params_array_default) /
				sizeof(lcore_params_array_default[0]);

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = RTE_MBUF_DEFAULT_DATAROOM,
		.offloads = DEV_RX_OFFLOAD_JUMBO_FRAME,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IP,
		},
	},
};

static struct lcore_conf s_lcore_conf[RTE_MAX_LCORE];

static int fwd_dst_port[RTE_MAX_ETHPORTS];

static struct rte_mempool *pktmbuf_pool[RTE_MAX_ETHPORTS][NB_SOCKETS];

struct loop_mode {
	int (*parse_fwd_dst)(int portid);
	rte_rx_callback_fn cb_parse_ptype;
	int (*main_loop)(void *dummy);
};

static int parse_port_fwd_dst(int portid)
{
	char *penv;
	char env_name[64];

	if (s_pktgen_tool)
		return 1;

	fwd_dst_port[portid] = -1;
	sprintf(env_name, "PORT%d_FWD", portid);
	penv = getenv(env_name);
	if (penv)
		fwd_dst_port[portid] = atoi(penv);

	if (fwd_dst_port[portid] < 0) {
		printf("error: destination of port %d fwd not set\n",
			portid);
		return 0;
	}

	printf("port forwarding from port %d to port %d\r\n",
		portid, fwd_dst_port[portid]);

	return 1;
}

static __thread struct rte_mbuf *loopback_pkts[512 * MAX_PKT_BURST];

static int loopback_start_fun(uint16_t rx_port,
	uint16_t tx_port, uint16_t rx_queue, uint16_t tx_queue)
{
	struct rte_eth_rxq_info rxqinfo;
	struct rte_eth_txq_info txqinfo;
	struct rte_mempool *pool;
	uint16_t nb_desc, loop, size = 0, total_nb, tx_nb = 0;
	int ret;
	char *penv = getenv("LOOPBACK_PACKETS_SIZE");

	if (penv)
		size = atoi(penv);

	if (!size)
		size = 60;

	ret = rte_eth_rx_queue_info_get(rx_port, rx_queue, &rxqinfo);
	if (ret) {
		printf("error: can't get info of RXQ %d of port %d\n",
			rx_queue, rx_port);
		return ret;
	}
	pool = rxqinfo.mp;

	ret = rte_eth_tx_queue_info_get(tx_port, tx_queue, &txqinfo);
	if (ret) {
		printf("error: can't get info of TXQ %d of port %d\n",
			tx_queue, tx_port);
		return ret;
	}
	nb_desc = txqinfo.nb_desc < 512 ? txqinfo.nb_desc : 512;

	for (loop = 0; loop < (nb_desc * MAX_PKT_BURST); loop++) {
		loopback_pkts[loop] = rte_pktmbuf_alloc(pool);
		if (!loopback_pkts[loop])
			break;
	}

	total_nb = loop;

	for (loop = 0; loop < total_nb; loop++) {
		loopback_pkts[loop]->data_off = RTE_PKTMBUF_HEADROOM;
		loopback_pkts[loop]->nb_segs = 1;
		loopback_pkts[loop]->next = 0;
		loopback_pkts[loop]->pkt_len = size;
		loopback_pkts[loop]->data_len = size;
		*rte_pktmbuf_mtod_offset(loopback_pkts[loop], char *, -1) = 0;
	}

	printf("Trying to inject %d mbufs whose sizes are %d"
			" to start loopback on port %d txq %d\r\n",
			total_nb, size, tx_port, tx_queue);

	for (loop = 0; loop < (total_nb / MAX_PKT_BURST); loop++) {
		ret = rte_eth_tx_burst(tx_port, tx_queue,
				&loopback_pkts[loop * MAX_PKT_BURST],
				MAX_PKT_BURST);
		tx_nb += ret;
		if (ret != MAX_PKT_BURST)
			break;
	}

	if (tx_nb < total_nb)
		rte_pktmbuf_free_bulk(&loopback_pkts[tx_nb],
			total_nb - tx_nb);

	printf("Total %d mbufs are injected.\r\n", tx_nb);

	return 0;
}

static uint8_t pkt_data_base[] = {
	0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x10, 0x94, 0x00, 0x00, 0x02,
	0x08, 0x00,
	0x45, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
	0xff, 0x11, 0x2f, 0xb4,
	0xc6, 0x12, 0x00, 0x00,
	0xc6, 0x12, 0x00, 0x00,
	0x04, 0x00, 0x04, 0x00,
	0x00, 0x00, 0x6b, 0xc9
};

static void pktgen_drain(struct lcore_conf *qconf)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	int i, nb_rx, rx_count;
	uint16_t portid;
	uint8_t queueid;

	for (i = 0; i < qconf->n_rx_queue; ++i) {
		portid = qconf->rx_queue_list[i].port_id;
		queueid = qconf->rx_queue_list[i].queue_id;
		rx_count = 100;
RX_DRAIN:
		nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst,
					MAX_PKT_BURST);
		if (nb_rx > 0) {
			rte_pktmbuf_free_bulk(pkts_burst, nb_rx);
			rx_count = 100;
			goto RX_DRAIN;
		}
		rx_count--;
		if (rx_count > 0)
			goto RX_DRAIN;
	}
}

static int s_pktgen_rx_drain;
#define PKTGEN_RX_DRAIN_DEFAULT 10
static void pktgen_rxtx(struct lcore_conf *qconf,
			struct rte_mempool *pool, int pkt_len, int rx_only)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	int i, nb_rx, ret, nb_tx, j, rx_count;
	uint16_t portid;
	uint8_t queueid;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	char *penv;

	if (!s_pktgen_rx_drain) {
		penv = getenv("PORT_FWD_PKTGEN_RX_DRAIN");
		if (penv)
			s_pktgen_rx_drain = atoi(penv);
		else
			s_pktgen_rx_drain = PKTGEN_RX_DRAIN_DEFAULT;
	}

	for (i = 0; i < qconf->n_rx_queue; ++i) {
		portid = qconf->rx_queue_list[i].port_id;
		queueid = qconf->rx_queue_list[i].queue_id;
		rx_count = s_pktgen_rx_drain;

RX_LOOP:
		nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst,
					MAX_PKT_BURST);
		if (nb_rx > 0) {
			for (j = 0; j < nb_rx; j++) {
				if ((int)pkts_burst[j]->pkt_len != pkt_len)
					printf("RX pkt should be %d, but it's %d\r\n",
						pkt_len,
						pkts_burst[j]->pkt_len);
				qconf->rx_statistic[portid].bytes +=
					pkts_burst[j]->pkt_len;
				qconf->rx_statistic[portid].bytes_fcs +=
					pkts_burst[j]->pkt_len +
					PKTGEN_ETH_FCS_SIZE;
				qconf->rx_statistic[portid].bytes_overhead +=
					pkts_burst[j]->pkt_len +
					PKTGEN_ETH_OVERHEAD_SIZE;
			}
			qconf->rx_statistic[portid].packets += nb_rx;
			rte_pktmbuf_free_bulk(pkts_burst, nb_rx);
		}
		if (nb_rx > 0) {
			/* To reduce the TX Congestion*/
			rx_count = s_pktgen_rx_drain;
			goto RX_LOOP;
		}
		rx_count--;
		if (rx_count > 0)
			goto RX_LOOP;

		if (rx_only)
			continue;
		ret = rte_pktmbuf_alloc_bulk(pool, pkts_burst, MAX_PKT_BURST);
		if (!ret) {
			for (j = 0; j < MAX_PKT_BURST; j++) {
				pkts_burst[j]->pkt_len = pkt_len;
				pkts_burst[j]->data_len = pkt_len;
				memcpy(rte_pktmbuf_mtod(pkts_burst[j], char *),
					pkt_data_base, sizeof(pkt_data_base));
				eth_hdr = rte_pktmbuf_mtod(pkts_burst[j],
					struct rte_ether_hdr *);
				ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
				ipv4_hdr->total_length =
					rte_cpu_to_be_16(pkt_len -
						sizeof(struct rte_ether_hdr));
				ipv4_hdr->src_addr =
					rte_cpu_to_be_32(qconf->tx_ip[portid]);
				qconf->tx_ip[portid]++;
				ipv4_hdr->hdr_checksum = 0;
				ipv4_hdr->hdr_checksum =
					rte_cpu_to_be_16(rte_ipv4_cksum(ipv4_hdr));
			}
			nb_tx = rte_eth_tx_burst(portid, queueid, pkts_burst,
						MAX_PKT_BURST);
			if (nb_tx > 0) {
				for (j = 0; j < nb_tx; j++) {
					qconf->tx_statistic[portid].bytes +=
						pkt_len;
					qconf->tx_statistic[portid].bytes_fcs +=
						pkt_len + PKTGEN_ETH_FCS_SIZE;
					qconf->tx_statistic[portid].bytes_overhead +=
						pkt_len + PKTGEN_ETH_OVERHEAD_SIZE;
				}
				qconf->tx_statistic[portid].packets += nb_tx;
			}
			/* Free any unsent packets. */
			if (unlikely(nb_tx < MAX_PKT_BURST)) {
				rte_pktmbuf_free_bulk(&pkts_burst[nb_tx],
					MAX_PKT_BURST - nb_tx);
			}
		}
	}
}

static int
port_fwd_dst_port(uint16_t src_port)
{
	return fwd_dst_port[src_port];
}

static int
main_loop(__attribute__((unused)) void *dummy)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned int lcore_id;
	int i, nb_rx, j, pkt_len;
	uint16_t idx;
	uint16_t nb_tx;
	uint16_t portid;
	int dstportid;
	uint8_t queueid;
	struct lcore_conf *qconf;
	int loopback_start = 0;
	char *penv = getenv("PORT_FWD_LOOPBACK_PORT");
	int loopback_port = -1;
	int pktgen_len = 64, rx_only = 0;

	if (penv)
		loopback_port = atoi(penv);

	if (s_pktgen_tool) {
		penv = getenv("PORT_FWD_PKTGEN_PKT_LEN");
		if (penv)
			pktgen_len = atoi(penv);
		if (pktgen_len < 64 || pktgen_len > 1518) {
			RTE_LOG(INFO, PMD, "PKT len error %d\r\n", pktgen_len);
			return 0;
		}
		penv = getenv("PORT_FWD_PKTGEN_RX_ONLY");
		if (penv)
			rx_only = atoi(penv);
	}

	lcore_id = rte_lcore_id();
	qconf = &s_lcore_conf[lcore_id];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, PMD, "lcore %u has nothing to do\n", lcore_id);
		return 0;
	}

	RTE_LOG(INFO, PMD, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_queue; i++) {
		portid = qconf->rx_queue_list[i].port_id;
		queueid = qconf->rx_queue_list[i].queue_id;
		RTE_LOG(INFO, PMD,
			" -- lcoreid=%u portid=%u rxqueueid=%hhu\n",
			lcore_id, portid, queueid);
	}

	while (!force_quit) {
		if (unlikely(s_pktgen_tool)) {
			pktgen_rxtx(qconf, pktmbuf_tx_pool,
				pktgen_len -
				PKTGEN_ETH_FCS_SIZE, rx_only);
			continue;
		}

		/* Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_queue; ++i) {
			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;

			dstportid = port_fwd_dst_port(portid);
			if (dstportid < 0) {
				printf("PORT RX err portid:%d\r\n", portid);
				dstportid = 0;
			}

			nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst,
				MAX_PKT_BURST);
			if (nb_rx == 0) {
				if (unlikely(loopback_port == dstportid &&
					!loopback_start)) {
					loopback_start_fun(portid, dstportid,
						queueid,
						qconf->tx_queue_id[dstportid]);
					loopback_start = 1;
				}
				continue;
			}

			if (nb_rx > 0) {
				for (j = 0; j < nb_rx; j++) {
					qconf->rx_statistic[portid].bytes +=
						pkts_burst[j]->pkt_len;
					qconf->rx_statistic[portid].bytes_fcs +=
						pkts_burst[j]->pkt_len + PKTGEN_ETH_FCS_SIZE;
					qconf->rx_statistic[portid].bytes_overhead +=
						pkts_burst[j]->pkt_len + PKTGEN_ETH_OVERHEAD_SIZE;
				}
				qconf->rx_statistic[portid].packets += nb_rx;
			}

			pkt_len = pkts_burst[0]->pkt_len;

			/* Send burst of TX packets, to second port of pair. */
			nb_tx = rte_eth_tx_burst(dstportid,
					qconf->tx_queue_id[dstportid],
					pkts_burst, nb_rx);

			if (nb_tx > 0) {
				for (j = 0; j < nb_tx; j++) {
					qconf->tx_statistic[dstportid].bytes +=
						pkt_len;
					qconf->tx_statistic[dstportid].bytes_fcs +=
						pkt_len + PKTGEN_ETH_FCS_SIZE;
					qconf->tx_statistic[dstportid].bytes_overhead +=
						pkt_len + PKTGEN_ETH_OVERHEAD_SIZE;
				}
				qconf->tx_statistic[dstportid].packets += nb_tx;
			}

			/* Free any unsent packets. */
			if (unlikely(nb_tx < nb_rx)) {
				for (idx = nb_tx; idx < nb_rx; idx++)
					rte_pktmbuf_free(pkts_burst[idx]);
			}
		}
	}

	if (s_pktgen_tool)
		pktgen_drain(qconf);

	return 0;
}

static struct loop_mode port_fwd_demo = {
	.parse_fwd_dst = parse_port_fwd_dst,
	.main_loop = main_loop,
};

static int
check_lcore_params(void)
{
	uint8_t queue, lcore;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		queue = lcore_params[i].queue_id;
		if (queue >= MAX_RX_QUEUE_PER_PORT) {
			printf("invalid queue number: %hhu\n", queue);
			return -1;
		}
		lcore = lcore_params[i].lcore_id;
		if (!rte_lcore_is_enabled(lcore)) {
			printf("error: lcore %hhu is not enabled\n", lcore);
			return -1;
		}
	}
	return 0;
}

static int
check_port_config(void)
{
	uint16_t portid;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		portid = lcore_params[i].port_id;
		if ((enabled_port_mask & (1 << portid)) == 0) {
			printf("port %u is not enabled in port mask\n", portid);
			return -1;
		}
		if (!rte_eth_dev_is_valid_port(portid)) {
			printf("port %u is not present on the board\n", portid);
			return -1;
		}
	}
	return 0;
}

static uint8_t
get_port_n_rx_queues(const uint16_t port)
{
	int queue = -1;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].port_id == port) {
			if (lcore_params[i].queue_id == queue+1)
				queue = lcore_params[i].queue_id;
			else
				rte_exit(EXIT_FAILURE,
					"queue ids of the port %d must be"
					" in sequence and must start with 0\n",
					lcore_params[i].port_id);
		}
	}
	return (uint8_t)(++queue);
}

static int
init_lcore_rx_queues(void)
{
	uint16_t i, nb_rx_queue;
	uint8_t lcore;

	for (i = 0; i < nb_lcore_params; ++i) {
		lcore = lcore_params[i].lcore_id;
		nb_rx_queue = s_lcore_conf[lcore].n_rx_queue;
		if (nb_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
			printf("error: too many queues (%u) for lcore: %u\n",
				(unsigned int)nb_rx_queue + 1, (unsigned int)lcore);
			return -1;
		}

		s_lcore_conf[lcore].rx_queue_list[nb_rx_queue].port_id =
				lcore_params[i].port_id;
		s_lcore_conf[lcore].rx_queue_list[nb_rx_queue].queue_id =
				lcore_params[i].queue_id;
		s_lcore_conf[lcore].n_rx_queue++;
	}
	return 0;
}

static int
parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static int
parse_config(const char *q_arg)
{
	char s[256];
	const char *p, *p0 = q_arg;
	char *end;
	enum fieldnames {
		FLD_PORT = 0,
		FLD_QUEUE,
		FLD_LCORE,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	char *str_fld[_NUM_FLD];
	int i;
	unsigned int size;

	nb_lcore_params = 0;

	p = strchr(p0, '(');
	while (p) {
		++p;
		p0 = strchr(p, ')');
		if (!p0)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		snprintf(s, sizeof(s), "%.*s", size, p);
		if (rte_strsplit(s, sizeof(s), str_fld,
			_NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] > 255)
				return -1;
		}
		if (nb_lcore_params >= MAX_LCORE_PARAMS) {
			printf("exceeded max number of lcore params: %hu\n",
				nb_lcore_params);
			return -1;
		}
		lcore_params_array[nb_lcore_params].port_id =
			(uint8_t)int_fld[FLD_PORT];
		lcore_params_array[nb_lcore_params].queue_id =
			(uint8_t)int_fld[FLD_QUEUE];
		lcore_params_array[nb_lcore_params].lcore_id =
			(uint8_t)int_fld[FLD_LCORE];
		++nb_lcore_params;
		p = strchr(p0, '(');
	}
	lcore_params = lcore_params_array;
	return 0;
}

#define MEMPOOL_CACHE_SIZE 256

static const char short_options[] =
	"p:"  /* portmask */
	"b:"  /* burst size */
	;

#define CMD_LINE_OPT_CONFIG "config"
#define CMD_LINE_OPT_PER_PORT_POOL "enable-per-port-pool"
enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options
	 */
	CMD_LINE_OPT_MIN_NUM = 256,
	CMD_LINE_OPT_CONFIG_NUM,
	CMD_LINE_OPT_PARSE_PER_PORT_POOL,
};

static const struct option lgopts[] = {
	{CMD_LINE_OPT_CONFIG, 1, 0, CMD_LINE_OPT_CONFIG_NUM},
	{CMD_LINE_OPT_PER_PORT_POOL, 0, 0, CMD_LINE_OPT_PARSE_PER_PORT_POOL},
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	unsigned int burst_size;

	argvopt = argv;

	/* Error or normal output strings. */
	while ((opt = getopt_long(argc, argvopt, short_options,
				lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			enabled_port_mask = parse_portmask(optarg);
			if (enabled_port_mask == 0) {
				fprintf(stderr, "Invalid portmask\n");
				return -1;
			}
			break;

		/* max_burst_size */
		case 'b':
			burst_size = (unsigned int)atoi(optarg);
			if (burst_size > max_pkt_burst) {
				printf("invalid burst size\n");
				return -1;
			}
			max_pkt_burst = burst_size;
			max_rx_burst = max_pkt_burst;
			max_tx_burst = max_rx_burst/2;
			break;

		/* long options */
		case CMD_LINE_OPT_CONFIG_NUM:
			ret = parse_config(optarg);
			if (ret) {
				fprintf(stderr, "Invalid config\n");
				return -1;
			}
			break;

		case CMD_LINE_OPT_PARSE_PER_PORT_POOL:
			printf("per port buffer pool is disabled\n");
			per_port_pool = 1;
			break;

		default:
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind - 1] = prgname;

	ret = optind - 1;
	optind = 1; /* reset getopt lib */
	return ret;
}

static void
print_ethaddr(const char *name, const struct rte_ether_addr *eth_addr)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];

	rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s%s", name, buf);
}

static int
init_mem(uint16_t portid, unsigned int nb_mbuf)
{
	int socketid;
	unsigned int lcore_id;
	char s[64];
	char *penv = getenv("PORT_FWD_PROC_SECONDARY_STANDALONE");

	if (penv)
		s_proc_2nd_standalone = atoi(penv);
	penv = getenv("PORT_FWD_PKTGEN_TOOL");
	if (penv)
		s_pktgen_tool = atoi(penv);

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;

		socketid = rte_lcore_to_socket_id(lcore_id);

		if (socketid >= NB_SOCKETS) {
			rte_exit(EXIT_FAILURE,
				"Socket %d of lcore %u is out of range %d\n",
				socketid, lcore_id, NB_SOCKETS);
		}

		if (s_pktgen_tool && (!pktmbuf_tx_pool)) {
			if (rte_eal_process_type() == RTE_PROC_SECONDARY &&
				s_proc_2nd_standalone) {
				snprintf(s, sizeof(s), "mbuf_tx_pool_2nd");
				pktmbuf_tx_pool =
					rte_pktmbuf_pool_create_by_ops(s,
						nb_mbuf,
						MEMPOOL_CACHE_SIZE, 0,
						RTE_MBUF_DEFAULT_BUF_SIZE,
						socketid,
						RTE_MBUF_DEFAULT_MEMPOOL_OPS);
			} else {
				snprintf(s, sizeof(s), "mbuf_tx_pool");
				pktmbuf_tx_pool =
					rte_pktmbuf_pool_create(s,
						nb_mbuf,
						MEMPOOL_CACHE_SIZE, 0,
						RTE_MBUF_DEFAULT_BUF_SIZE,
						socketid);
			}
		}

		if (pktmbuf_pool[portid][socketid] == NULL) {
			if (rte_eal_process_type() == RTE_PROC_SECONDARY &&
				s_proc_2nd_standalone) {
				snprintf(s, sizeof(s), "mbuf_pool_2nd_%d:%d",
					portid, socketid);
				pktmbuf_pool[portid][socketid] =
					rte_pktmbuf_pool_create_by_ops(s,
						nb_mbuf,
						MEMPOOL_CACHE_SIZE, 0,
						RTE_MBUF_DEFAULT_BUF_SIZE,
						socketid,
						RTE_MBUF_DEFAULT_MEMPOOL_OPS);
			} else {
				snprintf(s, sizeof(s), "mbuf_pool_%d:%d",
					portid, socketid);
				pktmbuf_pool[portid][socketid] =
					rte_pktmbuf_pool_create(s,
						nb_mbuf,
						MEMPOOL_CACHE_SIZE, 0,
						RTE_MBUF_DEFAULT_BUF_SIZE,
						socketid);
			}
			if (pktmbuf_pool[portid][socketid] == NULL)
				rte_exit(EXIT_FAILURE,
					"Cannot init mbuf pool on socket %d\n",
					socketid);
			else
				printf("mbuf pool[%d](count=%d) created on socket %d\n",
					portid, nb_mbuf, socketid);

		}
	}
	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						portid, rte_strerror(-ret));
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf(
					"Port%d Link Up. Speed %u Mbps -%s\n",
						portid, link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex"));
				else
					printf("Port %d Link Down\n", portid);
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

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

static int
parse_dst_port(uint16_t portid)
{
	if (port_fwd_demo.parse_fwd_dst(portid))
		return 1;

	return 0;
}

#define PKTGEN_STATISTICS_INTERVAL 5

#define G_BITS_SIZE ((double)(1000 * 1000 * 1000))

#include <unistd.h>

static void *perf_statistics(void *arg)
{
	unsigned int lcore_id, port_id;
	int port_num;
	struct lcore_conf *qconf;
	uint64_t rx_pkts[RTE_MAX_ETHPORTS];
	uint64_t tx_pkts[RTE_MAX_ETHPORTS];
	uint64_t rx_bytes_fcs[RTE_MAX_ETHPORTS];
	uint64_t tx_bytes_fcs[RTE_MAX_ETHPORTS];
	uint64_t rx_bytes_oh[RTE_MAX_ETHPORTS];
	uint64_t tx_bytes_oh[RTE_MAX_ETHPORTS];
	uint64_t rx_bytes_oh_old[RTE_MAX_ETHPORTS];
	uint64_t tx_bytes_oh_old[RTE_MAX_ETHPORTS];

	memset(rx_bytes_oh_old, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));
	memset(tx_bytes_oh_old, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));

loop:
	if (force_quit)
		return arg;

	sleep(PKTGEN_STATISTICS_INTERVAL);
	memset(rx_pkts, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));
	memset(tx_pkts, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));
	memset(rx_bytes_fcs, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));
	memset(tx_bytes_fcs, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));
	memset(rx_bytes_oh, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));
	memset(tx_bytes_oh, 0, RTE_MAX_ETHPORTS * sizeof(uint64_t));

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;
		qconf = &s_lcore_conf[lcore_id];
		port_num = enabled_port_num;
		for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
			if (enabled_port_mask & (1 << port_id) &&
				port_num > 0) {
				rx_pkts[port_id] +=
					qconf->rx_statistic[port_id].packets;
				tx_pkts[port_id] +=
					qconf->tx_statistic[port_id].packets;
				rx_bytes_fcs[port_id] +=
					qconf->rx_statistic[port_id].bytes_fcs;
				tx_bytes_fcs[port_id] +=
					qconf->tx_statistic[port_id].bytes_fcs;
				rx_bytes_oh[port_id] +=
					qconf->rx_statistic[port_id].bytes_overhead;
				tx_bytes_oh[port_id] +=
					qconf->tx_statistic[port_id].bytes_overhead;
				port_num--;
			}
		}
	}

	port_num = enabled_port_num;
	port_id = 0;
	while (port_num > 0) {
		if (enabled_port_mask & (1 << port_id)) {
			printf("PORT%d:\r\n", port_id);
			if (1) {
				struct rte_eth_stats stats;
				int get_st_ret =
					rte_eth_stats_get(port_id, &stats);

				if (!get_st_ret) {
					printf("Input: %ld bytes, %ld packets, %ld missed, %ld error\r\n",
						(unsigned long)stats.ibytes,
						(unsigned long)stats.ipackets,
						(unsigned long)stats.imissed,
						(unsigned long)stats.ierrors);
					printf("Output: %ld bytes, %ld packets, %ld error\r\n",
						(unsigned long)stats.obytes,
						(unsigned long)stats.opackets,
						(unsigned long)stats.oerrors);
				}
			}
			printf("TX: %lld pkts, %lld bits, %fGbps\r\n",
				(unsigned long long)tx_pkts[port_id],
				(unsigned long long)tx_bytes_fcs[port_id] * 8,
				(double)(tx_bytes_oh[port_id] -
				tx_bytes_oh_old[port_id]) * 8 /
				(PKTGEN_STATISTICS_INTERVAL * G_BITS_SIZE));
			printf("RX: %lld pkts, %lld bits, %fGbps\r\n\r\n",
				(unsigned long long)rx_pkts[port_id],
				(unsigned long long)rx_bytes_fcs[port_id] * 8,
				(double)(rx_bytes_oh[port_id] -
				rx_bytes_oh_old[port_id]) * 8 /
				(PKTGEN_STATISTICS_INTERVAL * G_BITS_SIZE));
			tx_bytes_oh_old[port_id] = tx_bytes_oh[port_id];
			rx_bytes_oh_old[port_id] = rx_bytes_oh[port_id];
			port_num--;
		}
		port_id++;
	}

	goto loop;

	return arg;
}

int
main(int argc, char **argv)
{
	struct lcore_conf *qconf;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	int ret;
	uint32_t nb_ports;
	uint16_t queueid, portid;
	uint32_t lcore_id;
	uint32_t nb_lcores;
	uint8_t queue, socketid;
	struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];
	uint16_t nb_rx_queue[RTE_MAX_ETHPORTS];
	uint16_t nb_tx_queue[RTE_MAX_ETHPORTS];
	uint32_t total_tx_queues = 0, total_rx_queues = 0;
	uint32_t nb_mbuf;
	struct rte_eth_conf local_port_conf[RTE_MAX_ETHPORTS];

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Port fwd invalid parameters\n");

	if (check_lcore_params() < 0)
		rte_exit(EXIT_FAILURE, "check_lcore_params failed\n");

	ret = init_lcore_rx_queues();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "init_lcore_rx_queues failed\n");

	nb_ports = rte_eth_dev_count_avail();

	if (check_port_config() < 0)
		rte_exit(EXIT_FAILURE, "check_port_config failed\n");

	nb_lcores = rte_lcore_count();

	RTE_ETH_FOREACH_DEV(portid) {
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;

		nb_rx_queue[portid] = get_port_n_rx_queues(portid);
		nb_tx_queue[portid] = nb_rx_queue[portid];
		total_rx_queues += nb_rx_queue[portid];
		total_tx_queues += nb_tx_queue[portid];
	}

	if (!per_port_pool) {
		nb_mbuf = nb_ports *
			(total_rx_queues * nb_rxd + total_tx_queues * nb_txd);
		nb_mbuf += nb_ports * nb_lcores * MAX_PKT_BURST +
			nb_lcores * MEMPOOL_CACHE_SIZE;
		nb_mbuf = RTE_MAX(nb_mbuf, (unsigned int)2048);
		ret = init_mem(0, nb_mbuf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"global mem pool(count=%d) init failed\n",
				nb_mbuf);
	} else {
		nb_mbuf = total_rx_queues * nb_rxd + total_tx_queues * nb_txd;
		nb_mbuf += nb_lcores * MAX_PKT_BURST +
			nb_lcores * MEMPOOL_CACHE_SIZE;
		nb_mbuf = RTE_MAX(nb_mbuf, (unsigned int)2048);
	}

	/* initialize all ports */
	RTE_ETH_FOREACH_DEV(portid) {
		memcpy(&local_port_conf[portid], &port_conf,
			sizeof(struct rte_eth_conf));

		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0) {
			printf("\nSkipping disabled port %d\n", portid);
			continue;
		}
		enabled_port_num++;

		/* init port */
		printf("Initializing port %d ... ", portid);
		fflush(stdout);

		printf("Creating queues: nb_rxq=%d nb_txq=%u... ",
			nb_rx_queue[portid], nb_tx_queue[portid]);

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				"Error during getting device (port %u) info: %s\n",
				portid, strerror(-ret));
		/* Enable Receive side SCATTER, if supported by NIC,
		 * when jumbo packet is enabled.
		 */
		if (local_port_conf[portid].rxmode.offloads &
				DEV_RX_OFFLOAD_JUMBO_FRAME)
			if (dev_info.rx_offload_capa & DEV_RX_OFFLOAD_SCATTER)
				local_port_conf[portid].rxmode.offloads |=
						DEV_RX_OFFLOAD_SCATTER;

		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf[portid].txmode.offloads |=
				DEV_TX_OFFLOAD_MBUF_FAST_FREE;

		local_port_conf[portid].rx_adv_conf.rss_conf.rss_hf &=
			dev_info.flow_type_rss_offloads;

		ret = rte_eth_dev_configure(portid, nb_rx_queue[portid],
				nb_tx_queue[portid], &local_port_conf[portid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"Cannot configure device: err=%d, port=%d\n",
				ret, portid);

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
				&nb_txd);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, "
				 "port=%d\n", ret, portid);

		ret = rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot get MAC address: err=%d, port=%d\n",
				 ret, portid);

		print_ethaddr(" Address:", &ports_eth_addr[portid]);
		printf("\r\n");

		/* init memory */
		if (per_port_pool) {
			ret = init_mem(portid, nb_mbuf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
					"init_mem (count=%d) failed for port=%d\n",
					nb_mbuf, portid);
		}
	}

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;
		qconf = &s_lcore_conf[lcore_id];
		printf("\nInitializing rxq/txq pairs on lcore %u ... ", lcore_id);
		fflush(stdout);
		/* init RX queues */
		for (queue = 0; queue < qconf->n_rx_queue; ++queue) {
			struct rte_eth_rxconf rxq_conf;

			portid = qconf->rx_queue_list[queue].port_id;
			queueid = qconf->rx_queue_list[queue].queue_id;

			socketid = (uint8_t)rte_lcore_to_socket_id(lcore_id);

			printf("rxq/txq=%d,%d,%d ", portid, queueid, socketid);
			fflush(stdout);

			ret = rte_eth_dev_info_get(portid, &dev_info);
			if (ret != 0)
				rte_exit(EXIT_FAILURE,
					"Error during getting device (port %u) info: %s\n",
					portid, strerror(-ret));

			rxq_conf = dev_info.default_rxconf;
			rxq_conf.offloads = port_conf.rxmode.offloads;
			if (!per_port_pool)
				ret = rte_eth_rx_queue_setup(portid, queueid,
						nb_rxd, socketid,
						&rxq_conf,
						pktmbuf_pool[0][socketid]);
			else
				ret = rte_eth_rx_queue_setup(portid, queueid,
						nb_rxd, socketid,
						&rxq_conf,
						pktmbuf_pool[portid][socketid]);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
				"rte_eth_rx_queue_setup: err=%d, port=%d\n",
				ret, portid);
			txconf = &dev_info.default_txconf;
			txconf->offloads = local_port_conf[portid].txmode.offloads;
			ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
						     socketid, txconf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
					"rte_eth_tx_queue_setup: err=%d, "
					"port=%d, queue=%d\n",
					ret, portid, queueid);
			qconf = &s_lcore_conf[lcore_id];
			qconf->tx_queue_id[portid] = queueid;

			qconf->tx_port_id[qconf->n_tx_port] = portid;
			qconf->n_tx_port++;
		}
	}

	printf("\n");

	/* start ports */
	RTE_ETH_FOREACH_DEV(portid) {
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"rte_eth_dev_start: err=%d, port=%d\n",
				ret, portid);

		ret = rte_eth_promiscuous_enable(portid);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				"rte_eth_promiscuous_enable: err=%s, port=%u\n",
				rte_strerror(-ret), portid);
	}

	printf("\n");

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;
		qconf = &s_lcore_conf[lcore_id];
		for (queue = 0; queue < qconf->n_rx_queue; ++queue) {
			portid = qconf->rx_queue_list[queue].port_id;
			if (parse_dst_port(portid) == 0)
				rte_exit(EXIT_FAILURE,
					"parse dst port failed\n");
		}
	}

	check_all_ports_link_status(enabled_port_mask);

	ret = 0;

	if (getenv("PORT_FWD_PERF_STATISTICS")) {
		pthread_t pid;

		pthread_create(&pid, NULL, perf_statistics, NULL);
	}

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(port_fwd_demo.main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	/* stop ports */
	RTE_ETH_FOREACH_DEV(portid) {
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}
