/*
 * Color decompression engine support
 *
 * Copyright (c) 2014, NVIDIA Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/nvhost.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>

#include "gk20a.h"
#include "channel_gk20a.h"
#include "mm_gk20a.h"
#include "cde_gk20a.h"
#include "fence_gk20a.h"
#include "gr_gk20a.h"
#include "debug_gk20a.h"

#include "hw_ccsr_gk20a.h"
#include "hw_pbdma_gk20a.h"

void gk20a_cde_dump(struct gk20a_cde_ctx *cde_ctx)
{
	int i;
	for (i = 0; i < cde_ctx->num_bufs; i++) {
		struct gk20a_cde_mem_desc *target_mem = cde_ctx->mem + i;
		u32 *target_mem_ptr = target_mem->cpuva;
		int j = 0;

		gk20a_dbg(gpu_dbg_cde, "cde: buffer=%d, size=%zu, gpuva=%llx\n",
			  i, target_mem->num_bytes, target_mem->gpu_va);

		for (j = 0; j < target_mem->num_bytes / sizeof(u32); j++)
			gk20a_dbg(gpu_dbg_cde, "0x%08x ", target_mem_ptr[j]);
		gk20a_dbg(gpu_dbg_cde, "\n\n");
	}
}

static void gk20a_deinit_cde_img(struct gk20a_cde_ctx *cde_ctx)
{
	struct device *dev = &cde_ctx->pdev->dev;
	int i;

	for (i = 0; i < cde_ctx->num_bufs; i++) {
		struct gk20a_cde_mem_desc *mem = cde_ctx->mem + i;
		gk20a_gmmu_unmap(cde_ctx->vm, mem->gpu_va, mem->num_bytes, 1);
		gk20a_free_sgtable(&mem->sgt);
		dma_free_coherent(dev, mem->num_bytes, mem->cpuva, mem->iova);
	}

	for (i = 0; i < cde_ctx->num_obj_ids; i++)
		gk20a_free_obj_ctx(cde_ctx->ch,
			&(struct nvhost_free_obj_ctx_args)
			{ cde_ctx->obj_ids[i] });

	kfree(cde_ctx->init_cmd);
	kfree(cde_ctx->convert_cmd);

	cde_ctx->convert_cmd = NULL;
	cde_ctx->init_cmd = NULL;
	cde_ctx->num_bufs = 0;
	cde_ctx->num_obj_ids = 0;
	cde_ctx->num_params = 0;
	cde_ctx->init_cmd_num_entries = 0;
	cde_ctx->convert_cmd_num_entries = 0;
}

static int gk20a_cde_remove(struct gk20a_cde_ctx *cde_ctx)
{
	struct gk20a *g = cde_ctx->g;
	struct channel_gk20a *ch = cde_ctx->ch;
	struct vm_gk20a *vm = ch->vm;

	/* free the channel */
	gk20a_free_channel(cde_ctx->ch, true);

	/* ..then release mapped memory */
	gk20a_deinit_cde_img(cde_ctx);
	gk20a_gmmu_unmap(vm, cde_ctx->backing_store_vaddr,
			 g->gr.compbit_store.size, 1);

	return 0;
}

int gk20a_cde_destroy(struct gk20a *g)
{
	struct gk20a_cde_app *cde_app = &g->cde_app;
	struct gk20a_cde_ctx *cde_ctx = cde_app->cde_ctx;
	int ret, i;

	if (!cde_app->initialised)
		return 0;

	for (i = 0; i < ARRAY_SIZE(cde_app->cde_ctx); i++, cde_ctx++)
		ret = gk20a_cde_remove(cde_ctx);

	cde_app->initialised = false;
	return ret;
}

static int gk20a_init_cde_buf(struct gk20a_cde_ctx *cde_ctx,
			      const struct firmware *img,
			      struct gk20a_cde_hdr_buf *buf)
{
	struct device *dev = &cde_ctx->pdev->dev;
	struct gk20a_cde_mem_desc *mem;
	int err;

	/* check that the file can hold the buf */
	if (buf->data_byte_offset != 0 &&
	    buf->data_byte_offset + buf->num_bytes > img->size) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid data section. buffer idx = %d",
			   cde_ctx->num_bufs);
		return -EINVAL;
	}

	/* check that we have enough buf elems available */
	if (cde_ctx->num_bufs > MAX_CDE_BUFS) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid data section. buffer idx = %d",
			   cde_ctx->num_bufs);
		return -ENOMEM;
	}

	/* allocate buf */
	mem = cde_ctx->mem + cde_ctx->num_bufs;
	mem->num_bytes = buf->num_bytes;
	mem->cpuva = dma_alloc_coherent(dev, mem->num_bytes, &mem->iova,
					GFP_KERNEL);
	if (!mem->cpuva) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: could not allocate device memory. buffer idx = %d",
			   cde_ctx->num_bufs);
		return -ENOMEM;
	}

	err = gk20a_get_sgtable(dev, &mem->sgt, mem->cpuva, mem->iova,
				mem->num_bytes);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: could not get sg table. buffer idx = %d",
			   cde_ctx->num_bufs);
		err = -ENOMEM;
		goto err_get_sgtable;
	}

	mem->gpu_va = gk20a_gmmu_map(cde_ctx->vm, &mem->sgt, mem->num_bytes, 0,
				     gk20a_mem_flag_none);
	if (!mem->gpu_va) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: could not map buffer to gpuva. buffer idx = %d",
			   cde_ctx->num_bufs);
		err = -ENOMEM;
		goto err_map_buffer;
	}

	/* copy the content */
	if (buf->data_byte_offset != 0)
		memcpy(mem->cpuva, img->data + buf->data_byte_offset,
		       buf->num_bytes);

	cde_ctx->num_bufs++;

	return 0;

