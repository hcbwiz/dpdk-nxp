/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019-2022 NXP
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
#include <rte_string_fns.h>
#include <rte_spinlock.h>
#include <rte_malloc.h>

#include <cmdline_parse.h>
#include <cmdline_parse_etheraddr.h>

#include "l1_l2_comm.h"

#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024

/* Static global variables used within this file. */
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

enum l1_l2_proc_type {
	proc_primary = 0,
	proc_standalone_secondary = 1,
};
static uint8_t s_proc_type = proc_primary;

static uint8_t s_layer = 1;

static uint8_t s_core = 1;

static uint16_t s_port_id;

static char s_port_name[64];

static uint8_t s_l2_downlink_print;

static uint8_t s_l1_uplink_print;

/* Global variables. */

static bool force_quit;

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IP,
		},
	},
};

static struct rte_mempool *l1_l2_mpool;

struct data_loop_conf s_data_loop_conf;

#define L1_TB_SIZE 17500

#define L2_SDU_SIZE 1500

#define L1_L2_MAX_CHAIN_NB (L1_TB_SIZE / L2_SDU_SIZE + 1)

static int
l2_app_uplink_data_prepare(struct rte_mbuf **mbufs, int count)
{
	int i;
	uint32_t remain_len = L1_TB_SIZE, nb_segs;

	if (remain_len % L2_SDU_SIZE)
		nb_segs = remain_len / L2_SDU_SIZE + 1;
	else
		nb_segs = remain_len / L2_SDU_SIZE;

	for (i = 0; i < count; i++) {
		mbufs[i]->data_off = RTE_PKTMBUF_HEADROOM;
		mbufs[i]->pkt_len = remain_len;
		mbufs[i]->nb_segs = nb_segs;
		nb_segs--;
		if (nb_segs) {
			mbufs[i]->data_len = L2_SDU_SIZE;
			if ((i + 1) >= count)
				return -ENOMEM;
			mbufs[i]->next = mbufs[i + 1];
		} else {
			mbufs[i]->data_len = remain_len;
			mbufs[i]->next = NULL;
			break;
		}
		remain_len -= L2_SDU_SIZE;
	}

	return (i + 1);
}

static uint16_t
l2_app_uplink_data_ready(struct rte_mbuf *mbufs)
{
	return rte_eth_tx_burst(s_port_id, 0, &mbufs, 1);
}

static void
l2_app_uplink_prepare(void)
{
	struct rte_mbuf *mbufs[L1_L2_MAX_CHAIN_NB];
	int ret, chain_count;
	uint32_t tx_pkts, tx_bytes;

	ret = rte_pktmbuf_alloc_bulk(l1_l2_mpool,
		mbufs, L1_L2_MAX_CHAIN_NB);
	if (ret)
		return;

	chain_count = l2_app_uplink_data_prepare(mbufs,
		L1_L2_MAX_CHAIN_NB);
	if (unlikely(chain_count <= 0)) {
		rte_pktmbuf_free_bulk(mbufs, L1_L2_MAX_CHAIN_NB);
		return;
	}
	if (chain_count < L1_L2_MAX_CHAIN_NB) {
		rte_pktmbuf_free_bulk(&mbufs[chain_count],
			L1_L2_MAX_CHAIN_NB - chain_count);
	}

	tx_pkts = mbufs[0]->nb_segs;
	tx_bytes = mbufs[0]->pkt_len;
	ret = l2_app_uplink_data_ready(mbufs[0]);
	if (unlikely(ret != 1)) {
		rte_pktmbuf_free_bulk(&mbufs[0], chain_count);
	} else {
		s_data_loop_conf.tx_statistic.packets += tx_pkts;
		s_data_loop_conf.tx_statistic.bytes += tx_bytes;
	}
}

static uint16_t
l2_app_downlink_recv(struct rte_mbuf **mbufs,
	int wait)
{
	uint16_t ret;

recv_again:
	ret = rte_eth_rx_burst(s_port_id, 0, mbufs, 1);
	if (ret)
		return ret;

	if (wait)
		goto recv_again;

	return 0;
}

