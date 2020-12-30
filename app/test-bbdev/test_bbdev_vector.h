/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017 Intel Corporation
 * Copyright 2020-2021 NXP
 */

#ifndef TEST_BBDEV_VECTOR_H_
#define TEST_BBDEV_VECTOR_H_

#include <rte_pmd_bbdev_la12xx.h>
#include <rte_bbdev_op.h>

/* Flags which are set when specific parameter is define in vector file */
enum {
	TEST_BBDEV_VF_E = (1ULL << 0),
	TEST_BBDEV_VF_EA = (1ULL << 1),
	TEST_BBDEV_VF_EB = (1ULL << 2),
	TEST_BBDEV_VF_K = (1ULL << 3),
	TEST_BBDEV_VF_K_NEG = (1ULL << 4),
	TEST_BBDEV_VF_K_POS = (1ULL << 5),
	TEST_BBDEV_VF_C_NEG = (1ULL << 6),
	TEST_BBDEV_VF_C = (1ULL << 7),
	TEST_BBDEV_VF_CAB = (1ULL << 8),
	TEST_BBDEV_VF_RV_INDEX = (1ULL << 9),
	TEST_BBDEV_VF_ITER_MAX = (1ULL << 10),
	TEST_BBDEV_VF_ITER_MIN = (1ULL << 11),
	TEST_BBDEV_VF_EXPECTED_ITER_COUNT = (1ULL << 12),
	TEST_BBDEV_VF_EXT_SCALE = (1ULL << 13),
	TEST_BBDEV_VF_NUM_MAPS = (1ULL << 14),
	TEST_BBDEV_VF_NCB = (1ULL << 15),
	TEST_BBDEV_VF_NCB_NEG = (1ULL << 16),
	TEST_BBDEV_VF_NCB_POS = (1ULL << 17),
	TEST_BBDEV_VF_R = (1ULL << 18),
	TEST_BBDEV_VF_BG = (1ULL << 19),
	TEST_BBDEV_VF_ZC = (1ULL << 20),
	TEST_BBDEV_VF_F = (1ULL << 21),
	TEST_BBDEV_VF_QM = (1ULL << 22),
	TEST_BBDEV_VF_CODE_BLOCK_MODE = (1ULL << 23),
	TEST_BBDEV_VF_OP_FLAGS = (1ULL << 24),
	TEST_BBDEV_VF_EXPECTED_STATUS = (1ULL << 25),
	TEST_BBDEV_VF_NETWORK_ORDER = (1ULL << 26),
	TEST_BBDEV_VF_EN_SCRAMBLE = (1ULL << 27),
	TEST_BBDEV_VF_Q = (1ULL << 28),
	TEST_BBDEV_VF_N_ID = (1ULL << 29),
	TEST_BBDEV_VF_N_RNTI = (1ULL << 30),
	TEST_BBDEV_VF_SD_CD_DEMUX = (1ULL << 31),
	TEST_BBDEV_VF_SD_LLRS_PER_RE = (1ULL << 32),
	TEST_BBDEV_VF_SD_CD_DEMUX_PARAMS = (1ULL << 33),
	TEST_BBDEV_VF_SE_CE_MUX = (1ULL << 34),
	TEST_BBDEV_VF_SE_BITS_PER_RE = (1ULL << 35),
	TEST_BBDEV_VF_SE_CE_MUX_PARAMS = (1ULL << 36),
	TEST_BBDEV_VF_SD_NON_COMPACT_HARQ = (1ULL << 37),
	TEST_BBDEV_VF_SD_COMPACT_HARQ_CB_MASK = (1ULL << 38),
};

enum op_data_type {
	DATA_INPUT = 0,
	DATA_SOFT_OUTPUT,
	DATA_HARD_OUTPUT,
	DATA_INTERM_OUTPUT,
	DATA_HARQ_INPUT,
	DATA_HARQ_OUTPUT,
	DATA_NUM_TYPES,
};

struct op_data_buf {
	uint32_t *addr;
	uint32_t length;
};

struct op_data_entries {
	struct op_data_buf segments[RTE_BBDEV_TURBO_MAX_CODE_BLOCKS];
	unsigned int nb_segments;
};

struct test_bbdev_vector {
	enum rte_bbdev_op_type op_type;
	int expected_status;
	uint64_t mask;
	int network_order;
	union {
		struct rte_bbdev_op_turbo_dec turbo_dec;
		struct rte_bbdev_op_turbo_enc turbo_enc;
		struct rte_bbdev_op_ldpc_dec ldpc_dec;
		struct rte_bbdev_op_ldpc_enc ldpc_enc;
		struct rte_pmd_la12xx_op la12xx_op;
	};
	uint16_t core_mask;
	/* Additional storage for op data entries */
	struct op_data_entries entries[DATA_NUM_TYPES];
};

/* fills test vector parameters based on test file */
int
test_bbdev_vector_read(const char *filename,
		struct test_bbdev_vector *vector);


#endif /* TEST_BBDEV_VECTOR_H_ */