err_map_buffer:
	gk20a_free_sgtable(&mem->sgt);
	kfree(mem->sgt);
err_get_sgtable:
	dma_free_coherent(dev, mem->num_bytes, &mem->cpuva, mem->iova);
	return err;
}

static int gk20a_replace_data(struct gk20a_cde_ctx *cde_ctx, void *target,
			      int type, s32 shift, u64 mask, u64 value)
{
	u32 *target_mem_ptr = target;
	u64 *target_mem_ptr_u64 = target;
	u64 current_value, new_value;

	value = (shift >= 0) ? value << shift : value >> -shift;
	value &= mask;

	/* read current data from the location */
	if (type == TYPE_PARAM_TYPE_U32)
		current_value = *target_mem_ptr;
	else if (type == TYPE_PARAM_TYPE_U64_LITTLE)
		current_value = *target_mem_ptr_u64;
	else if (type == TYPE_PARAM_TYPE_U64_BIG) {
		current_value = *target_mem_ptr_u64;
		current_value = (u64)(current_value >> 32) |
			(u64)(current_value << 32);
	} else {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: unknown type. type=%d",
			   type);
		return -EINVAL;
	}

	current_value &= ~mask;
	new_value = current_value | value;

	/* store the element data back */
	if (type == TYPE_PARAM_TYPE_U32)
		*target_mem_ptr = (u32)new_value;
	else if (type == TYPE_PARAM_TYPE_U64_LITTLE)
		*target_mem_ptr_u64 = new_value;
	else  {
		new_value = (u64)(new_value >> 32) |
			(u64)(new_value << 32);
		*target_mem_ptr_u64 = new_value;
	}

	return 0;
}

static int gk20a_init_cde_replace(struct gk20a_cde_ctx *cde_ctx,
				      const struct firmware *img,
				      struct gk20a_cde_hdr_replace *replace)
{
	struct gk20a_cde_mem_desc *source_mem;
	struct gk20a_cde_mem_desc *target_mem;
	u32 *target_mem_ptr;
	u64 vaddr;
	int err;

	if (replace->target_buf >= cde_ctx->num_bufs ||
	    replace->source_buf >= cde_ctx->num_bufs) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid buffer. target_buf=%u, source_buf=%u, num_bufs=%d",
			   replace->target_buf, replace->source_buf,
			   cde_ctx->num_bufs);
		return -EINVAL;
	}

	source_mem = cde_ctx->mem + replace->source_buf;
	target_mem = cde_ctx->mem + replace->target_buf;
	target_mem_ptr = target_mem->cpuva;

	if (source_mem->num_bytes < (replace->source_byte_offset + 3) ||
	    target_mem->num_bytes < (replace->target_byte_offset + 3)) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid buffer offsets. target_buf_offs=%lld, source_buf_offs=%lld, source_buf_size=%zu, dest_buf_size=%zu",
			   replace->target_byte_offset,
			   replace->source_byte_offset,
			 source_mem->num_bytes,
			 target_mem->num_bytes);
		return -EINVAL;
	}

	/* calculate the target pointer */
	target_mem_ptr += (replace->target_byte_offset / sizeof(u32));

	/* determine patch value */
	vaddr = source_mem->gpu_va + replace->source_byte_offset;
	err = gk20a_replace_data(cde_ctx, target_mem_ptr, replace->type,
				 replace->shift, replace->mask,
				 vaddr);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: replace failed. err=%d, target_buf=%u, target_buf_offs=%lld, source_buf=%u, source_buf_offs=%lld",
			   err, replace->target_buf,
			   replace->target_byte_offset,
			   replace->source_buf,
			   replace->source_byte_offset);
	}

	return err;
}

static int gk20a_cde_patch_params(struct gk20a_cde_ctx *cde_ctx)
{
	struct gk20a *g = cde_ctx->g;
	struct gk20a_cde_mem_desc *target_mem;
	u32 *target_mem_ptr;
	u64 new_data;
	int user_id = 0, i, err;

	for (i = 0; i < cde_ctx->num_params; i++) {
		struct gk20a_cde_hdr_param *param = cde_ctx->params + i;
		target_mem = cde_ctx->mem + param->target_buf;
		target_mem_ptr = target_mem->cpuva;
		target_mem_ptr += (param->target_byte_offset / sizeof(u32));

		switch (param->id) {
		case TYPE_PARAM_COMPTAGS_PER_CACHELINE:
			new_data = g->gr.comptags_per_cacheline;
			break;
		case TYPE_PARAM_GPU_CONFIGURATION:
			new_data = g->ltc_count * g->gr.slices_per_ltc *
				g->gr.cacheline_size;
			break;
		case TYPE_PARAM_FIRSTPAGEOFFSET:
			new_data = cde_ctx->src_param_offset;
			break;
		case TYPE_PARAM_NUMPAGES:
			new_data = cde_ctx->src_param_lines;
			break;
		case TYPE_PARAM_BACKINGSTORE:
			new_data = cde_ctx->backing_store_vaddr;
			break;
		case TYPE_PARAM_DESTINATION:
			new_data = cde_ctx->dest_vaddr;
			break;
		case TYPE_PARAM_DESTINATION_SIZE:
			new_data = cde_ctx->dest_size;
			break;
		case TYPE_PARAM_BACKINGSTORE_SIZE:
			new_data = g->gr.compbit_store.size;
			break;
		case TYPE_PARAM_SOURCE_SMMU_ADDR:
			new_data = gk20a_mm_gpuva_to_iova(cde_ctx->vm,
							  cde_ctx->src_vaddr);
			if (new_data == 0)
				err = -EINVAL;
			break;
		case TYPE_PARAM_BACKINGSTORE_BASE_HW:
			new_data = g->gr.compbit_store.base_hw;
			break;
		default:
			user_id = param->id - NUM_RESERVED_PARAMS;
			if (user_id < 0 || user_id >= MAX_CDE_USER_PARAMS)
				continue;
			new_data = cde_ctx->user_param_values[user_id];
		}

		gk20a_dbg(gpu_dbg_cde, "cde: patch: idx_in_file=%d	param_id=%d	target_buf=%u	target_byte_offset=%lld	data_value=0x%llx	data_offset/data_diff=%lld	data_type=%d	data_shift=%d	data_mask=0x%llx",
			  i, param->id, param->target_buf,
			  param->target_byte_offset, new_data,
			  param->data_offset, param->type, param->shift,
			  param->mask);

		new_data += param->data_offset;

		err = gk20a_replace_data(cde_ctx, target_mem_ptr, param->type,
					 param->shift, param->mask, new_data);

		if (err) {
			gk20a_warn(&cde_ctx->pdev->dev, "cde: patch failed. err=%d, idx=%d, id=%d, target_buf=%u, target_buf_offs=%lld, patch_value=%llu",
				   err, i, param->id, param->target_buf,
				   param->target_byte_offset, new_data);
			return err;
		}
	}

	return 0;
}

