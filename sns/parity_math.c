/*
 * Copyright (c) 2013-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#include "lib/arith.h"
#include "lib/assert.h"
#include "lib/errno.h"
#include "lib/memory.h"
#include "lib/misc.h" /* SET0() */
#include "lib/types.h"

#include "sns/parity_ops.h"
#include "sns/parity_math.h"

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_SNS
#include "lib/trace.h"

#define ir_invalid_col_t UINT8_MAX

/* Forward declarations */
static void xor_calculate(struct m0_parity_math *math,
                          const struct m0_buf *data,
                          struct m0_buf *parity);

#ifndef __KERNEL__
static void isal_encode(struct m0_parity_math *math,
                        const struct m0_buf *data,
                        struct m0_buf *parity);
#else
static void reed_solomon_encode(struct m0_parity_math *math,
                                const struct m0_buf *data,
                                struct m0_buf *parity);
#endif /* __KERNEL__ */

static void xor_diff(struct m0_parity_math *math,
		     struct m0_buf         *old,
		     struct m0_buf         *new,
		     struct m0_buf         *parity,
		     uint32_t               index);

static void reed_solomon_diff(struct m0_parity_math *math,
		              struct m0_buf         *old,
		              struct m0_buf         *new,
		              struct m0_buf         *parity,
		              uint32_t               index);

static void xor_recover(struct m0_parity_math *math,
                        struct m0_buf *data,
                        struct m0_buf *parity,
                        struct m0_buf *fails,
			enum m0_parity_linsys_algo algo);

#ifndef __KERNEL__
static void isal_recover(struct m0_parity_math *math,
			 struct m0_buf *data,
			 struct m0_buf *parity,
			 struct m0_buf *fails,
			 enum m0_parity_linsys_algo algo);

static int isal_gen_recov_coeff_tbl(uint32_t data_count, uint32_t parity_count,
				    struct m0_buf *failed_idx_buf,
				    struct m0_buf *alive_idx_buf,
				    uint8_t *encode_matrix, uint8_t *g_tbls);
#else
static void reed_solomon_recover(struct m0_parity_math *math,
                                 struct m0_buf *data,
                                 struct m0_buf *parity,
                                 struct m0_buf *fails,
				 enum m0_parity_linsys_algo algo);
#endif /* __KERNEL__ */

static void fail_idx_xor_recover(struct m0_parity_math *math,
				 struct m0_buf *data,
				 struct m0_buf *parity,
				 const uint32_t failure_index);

static void fail_idx_reed_solomon_recover(struct m0_parity_math *math,
					  struct m0_buf *data,
					  struct m0_buf *parity,
					  const uint32_t failure_index);
/**
 * Inverts the encoding matrix and generates a recovery matrix for lost data.
 * When all failed blocks are parity blocks this function plays no role.
 */
static int data_recov_mat_construct(struct m0_sns_ir *ir);

/**
 * Constructs out_mat by selecting a subset of rows from in_mat.
 * There is one-to-one mapping between blocks and rows of the in_mat.
 * A particular row is selected if its corresponding block has
 * sib_status == status.
 */
static void submatrix_construct(struct m0_matrix *in_mat,
				struct m0_sns_ir_block *blocks,
				enum m0_sns_ir_block_status status,
				struct m0_matrix *out_mat);
/**
 * Recovery of each failed block depends upon subset of alive blocks.
 * This routine prepares a bitmap indicating this dependency. If a bit at
 *  location 'x' is set 'true' then it implies that f_block has no dependency
 *  on block with index 'x'.
 */
static void dependency_bitmap_prepare(struct m0_sns_ir_block *f_block,
				      struct m0_sns_ir *ir);

static inline uint32_t recov_mat_col(const struct m0_sns_ir_block *alive_block,
				     const struct m0_sns_ir_block *failed_block,
				     const struct m0_sns_ir *ir);
/**
 * Updates the dependency-bitmap for a failed block once contribution
 * by available block is computed.
 */
static void dependency_bitmap_update(struct m0_sns_ir_block *f_block,
				     const struct m0_bitmap *bitmap);

/**
 * Core routine to recover failed_block based on available_block.
 */
static void incr_recover(struct m0_sns_ir_block *failed_block,
			 const struct m0_sns_ir_block *available_block,
			 struct m0_sns_ir *ir);

/**
 * Constant times x plus y, over galois field. Naming convention for this
 * function is borrowed from BLAS routines.
 */
static void gfaxpy(struct m0_bufvec *y, struct m0_bufvec *x,
		   m0_parity_elem_t alpha);

/**
 * Adds correction for a remote block that is received before all blocks
 * local to a node are transformed. This correction is needed only when
 * set of failed blocks involve both data and parity blocks.
 */
static void forward_rectification(struct m0_sns_ir *ir,
				  struct m0_bufvec *in_bufvec,
				  uint32_t failed_index);
/**
 * When a set of failed blocks contain both data and parity blocks, this
 * function transforms partially recovered data block, local to a node
 * for computing failed parity.
 */
static void failed_data_blocks_xform(struct m0_sns_ir *ir);
static inline bool is_valid_block_idx(const  struct m0_sns_ir *ir,
				      uint32_t block_idx);

static bool is_data(const struct m0_sns_ir *ir, uint32_t index);


static bool is_usable(const struct m0_sns_ir *ir,
		      const struct m0_bitmap *in_bmap,
		      struct m0_sns_ir_block *failed_block);

static uint32_t last_usable_block_id(const struct m0_sns_ir *ir,
				     uint32_t block_idx);
static inline  bool are_failures_mixed(const struct m0_sns_ir *ir);

static inline const struct m0_matrix* recovery_mat_get(const struct m0_sns_ir
						       *ir,
						       uint32_t failed_idx);

static inline uint32_t block_count(const struct m0_sns_ir *ir);

static void (*calculate[M0_PARITY_CAL_ALGO_NR])(struct m0_parity_math *math,
						const struct m0_buf *data,
						struct m0_buf *parity) = {
	[M0_PARITY_CAL_ALGO_XOR] = xor_calculate,
#ifndef __KERNEL__
	[M0_PARITY_CAL_ALGO_ISA] = isal_encode,
#else
	[M0_PARITY_CAL_ALGO_REED_SOLOMON] = reed_solomon_encode,
#endif /* __KERNEL__ */
};

static void (*diff[M0_PARITY_CAL_ALGO_NR])(struct m0_parity_math *math,
					   struct m0_buf         *old,
					   struct m0_buf         *new,
					   struct m0_buf         *parity,
					   uint32_t               index) = {
	[M0_PARITY_CAL_ALGO_XOR]          = xor_diff,
	[M0_PARITY_CAL_ALGO_REED_SOLOMON] = reed_solomon_diff,
};

static void (*recover[M0_PARITY_CAL_ALGO_NR])(struct m0_parity_math *math,
					      struct m0_buf *data,
					      struct m0_buf *parity,
					      struct m0_buf *fails,
					      enum m0_parity_linsys_algo algo) = {
	[M0_PARITY_CAL_ALGO_XOR] = xor_recover,
#ifndef __KERNEL__
	[M0_PARITY_CAL_ALGO_ISA] = isal_recover,
#else
	[M0_PARITY_CAL_ALGO_REED_SOLOMON] = reed_solomon_recover,
#endif /* __KERNEL__ */
};