static void
l2_app_downlink_data_process(struct rte_mbuf *mbuf)
{
	struct rte_mbuf *next_mbuf = NULL;
	uint32_t seg_idx = 0, total_len = 0, pkt_len, nb_segs;

	pkt_len = mbuf->pkt_len;
	nb_segs = mbuf->nb_segs;
	if (s_l2_downlink_print) {
		printf("l2_rx_from_l1 total len:%d, segs:%d\r\n",
			pkt_len, nb_segs);
	}
	while (1) {
		if (s_l2_downlink_print) {
			printf("l2_rx_from_l1 len[%d]:%d\r\n",
				seg_idx, mbuf->data_len);
		}
		total_len += mbuf->data_len;
		if (mbuf->next) {
			next_mbuf = mbuf->next;
			mbuf->next = NULL;
			mbuf->nb_segs = 0;
			rte_pktmbuf_free(mbuf);
			mbuf = next_mbuf;
			seg_idx++;
		} else {
			mbuf->nb_segs = 0;
			rte_pktmbuf_free(mbuf);
			break;
		}
	}

	if (total_len != pkt_len) {
		fprintf(stderr,
			"TB len from L1 total_len(%d) != pkt_len(%d)\r\n",
			total_len, pkt_len);
	}

	if (nb_segs != (seg_idx + 1)) {
		fprintf(stderr,
			"TB chain number from L1 nb_segs(%d) != seg_idx++(%d)\r\n",
			nb_segs, seg_idx + 1);
	}
	s_data_loop_conf.rx_statistic.packets += nb_segs;
	s_data_loop_conf.rx_statistic.bytes += total_len;
}

static void
l2_app_downlink_handle(void)
{
	struct rte_mbuf *mbufs = NULL;
	uint16_t recv_nb;

	/**Receive from Uplink*/
	recv_nb = l2_app_downlink_recv(&mbufs, 0);
	if (!recv_nb)
		return;

	l2_app_downlink_data_process(mbufs);
}

static void l2_app_handle(void)
{
	while (!force_quit) {
		l2_app_uplink_prepare();
		l2_app_downlink_handle();
	}
}

static uint16_t
l1_app_uplink_recv(struct rte_mbuf **mbufs)
{
	uint16_t ret;

	ret = rte_eth_rx_burst(s_port_id, 0, mbufs, 1);

	return ret;
}

static void
l1_app_uplink_data_process(struct rte_mbuf *mbuf)
{
	uint32_t pkt_len, data_len, nb_segs;

	pkt_len = mbuf->pkt_len;
	data_len = mbuf->data_len;
	nb_segs = mbuf->nb_segs;
	if (s_l1_uplink_print) {
		printf("l1 rx from l2 total len:%d(%d), segs:%d\r\n",
			pkt_len, data_len, nb_segs);
	}

	if (pkt_len != data_len) {
		fprintf(stderr,
			"TB is not continue pkt_len(%d) != data_len(%d)\r\n",
			pkt_len, data_len);
	}
	s_data_loop_conf.rx_statistic.packets++;
	s_data_loop_conf.rx_statistic.bytes += pkt_len;
}

static void
l1_app_uplink_handle(void)
{
	struct rte_mbuf *mbufs = NULL;
	uint16_t recv_nb;

	/**Receive from Downlink*/
	recv_nb = l1_app_uplink_recv(&mbufs);
	if (!recv_nb)
		return;

	l1_app_uplink_data_process(mbufs);
}

static int
l1_app_downlink_data_prepare(struct rte_mbuf *mbuf)
{
	mbuf->pkt_len = L1_TB_SIZE;
	mbuf->data_len = L1_TB_SIZE;
	mbuf->data_off = RTE_PKTMBUF_HEADROOM;
	/**Process data*/

	return 0;
}

static uint16_t
l1_app_downlink_send(struct rte_mbuf *mbufs)
{
	return rte_eth_tx_burst(s_port_id, 0, &mbufs, 1);
}