static int gk20a_init_cde_param(struct gk20a_cde_ctx *cde_ctx,
				const struct firmware *img,
				struct gk20a_cde_hdr_param *param)
{
	struct gk20a_cde_mem_desc *target_mem;

	if (param->target_buf >= cde_ctx->num_bufs) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid buffer parameter. param idx = %d, target_buf=%u, num_bufs=%u",
			   cde_ctx->num_params, param->target_buf,
			   cde_ctx->num_bufs);
		return -EINVAL;
	}

	target_mem = cde_ctx->mem + param->target_buf;
	if (target_mem->num_bytes < (param->target_byte_offset + 3)) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid buffer parameter. param idx = %d, target_buf_offs=%lld, target_buf_size=%zu",
			   cde_ctx->num_params, param->target_byte_offset,
			   target_mem->num_bytes);
		return -EINVAL;
	}

	/* does this parameter fit into our parameter structure */
	if (cde_ctx->num_params >= MAX_CDE_PARAMS) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: no room for new parameters param idx = %d",
			   cde_ctx->num_params);
		return -ENOMEM;
	}

	/* is the given id valid? */
	if (param->id >= NUM_RESERVED_PARAMS + MAX_CDE_USER_PARAMS) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: parameter id is not valid. param idx = %d, id=%u, max=%u",
			   param->id, cde_ctx->num_params,
			   NUM_RESERVED_PARAMS + MAX_CDE_USER_PARAMS);
		return -EINVAL;
	}

	cde_ctx->params[cde_ctx->num_params] = *param;
	cde_ctx->num_params++;

	return 0;
}

static int gk20a_init_cde_required_class(struct gk20a_cde_ctx *cde_ctx,
					 const struct firmware *img,
					 u32 required_class)
{
	struct nvhost_alloc_obj_ctx_args alloc_obj_ctx;
	int err;

	if (cde_ctx->num_obj_ids >= MAX_CDE_OBJ_IDS) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: running out of class ids");
		return -ENOMEM;
	}

	alloc_obj_ctx.class_num = required_class;
	alloc_obj_ctx.padding = 0;

	err = gk20a_alloc_obj_ctx(cde_ctx->ch, &alloc_obj_ctx);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: failed to allocate ctx. err=%d",
			   err);
		return err;
	}

	cde_ctx->obj_ids[cde_ctx->num_obj_ids] = alloc_obj_ctx.obj_id;
	cde_ctx->num_obj_ids++;

	return 0;
}

static int gk20a_init_cde_command(struct gk20a_cde_ctx *cde_ctx,
				  const struct firmware *img,
				  u32 op,
				  struct gk20a_cde_cmd_elem *cmd_elem,
				  u32 num_elems)
{
	struct nvhost_gpfifo **gpfifo, *gpfifo_elem;
	u32 *num_entries;
	int i;

	/* check command type */
	if (op == TYPE_BUF_COMMAND_INIT) {
		gpfifo = &cde_ctx->init_cmd;
		num_entries = &cde_ctx->init_cmd_num_entries;
	} else if (op == TYPE_BUF_COMMAND_CONVERT) {
		gpfifo = &cde_ctx->convert_cmd;
		num_entries = &cde_ctx->convert_cmd_num_entries;
	} else {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: unknown command. op=%u",
			   op);
		return -EINVAL;
	}

	/* allocate gpfifo entries to be pushed */
	*gpfifo = kzalloc(sizeof(struct nvhost_gpfifo) * num_elems,
			  GFP_KERNEL);
	if (!*gpfifo) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: could not allocate memory for gpfifo entries");
		return -ENOMEM;
	}

	gpfifo_elem = *gpfifo;
	for (i = 0; i < num_elems; i++, cmd_elem++, gpfifo_elem++) {
		struct gk20a_cde_mem_desc *target_mem;

		/* validate the current entry */
		if (cmd_elem->target_buf >= cde_ctx->num_bufs) {
			gk20a_warn(&cde_ctx->pdev->dev, "cde: target buffer is not available (target=%u, num_bufs=%u)",
				   cmd_elem->target_buf, cde_ctx->num_bufs);
			return -EINVAL;
		}

		target_mem = cde_ctx->mem + cmd_elem->target_buf;
		if (target_mem->num_bytes <
		    cmd_elem->target_byte_offset + cmd_elem->num_bytes) {
			gk20a_warn(&cde_ctx->pdev->dev, "cde: target buffer cannot hold all entries (target_size=%zu, target_byte_offset=%lld, num_bytes=%llu)",
				   target_mem->num_bytes,
				   cmd_elem->target_byte_offset,
				   cmd_elem->num_bytes);
			return -EINVAL;
		}

		/* store the element into gpfifo */
		gpfifo_elem->entry0 =
			u64_lo32(target_mem->gpu_va +
			cmd_elem->target_byte_offset);
		gpfifo_elem->entry1 =
			u64_hi32(target_mem->gpu_va +
			cmd_elem->target_byte_offset) |
			pbdma_gp_entry1_length_f(cmd_elem->num_bytes /
						 sizeof(u32));
	}

	*num_entries = num_elems;
	return 0;
}