static void (*fidx_recover[M0_PARITY_CAL_ALGO_NR])(struct m0_parity_math *math,
						   struct m0_buf *data,
						   struct m0_buf *parity,
						   const uint32_t fidx) = {
	[M0_PARITY_CAL_ALGO_XOR] = fail_idx_xor_recover,
	[M0_PARITY_CAL_ALGO_REED_SOLOMON] = fail_idx_reed_solomon_recover,
};

enum {
	SNS_PARITY_MATH_DATA_BLOCKS_MAX = 1 << (M0_PARITY_W - 1),
	BAD_FAIL_INDEX = -1
};

/* m0_parity_* are to much eclectic. just more simple names. */
static int gadd(int x, int y)
{
	return m0_parity_add(x, y);
}

#ifdef __KERNEL__
static int gsub(int x, int y)
{
	return m0_parity_sub(x, y);
}
#endif /* __KERNEL__ */

static int gmul(int x, int y)
{
	return m0_parity_mul(x, y);
}

#ifdef __KERNEL__
static int gdiv(int x, int y)
{
	return m0_parity_div(x, y);
}

static int gpow(int x, int p)
{
	return m0_parity_pow(x, p);
}

/* Fills vandermonde matrix with initial values. */
static int vandmat_init(struct m0_matrix *m, uint32_t data_count,
			uint32_t parity_count)
{
	int ret;
	uint32_t y;
	uint32_t x;
	uint32_t mat_height = data_count + parity_count;
	uint32_t mat_width = data_count;

	ret = m0_matrix_init(m, mat_width, mat_height);
	if (ret < 0)
		return ret;

	for (y = 0; y < mat_height; ++y)
		for (x = 0; x < mat_width; ++x)
			*m0_matrix_elem_get(m, x, y) = gpow(y, x);

	return ret;
}

static void vandmat_fini(struct m0_matrix *mat)
{
	m0_matrix_fini(mat);
}

/* Checks if row has only one element equals 1, and 0 others */
static bool check_row_is_id(struct m0_matrix *m, uint32_t row)
{
	bool ret = true;
	uint32_t x;

	for (x = 0; x < m->m_width && ret; ++x)
		ret &= (row == x) == *m0_matrix_elem_get(m, x, row);

	return ret;
}

/* Normalises vandermonde matrix, upper part of which becomes identity matrix
 * in case of success. */
static int vandmat_norm(struct m0_matrix *m)
{
	uint32_t y;

	for (y = 0; y < m->m_width; ++y) {
		uint32_t x = 0;
		m0_matrix_col_operate(m, y, *m0_matrix_elem_get(m, y, y), gdiv);

		for (x = 0; x < m->m_width; ++x)
			if (x != y)
				m0_matrix_cols_operate(m, x, y, gsub, 0, gmul,
                                            *m0_matrix_elem_get(m, x, y), gsub);

		/* Assert if units are not configured properly. */
		M0_ASSERT(check_row_is_id(m, y));
	}

	return 0;
}
#endif /* __KERNEL__ */

M0_INTERNAL void m0_parity_math_fini(struct m0_parity_math *math)
{
	M0_ENTRY();
#ifndef __KERNEL__
	if (math->pmi_parity_algo == M0_PARITY_CAL_ALGO_ISA) {
		m0_free(math->encode_matrix);
		m0_free(math->g_tbls);
	}
#else
	if (math->pmi_parity_algo == M0_PARITY_CAL_ALGO_REED_SOLOMON) {
		vandmat_fini(&math->pmi_vandmat);
		m0_matrix_fini(&math->pmi_vandmat_parity_slice);
		m0_matvec_fini(&math->pmi_data);
		m0_matvec_fini(&math->pmi_parity);

		m0_linsys_fini(&math->pmi_sys);
		m0_matrix_fini(&math->pmi_sys_mat);
		m0_matvec_fini(&math->pmi_sys_vec);
		m0_matvec_fini(&math->pmi_sys_res);
	}
#endif /* __KERNEL__ */
	M0_LEAVE();
}

M0_INTERNAL int m0_parity_math_init(struct m0_parity_math *math,
				    uint32_t data_count, uint32_t parity_count)
{
	int ret = 0;

	M0_PRE(data_count >= 1);
	M0_PRE(parity_count >= 1);
	M0_PRE(data_count >= parity_count);
	M0_PRE(data_count <= SNS_PARITY_MATH_DATA_BLOCKS_MAX);

	M0_SET0(math);

	M0_ENTRY();

	math->pmi_data_count	= data_count;
	math->pmi_parity_count	= parity_count;

	if (parity_count == 1) {
		math->pmi_parity_algo = M0_PARITY_CAL_ALGO_XOR;
		return 0;
#ifndef __KERNEL__
	} else {
		uint32_t total_count = data_count + parity_count;

		M0_LOG(M0_DEBUG, "use Intel ISA for parity calculation.");

		math->pmi_parity_algo = M0_PARITY_CAL_ALGO_ISA;

		M0_ALLOC_ARR(math->encode_matrix, (total_count * data_count));
		if (math->encode_matrix == NULL) {
			ret = M0_ERR(-ENOMEM);
			goto handle_error;
		}

		M0_ALLOC_ARR(math->g_tbls, (data_count * parity_count * 32));
		if (math->g_tbls == NULL) {
			ret = M0_ERR(-ENOMEM);
			goto handle_error;
		}

		M0_LOG(M0_DEBUG, "generate a matrix of coefficients to be used "
		       "for encoding.");
		gf_gen_rs_matrix(math->encode_matrix, total_count, data_count);
	}
#else
	} else {
		math->pmi_parity_algo = M0_PARITY_CAL_ALGO_REED_SOLOMON;

		ret = vandmat_init(&math->pmi_vandmat, data_count,
				   parity_count);
		if (ret < 0)
			goto handle_error;

		ret = vandmat_norm(&math->pmi_vandmat);
		if (ret < 0)
			goto handle_error;

		ret = m0_matrix_init(&math->pmi_vandmat_parity_slice,
				     data_count, parity_count);
		if (ret < 0)
			goto handle_error;

		m0_matrix_submatrix_get(&math->pmi_vandmat,
				        &math->pmi_vandmat_parity_slice, 0,
					data_count);

		ret = m0_matvec_init(&math->pmi_data, data_count);
		if (ret < 0)
			goto handle_error;

		ret = m0_matvec_init(&math->pmi_parity, parity_count);
		if (ret < 0)
			goto handle_error;

		ret = m0_matvec_init(&math->pmi_sys_vec, math->pmi_data.mv_size);
		if (ret < 0)
			goto handle_error;

		ret = m0_matvec_init(&math->pmi_sys_res, math->pmi_data.mv_size);
		if (ret < 0)
			goto handle_error;

		ret = m0_matrix_init(&math->pmi_sys_mat, math->pmi_data.mv_size,
				     math->pmi_data.mv_size);
		if (ret < 0)
			goto handle_error;
	}
#endif /* __KERNEL__ */
	M0_LEAVE();

	return ret;
 handle_error:
	m0_parity_math_fini(math);

	M0_LEAVE();
	return ret;
}

static void xor_calculate(struct m0_parity_math *math,
			  const struct m0_buf *data,
			  struct m0_buf *parity)
{
        uint32_t          ei; /* block element index. */
        uint32_t          ui; /* unit index. */
        uint32_t          block_size = data[0].b_nob;
	m0_parity_elem_t  pe;

	M0_PRE(block_size == parity[0].b_nob);
	for (ui = 1; ui < math->pmi_data_count; ++ui)
		M0_PRE(block_size == data[ui].b_nob);

