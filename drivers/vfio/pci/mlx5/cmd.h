/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef MLX5_VFIO_CMD_H
#define MLX5_VFIO_CMD_H

#include <linux/kernel.h>
#include <linux/vfio_pci_core.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/vport.h>
#include <linux/mlx5/cq.h>
#include <linux/mlx5/qp.h>

enum mlx5_vf_migf_state {
	MLX5_MIGF_STATE_ERROR = 1,
	MLX5_MIGF_STATE_PRE_COPY_ERROR,
	MLX5_MIGF_STATE_PRE_COPY,
	MLX5_MIGF_STATE_SAVE_STOP_COPY_CHUNK,
	MLX5_MIGF_STATE_COMPLETE,
};

enum mlx5_vf_load_state {
	MLX5_VF_LOAD_STATE_READ_HEADER,
	MLX5_VF_LOAD_STATE_PREP_HEADER_DATA,
	MLX5_VF_LOAD_STATE_READ_HEADER_DATA,
	MLX5_VF_LOAD_STATE_PREP_IMAGE,
	MLX5_VF_LOAD_STATE_READ_IMAGE,
	MLX5_VF_LOAD_STATE_LOAD_IMAGE,
};

struct mlx5_vf_migration_tag_stop_copy_data {
	__le64 stop_copy_size;
};

enum mlx5_vf_migf_header_flags {
	MLX5_MIGF_HEADER_FLAGS_TAG_MANDATORY = 0,
	MLX5_MIGF_HEADER_FLAGS_TAG_OPTIONAL = 1 << 0,
};

enum mlx5_vf_migf_header_tag {
	MLX5_MIGF_HEADER_TAG_FW_DATA = 0,
	MLX5_MIGF_HEADER_TAG_STOP_COPY_SIZE = 1 << 0,
};

struct mlx5_vf_migration_header {
	__le64 record_size;
	/* For future use in case we may need to change the kernel protocol */
	__le32 flags; /* Use mlx5_vf_migf_header_flags */
	__le32 tag; /* Use mlx5_vf_migf_header_tag */
	__u8 data[]; /* Its size is given in the record_size */
};

struct mlx5_vhca_data_buffer {
	struct page **page_list;
	struct dma_iova_state state;
	loff_t start_pos;
	u64 length;
	u32 npages;
	u32 mkey;
	u32 *mkey_in;
	enum dma_data_direction dma_dir;
	u8 stop_copy_chunk_num;
	struct list_head buf_elm;
	struct mlx5_vf_migration_file *migf;
};

struct mlx5vf_async_data {
	struct mlx5_async_work cb_work;
	struct work_struct work;
	struct mlx5_vhca_data_buffer *buf;
	struct mlx5_vhca_data_buffer *header_buf;
	int status;
	u8 stop_copy_chunk:1;
	void *out;
};

struct mlx5vf_save_work_data {
	struct mlx5_vf_migration_file *migf;
	size_t next_required_umem_size;
	struct work_struct work;
	u8 chunk_num;
};

#define MAX_NUM_CHUNKS 2

struct mlx5_vf_migration_file {
	struct file *filp;
	struct mutex lock;
	enum mlx5_vf_migf_state state;

	enum mlx5_vf_load_state load_state;
	u32 pdn;
	loff_t max_pos;
	u64 record_size;
	u32 record_tag;
	u64 stop_copy_prep_size;
	u64 pre_copy_initial_bytes;
	size_t next_required_umem_size;
	u8 num_ready_chunks;
	/* Upon chunk mode preserve another set of buffers for stop_copy phase */
	struct mlx5_vhca_data_buffer *buf[MAX_NUM_CHUNKS];
	struct mlx5_vhca_data_buffer *buf_header[MAX_NUM_CHUNKS];
	struct mlx5vf_save_work_data save_data[MAX_NUM_CHUNKS];
	spinlock_t list_lock;
	struct list_head buf_list;
	struct list_head avail_list;
	struct mlx5vf_pci_core_device *mvdev;
	wait_queue_head_t poll_wait;
	struct completion save_comp;
	struct mlx5_async_ctx async_ctx;
	struct mlx5vf_async_data async_data;
};

struct mlx5_vhca_cq_buf {
	struct mlx5_frag_buf_ctrl fbc;
	struct mlx5_frag_buf frag_buf;
	int cqe_size;
	int nent;
};

struct mlx5_vhca_cq {
	struct mlx5_vhca_cq_buf buf;
	struct mlx5_db db;
	struct mlx5_core_cq mcq;
	size_t ncqe;
};

struct mlx5_vhca_recv_buf {
	u32 npages;
	struct page **page_list;
	struct dma_iova_state state;
	u32 next_rq_offset;
	u32 *mkey_in;
	u32 mkey;
};

struct mlx5_vhca_qp {
	struct mlx5_frag_buf buf;
	struct mlx5_db db;
	struct mlx5_vhca_recv_buf recv_buf;
	u32 tracked_page_size;
	u32 max_msg_size;
	u32 qpn;
	struct {
		unsigned int pc;
		unsigned int cc;
		unsigned int wqe_cnt;
		__be32 *db;
		struct mlx5_frag_buf_ctrl fbc;
	} rq;
};