static int gk20a_init_cde_img(struct gk20a_cde_ctx *cde_ctx,
			      const struct firmware *img)
{
	u32 *data = (u32 *)img->data;
	u32 version, num_of_elems;
	struct gk20a_cde_hdr_elem *elem;
	u32 min_size = 0;
	int err = 0;
	int i;

	min_size += 2 * sizeof(u32);
	if (img->size < min_size) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: invalid image header");
		return -EINVAL;
	}

	version = data[0];
	num_of_elems = data[1];

	min_size += num_of_elems * sizeof(*elem);
	if (img->size < min_size) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: bad image");
		return -EINVAL;
	}

	elem = (struct gk20a_cde_hdr_elem *)&data[2];
	for (i = 0; i < num_of_elems; i++) {
		int err = 0;
		switch (elem->type) {
		case TYPE_BUF:
			err = gk20a_init_cde_buf(cde_ctx, img, &elem->buf);
			break;
		case TYPE_REPLACE:
			err = gk20a_init_cde_replace(cde_ctx, img,
						     &elem->replace);
			break;
		case TYPE_PARAM:
			err = gk20a_init_cde_param(cde_ctx, img, &elem->param);
			break;
		case TYPE_REQUIRED_CLASS:
			err = gk20a_init_cde_required_class(cde_ctx, img,
				elem->required_class);
			break;
		case TYPE_COMMAND:
		{
			struct gk20a_cde_cmd_elem *cmd = (void *)
				&img->data[elem->command.data_byte_offset];
			err = gk20a_init_cde_command(cde_ctx, img,
				elem->command.op, cmd,
				elem->command.num_entries);
			break;
		}
		default:
			gk20a_warn(&cde_ctx->pdev->dev, "cde: unknown header element");
			err = -EINVAL;
		}

		if (err)
			goto deinit_image;

		elem++;
	}

	if (!cde_ctx->init_cmd || !cde_ctx->init_cmd_num_entries) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: convert command not defined");
		err = -EINVAL;
		goto deinit_image;
	}

	if (!cde_ctx->convert_cmd || !cde_ctx->convert_cmd_num_entries) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: convert command not defined");
		err = -EINVAL;
		goto deinit_image;
	}

	return 0;

deinit_image:
	gk20a_deinit_cde_img(cde_ctx);
	return err;
}

static int gk20a_cde_execute_buffer(struct gk20a_cde_ctx *cde_ctx,
				    u32 op, struct nvhost_fence *fence,
				    u32 flags, struct gk20a_fence **fence_out)
{
	struct nvhost_gpfifo *gpfifo = NULL;
	int num_entries = 0;

	/* check command type */
	if (op == TYPE_BUF_COMMAND_INIT) {
		gpfifo = cde_ctx->init_cmd;
		num_entries = cde_ctx->init_cmd_num_entries;
	} else if (op == TYPE_BUF_COMMAND_CONVERT) {
		gpfifo = cde_ctx->convert_cmd;
		num_entries = cde_ctx->convert_cmd_num_entries;
	} else {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: unknown buffer");
		return -EINVAL;
	}

	if (gpfifo == NULL || num_entries == 0) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: buffer not available");
		return -ENOSYS;
	}

	return gk20a_submit_channel_gpfifo(cde_ctx->ch, gpfifo,
					   num_entries, flags, fence, fence_out);
}