	for (ei = 0; ei < block_size; ++ei) {
		pe = 0;
		for (ui = 0; ui < math->pmi_data_count; ++ui)
			pe ^= (m0_parity_elem_t)((uint8_t*)data[ui].b_addr)[ei];

		((uint8_t*)parity[0].b_addr)[ei] = pe;
	}

}

static void reed_solomon_diff(struct m0_parity_math *math,
			      struct m0_buf         *old,
			      struct m0_buf         *new,
			      struct m0_buf         *parity,
			      uint32_t               index)
{
	struct m0_matrix *mat;
	uint32_t          ei;
	uint32_t          ui;
	uint8_t		  diff_data;
	m0_parity_elem_t  mat_elem;

	M0_PRE(math   != NULL);
	M0_PRE(old    != NULL);
	M0_PRE(new    != NULL);
	M0_PRE(parity != NULL);
	M0_PRE(index  <  math->pmi_data_count);
	M0_PRE(old[index].b_nob == new[index].b_nob);
	M0_PRE(m0_forall(i, math->pmi_parity_count,
		         new[index].b_nob == parity[i].b_nob));

	mat = &math->pmi_vandmat_parity_slice;
	for (ui = 0; ui < math->pmi_parity_count; ++ui) {
		for (ei = 0; ei < new[index].b_nob; ++ei) {
			mat_elem = *m0_matrix_elem_get(mat, index, ui);
			diff_data = ((uint8_t *)old[index].b_addr)[ei] ^
				    ((uint8_t *)new[index].b_addr)[ei];
			((uint8_t*)parity[ui].b_addr)[ei] ^=
				gmul(diff_data, mat_elem);
		}
	}
}

static void xor_diff(struct m0_parity_math *math,
		     struct m0_buf         *old,
		     struct m0_buf         *new,
		     struct m0_buf         *parity,
		     uint32_t               index)
{
	uint32_t ei;

	M0_PRE(math   != NULL);
	M0_PRE(old    != NULL);
	M0_PRE(new    != NULL);
	M0_PRE(parity != NULL);
	M0_PRE(index  <  math->pmi_data_count);
	M0_PRE(old[index].b_nob == new[index].b_nob);
	M0_PRE(new[index].b_nob == parity[0].b_nob);

	for (ei = 0; ei < new[index].b_nob; ++ei) {
		((uint8_t*)parity[0].b_addr)[ei] ^=
			((uint8_t *)old[index].b_addr)[ei] ^
			((uint8_t *)new[index].b_addr)[ei];
	}
}

#ifdef __KERNEL__
static void reed_solomon_encode(struct m0_parity_math *math,
				const struct m0_buf *data,
				struct m0_buf *parity)
{
#define PARITY_MATH_REGION_ENABLE 0

#if !PARITY_MATH_REGION_ENABLE
	uint32_t	  ei; /* block element index. */
#endif
	uint32_t	  pi; /* parity unit index. */
	uint32_t	  di; /* data unit index. */
	m0_parity_elem_t  mat_elem;
	uint32_t	  block_size = data[0].b_nob;


	for (di = 1; di < math->pmi_data_count; ++di)
		M0_ASSERT(block_size == data[di].b_nob);

	for (pi = 0; pi < math->pmi_parity_count; ++pi)
		M0_ASSERT(block_size == parity[pi].b_nob);

	for (pi = 0; pi < math->pmi_parity_count; ++pi) {
		for (di = 0; di < math->pmi_data_count; ++di) {
			mat_elem =
			*m0_matrix_elem_get(&math->pmi_vandmat_parity_slice,
					    di, pi);
			if (mat_elem == 0)
				continue;
#if !PARITY_MATH_REGION_ENABLE
			for (ei = 0; ei < block_size; ++ei) {
				if (di == 0)
					((uint8_t*)parity[pi].b_addr)[ei] =
						M0_PARITY_ZERO;

				((uint8_t*)parity[pi].b_addr)[ei] =
					gadd(((uint8_t*)parity[pi].b_addr)[ei],
					     gmul(mat_elem,
						  ((uint8_t*)data[di].b_addr)
						  [ei]));
			}
#else
			if (di == 0)
				memset((uint8_t*)parity[pi].b_addr,
				       M0_PARITY_ZERO, block_size);

			m0_parity_region_mac(
				(m0_parity_elem_t*) parity[pi].b_addr,
				(const m0_parity_elem_t*) data[di].b_addr,
				block_size,
				mat_elem);
#endif
		}
	}

#undef PARITY_MATH_REGION_ENABLE
}
#endif /* __KERNEL__ */

#ifndef __KERNEL__
static void isal_encode(struct m0_parity_math *math,
			const struct m0_buf *data,
			struct m0_buf *parity)
{
	uint8_t **data_frags = NULL;
	uint8_t **parity_frags = NULL;
	uint32_t  i;
	uint32_t  data_count;
	uint32_t  parity_count;
	uint32_t  block_size;
	int	  ret = 0;

	M0_PRE(math != NULL);
	M0_PRE(data != NULL);
	M0_PRE(parity != NULL);

	M0_ENTRY();

	data_count = math->pmi_data_count;
	parity_count = math->pmi_parity_count;
	block_size = data[0].b_nob;

	M0_ALLOC_ARR(data_frags, data_count);
	if (data_frags == NULL) {
		ret = M0_ERR_INFO(-ENOMEM, "failed to allocate memory for "
				  "array of data fragments");
		goto fini;
	}

	M0_ALLOC_ARR(parity_frags, parity_count);
	if (parity_frags == NULL) {
		ret = M0_ERR_INFO(-ENOMEM, "failed to allocate memory for "
				  "array of parity fragments");
		goto fini;
	}

	data_frags[0] = (uint8_t *)data[0].b_addr;
	for (i = 1; i < data_count; ++i) {
		if (block_size != data[i].b_nob) {
			ret = M0_ERR_INFO(-EINVAL, "data block size mismatch. "
					  "block_size = %u, data[%u].b_nob=%u",
					  block_size, i, (uint32_t)data[i].b_nob);
			goto fini;
		}
		data_frags[i] = (uint8_t *)data[i].b_addr;
	}

	for (i = 0; i < parity_count; ++i) {
		if (block_size != parity[i].b_nob) {
			ret = M0_ERR_INFO(-EINVAL, "parity block size mismatch. "
					  "block_size = %u, parity[%u].b_nob=%u",
					  block_size, i, (uint32_t)parity[i].b_nob);
			goto fini;
		}
		parity_frags[i] = (uint8_t *)parity[i].b_addr;
	}

	M0_LOG(M0_DEBUG, "initialize tables for fast Erasure Code encode.");
	ec_init_tables(data_count, parity_count,
		       &math->encode_matrix[data_count * data_count],
		       math->g_tbls);

	M0_LOG(M0_DEBUG, "generate erasure codes on given blocks of data.");
	ec_encode_data(block_size, data_count, parity_count,
		       math->g_tbls, data_frags, parity_frags);

fini:
	m0_free(data_frags);
	m0_free(parity_frags);

	/* TODO: Return error code instead of assert */
	M0_ASSERT(ret == 0);

	M0_LEAVE();
}
#endif /* __KERNEL__ */

M0_INTERNAL void m0_parity_math_calculate(struct m0_parity_math *math,
					  struct m0_buf *data,
					  struct m0_buf *parity)
{
	M0_ENTRY();
	(*calculate[math->pmi_parity_algo])(math, data, parity);
	M0_LEAVE();
}