static void
l1_app_downlink_process(void)
{
	struct rte_mbuf *mbuf;
	int ret;
	uint16_t sent;
	uint32_t tx_pkts, tx_bytes;

	mbuf = rte_pktmbuf_alloc(l1_l2_mpool);
	if (!mbuf)
		return;

	if (mbuf->buf_len < (L1_TB_SIZE + RTE_PKTMBUF_HEADROOM)) {
		fprintf(stderr,
			"Buf(len = %d) < TB(len = %d) + header room(%d)\r\n",
			mbuf->buf_len, L1_TB_SIZE, RTE_PKTMBUF_HEADROOM);
		rte_pktmbuf_free(mbuf);
		return;
	}

	ret = l1_app_downlink_data_prepare(mbuf);
	if (ret) {
		rte_pktmbuf_free(mbuf);
		return;
	}

	tx_pkts = 1;
	tx_bytes = mbuf->pkt_len;
	sent = l1_app_downlink_send(mbuf);
	if (sent != 1) {
		rte_pktmbuf_free(mbuf);

		return;
	}

	s_data_loop_conf.tx_statistic.packets += tx_pkts;
	s_data_loop_conf.tx_statistic.bytes += tx_bytes;
}


static void l1_app_handle(void)
{
	while (!force_quit) {
		l1_app_uplink_handle();
		l1_app_downlink_process();
	}
}

static int
main_loop(__attribute__((unused)) void *dummy)
{
	if (s_layer == 1)
		l1_app_handle();
	else if (s_layer == 2)
		l2_app_handle();

	return 0;
}

static int
parse_port_name(const char *sname)
{
	strcpy(s_port_name, sname);

	return 0;
}

static int
parse_core(const char *score)
{
	s_core = *score;
	if (s_core > RTE_MAX_LCORE)
		return -EINVAL;

	return 0;
}

static int
parse_layer(const char *slayer)
{

	if (*slayer == 1)
		s_layer = 1;
	else if (*slayer == 2)
		s_layer = 2;
	else
		return -EINVAL;

	return 0;
}

#define MEMPOOL_CACHE_SIZE 256

#define CMD_LINE_OPT_PNAME "port_nm"
#define CMD_LINE_OPT_CORE "core"
#define CMD_LINE_OPT_LAYER "layer"
enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options
	 */
	CMD_LINE_OPT_MIN_NUM = 256,
	CMD_LINE_OPT_PNAME_NUM,
	CMD_LINE_OPT_CORE_NUM,
	CMD_LINE_OPT_LAYER_NUM,
};

static const struct option lgopts[] = {
	{CMD_LINE_OPT_PNAME, 1, 0, CMD_LINE_OPT_PNAME_NUM},
	{CMD_LINE_OPT_CORE, 1, 0, CMD_LINE_OPT_CORE_NUM},
	{CMD_LINE_OPT_LAYER, 1, 0, CMD_LINE_OPT_LAYER_NUM},
	{NULL, 0, 0, 0}
};

const char sopts[] = "h";

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;

	/* Error or normal output strings. */
	while ((opt = getopt_long(argc, argvopt, sopts,
				lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* long options */
		case CMD_LINE_OPT_PNAME_NUM:
			ret = parse_port_name(optarg);
			if (ret) {
				fprintf(stderr, "Invalid name\n");
				return ret;
			}
			break;

		case CMD_LINE_OPT_CORE_NUM:
			ret = parse_core(optarg);
			if (ret) {
				fprintf(stderr, "Invalid core\n");
				return ret;
			}
			break;

		case CMD_LINE_OPT_LAYER_NUM:
			ret = parse_layer(optarg);
			if (ret) {
				fprintf(stderr, "Invalid layer\n");
				return ret;
			}
			break;

		default:
			return -ENOTSUP;
		}
	}

	if (optind >= 0)
		argv[optind - 1] = prgname;

	ret = optind - 1;
	optind = 1; /* reset getopt lib */
	return ret;
}