struct mlx5_vhca_page_tracker {
	u32 id;
	u32 pdn;
	u8 is_err:1;
	u8 object_changed:1;
	struct mlx5_uars_page *uar;
	struct mlx5_vhca_cq cq;
	struct mlx5_vhca_qp *host_qp;
	struct mlx5_vhca_qp *fw_qp;
	struct mlx5_nb nb;
	int status;
};

struct mlx5vf_pci_core_device {
	struct vfio_pci_core_device core_device;
	int vf_id;
	u16 vhca_id;
	u8 migrate_cap:1;
	u8 deferred_reset:1;
	u8 mdev_detach:1;
	u8 log_active:1;
	u8 chunk_mode:1;
	struct completion tracker_comp;
	/* protect migration state */
	struct mutex state_mutex;
	enum vfio_device_mig_state mig_state;
	/* protect the reset_done flow */
	spinlock_t reset_lock;
	struct mlx5_vf_migration_file *resuming_migf;
	struct mlx5_vf_migration_file *saving_migf;
	struct mlx5_vhca_page_tracker tracker;
	struct workqueue_struct *cb_wq;
	struct notifier_block nb;
	struct mlx5_core_dev *mdev;
};

enum {
	MLX5VF_QUERY_INC = (1UL << 0),
	MLX5VF_QUERY_FINAL = (1UL << 1),
	MLX5VF_QUERY_CLEANUP = (1UL << 2),
};

int mlx5vf_cmd_suspend_vhca(struct mlx5vf_pci_core_device *mvdev, u16 op_mod);
int mlx5vf_cmd_resume_vhca(struct mlx5vf_pci_core_device *mvdev, u16 op_mod);
int mlx5vf_cmd_query_vhca_migration_state(struct mlx5vf_pci_core_device *mvdev,
					  size_t *state_size, u64 *total_size,
					  u8 query_flags);
void mlx5vf_cmd_set_migratable(struct mlx5vf_pci_core_device *mvdev,
			       const struct vfio_migration_ops *mig_ops,
			       const struct vfio_log_ops *log_ops);
void mlx5vf_cmd_remove_migratable(struct mlx5vf_pci_core_device *mvdev);
void mlx5vf_cmd_close_migratable(struct mlx5vf_pci_core_device *mvdev);
int mlx5vf_cmd_save_vhca_state(struct mlx5vf_pci_core_device *mvdev,
			       struct mlx5_vf_migration_file *migf,
			       struct mlx5_vhca_data_buffer *buf, bool inc,
			       bool track);
int mlx5vf_cmd_load_vhca_state(struct mlx5vf_pci_core_device *mvdev,
			       struct mlx5_vf_migration_file *migf,
			       struct mlx5_vhca_data_buffer *buf);
int mlx5vf_cmd_alloc_pd(struct mlx5_vf_migration_file *migf);
void mlx5vf_cmd_dealloc_pd(struct mlx5_vf_migration_file *migf);
void mlx5fv_cmd_clean_migf_resources(struct mlx5_vf_migration_file *migf);
struct mlx5_vhca_data_buffer *
mlx5vf_alloc_data_buffer(struct mlx5_vf_migration_file *migf, u32 npages,
			 enum dma_data_direction dma_dir);
void mlx5vf_free_data_buffer(struct mlx5_vhca_data_buffer *buf);
struct mlx5_vhca_data_buffer *
mlx5vf_get_data_buffer(struct mlx5_vf_migration_file *migf, u32 npages,
		       enum dma_data_direction dma_dir);
void mlx5vf_put_data_buffer(struct mlx5_vhca_data_buffer *buf);
static inline struct page *
mlx5vf_get_migration_page(struct mlx5_vhca_data_buffer *buf,
			  unsigned long offset)
{
	int page_entry = offset / PAGE_SIZE;

	if (page_entry >= buf->npages)
		return NULL;

	return buf->page_list[page_entry];
}
void mlx5vf_state_mutex_unlock(struct mlx5vf_pci_core_device *mvdev);
void mlx5vf_disable_fds(struct mlx5vf_pci_core_device *mvdev,
			enum mlx5_vf_migf_state *last_save_state);
void mlx5vf_mig_file_cleanup_cb(struct work_struct *_work);
void mlx5vf_mig_file_set_save_work(struct mlx5_vf_migration_file *migf,
				   u8 chunk_num, size_t next_required_umem_size);
int mlx5vf_start_page_tracker(struct vfio_device *vdev,
		struct rb_root_cached *ranges, u32 nnodes, u64 *page_size);
int mlx5vf_stop_page_tracker(struct vfio_device *vdev);
int mlx5vf_tracker_read_and_clear(struct vfio_device *vdev, unsigned long iova,
			unsigned long length, struct iova_bitmap *dirty);
#endif /* MLX5_VFIO_CMD_H */