int gk20a_cde_convert(struct gk20a *g, struct dma_buf *src,
		      struct dma_buf *dst,
		      s32 dst_kind, u64 dst_byte_offset,
		      u32 dst_size, struct nvhost_fence *fence,
		      u32 __flags, struct gk20a_cde_param *params,
		      int num_params, struct gk20a_fence **fence_out)
{
	struct gk20a_cde_app *cde_app = &g->cde_app;
	struct gk20a_comptags comptags;
	struct gk20a_cde_ctx *cde_ctx;
	u64 dst_vaddr = 0, src_vaddr = 0;
	u32 flags;
	int err, i;

	if (!cde_app->initialised) {
		gk20a_warn(&g->dev->dev, "cde: conversion requrest but no image has been provided");
		return -ENOSYS;
	}

	mutex_lock(&cde_app->mutex);

	/* pick next free cde context */
	cde_ctx = cde_app->cde_ctx + cde_app->cde_ctx_ptr;
	cde_app->cde_ctx_ptr = (cde_app->cde_ctx_ptr + 1) %
		ARRAY_SIZE(cde_app->cde_ctx);

	/* First, map the buffers to local va */

	/* ensure that the dst buffer has drvdata */
	err = gk20a_dmabuf_alloc_drvdata(dst, &g->dev->dev);
	if (err)
		goto exit_unlock;

	/* map the destination buffer */
	get_dma_buf(dst); /* a ref for gk20a_vm_map */
	dst_vaddr = gk20a_vm_map(g->cde_app.vm, dst, 0,
				 0, dst_kind, NULL, true,
				 gk20a_mem_flag_none,
				 0, 0);
	if (!dst_vaddr) {
		dma_buf_put(dst);
		err = -EINVAL;
		goto exit_unlock;
	}

	/* ensure that the src buffer has drvdata */
	err = gk20a_dmabuf_alloc_drvdata(src, &g->dev->dev);
	if (err)
		goto exit_unlock;

	/* map the source buffer to prevent premature release */
	get_dma_buf(src); /* a ref for gk20a_vm_map */
	src_vaddr = gk20a_vm_map(g->cde_app.vm, src, 0,
				 0, dst_kind, NULL, true,
				 gk20a_mem_flag_none,
				 0, 0);
	if (!src_vaddr) {
		dma_buf_put(src);
		err = -EINVAL;
		goto exit_unlock;
	}

	if (!dst_size)
		dst_size = dst->size - dst_byte_offset;

	/* reload buffer converter if it has failed */
	if (cde_ctx->ch->has_timedout) {
		mutex_unlock(&cde_app->mutex);
		err = gk20a_cde_reload(g);
		if (err)
			return err;
		mutex_lock(&cde_app->mutex);
	}

	/* wait for channel idle */
	err = gk20a_channel_finish(cde_ctx->ch, 2000);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: old work could not be finished");
		goto exit_unlock;
	}

	/* disable the channel */
	gk20a_writel(g, ccsr_channel_r(cde_ctx->ch->hw_chid),
		gk20a_readl(g, ccsr_channel_r(cde_ctx->ch->hw_chid)) |
		ccsr_channel_enable_clr_true_f());
	gk20a_fifo_preempt_channel(g, cde_ctx->ch->hw_chid);
	channel_gk20a_unbind(&g->fifo.channel[cde_ctx->ch->hw_chid]);

	/* reinitialise the graphics context of the channel */
	gr_gk20a_load_golden_ctx_image(g, cde_ctx->ch);

	/* re-enable the channel */
	g->ops.fifo.bind_channel(&g->fifo.channel[cde_ctx->ch->hw_chid]);
	gk20a_writel(g, ccsr_channel_r(cde_ctx->ch->hw_chid),
		gk20a_readl(g, ccsr_channel_r(cde_ctx->ch->hw_chid)) |
		ccsr_channel_enable_set_true_f());

	/* store source buffer compression tags */
	gk20a_get_comptags(&g->dev->dev, src, &comptags);
	cde_ctx->src_vaddr = src_vaddr;
	cde_ctx->src_param_offset = comptags.offset;
	cde_ctx->src_param_lines = comptags.lines;

	/* store information about destination */
	cde_ctx->dest_vaddr = dst_vaddr + dst_byte_offset;
	cde_ctx->dest_size = dst_size;

	/* remove existing argument data */
	memset(cde_ctx->user_param_values, 0,
	       sizeof(cde_ctx->user_param_values));

	/* read user space arguments for the conversion */
	for (i = 0; i < num_params; i++) {
		struct gk20a_cde_param *param = params + i;
		int id = param->id - NUM_RESERVED_PARAMS;

		if (id < 0 || id >= MAX_CDE_USER_PARAMS) {
			gk20a_warn(&cde_ctx->pdev->dev, "cde: unknown user parameter");
			err = -EINVAL;
			goto exit_unlock;
		}
		cde_ctx->user_param_values[id] = param->value;
	}

	/* patch data */
	err = gk20a_cde_patch_params(cde_ctx);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: failed to patch parameters");
		goto exit_unlock;
	}

	gk20a_dbg(gpu_dbg_cde, "cde: buffer=cbc, size=%zu, gpuva=%llx\n",
		 g->gr.compbit_store.size, cde_ctx->backing_store_vaddr);
	gk20a_dbg(gpu_dbg_cde, "cde: buffer=dst, size=%llu, gpuva=%llx\n",
		 cde_ctx->dest_size, cde_ctx->dest_vaddr);
	gk20a_cde_dump(cde_ctx);

	/* execute the init push buffer */
	err = gk20a_cde_execute_buffer(cde_ctx, TYPE_BUF_COMMAND_INIT,
				       NULL, 0, NULL);
	if (err)
		goto exit_unlock;

	/* take always the postfence as it is needed for protecting the
	 * cde context */
	flags = __flags | NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_GET;

	/* execute the conversion buffer */
	err = gk20a_cde_execute_buffer(cde_ctx, TYPE_BUF_COMMAND_CONVERT,
				       fence, flags, fence_out);

exit_unlock:

	/* unmap the buffers - channel holds references to them now */
	if (dst_vaddr)
		gk20a_vm_unmap(g->cde_app.vm, dst_vaddr);
	if (src_vaddr)
		gk20a_vm_unmap(g->cde_app.vm, src_vaddr);

	mutex_unlock(&cde_app->mutex);

	return err;
}

int gk20a_cde_load(struct gk20a_cde_ctx *cde_ctx)
{
	struct gk20a *g = cde_ctx->g;
	const struct firmware *img;
	struct channel_gk20a *ch;
	struct gr_gk20a *gr = &g->gr;
	int err = 0;
	u64 vaddr;

	img = gk20a_request_firmware(g, "gpu2cde.bin");
	if (!img) {
		dev_err(&cde_ctx->pdev->dev, "cde: could not fetch the firmware");
		return -ENOSYS;
	}

	ch = gk20a_open_new_channel(g);
	if (!ch) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: gk20a channel not available");
		err = -ENOMEM;
		goto err_get_gk20a_channel;
	}

	/* bind the channel to the vm */
	gk20a_vm_get(&g->mm.pmu.vm);
	ch->vm = &g->mm.pmu.vm;
	err = channel_gk20a_commit_va(ch);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: could not bind vm");
		goto err_commit_va;
	}

	/* allocate gpfifo (1024 should be more than enough) */
	err = gk20a_alloc_channel_gpfifo(ch,
		&(struct nvhost_alloc_gpfifo_args){1024, 0});
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: unable to allocate gpfifo");
		goto err_alloc_gpfifo;
	}

	/* map backing store to gpu virtual space */
	vaddr = gk20a_gmmu_map(ch->vm, &gr->compbit_store.sgt,
			       g->gr.compbit_store.size, 0,
			       gk20a_mem_flag_none);

	if (!vaddr) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: cannot map compression bit backing store");
		goto err_map_backingstore;
	}

	/* store initialisation data */
	cde_ctx->ch = ch;
	cde_ctx->vm = ch->vm;
	cde_ctx->backing_store_vaddr = vaddr;

	/* initialise the firmware */
	err = gk20a_init_cde_img(cde_ctx, img);
	if (err) {
		gk20a_warn(&cde_ctx->pdev->dev, "cde: image initialisation failed");
		goto err_init_cde_img;
	}

	/* initialisation done */
	release_firmware(img);

	return 0;