M0_INTERNAL void m0_parity_math_diff(struct m0_parity_math *math,
				     struct m0_buf *old,
				     struct m0_buf *new,
				     struct m0_buf *parity, uint32_t index)
{
	M0_ENTRY();
	(*diff[math->pmi_parity_algo])(math, old, new, parity, index);
	M0_LEAVE();
}

M0_INTERNAL void m0_parity_math_refine(struct m0_parity_math *math,
				       struct m0_buf *data,
				       struct m0_buf *parity,
				       uint32_t data_ind_changed)
{
	M0_ENTRY();
	/* for simplicity: */
	m0_parity_math_calculate(math, data, parity);
	M0_LEAVE();
}

/* Counts number of failed blocks. */
static uint32_t fails_count(uint8_t *fail, uint32_t unit_count)
{
	uint32_t x;
	uint32_t count = 0;

	for (x = 0; x < unit_count; ++x)
		count += !!fail[x];

	return count;
}

/* Fills 'mat' and 'vec' with data passed to recovery algorithm. */
static void recovery_vec_fill(struct m0_parity_math *math,
			       uint8_t *fail, uint32_t unit_count, /* in. */
			       struct m0_matvec *vec) /* out. */
{
	uint32_t f;
	uint32_t y = 0;

	for (f = 0; f < unit_count; ++f) {
		/*
		 * if (block is ok) and
		 * (not enough equations to solve system).
		 */
		if (!fail[f] && y < vec->mv_size) {
			/* copy vec. */
			*m0_matvec_elem_get(vec, y) = f < math->pmi_data_count
				? *m0_matvec_elem_get(&math->pmi_data, f)
				: *m0_matvec_elem_get(&math->pmi_parity,
						f - math->pmi_data_count);
			++y;
		}
	}
}

/* Fills 'mat' with data passed to recovery algorithm. */
static void recovery_mat_fill(struct m0_parity_math *math,
			      uint8_t *fail, uint32_t unit_count, /* in. */
			      struct m0_matrix *mat) /* out. */
{
	uint32_t f;
	uint32_t y = 0;
	uint32_t x;

	for (f = 0; f < unit_count; ++f) {
		if (!fail[f] && y < mat->m_height) {
			for (x = 0; x < mat->m_width; ++x)
				*m0_matrix_elem_get(mat, x, y) =
					*m0_matrix_elem_get(&math->pmi_vandmat,
							    x, f);
			++y;
		}
	}
}

/* Updates internal structures of 'math' with recovered data. */
static void parity_math_recover(struct m0_parity_math *math,
				uint8_t *fail, uint32_t unit_count,
				enum m0_parity_linsys_algo algo)
{
	struct m0_matrix *mat = &math->pmi_sys_mat;
	struct m0_matvec *vec = &math->pmi_sys_vec;
	struct m0_matvec *res = &math->pmi_sys_res;
	struct m0_linsys *sys = &math->pmi_sys;
	uint32_t          i;

	recovery_vec_fill(math, fail, unit_count, vec);
	if (algo == M0_LA_GAUSSIAN) {
		recovery_mat_fill(math, fail, unit_count, mat);
		m0_linsys_init(sys, mat, vec, res);
		m0_linsys_solve(sys);
	} else {
		for (i = 0; i < math->pmi_data_count; ++i) {
			if (fail[i] == 0)
				continue;
			m0_matrix_vec_multiply(&math->pmi_recov_mat, vec, res, m0_parity_mul, m0_parity_add);
		}
	}
}

M0_INTERNAL int m0_parity_recov_mat_gen(struct m0_parity_math *math,
					uint8_t *fail)
{
	int rc;

	recovery_mat_fill(math, fail, math->pmi_data_count + math->pmi_parity_count,
			  &math->pmi_sys_mat);
	m0_matrix_init(&math->pmi_recov_mat, math->pmi_sys_mat.m_width,
		       math->pmi_sys_mat.m_height);
	rc = m0_matrix_invert(&math->pmi_sys_mat, &math->pmi_recov_mat);

	return rc == 0 ? M0_RC(0) : M0_ERR(rc);
}

M0_INTERNAL void m0_parity_recov_mat_destroy(struct m0_parity_math *math)
{
	m0_matrix_fini(&math->pmi_recov_mat);
}

static void xor_recover(struct m0_parity_math *math,
			struct m0_buf *data,
			struct m0_buf *parity,
			struct m0_buf *fails,
			enum m0_parity_linsys_algo algo)
{
	uint32_t          ei; /* block element index. */
	uint32_t          ui; /* unit index. */
	uint8_t          *fail;
	uint32_t          fail_count;
	uint32_t          unit_count;
	uint32_t          block_size = data[0].b_nob;
	m0_parity_elem_t  pe;
	int		  fail_index = BAD_FAIL_INDEX;

	unit_count = math->pmi_data_count + math->pmi_parity_count;
	fail = (uint8_t*) fails->b_addr;
	fail_count = fails_count(fail, unit_count);

	M0_PRE(fail_count == 1);
	M0_PRE(fail_count <= math->pmi_parity_count);
	M0_PRE(block_size == parity[0].b_nob);

	for (ui = 1; ui < math->pmi_data_count; ++ui)
		M0_PRE(block_size == data[ui].b_nob);

	for (ei = 0; ei < block_size; ++ei) {
		pe = 0;
                for (ui = 0; ui < math->pmi_data_count; ++ui) {
			if (fail[ui] != 1)
				pe ^= (m0_parity_elem_t)((uint8_t*)
				       data[ui].b_addr)[ei];
			else
				fail_index = ui;
                }
		/* Now ui points to parity block, test if it was failed. */
		if (fail[ui] != 1) {
			M0_ASSERT(fail_index != BAD_FAIL_INDEX);
			((uint8_t*)data[fail_index].b_addr)[ei] = pe ^
				((uint8_t*)parity[0].b_addr)[ei];
		} else /* Parity was lost, so recover it. */
			((uint8_t*)parity[0].b_addr)[ei] = pe;
        }
}