static int
init_l1_l2_mem(unsigned int nb_mbuf, uint32_t buf_size)
{
	char s[64];

	if (s_layer == 1)
		sprintf(s, "layer1_mpool");
	else if (s_layer == 1)
		sprintf(s, "layer2_mpool");
	else
		return -EINVAL;

	if (s_proc_type == proc_standalone_secondary) {
		l1_l2_mpool = rte_pktmbuf_pool_create_by_ops(s,
			nb_mbuf,
			MEMPOOL_CACHE_SIZE, 0,
			buf_size, 0,
			RTE_MBUF_DEFAULT_MEMPOOL_OPS);
	} else {
		l1_l2_mpool = rte_pktmbuf_pool_create(s,
			nb_mbuf,
			MEMPOOL_CACHE_SIZE, 0,
			buf_size, 0);
	}
	if (!l1_l2_mpool)
		rte_exit(EXIT_FAILURE, "Cannot init l1/l2 mpool(%s)\n", s);
	else
		printf("l1/l2 mpool(count=%d) created\n", nb_mbuf);

	return 0;
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

#define PERF_STATISTICS_INTERVAL 5

#define G_BITS_SIZE ((double)(1000 * 1000 * 1000))

#include <unistd.h>

static inline void
port_fwd_dump_port_status(struct rte_eth_stats *stats)
{
	printf("Input: %ld bytes, %ld packets, %ld missed, %ld error\n",
		(unsigned long)stats->ibytes,
		(unsigned long)stats->ipackets,
		(unsigned long)stats->imissed,
		(unsigned long)stats->ierrors);
	printf("Output: %ld bytes, %ld packets, %ld error\n",
		(unsigned long)stats->obytes,
		(unsigned long)stats->opackets,
		(unsigned long)stats->oerrors);
}

static void *perf_statistics(void *arg)
{
	cpu_set_t cpuset;
	int ret;
	struct perf_statistic *rxs, *txs;
	uint64_t rx_pkts = 0;
	uint64_t tx_pkts = 0;
	uint64_t rx_bytes = 0;
	uint64_t tx_bytes = 0;
	uint64_t rx_bytes_old = 0;
	uint64_t tx_bytes_old = 0;
	struct rte_eth_stats stats;

	CPU_SET(0, &cpuset);
	ret = pthread_setaffinity_np(pthread_self(),
			sizeof(cpu_set_t), &cpuset);
	printf("affinity statistics thread to cpu 0 %s\r\n",
		ret ? "failed" : "success");

loop:
	if (force_quit)
		return arg;

	sleep(PERF_STATISTICS_INTERVAL);

	rxs = &s_data_loop_conf.rx_statistic;
	txs = &s_data_loop_conf.tx_statistic;
	rx_pkts += rxs->packets;
	tx_pkts += txs->packets;
	rx_bytes += rxs->bytes;
	tx_bytes += txs->bytes;

	ret = rte_eth_stats_get(s_port_id, &stats);
	if (ret)
		goto skip_print_hw_status;

	port_fwd_dump_port_status(&stats);

skip_print_hw_status:
	printf("TX: %lld pkts, %lld bits, %fGbps\r\n",
		(unsigned long long)tx_pkts,
		(unsigned long long)tx_bytes * 8,
		(double)(tx_bytes - tx_bytes_old) * 8 /
		(PERF_STATISTICS_INTERVAL * G_BITS_SIZE));
	printf("RX: %lld pkts, %lld bits, %fGbps\r\n",
		(unsigned long long)rx_pkts,
		(unsigned long long)rx_bytes * 8,
		(double)(rx_bytes - rx_bytes_old) * 8 /
		(PERF_STATISTICS_INTERVAL * G_BITS_SIZE));
	tx_bytes_old = tx_bytes;
	rx_bytes_old = rx_bytes;

	goto loop;

	return arg;
}

int
main(int argc, char **argv)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	int ret, port_found = 0;
	uint16_t portid;
	uint32_t nb_buf;
	struct rte_eth_conf local_port_conf;
	struct rte_eth_rxconf rxq_conf;
	uint32_t data_room_size;
	struct rte_eth_link link;

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
		rte_exit(EXIT_FAILURE, "l1_l2_comm invalid parameters\n");

	if (rte_eal_process_type() == RTE_PROC_SECONDARY)
		s_proc_type = proc_standalone_secondary;

	port_conf.rxmode.max_lro_pkt_size = RTE_MBUF_DEFAULT_DATAROOM;

	nb_buf = 4096;
	if (s_layer == 1) {
		data_room_size = (L1_TB_SIZE + RTE_PKTMBUF_HEADROOM +
			MEMPOOL_CACHE_SIZE) /
			MEMPOOL_CACHE_SIZE * MEMPOOL_CACHE_SIZE;
	} else {
		data_room_size = (L2_SDU_SIZE + RTE_PKTMBUF_HEADROOM);
	}
	ret = init_l1_l2_mem(nb_buf, data_room_size);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE,
			"l1/l2 mem pool(count = %d) init failed\n",
			nb_buf);
	}

	/* initialize all ports */
	RTE_ETH_FOREACH_DEV(portid) {
		/* init port */
		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret) {
			fprintf(stderr, "Failed to get info of port%d\n",
				portid);
			continue;
		}
		if (strcmp(s_port_name, dev_info.device->name))
			continue;
		/* Enable Receive side SCATTER, if supported by NIC,
		 * when jumbo packet is enabled.
		 */
		if (dev_info.rx_offload_capa & DEV_RX_OFFLOAD_SCATTER) {
			local_port_conf.rxmode.offloads |=
				DEV_RX_OFFLOAD_SCATTER;
		}

		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE) {
			local_port_conf.txmode.offloads |=
				DEV_TX_OFFLOAD_MBUF_FAST_FREE;
		}

		local_port_conf.rx_adv_conf.rss_conf.rss_hf &=
			dev_info.flow_type_rss_offloads;

		ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
				"Cannot configure device: err=%d, port=%d\n",
				ret, portid);
		}

		if (!rte_lcore_is_enabled(s_core)) {
			rte_exit(EXIT_FAILURE,
				"Core %d is not enabled\n",
				s_core);
		}

		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd, 0, &rxq_conf,
			l1_l2_mpool);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
				"port%d rxq%d setup failed(%d)\n",
				portid, 0, ret);
		}
		txconf = &dev_info.default_txconf;
		txconf->offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd, 0, txconf);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
				"port%d txq%d setup failed(%d)\n",
				portid, 0, ret);
		}

		ret = rte_eth_dev_start(portid);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
				"rte_eth_dev_start: err=%d, port=%d\n",
				ret, portid);
		}

		ret = rte_eth_promiscuous_enable(portid);
		if (ret != 0) {
			rte_exit(EXIT_FAILURE,
				"rte_eth_promiscuous_enable: err=%s, port=%u\n",
				rte_strerror(-ret), portid);
		}

		ret = rte_eth_link_get_nowait(portid, &link);
		if (ret < 0) {
			printf("Port %u link get failed: %s\n",
				portid, rte_strerror(-ret));
		} else {
			if (link.link_status) {
				printf("Port%d Link Up. Speed %u Mbps -%s\n",
					portid, link.link_speed,
					(link.link_duplex ==
					ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex"));
			} else {
				printf("Port %d Link Down\n", portid);
			}
		}
		port_found = 1;
		break;
	}

	if (!port_found) {
		printf("No port found\n");
		printf("Bye...\n");
		return 0;
	}

	s_port_id = portid;
	ret = 0;

	if (getenv("L1_L2_PERF_STATISTICS")) {
		pthread_t pid;

		pthread_create(&pid, NULL, perf_statistics, NULL);
	}

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MAIN);

	printf("Closing port %d...", portid);
	rte_eth_dev_stop(portid);
	rte_eth_dev_close(portid);
	printf(" Done\n");

	printf("Bye...\n");

	return ret;
}