err_init_cde_img:
	gk20a_gmmu_unmap(ch->vm, vaddr, g->gr.compbit_store.size, 1);
err_map_backingstore:
err_alloc_gpfifo:
	gk20a_vm_put(ch->vm);
err_commit_va:
err_get_gk20a_channel:
	release_firmware(img);
	dev_err(&cde_ctx->pdev->dev, "cde: couldn't initialise buffer converter: %d",
		err);
	return err;
}

int gk20a_cde_reload(struct gk20a *g)
{
	struct gk20a_cde_app *cde_app = &g->cde_app;
	struct gk20a_cde_ctx *cde_ctx = cde_app->cde_ctx;
	int err, i;

	if (!cde_app->initialised) {
		gk20a_busy(g->dev);
		gk20a_init_cde_support(g);
		gk20a_idle(g->dev);
		if (!cde_app->initialised)
			return -ENOSYS;
		return 0;
	}

	gk20a_busy(g->dev);
	mutex_lock(&cde_app->mutex);
	for (i = 0; i < ARRAY_SIZE(cde_app->cde_ctx); i++, cde_ctx++) {
		gk20a_cde_remove(cde_ctx);
		err = gk20a_cde_load(cde_ctx);
	}

	cde_app->cde_ctx_ptr = 0;
	mutex_unlock(&cde_app->mutex);

	gk20a_idle(g->dev);
	return err;
}

int gk20a_init_cde_support(struct gk20a *g)
{
	struct gk20a_cde_app *cde_app = &g->cde_app;
	struct gk20a_cde_ctx *cde_ctx = cde_app->cde_ctx;
	int ret, i;

	if (cde_app->initialised)
		return 0;

	mutex_init(&cde_app->mutex);
	mutex_lock(&cde_app->mutex);

	for (i = 0; i < ARRAY_SIZE(cde_app->cde_ctx); i++, cde_ctx++) {
		cde_ctx->g = g;
		cde_ctx->pdev = g->dev;
		ret = gk20a_cde_load(cde_ctx);
		if (ret)
			goto err_init_instance;
	}

	/* take shadow to the vm for general usage */
	cde_app->vm = cde_app->cde_ctx->vm;

	cde_app->cde_ctx_ptr = 0;
	cde_app->initialised = true;
	mutex_unlock(&cde_app->mutex);

	return 0;

err_init_instance:

	/* deinitialise initialised channels */
	while (i--) {
		gk20a_cde_remove(cde_ctx);
		cde_ctx--;
	}
	return ret;
}

enum cde_launch_patch_offset {
	/* dst buffer width in roptiles */
	PATCH_USER_CONST_XTILES,
	/* dst buffer height in roptiles */
	PATCH_USER_CONST_YTILES,
	/* dst buffer log2(block height) */
	PATCH_USER_CONST_BLOCKHEIGHTLOG2,
	/* dst buffer pitch in bytes */
	PATCH_USER_CONST_DSTPITCH,
	/* dst buffer write offset */
	PATCH_USER_CONST_DSTOFFSET,
	/* comp cache index of the first page of the surface,
	 * kernel looks it up from PTE */
	PATCH_USER_CONST_FIRSTPAGEOFFSET,
	/* gmmu translated surface address, kernel fills */
	PATCH_USER_CONST_SURFADDR,
	/* dst buffer address >> 8, kernel fills */
	PATCH_VPC_DSTIMAGE_ADDR,
	/* dst buffer address >> 8, kernel fills */
	PATCH_VPC_DSTIMAGE_ADDR2,
	/* dst buffer size - 1, kernel fills */
	PATCH_VPC_DSTIMAGE_SIZE_MINUS_ONE,
	/* dst buffer size - 1, kernel fills */
	PATCH_VPC_DSTIMAGE_SIZE_MINUS_ONE2,
	/* dst buffer size, kernel fills */
	PATCH_VPC_DSTIMAGE_SIZE,
	/* dst buffer width in roptiles / work group width */
	PATCH_VPC_CURRENT_GRID_SIZE_X,
	/* dst buffer height in roptiles / work group height */
	PATCH_VPC_CURRENT_GRID_SIZE_Y,
	/* 1 */
	PATCH_VPC_CURRENT_GRID_SIZE_Z,
	/* work group width, 16 seems to be quite optimal */
	PATCH_VPC_CURRENT_GROUP_SIZE_X,
	/* work group height, 8 seems to be quite optimal */
	PATCH_VPC_CURRENT_GROUP_SIZE_Y,
	/* 1 */
	PATCH_VPC_CURRENT_GROUP_SIZE_Z,
	/* same as PATCH_VPC_CURRENT_GRID_SIZE_X */
	PATCH_QMD_CTA_RASTER_WIDTH,
	/* same as PATCH_VPC_CURRENT_GRID_SIZE_Y */
	PATCH_QMD_CTA_RASTER_HEIGHT,
	/* same as PATCH_VPC_CURRENT_GRID_SIZE_Z */
	PATCH_QMD_CTA_RASTER_DEPTH,
	/* same as PATCH_VPC_CURRENT_GROUP_SIZE_X */
	PATCH_QMD_CTA_THREAD_DIMENSION0,
	/* same as PATCH_VPC_CURRENT_GROUP_SIZE_Y */
	PATCH_QMD_CTA_THREAD_DIMENSION1,
	/* same as PATCH_VPC_CURRENT_GROUP_SIZE_Z */
	PATCH_QMD_CTA_THREAD_DIMENSION2,