#ifndef __KERNEL__
static int isal_gen_recov_coeff_tbl(uint32_t data_count, uint32_t parity_count,
				    struct m0_buf *failed_idx_buf,
				    struct m0_buf *alive_idx_buf,
				    uint8_t *encode_mat, uint8_t *g_tbls)
{
	uint8_t *decode_mat = NULL;
	uint8_t *temp_mat = NULL;
	uint8_t *invert_mat = NULL;
	uint8_t *failed_ids = NULL;
	uint8_t *alive_ids = NULL;
	uint32_t unit_count;
	uint32_t i;
	uint32_t j;
	uint32_t r;
	uint8_t	 s;
	int	 ret = 0;

	// M0_PRE(failed_idx_buf != NULL);
	// M0_PRE(failed_idx_buf->b_addr != NULL);
	// M0_PRE(failed_idx_buf->b_nob > 0);
	// M0_PRE(failed_idx_buf->b_nob <= parity_count);
	// M0_PRE(alive_idx_buf != NULL);
	// M0_PRE(alive_idx_buf->b_addr != NULL);
	// M0_PRE(alive_idx_buf->b_nob >=  data_count);
	// M0_PRE(encode_mat != NULL);
	// M0_PRE(g_tbls != NULL);

	failed_ids = (uint8_t *)failed_idx_buf->b_addr;
	alive_ids = (uint8_t *)alive_idx_buf->b_addr;

	unit_count = data_count + parity_count;

	M0_ALLOC_ARR(decode_mat, (unit_count * data_count));
	if (decode_mat == NULL) {
		ret = M0_ERR(-ENOMEM);
		goto exit;
	}

	M0_ALLOC_ARR(temp_mat, (unit_count * data_count));
	if (temp_mat == NULL) {
		ret = M0_ERR(-ENOMEM);
		goto exit;
	}

	M0_ALLOC_ARR(invert_mat, (unit_count * data_count));
	if (invert_mat == NULL) {
		ret = M0_ERR(-ENOMEM);
		goto exit;
	}

	/* Construct temp_mat (matrix that encoded remaining frags)
	by removing erased rows */
	for (i = 0; i < data_count; i++) {
		r = alive_ids[i];
		for (j = 0; j < data_count; j++)
			temp_mat[data_count * i + j] =
				encode_mat[data_count * r + j];
	}

	/* Invert matrix to get recovery matrix */
	ret = gf_invert_matrix(temp_mat, invert_mat, data_count);
	if (ret != 0)
		goto exit;

	/* Create decode matrix */
	for (r = 0; r < failed_idx_buf->b_nob; r++) {
		/* Get decode matrix with only wanted recovery rows */
		if (failed_ids[r] < data_count) {    /* A src err */
			for (i = 0; i < data_count; i++)
				decode_mat[data_count * r + i] =
					invert_mat[data_count * failed_ids[r] + i];
		}
		/* For non-src (parity) erasures need to multiply
			encode matrix * invert */
		else { /* A parity err */
			for (i = 0; i < data_count; i++) {
				s = 0;
				for (j = 0; j < data_count; j++)
					s ^= gf_mul(invert_mat[j * data_count + i],
						    encode_mat[data_count * failed_ids[r] + j]);
				decode_mat[data_count * r + i] = s;
			}
		}
	}

	ec_init_tables(data_count, (uint32_t)failed_idx_buf->b_nob, decode_mat, g_tbls);

exit:
	m0_free(temp_mat);
	m0_free(invert_mat);
	m0_free(decode_mat);

	return ret;
}

static void isal_recover(struct m0_parity_math *math,
			 struct m0_buf *data,
			 struct m0_buf *parity,
			 struct m0_buf *fails,
			 enum m0_parity_linsys_algo algo)
{
	struct m0_buf failed_idx_buf;
	struct m0_buf alive_idx_buf;
	uint32_t      fail_count;
	uint32_t      unit_count;
	uint32_t      data_count;
	uint32_t      parity_count;
	uint32_t      block_size = data[0].b_nob;
	uint32_t      i;
	uint32_t      j;
	uint32_t      r;
	uint8_t	     *fail = NULL;
	uint8_t	    **data_in = NULL;
	uint8_t	    **data_out = NULL;
	uint8_t	     *failed_ids = NULL;
	uint8_t	     *alive_ids = NULL;
	int	      ret = 0;

	M0_PRE(math != NULL);
	M0_PRE(data != NULL);
	M0_PRE(parity != NULL);
	M0_PRE(fails != NULL);

	M0_ENTRY();

	data_count = math->pmi_data_count;
	parity_count = math->pmi_parity_count;

	unit_count = data_count + parity_count;
	fail = (uint8_t*) fails->b_addr;

	M0_ALLOC_ARR(failed_ids, parity_count);
	M0_ASSERT(failed_ids != NULL);

	M0_ALLOC_ARR(alive_ids, parity_count);
	M0_ASSERT(alive_ids != NULL);

	// M0_ASSERT(m0_buf_alloc(&failed_idx_buf, parity_count) == 0);
	// M0_ASSERT(m0_buf_alloc(&alive_idx_buf, unit_count) == 0);

	// failed_ids = (uint8_t *)failed_idx_buf.b_addr;
	// alive_ids = (uint8_t *)alive_idx_buf.b_addr;

	m0_buf_init(&failed_idx_buf, failed_ids, 0);
	m0_buf_init(&alive_idx_buf, alive_ids, 0);

	for (i = 0; i < unit_count; i++) {
		if (fail[i])
			failed_ids[failed_idx_buf->b_nob++] = i;
		else
			alive_ids[alive_idx_buf->b_nob++] = i
	}
	fail_count = failed_idx_buf->b_nob;

	// for (i = 0, fail_count = 0, j = 0, r = 0; i < unit_count; i++) {
	// 	if (fail[i]) {
	// 		failed_ids[j++] = i;
	// 		fail_count++;
	// 	}
	// 	else
	// 		alive_ids[r++] = i;
	// }
	// failed_idx_buf.b_nob = fail_count;
	// alive_idx_buf.b_nob = unit_count - fail_count;

	M0_ASSERT(fail_count > 0);
	M0_ASSERT(fail_count <= parity_count);

	for (i = 1; i < data_count; ++i)
		M0_ASSERT(block_size == data[i].b_nob);

	for (i = 0; i < parity_count; ++i)
		M0_ASSERT(block_size == parity[i].b_nob);

	M0_ALLOC_ARR(data_in, data_count);
	M0_ASSERT(data_in != NULL);

	M0_ALLOC_ARR(data_out, fail_count);
	M0_ASSERT(data_out != NULL);

	for (i = 0, j = 0; i < fail_count; i++) {
		if (failed_ids[i] < data_count)
			data_out[j++] = (uint8_t *)data[failed_ids[i]].b_addr;
		else
			data_out[j++] =
				(uint8_t *)parity[failed_ids[i] - data_count].b_addr;
	}

	for (i = 0, j = 0; i < data_count; i++) {
		if (alive_ids[i] < data_count)
			data_in[j++] = (uint8_t *)data[alive_ids[i]].b_addr;
		else
			data_in[j++] =
				(uint8_t *)parity[alive_ids[i] - data_count].b_addr;
	}

	/* Get encoding coefficient tables */
	ret = isal_gen_recov_coeff_tbl(data_count, parity_count, &failed_idx_buf,
				       &alive_idx_buf, math->encode_matrix,
				       math->g_tbls);
	M0_ASSERT(ret == 0);

	/* Recover data */
	ec_encode_data(block_size, data_count, fail_count,
		       math->g_tbls, data_in, data_out);

	// m0_buf_free(&failed_idx_buf);
	// m0_buf_free(&alive_idx_buf);
	m0_free(failed_ids);
	m0_free(alive_ids);
	m0_free(data_in);
	m0_free(data_out);
}
#endif

#ifdef __KERNEL__
static void reed_solomon_recover(struct m0_parity_math *math,
				 struct m0_buf *data,
				 struct m0_buf *parity,
				 struct m0_buf *fails,
				 enum m0_parity_linsys_algo algo)
{
	uint32_t ei; /* block element index. */
	uint32_t ui; /* unit index. */
	uint8_t *fail;
	uint32_t fail_count;
	uint32_t unit_count = math->pmi_data_count + math->pmi_parity_count;
	uint32_t block_size = data[0].b_nob;

	fail = (uint8_t*) fails->b_addr;
	fail_count = fails_count(fail, unit_count);

	M0_ASSERT(fail_count > 0);
	M0_ASSERT(fail_count <= math->pmi_parity_count);

	for (ui = 1; ui < math->pmi_data_count; ++ui)
		M0_ASSERT(block_size == data[ui].b_nob);

	for (ui = 0; ui < math->pmi_parity_count; ++ui)
		M0_ASSERT(block_size == parity[ui].b_nob);

	for (ei = 0; ei < block_size; ++ei) {
		struct m0_matvec *recovered = &math->pmi_sys_res;

		/* load data and parity. */
		for (ui = 0; ui < math->pmi_data_count; ++ui)
			*m0_matvec_elem_get(&math->pmi_data, ui) =
				((uint8_t*)data[ui].b_addr)[ei];

		for (ui = 0; ui < math->pmi_parity_count; ++ui)
			*m0_matvec_elem_get(&math->pmi_parity, ui) =
				((uint8_t*)parity[ui].b_addr)[ei];

		/* recover data. */
		parity_math_recover(math, fail, unit_count, algo);
		/* store data. */
		for (ui = 0; ui < math->pmi_data_count; ++ui) {
			if (fail[ui] == 0)
				continue;
			((uint8_t*)data[ui].b_addr)[ei] =
				*m0_matvec_elem_get(recovered, ui);
		}
	}
}
#endif /* __KERNEL__ */

M0_INTERNAL void m0_parity_math_recover(struct m0_parity_math *math,
					struct m0_buf *data,
					struct m0_buf *parity,
					struct m0_buf *fails,
					enum m0_parity_linsys_algo algo)
{
	M0_ENTRY();
	(*recover[math->pmi_parity_algo])(math, data, parity, fails, algo);
	M0_LEAVE();
}

static void fail_idx_xor_recover(struct m0_parity_math *math,
				 struct m0_buf *data,
				 struct m0_buf *parity,
				 const uint32_t failure_index)
{
        uint32_t          ei; /* block element index. */
        uint32_t          ui; /* unit index. */
        uint32_t          unit_count;
        uint32_t          block_size = data[0].b_nob;
        m0_parity_elem_t  pe;

	M0_PRE(block_size == parity[0].b_nob);

        unit_count = math->pmi_data_count + math->pmi_parity_count;
        M0_ASSERT(failure_index < unit_count);

	for (ui = 1; ui < math->pmi_data_count; ++ui)
		M0_ASSERT(block_size == data[ui].b_nob);

        for (ei = 0; ei < block_size; ++ei) {
                pe = 0;
                for (ui = 0; ui < math->pmi_data_count; ++ui)
			if (ui != failure_index)
				pe ^= (m0_parity_elem_t)((uint8_t*)
				       data[ui].b_addr)[ei];

                if (ui != failure_index)
			((uint8_t*)data[failure_index].b_addr)[ei] = pe ^
				((uint8_t*)parity[0].b_addr)[ei];
                else /* Parity was lost, so recover it. */
                        ((uint8_t*)parity[0].b_addr)[ei] = pe;
        }

}

/** @todo Iterative reed-solomon decode to be implemented. */
static void fail_idx_reed_solomon_recover(struct m0_parity_math *math,
					  struct m0_buf *data,
					  struct m0_buf *parity,
					  const uint32_t failure_index)
{
}

M0_INTERNAL void m0_parity_math_fail_index_recover(struct m0_parity_math *math,
						   struct m0_buf *data,
						   struct m0_buf *parity,
						   const uint32_t fidx)
{
	M0_ENTRY();
	(*fidx_recover[math->pmi_parity_algo])(math, data, parity, fidx);
	M0_LEAVE();
}

M0_INTERNAL void m0_parity_math_buffer_xor(struct m0_buf *dest,
					   const struct m0_buf *src)
{
        uint32_t ei; /* block element index. */

        for (ei = 0; ei < src[0].b_nob; ++ei)
		((uint8_t*)dest[0].b_addr)[ei] ^=
			(m0_parity_elem_t)((uint8_t*)src[0].b_addr)[ei];
}

M0_INTERNAL int m0_sns_ir_init(const struct m0_parity_math *math,
			       uint32_t local_nr, struct m0_sns_ir *ir)
{
	uint32_t i;
	int	 ret = 0;

	M0_PRE(math != NULL);
	M0_PRE(ir != NULL);

	M0_SET0(ir);
	ir->si_data_nr		   = math->pmi_data_count;
	ir->si_parity_nr	   = math->pmi_parity_count;
	ir->si_local_nr		   = local_nr;
	ir->si_vandmat		   = math->pmi_vandmat;
	ir->si_parity_recovery_mat = math->pmi_vandmat_parity_slice;
	ir->si_failed_data_nr	   = 0;
	ir->si_alive_nr		   = block_count(ir);

	M0_ALLOC_ARR(ir->si_blocks, ir->si_alive_nr);
	if (ir->si_blocks == NULL)
		return M0_ERR(-ENOMEM);

	for (i = 0; i < block_count(ir); ++i) {
		ir->si_blocks[i].sib_idx = i;
		ir->si_blocks[i].sib_status = M0_SI_BLOCK_ALIVE;
		ir->si_blocks[i].sib_data_recov_mat_col = ir_invalid_col_t;
		ret = m0_bitmap_init(&ir->si_blocks[i].sib_bitmap,
				     block_count(ir));
		if (ret != 0)
			goto fini;
	}
	return ret;
fini:
	while (--i > -1)
		m0_bitmap_fini(&ir->si_blocks[i].sib_bitmap);
	return ret;
}

M0_INTERNAL int m0_sns_ir_failure_register(struct m0_bufvec *recov_addr,
					   uint32_t failed_index,
					   struct m0_sns_ir *ir)
{
	struct m0_sns_ir_block *block;

	M0_PRE(ir != NULL);
	M0_PRE(ir->si_blocks != NULL);
	M0_PRE(recov_addr != NULL);
	M0_PRE(is_valid_block_idx(ir, failed_index));

	block = &ir->si_blocks[failed_index];
	M0_PRE(block->sib_status != M0_SI_BLOCK_FAILED);

	block->sib_addr   = recov_addr;
	block->sib_status = M0_SI_BLOCK_FAILED;

	M0_CNT_DEC(ir->si_alive_nr);
	if (is_data(ir, failed_index))
		M0_CNT_INC(ir->si_failed_data_nr);

	return (ir->si_alive_nr < ir->si_data_nr) ? -EDQUOT : 0;
}

M0_INTERNAL int m0_sns_ir_mat_compute(struct m0_sns_ir *ir)
{
	int			ret = 0;
	uint32_t		i;
	uint32_t		j;
	struct m0_sns_ir_block *blocks;

	M0_PRE(ir != NULL);

	blocks = ir->si_blocks;
	if (ir->si_failed_data_nr != 0) {
		for (j = 0, i = 0; j < block_count(ir) && i < ir->si_data_nr;
		     ++j) {
			if (blocks[j].sib_status == M0_SI_BLOCK_ALIVE) {
				blocks[j].sib_data_recov_mat_col = i;
				++i;
			}
		}
		ret = data_recov_mat_construct(ir);
		if (ret != 0)
			return ret;
	}

	for (j = 0, i = 0; j < block_count(ir); ++j) {
		if (blocks[j].sib_status == M0_SI_BLOCK_FAILED) {
			blocks[j].sib_recov_mat_row = is_data(ir, j) ? i :
				j - ir->si_data_nr;
			dependency_bitmap_prepare(&blocks[j], ir);
			++i;
		}
	}
	return ret;
}