	NUM_CDE_LAUNCH_PATCHES
};

enum cde_launch_patch_id {
	PATCH_QMD_CTA_RASTER_WIDTH_ID = 1024,
	PATCH_QMD_CTA_RASTER_HEIGHT_ID = 1025,
	PATCH_QMD_CTA_RASTER_DEPTH_ID = 1026,
	PATCH_QMD_CTA_THREAD_DIMENSION0_ID = 1027,
	PATCH_QMD_CTA_THREAD_DIMENSION1_ID = 1028,
	PATCH_QMD_CTA_THREAD_DIMENSION2_ID = 1029,
	PATCH_USER_CONST_XTILES_ID = 1030,
	PATCH_USER_CONST_YTILES_ID = 1031,
	PATCH_USER_CONST_BLOCKHEIGHTLOG2_ID = 1032,
	PATCH_USER_CONST_DSTPITCH_ID = 1033,
	PATCH_USER_CONST_DSTOFFSET_ID = 1034,
	PATCH_VPC_CURRENT_GRID_SIZE_X_ID = 1035,
	PATCH_VPC_CURRENT_GRID_SIZE_Y_ID = 1036,
	PATCH_VPC_CURRENT_GRID_SIZE_Z_ID = 1037,
	PATCH_VPC_CURRENT_GROUP_SIZE_X_ID = 1038,
	PATCH_VPC_CURRENT_GROUP_SIZE_Y_ID = 1039,
	PATCH_VPC_CURRENT_GROUP_SIZE_Z_ID = 1040,
};

static int gk20a_buffer_convert_gpu_to_cde(
		struct gk20a *g, struct dma_buf *dmabuf, u32 consumer,
		u64 offset, u64 compbits_offset,
		u32 width, u32 height, u32 block_height_log2,
		u32 submit_flags, struct nvhost_fence *fence_in,
		struct gk20a_fence **fence_out)
{
	struct gk20a_cde_param params[NUM_CDE_LAUNCH_PATCHES];
	int param = 0;
	int err = 0;

	/* Compute per launch parameters */
	const bool transpose = (consumer == NVHOST_GPU_COMPBITS_CDEV);
	const int transposed_width = transpose ? height : width;
	const int transposed_height = transpose ? width : height;
	const int xtiles = (transposed_width + 7) >> 3;
	const int ytiles = (transposed_height + 7) >> 3;
	const int wgx = 16;
	const int wgy = 8;
	const int compbits_per_byte = 4; /* one byte stores 4 compbit pairs */
	const int dst_stride = 128; /* TODO chip constant */
	const int xalign = compbits_per_byte * wgx;
	const int yalign = wgy;
	const int tilepitch = roundup(xtiles, xalign) / compbits_per_byte;
	const int ytilesaligned = roundup(ytiles, yalign);
	const int gridw = roundup(tilepitch, wgx) / wgx;
	const int gridh = roundup(ytilesaligned, wgy) / wgy;

	if (xtiles > 4096 / 8 || ytiles > 4096 / 8)
		gk20a_warn(&g->dev->dev, "cde: surface is exceptionally large (xtiles=%d, ytiles=%d)",
			   xtiles, ytiles);

	gk20a_dbg(gpu_dbg_cde, "w=%d, h=%d, bh_log2=%d, compbits_offset=0x%llx",
		  width, height, block_height_log2, compbits_offset);
	gk20a_dbg(gpu_dbg_cde, "resolution (%d, %d) tiles (%d, %d) invocations (%d, %d)",
		  width, height, xtiles, ytiles, tilepitch, ytilesaligned);
	gk20a_dbg(gpu_dbg_cde, "group (%d, %d) grid (%d, %d)",
		  wgx, wgy, gridw, gridh);

	if (tilepitch % wgx != 0 || ytilesaligned % wgy != 0) {
		gk20a_warn(&g->dev->dev,
			"grid size (%d, %d) is not a multiple of work group size (%d, %d)",
			tilepitch, ytilesaligned, wgx, wgy);
		return -EINVAL;
	}