static int data_recov_mat_construct(struct m0_sns_ir *ir)
{
	int		 ret = 0;
	struct m0_matrix encode_mat;
	struct m0_matrix encode_mat_inverse;
	M0_SET0(&encode_mat);
	M0_SET0(&encode_mat_inverse);

	M0_PRE(ir != NULL);
	M0_PRE(ir->si_blocks != NULL);

	ret = m0_matrix_init(&encode_mat, ir->si_data_nr,
			     ir->si_data_nr);
	if (ret != 0)
		goto fini;
	submatrix_construct(&ir->si_vandmat, ir->si_blocks,
			    M0_SI_BLOCK_ALIVE, &encode_mat);
	ret = m0_matrix_init(&encode_mat_inverse, encode_mat.m_width,
			     encode_mat.m_height);
	if (ret != 0)
		goto fini;
	ret = m0_matrix_invert(&encode_mat, &encode_mat_inverse);
	if (ret != 0)
		goto fini;
	ret = m0_matrix_init(&ir->si_data_recovery_mat, ir->si_data_nr,
			     ir->si_failed_data_nr);
	if (ret != 0)
		goto fini;
	submatrix_construct(&encode_mat_inverse, ir->si_blocks,
			    M0_SI_BLOCK_FAILED, &ir->si_data_recovery_mat);
fini:
	m0_matrix_fini(&encode_mat);
	m0_matrix_fini(&encode_mat_inverse);
	return ret;
}

static void submatrix_construct(struct m0_matrix *in_mat,
				struct m0_sns_ir_block *blocks,
				enum m0_sns_ir_block_status status,
				struct m0_matrix *out_mat)
{
	uint32_t out_row;
	uint32_t in_row;

	M0_PRE(in_mat != NULL);
	M0_PRE(out_mat != NULL);
	M0_PRE(blocks != NULL);
	M0_PRE(m0_matrix_is_init(in_mat));
	M0_PRE(m0_matrix_is_init(out_mat));
	M0_PRE(out_mat->m_width == in_mat->m_width);

	for (in_row = 0, out_row = 0; in_row < in_mat->m_height &&
	     out_row < out_mat->m_height; ++in_row) {
		if (blocks[in_row].sib_status == status) {
			m0_matrix_row_copy(out_mat, in_mat, out_row, in_row);
			++out_row;
		}
	}
}

static void dependency_bitmap_prepare(struct m0_sns_ir_block *f_block,
				      struct m0_sns_ir *ir)
{
	uint32_t i;

	M0_PRE(f_block != NULL && ir != NULL);
	M0_PRE(f_block->sib_status == M0_SI_BLOCK_FAILED);

	if (is_data(ir, f_block->sib_idx)) {
		for (i = 0; i < block_count(ir); ++i) {
			if (ir->si_blocks[i].sib_status != M0_SI_BLOCK_ALIVE)
				continue;
			if (ir->si_blocks[i].sib_data_recov_mat_col !=
			    ir_invalid_col_t)
				m0_bitmap_set(&f_block->sib_bitmap,
					      ir->si_blocks[i].sib_idx, true);
		}
	} else {
		for (i = 0; i < ir->si_data_nr; ++i) {
			m0_bitmap_set(&f_block->sib_bitmap,
				      ir->si_blocks[i].sib_idx, true);
		}
	}
}

static inline uint32_t recov_mat_col(const struct m0_sns_ir_block *alive_block,
				     const struct m0_sns_ir_block *failed_block,
				     const struct m0_sns_ir *ir)
{
	M0_PRE(alive_block != NULL);
	M0_PRE(failed_block != NULL);
	return is_data(ir, failed_block->sib_idx) ?
		alive_block->sib_data_recov_mat_col : alive_block->sib_idx;
}

M0_INTERNAL void m0_sns_ir_fini(struct m0_sns_ir *ir)
{
	uint32_t j;

	M0_PRE(ir != NULL);

	for (j = 0; j < block_count(ir); ++j) {
		m0_bitmap_fini(&ir->si_blocks[j].sib_bitmap);
	}

	m0_matrix_fini(&ir->si_data_recovery_mat);
	m0_free(ir->si_blocks);
}

M0_INTERNAL void m0_sns_ir_recover(struct m0_sns_ir *ir,
				   struct m0_bufvec *bufvec,
				   const struct m0_bitmap *bitmap,
				   uint32_t failed_index,
				   enum m0_sns_ir_block_type block_type)
{
	uint32_t		j;
	size_t			block_idx = 0;
	size_t		        b_set_nr;
	struct m0_sns_ir_block *blocks;
	struct m0_sns_ir_block *alive_block;

	M0_PRE(ir != NULL && bufvec != NULL && bitmap != NULL);

	b_set_nr = m0_bitmap_set_nr(bitmap);
	M0_PRE(b_set_nr > 0);
	M0_PRE(ergo(b_set_nr > 1, block_type == M0_SI_BLOCK_REMOTE));
	M0_PRE(ergo(block_type == M0_SI_BLOCK_REMOTE,
		    is_valid_block_idx(ir, failed_index)));
	M0_PRE(ergo(block_type == M0_SI_BLOCK_REMOTE,
	            ir->si_blocks[failed_index].sib_status ==
		    M0_SI_BLOCK_FAILED));
	M0_PRE(ergo(block_type == M0_SI_BLOCK_LOCAL, ir->si_local_nr != 0));
	if (b_set_nr == 1) {
		for (block_idx = 0; block_idx < bitmap->b_nr; ++block_idx) {
			if (m0_bitmap_get(bitmap, block_idx))
				break;
		}
	}
	M0_ASSERT(is_valid_block_idx(ir, block_idx));
	blocks = ir->si_blocks;
	switch (block_type) {
	/* Input block is assumed to be an untransformed block, and is used for
	 * recovering all failed blocks */
	case M0_SI_BLOCK_LOCAL:

		M0_CNT_DEC(ir->si_local_nr);
		alive_block = &blocks[block_idx];
		alive_block->sib_addr = bufvec;
		for (j = 0; j < block_count(ir); ++j)
			if (ir->si_blocks[j].sib_status == M0_SI_BLOCK_FAILED) {
				incr_recover(&blocks[j], alive_block, ir);
				m0_bitmap_set(&blocks[j].sib_bitmap,
					      alive_block->sib_idx, false);
			}
		if (ir->si_local_nr == 0 && are_failures_mixed(ir)) {
			failed_data_blocks_xform(ir);
		}
		break;

	/* Input block is assumed to be a transformed block, and is
	 * cummulatively added to a block with index failed_index. */
	case M0_SI_BLOCK_REMOTE:
		if (!is_usable(ir, (struct m0_bitmap*) bitmap,
			       &blocks[failed_index]))
			break;
		gfaxpy(blocks[failed_index].sib_addr, bufvec,
		       1);
		dependency_bitmap_update(&blocks[failed_index],
					 bitmap);
		if (is_data(ir, failed_index) && are_failures_mixed(ir) &&
		    ir->si_local_nr != 0)
			forward_rectification(ir, bufvec, failed_index);

		break;
	}
}

static void incr_recover(struct m0_sns_ir_block *failed_block,
			 const struct m0_sns_ir_block *alive_block,
			 struct m0_sns_ir *ir)
{
	const struct m0_matrix *mat;
	uint32_t		row;
	uint32_t		col;
	uint32_t		last_usable_bid;
	m0_parity_elem_t	mat_elem;