	/* Write parameters */
#define WRITE_PATCH(NAME, VALUE) \
		params[param++] = (struct gk20a_cde_param){NAME##_ID, 0, VALUE}
	WRITE_PATCH(PATCH_USER_CONST_XTILES, xtiles);
	WRITE_PATCH(PATCH_USER_CONST_YTILES, ytiles);
	WRITE_PATCH(PATCH_USER_CONST_BLOCKHEIGHTLOG2, block_height_log2);
	WRITE_PATCH(PATCH_USER_CONST_DSTPITCH, dst_stride);
	WRITE_PATCH(PATCH_USER_CONST_DSTOFFSET,
		    (transpose ? 4 : 0) | g->cde_app.shader_parameter);
	WRITE_PATCH(PATCH_VPC_CURRENT_GRID_SIZE_X, gridw);
	WRITE_PATCH(PATCH_VPC_CURRENT_GRID_SIZE_Y, gridh);
	WRITE_PATCH(PATCH_VPC_CURRENT_GRID_SIZE_Z, 1);
	WRITE_PATCH(PATCH_VPC_CURRENT_GROUP_SIZE_X, wgx);
	WRITE_PATCH(PATCH_VPC_CURRENT_GROUP_SIZE_Y, wgy);
	WRITE_PATCH(PATCH_VPC_CURRENT_GROUP_SIZE_Z, 1);
	WRITE_PATCH(PATCH_QMD_CTA_RASTER_WIDTH, gridw);
	WRITE_PATCH(PATCH_QMD_CTA_RASTER_HEIGHT, gridh);
	WRITE_PATCH(PATCH_QMD_CTA_RASTER_DEPTH, 1);
	WRITE_PATCH(PATCH_QMD_CTA_THREAD_DIMENSION0, wgx);
	WRITE_PATCH(PATCH_QMD_CTA_THREAD_DIMENSION1, wgy);
	WRITE_PATCH(PATCH_QMD_CTA_THREAD_DIMENSION2, 1);
#undef WRITE_PATCH

	gk20a_busy(g->dev);
	err = gk20a_init_cde_support(g);
	if (err)
		goto out;
	err = gk20a_cde_convert(g, dmabuf, dmabuf,
				0, /* dst kind */
				compbits_offset,
				0, /* dst_size, 0 = auto */
				fence_in, submit_flags,
				params, param, fence_out);
out:
	gk20a_idle(g->dev);
	return err;
}

int gk20a_prepare_compressible_read(
		struct gk20a *g, u32 buffer_fd, u32 request, u64 offset,
		u64 compbits_hoffset, u64 compbits_voffset,
		u32 width, u32 height, u32 block_height_log2,
		u32 submit_flags, struct nvhost_fence *fence,
		u32 *valid_compbits, struct gk20a_fence **fence_out)
{
	int err = 0;
	struct gk20a_buffer_state *state;
	struct dma_buf *dmabuf;
	u32 missing_bits;

	if (!g->cde_app.initialised) {
		err = gk20a_cde_reload(g);
		if (err)
			return err;
	}

	dmabuf = dma_buf_get(buffer_fd);
	if (IS_ERR(dmabuf))
		return -EINVAL;

	err = gk20a_dmabuf_get_state(dmabuf, dev_from_gk20a(g), offset, &state);
	if (err) {
		dma_buf_put(dmabuf);
		return err;
	}

	missing_bits = (state->valid_compbits ^ request) & request;

	mutex_lock(&state->lock);

	if (state->valid_compbits && request == NVHOST_GPU_COMPBITS_NONE) {

		gk20a_fence_put(state->fence);
		state->fence = NULL;
		/* state->fence = decompress();
		state->valid_compbits = 0; */
		err = -EINVAL;
		goto out;
	} else if (missing_bits) {
		struct gk20a_fence *new_fence = NULL;
		if ((state->valid_compbits & NVHOST_GPU_COMPBITS_GPU) &&
			(missing_bits & NVHOST_GPU_COMPBITS_CDEH)) {
			err = gk20a_buffer_convert_gpu_to_cde(
					g, dmabuf,
					NVHOST_GPU_COMPBITS_CDEH,
					offset, compbits_hoffset,
					width, height, block_height_log2,
					submit_flags, fence,
					&new_fence);
			if (err)
				goto out;

			/* CDEH bits generated, update state & fence */
			gk20a_fence_put(state->fence);
			state->fence = new_fence;
			state->valid_compbits |= NVHOST_GPU_COMPBITS_CDEH;
		}
		if ((state->valid_compbits & NVHOST_GPU_COMPBITS_GPU) &&
			(missing_bits & NVHOST_GPU_COMPBITS_CDEV)) {
			err = gk20a_buffer_convert_gpu_to_cde(
					g, dmabuf,
					NVHOST_GPU_COMPBITS_CDEV,
					offset, compbits_voffset,
					width, height, block_height_log2,
					submit_flags, fence,
					&new_fence);
			if (err)
				goto out;

			/* CDEH bits generated, update state & fence */
			gk20a_fence_put(state->fence);
			state->fence = new_fence;
			state->valid_compbits |= NVHOST_GPU_COMPBITS_CDEV;
		}
	}

	if (state->fence && fence_out)
		*fence_out = gk20a_fence_get(state->fence);

	if (valid_compbits)
		*valid_compbits = state->valid_compbits;

out:
	mutex_unlock(&state->lock);
	dma_buf_put(dmabuf);
	return 0;
}

int gk20a_mark_compressible_write(struct gk20a *g, u32 buffer_fd,
				  u32 valid_compbits, u64 offset)
{
	int err;
	struct gk20a_buffer_state *state;
	struct dma_buf *dmabuf;

	dmabuf = dma_buf_get(buffer_fd);
	if (IS_ERR(dmabuf)) {
		dev_err(dev_from_gk20a(g), "invalid dmabuf");
		return -EINVAL;
	}

	err = gk20a_dmabuf_get_state(dmabuf, dev_from_gk20a(g), offset, &state);
	if (err) {
		dev_err(dev_from_gk20a(g), "could not get state from dmabuf");
		dma_buf_put(dmabuf);
		return err;
	}

	mutex_lock(&state->lock);

	/* Update the compbits state. */
	state->valid_compbits = valid_compbits;

	/* Discard previous compbit job fence. */
	gk20a_fence_put(state->fence);
	state->fence = NULL;

	mutex_unlock(&state->lock);
	dma_buf_put(dmabuf);
	return 0;
}

static ssize_t gk20a_cde_reload_write(struct file *file,
	const char __user *userbuf, size_t count, loff_t *ppos)
{
	struct gk20a *g = file->private_data;
	gk20a_cde_reload(g);
	return count;
}

static const struct file_operations gk20a_cde_reload_fops = {
	.open		= simple_open,
	.write		= gk20a_cde_reload_write,
};

void gk20a_cde_debugfs_init(struct platform_device *dev)
{
	struct gk20a_platform *platform = platform_get_drvdata(dev);
	struct gk20a *g = get_gk20a(dev);

	debugfs_create_u32("cde_parameter", S_IWUSR | S_IRUGO,
			   platform->debugfs, &g->cde_app.shader_parameter);
	debugfs_create_file("reload_cde_firmware", S_IWUSR, platform->debugfs,
			    g, &gk20a_cde_reload_fops);
}