	M0_PRE(failed_block != NULL);
	M0_PRE(alive_block != NULL);
	M0_PRE(ir != NULL);
	last_usable_bid = last_usable_block_id(ir, failed_block->sib_idx);
	if (alive_block->sib_idx <= last_usable_bid &&
	    m0_bitmap_get(&failed_block->sib_bitmap, alive_block->sib_idx)) {
		mat = recovery_mat_get(ir, failed_block->sib_idx);
		row = failed_block->sib_recov_mat_row;
		col = recov_mat_col(alive_block, failed_block, ir);
		mat_elem = *m0_matrix_elem_get(mat, col, row);
		gfaxpy(failed_block->sib_addr, alive_block->sib_addr,
		       mat_elem);
	}
}

static void gfaxpy(struct m0_bufvec *y, struct m0_bufvec *x,
		   m0_parity_elem_t alpha)
{
	uint32_t		i;
	uint32_t		seg_size;
	uint8_t		       *y_addr;
	uint8_t		       *x_addr;
	m0_bcount_t		step;
	struct m0_bufvec_cursor x_cursor;
	struct m0_bufvec_cursor y_cursor;

	M0_PRE(y != NULL && x != NULL);
	M0_PRE(y->ov_vec.v_nr != 0);
	M0_PRE(y->ov_vec.v_nr == x->ov_vec.v_nr);
	M0_PRE(y->ov_vec.v_count[0] != 0);
	M0_PRE(y->ov_vec.v_count[0] == x->ov_vec.v_count[0]);
	seg_size = y->ov_vec.v_count[0];
	M0_PRE(m0_forall(i, y->ov_vec.v_nr, y->ov_vec.v_count[i] == seg_size));

	m0_bufvec_cursor_init(&y_cursor, y);
	m0_bufvec_cursor_init(&x_cursor, x);
	do {
		x_addr  = m0_bufvec_cursor_addr(&x_cursor);
		y_addr  = m0_bufvec_cursor_addr(&y_cursor);

		switch (alpha) {
		/* This is a special case of the 'default' case that follows.
		 * Here we avoid unnecessary call to gmul.*/
		case 1:
			for (i = 0; i <seg_size; ++i) {
				y_addr[i] = gadd(y_addr[i],
						 x_addr[i]);
			}
			break;
		default:
			for (i = 0; i < seg_size; ++i) {
				y_addr[i] = gadd(y_addr[i], gmul(x_addr[i],
							         alpha));
			}
			break;
		}
		step = m0_bufvec_cursor_step(&y_cursor);
	} while (!m0_bufvec_cursor_move(&x_cursor, step) &&
		 !m0_bufvec_cursor_move(&y_cursor, step));
}

static void dependency_bitmap_update(struct m0_sns_ir_block *block,
				     const struct m0_bitmap *bitmap)
{
	size_t i;
	size_t b_set_nr;

	b_set_nr = m0_bitmap_set_nr(bitmap);
	for (i = 0; i < bitmap->b_nr && b_set_nr != 0; ++i) {
		if (m0_bitmap_get(bitmap, i)) {
			m0_bitmap_set(&block->sib_bitmap, i, false);
			--b_set_nr;
		}
	}
}

static void forward_rectification(struct m0_sns_ir *ir,
				  struct m0_bufvec *in_bufvec,
				  uint32_t failed_index)
{
	struct m0_sns_ir_block in_block;
	uint32_t	       j;

	M0_PRE(ir->si_local_nr != 0);
	M0_PRE(is_data(ir, failed_index));
	M0_PRE(ir->si_blocks[failed_index].sib_status == M0_SI_BLOCK_FAILED);

	memcpy(&in_block, &ir->si_blocks[failed_index], sizeof in_block);
	in_block.sib_addr = in_bufvec;
	for (j = 0; j < block_count(ir); ++j)
		if (!is_data(ir, ir->si_blocks[j].sib_idx) &&
		    ir->si_blocks[j].sib_status == M0_SI_BLOCK_FAILED)
			incr_recover(&ir->si_blocks[j],
				     &in_block,
				     ir);
}

static void failed_data_blocks_xform(struct m0_sns_ir *ir)
{
	struct m0_sns_ir_block *res_block;
	struct m0_sns_ir_block *par_block;
	uint32_t		i;
	uint32_t		j;

	M0_PRE(ir->si_local_nr == 0);
	res_block = ir->si_blocks;
	par_block = ir->si_blocks;

	for (i = 0; i < block_count(ir); ++i) {
		if (is_data(ir, ir->si_blocks[i].sib_idx) &&
			    ir->si_blocks[i].sib_status == M0_SI_BLOCK_FAILED)
			for (j = 0; j < block_count(ir); ++j) {
				if (!is_data(ir, ir->si_blocks[j].sib_idx) &&
					     ir->si_blocks[j].sib_status ==
					     M0_SI_BLOCK_FAILED) {
					incr_recover(&par_block[j],
						     &res_block[i], ir);
					m0_bitmap_set(&par_block[j].sib_bitmap,
						      res_block[i].sib_idx,
						      false);
				}
			}
	}
}

static inline bool is_valid_block_idx(const struct m0_sns_ir *ir,
				      uint32_t block_idx)
{
	return block_idx < block_count(ir);
}

static bool is_data(const struct m0_sns_ir *ir, uint32_t index)
{
	M0_PRE(is_valid_block_idx(ir, index));
	return index < ir->si_data_nr;
}

static bool is_usable(const struct m0_sns_ir *ir,
		      const struct m0_bitmap *in_bmap,
		      struct m0_sns_ir_block *failed_block)
{
	size_t		  i;
	uint32_t	  last_usable_bid;

	M0_PRE(in_bmap != NULL && failed_block != NULL && ir != NULL);
	M0_PRE(in_bmap->b_nr == failed_block->sib_bitmap.b_nr);

	last_usable_bid = last_usable_block_id(ir, failed_block->sib_idx);
	if (last_usable_bid == block_count(ir))
		return false;
	for (i = 0; i <= last_usable_bid; ++i) {
		if (m0_bitmap_get(in_bmap, i) &&
		    !m0_bitmap_get(&failed_block->sib_bitmap, i))
			return false;
	}
	return true;
}

static uint32_t last_usable_block_id(const struct m0_sns_ir *ir,
				     uint32_t block_idx)
{
	uint32_t i;
	uint32_t last_usable_bid = block_count(ir);

	if (is_data(ir, block_idx)) {
		for (i = 0; i  < block_count(ir); ++i) {
			if (ir->si_blocks[i].sib_status == M0_SI_BLOCK_ALIVE) {
				if (ir->si_blocks[i].sib_data_recov_mat_col ==
				    ir_invalid_col_t)
					return last_usable_bid;
				last_usable_bid = i;
			}
		}
	} else
		return ir->si_data_nr - 1;
	return last_usable_bid;
}

static inline const struct m0_matrix* recovery_mat_get(const struct m0_sns_ir
						       *ir, uint32_t failed_idx)
{
	return is_data(ir, failed_idx) ? &ir->si_data_recovery_mat :
		&ir->si_parity_recovery_mat;
}

static inline  bool are_failures_mixed(const struct m0_sns_ir *ir)
{
	return !!ir->si_failed_data_nr &&
		block_count(ir) != ir->si_failed_data_nr + ir->si_alive_nr;
}

static inline uint32_t block_count(const struct m0_sns_ir *ir)
{
	return ir->si_data_nr + ir->si_parity_nr;
}

#undef M0_TRACE_SUBSYSTEM

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
