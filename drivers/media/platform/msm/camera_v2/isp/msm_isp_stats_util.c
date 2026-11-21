/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/io.h>
#include <linux/atomic.h>
#include <media/v4l2-subdev.h>
#include <media/msmb_isp.h>
#include "msm_isp_util.h"
#include "msm_camera_io_util.h"
#include "msm_isp_stats_util.h"

static int msm_isp_stats_cfg_ping_pong_address(struct vfe_device *vfe_dev,
	struct msm_vfe_stats_stream *stream_info, uint32_t pingpong_status,
	struct msm_isp_buffer **done_buf)
{
	int rc = -1;
	struct msm_isp_buffer *buf;
	uint32_t pingpong_bit = 0;
	uint32_t bufq_handle = stream_info->bufq_handle;
	uint32_t stats_pingpong_offset;
	uint32_t stats_idx = STATS_IDX(stream_info->stream_handle);

	if (stats_idx >= vfe_dev->hw_info->stats_hw_info->num_stats_type ||
		stats_idx >= MSM_ISP_STATS_MAX) {
		pr_err("%s Invalid stats index %d", __func__, stats_idx);
		return -EINVAL;
	}

	stats_pingpong_offset =
		vfe_dev->hw_info->stats_hw_info->stats_ping_pong_offset[
		stats_idx];

	pingpong_bit = (~(pingpong_status >> stats_pingpong_offset) & 0x1);
	rc = vfe_dev->buf_mgr->ops->get_buf(vfe_dev->buf_mgr,
			vfe_dev->pdev->id, bufq_handle, &buf);
	if (rc < 0) {
		vfe_dev->error_info.stats_framedrop_count[stats_idx]++;
		return rc;
	}

	if (buf->num_planes != 1) {
		pr_err("%s: Invalid buffer\n", __func__);
		rc = -EINVAL;
		goto buf_error;
	}

	vfe_dev->hw_info->vfe_ops.stats_ops.update_ping_pong_addr(
		vfe_dev, stream_info,
		pingpong_status, buf->mapped_info[0].paddr +
		stream_info->buffer_offset);

	if (stream_info->buf[pingpong_bit] && done_buf)
		*done_buf = stream_info->buf[pingpong_bit];

	stream_info->buf[pingpong_bit] = buf;
	return 0;
buf_error:
	vfe_dev->buf_mgr->ops->put_buf(vfe_dev->buf_mgr,
		buf->bufq_handle, buf->buf_idx);
	return rc;
}

void msm_isp_process_stats_irq(struct vfe_device *vfe_dev,
	uint32_t irq_status0, uint32_t irq_status1,
	struct msm_isp_timestamp *ts)
{
	int i, j, rc;
	struct msm_isp_event_data buf_event;
	struct msm_isp_stats_event *stats_event = &buf_event.u.stats;
	struct msm_isp_buffer *done_buf;
	struct msm_vfe_stats_stream *stream_info = NULL;
	uint32_t pingpong_status;
	uint32_t comp_stats_type_mask = 0, mask;
	uint32_t groups[MAX_NUM_STATS_COMP_MASK];
	uint32_t stats_comp_mask = 0, stats_irq_mask = 0;
	uint32_t num_stats_comp_mask =
		vfe_dev->hw_info->stats_hw_info->num_stats_comp_mask;
	stats_comp_mask = vfe_dev->hw_info->vfe_ops.stats_ops.
		get_comp_mask(irq_status0, irq_status1);
	stats_irq_mask = vfe_dev->hw_info->vfe_ops.stats_ops.
		get_wm_mask(irq_status0, irq_status1);
	if (!(stats_comp_mask || stats_irq_mask))
		return;
	ISP_DBG("%s: status: 0x%x\n", __func__, irq_status0);

	if (num_stats_comp_mask > ARRAY_SIZE(groups))
		return;
	for (j = 0; j < num_stats_comp_mask; j++) {
		groups[j] = atomic_read(&vfe_dev->stats_data.stats_comp_mask[j]);
		stats_irq_mask &= ~groups[j];
	}
	pingpong_status = vfe_dev->hw_info->vfe_ops.stats_ops.get_pingpong_status(vfe_dev);
	for (j = 0; j <= num_stats_comp_mask; j++) {
		if (!j)
			mask = stats_irq_mask;
		else if (stats_comp_mask & BIT(j - 1))
			mask = groups[j - 1];
		else
			continue;
		if (!mask)
			continue;
		memset(&buf_event, 0, sizeof(struct msm_isp_event_data));
		buf_event.timestamp = ts->event_time;
		buf_event.frame_id =
			vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id;
		buf_event.input_intf = VFE_PIX_0;

		for (i = 0; i < vfe_dev->hw_info->stats_hw_info->num_stats_type;
			i++) {
			if (!(mask & BIT(i)))
				continue;

			stream_info = &vfe_dev->stats_data.stream_info[i];
			done_buf = NULL;
			msm_isp_stats_cfg_ping_pong_address(vfe_dev,
				stream_info, pingpong_status, &done_buf);
			if (done_buf) {
				rc = vfe_dev->buf_mgr->ops->buf_divert(
					vfe_dev->buf_mgr, done_buf->bufq_handle,
					done_buf->buf_idx, &ts->buf_time,
					vfe_dev->axi_data.
					src_info[VFE_PIX_0].frame_id);
				if (rc != 0)
					continue;

				stats_event->stats_buf_idxs
					[stream_info->stats_type] =
					done_buf->buf_idx;
				if (!stream_info->composite_flag) {
					stats_event->stats_mask =
						1 << stream_info->stats_type;
					ISP_DBG("%s: stats frameid: 0x%x %d\n",
						__func__, buf_event.frame_id,
						stream_info->stats_type);
					msm_isp_send_event(vfe_dev,
						ISP_EVENT_STATS_NOTIFY +
						stream_info->stats_type,
						&buf_event);
				} else {
					comp_stats_type_mask |=
						1 << stream_info->stats_type;
				}
			}
		}

		if (comp_stats_type_mask) {
			ISP_DBG("%s: comp_stats frameid: 0x%x, 0x%x\n",
				__func__, buf_event.frame_id,
				comp_stats_type_mask);
			stats_event->stats_mask = comp_stats_type_mask;
			msm_isp_send_event(vfe_dev,
				ISP_EVENT_COMP_STATS_NOTIFY, &buf_event);
			comp_stats_type_mask = 0;
		}
	}
}

int msm_isp_stats_create_stream(struct vfe_device *vfe_dev,
	struct msm_vfe_stats_stream_request_cmd *stream_req_cmd)
{
	int rc = -1;
	struct msm_vfe_stats_stream *stream_info = NULL;
	struct msm_vfe_stats_shared_data *stats_data = &vfe_dev->stats_data;
	uint32_t stats_idx;

	if (!(vfe_dev->hw_info->stats_hw_info->stats_capability_mask &
		(1 << stream_req_cmd->stats_type))) {
		pr_err("%s: Stats type not supported\n", __func__);
		return rc;
	}

	stats_idx = vfe_dev->hw_info->vfe_ops.stats_ops.
		get_stats_idx(stream_req_cmd->stats_type);

	if (stats_idx >= vfe_dev->hw_info->stats_hw_info->num_stats_type) {
		pr_err("%s Invalid stats index %d", __func__, stats_idx);
		return -EINVAL;
	}

	stream_info = &stats_data->stream_info[stats_idx];
	if (stream_info->state != STATS_AVALIABLE) {
		pr_err("%s: Stats already requested\n", __func__);
		return rc;
	}

	if (stream_req_cmd->framedrop_pattern >= MAX_SKIP) {
		pr_err("%s: Invalid framedrop pattern\n", __func__);
		return rc;
	}

	if (stream_req_cmd->irq_subsample_pattern >= MAX_SKIP) {
		pr_err("%s: Invalid irq subsample pattern\n", __func__);
		return rc;
	}

	stream_info->session_id = stream_req_cmd->session_id;
	stream_info->stream_id = stream_req_cmd->stream_id;
	stream_info->composite_flag = stream_req_cmd->composite_flag;
	stream_info->stats_type = stream_req_cmd->stats_type;
	stream_info->buffer_offset = stream_req_cmd->buffer_offset;
	stream_info->framedrop_pattern = stream_req_cmd->framedrop_pattern;
	stream_info->init_stats_frame_drop = stream_req_cmd->init_frame_drop;
	stream_info->irq_subsample_pattern =
		stream_req_cmd->irq_subsample_pattern;
	stream_info->state = STATS_INACTIVE;

	if ((vfe_dev->stats_data.stream_handle_cnt << 8) == 0)
		vfe_dev->stats_data.stream_handle_cnt++;

	stream_req_cmd->stream_handle =
		(++vfe_dev->stats_data.stream_handle_cnt) << 8 | stats_idx;

	stream_info->stream_handle = stream_req_cmd->stream_handle;
	return 0;
}

int msm_isp_request_stats_stream(struct vfe_device *vfe_dev, void *arg)
{
	int rc = -1;
	struct msm_vfe_stats_stream_request_cmd *stream_req_cmd = arg;
	struct msm_vfe_stats_stream *stream_info = NULL;
	struct msm_vfe_stats_shared_data *stats_data = &vfe_dev->stats_data;
	uint32_t framedrop_period;
	uint32_t stats_idx;

	rc = msm_isp_stats_create_stream(vfe_dev, stream_req_cmd);
	if (rc < 0) {
		pr_err("%s: create stream failed\n", __func__);
		return rc;
	}

	stats_idx = STATS_IDX(stream_req_cmd->stream_handle);

	if (stats_idx >= vfe_dev->hw_info->stats_hw_info->num_stats_type) {
		pr_err("%s Invalid stats index %d", __func__, stats_idx);
		return -EINVAL;
	}

	stream_info = &stats_data->stream_info[stats_idx];

	framedrop_period = msm_isp_get_framedrop_period(
	   stream_req_cmd->framedrop_pattern);

	if (stream_req_cmd->framedrop_pattern == SKIP_ALL)
		stream_info->framedrop_pattern = 0x0;
	else
		stream_info->framedrop_pattern = 0x1;
	stream_info->framedrop_period = framedrop_period - 1;

	if (!stream_info->composite_flag)
		vfe_dev->hw_info->vfe_ops.stats_ops.
			cfg_wm_irq_mask(vfe_dev, stream_info);

	if (stream_info->init_stats_frame_drop == 0)
		vfe_dev->hw_info->vfe_ops.stats_ops.cfg_wm_reg(vfe_dev,
			stream_info);

	return rc;
}

int msm_isp_release_stats_stream(struct vfe_device *vfe_dev, void *arg)
{
	int rc = -1;
	struct msm_vfe_stats_stream_cfg_cmd stream_cfg_cmd;
	struct msm_vfe_stats_stream_release_cmd *stream_release_cmd = arg;
	struct msm_vfe_stats_shared_data *stats_data = &vfe_dev->stats_data;
	int stats_idx = STATS_IDX(stream_release_cmd->stream_handle);
	struct msm_vfe_stats_stream *stream_info = NULL;

	if (stats_idx >= vfe_dev->hw_info->stats_hw_info->num_stats_type) {
		pr_err("%s Invalid stats index %d", __func__, stats_idx);
		return -EINVAL;
	}

	stream_info = &stats_data->stream_info[stats_idx];
	if (stream_info->stream_handle != stream_release_cmd->stream_handle)
		return -EINVAL;
	if (stream_info->state == STATS_AVALIABLE) {
		pr_err("%s: stream already release\n", __func__);
		return rc;
	} else if (stream_info->state != STATS_INACTIVE || stream_info->buf[0] ||
		   stream_info->buf[1]) {
		stream_cfg_cmd.enable = 0;
		stream_cfg_cmd.num_streams = 1;
		stream_cfg_cmd.stream_handle[0] =
			stream_release_cmd->stream_handle;
		rc = msm_isp_cfg_stats_stream(vfe_dev, &stream_cfg_cmd);
		if (rc)
			return rc;
	}

	if (!stream_info->composite_flag)
		vfe_dev->hw_info->vfe_ops.stats_ops.
			clear_wm_irq_mask(vfe_dev, stream_info);

	vfe_dev->hw_info->vfe_ops.stats_ops.clear_wm_reg(vfe_dev, stream_info);
	memset(stream_info, 0, sizeof(struct msm_vfe_stats_stream));
	return 0;
}

static int msm_isp_init_stats_ping_pong_reg(
	struct vfe_device *vfe_dev,
	struct msm_vfe_stats_stream *stream_info)
{
	int rc = 0;
	stream_info->bufq_handle =
		vfe_dev->buf_mgr->ops->get_bufq_handle(
		vfe_dev->buf_mgr, stream_info->session_id,
		stream_info->stream_id);
	if (stream_info->bufq_handle == 0) {
		pr_err("%s: no buf configured for stream: 0x%x\n",
			__func__, stream_info->stream_handle);
		return -EINVAL;
	}

	rc = msm_isp_stats_cfg_ping_pong_address(vfe_dev,
		stream_info, VFE_PING_FLAG, NULL);
	if (rc < 0) {
		pr_err("%s: No free buffer for ping\n", __func__);
		return rc;
	}
	rc = msm_isp_stats_cfg_ping_pong_address(vfe_dev,
		stream_info, VFE_PONG_FLAG, NULL);
	if (rc < 0) {
		msm_isp_put_ping_pong_buffers(vfe_dev, stream_info->buf);
		pr_err("%s: No free buffer for pong\n", __func__);
		return rc;
	}
	return rc;
}


void msm_isp_update_stats_framedrop_reg(struct vfe_device *vfe_dev)
{
	int i;
	struct msm_vfe_stats_shared_data *stats_data = &vfe_dev->stats_data;
	struct msm_vfe_stats_stream *stream_info = NULL;

	for (i = 0; i < vfe_dev->hw_info->stats_hw_info->num_stats_type; i++) {
		stream_info = &stats_data->stream_info[i];
		if (stream_info->state != STATS_ACTIVE)
			continue;

		if (stream_info->init_stats_frame_drop) {
			stream_info->init_stats_frame_drop--;
			if (stream_info->init_stats_frame_drop == 0) {
				vfe_dev->hw_info->vfe_ops.stats_ops.cfg_wm_reg(
					vfe_dev, stream_info);
			}
		}
	}
}

void msm_isp_stats_set_group(struct vfe_device *vfe_dev, uint32_t mask,
	uint8_t group, bool enable, uint32_t offset, uint8_t skip)
{
	struct msm_vfe_stats_shared_data *stats = &vfe_dev->stats_data;
	uint32_t bits, shift, reg, updated;

	if (!mask || group >= MAX_NUM_STATS_COMP_MASK ||
	    group >= vfe_dev->hw_info->stats_hw_info->num_stats_comp_mask)
		return;

	bits = atomic_read(&stats->stats_comp_mask[group]);
	bits = enable ? bits | mask : bits & ~mask;
	shift = 16 + 8 * group;
	reg = msm_camera_io_r(vfe_dev->vfe_base + offset);
	updated = (reg & ~(0xffU << shift)) | (((bits >> skip) & 0xff) << shift);
	stats->reg_mask = updated;
	atomic_set(&stats->stats_comp_mask[group], bits);
	if (updated != reg)
		msm_camera_io_w(updated, vfe_dev->vfe_base + offset);
}

void msm_isp_stats_stream_update(struct vfe_device *vfe_dev)
{
	struct msm_vfe_stats_shared_data *stats = &vfe_dev->stats_data;
	struct msm_vfe_stats_ops *ops = &vfe_dev->hw_info->vfe_ops.stats_ops;
	uint32_t modules[2] = { 0 };
	uint32_t groups[2][MAX_NUM_STATS_COMP_MASK] = { { 0 } };
	unsigned long flags;
	int i, enable, group;

	spin_lock_irqsave(&vfe_dev->shared_data_lock, flags);
	if (!atomic_read(&stats->stats_update))
		goto out;
	for (i = 0; i < vfe_dev->hw_info->stats_hw_info->num_stats_type; i++) {
		struct msm_vfe_stats_stream *stream = &stats->stream_info[i];

		switch (stream->state) {
		case STATS_START_PENDING:
		case STATS_STOP_PENDING:
			enable = stream->state == STATS_START_PENDING;
			modules[enable] |= BIT(i);
			group = stream->composite_flag;
			if (group > 0 && group <= MAX_NUM_STATS_COMP_MASK)
				groups[enable][group - 1] |= BIT(i);
			stream->state = enable ? STATS_STARTING : STATS_STOPPING;
			break;
		case STATS_STARTING:
			stream->state = STATS_ACTIVE;
			break;
		case STATS_STOPPING:
			stream->state = STATS_INACTIVE;
			break;
		default:
			break;
		}
	}
	for (enable = 0; enable < 2; enable++) {
		if (!modules[enable])
			continue;
		ops->enable_module(vfe_dev, modules[enable], enable);
		for (group = 0; group < MAX_NUM_STATS_COMP_MASK; group++)
			if (groups[enable][group])
				ops->cfg_comp_mask(vfe_dev, groups[enable][group],
						   group, enable);
	}
	if (atomic_dec_and_test(&stats->stats_update))
		complete(&vfe_dev->stats_config_complete);
out:
	spin_unlock_irqrestore(&vfe_dev->shared_data_lock, flags);
}

static int msm_isp_stats_wait_for_cfg_done(struct vfe_device *vfe_dev)
{
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&vfe_dev->shared_data_lock, flags);
	INIT_COMPLETION(vfe_dev->stats_config_complete);
	atomic_set(&vfe_dev->stats_data.stats_update, 2);
	vfe_dev->hw_info->vfe_ops.core_ops.reg_update(vfe_dev, BIT(VFE_PIX_0));
	spin_unlock_irqrestore(&vfe_dev->shared_data_lock, flags);
	wait_for_completion_timeout(&vfe_dev->stats_config_complete,
				    msecs_to_jiffies(VFE_MAX_CFG_TIMEOUT));
	spin_lock_irqsave(&vfe_dev->shared_data_lock, flags);
	if (atomic_read(&vfe_dev->stats_data.stats_update)) {
		atomic_set(&vfe_dev->stats_data.stats_update, 0);
		rc = -ETIMEDOUT;
	}
	spin_unlock_irqrestore(&vfe_dev->shared_data_lock, flags);
	if (rc)
		pr_err("%s: wait timeout\n", __func__);
	return rc;
}

int msm_isp_cfg_stats_stream(struct vfe_device *vfe_dev, void *arg)
{
	struct msm_vfe_stats_stream_cfg_cmd *cmd = arg;
	struct msm_vfe_stats_shared_data *stats = &vfe_dev->stats_data;
	struct msm_vfe_stats_ops *ops = &vfe_dev->hw_info->vfe_ops.stats_ops;
	const unsigned int count = vfe_dev->hw_info->stats_hw_info->num_stats_type;
	const unsigned int groups = vfe_dev->hw_info->stats_hw_info->num_stats_comp_mask;
	struct msm_vfe_stats_stream *stream;
	uint32_t comp[MAX_NUM_STATS_COMP_MASK] = { 0 };
	uint32_t seen = 0, mask = 0, prepared = 0;
	bool enable = !!cmd->enable;
	bool running = vfe_dev->axi_data.src_info[VFE_PIX_0].active;
	unsigned int i, idx;
	int rc;

	if (cmd->num_streams > ARRAY_SIZE(cmd->stream_handle) ||
	    groups > ARRAY_SIZE(comp) || count > ARRAY_SIZE(stats->stream_info))
		return -EINVAL;
	for (i = 0; i < cmd->num_streams; i++) {
		idx = STATS_IDX(cmd->stream_handle[i]);
		if (idx >= count || (seen & BIT(idx)))
			return -EINVAL;
		seen |= BIT(idx);
		stream = &stats->stream_info[idx];
		if (stream->stream_handle != cmd->stream_handle[i] ||
		    stream->state == STATS_AVALIABLE || stream->composite_flag > groups)
			return -EINVAL;
		if (enable && stream->state != STATS_INACTIVE)
			return -EBUSY;
		if (!enable && stream->state == STATS_INACTIVE &&
		    !stream->buf[0] && !stream->buf[1])
			continue;
		mask |= BIT(idx);
		if (stream->composite_flag)
			comp[stream->composite_flag - 1] |= BIT(idx);
	}
	if (!mask)
		return 0;

	if (enable) {
		rc = ops->check_streams(stats->stream_info);
		if (rc)
			return rc;
		for (i = 0; i < count; i++) {
			stream = &stats->stream_info[i];
			/* A previously timed-out stop may have since latched. */
			if ((mask & BIT(i)) && (stream->buf[0] || stream->buf[1])) {
				msm_isp_put_ping_pong_buffers(vfe_dev, stream->buf);
				stats->num_active_stream--;
			}
		}
		if (!stats->num_active_stream)
			ops->cfg_ub(vfe_dev);
		if (ops->update_cgc_override)
			ops->update_cgc_override(vfe_dev, mask, 1);
		for (i = 0; i < count; i++) {
			if (!(mask & BIT(i)))
				continue;
			rc = msm_isp_init_stats_ping_pong_reg(vfe_dev, &stats->stream_info[i]);
			if (rc)
				goto put_prepared;
			prepared |= BIT(i);
		}
		stats->num_active_stream += hweight32(mask);
	}
	for (i = 0; i < count; i++) {
		if (!(mask & BIT(i)))
			continue;
		stream = &stats->stream_info[i];
		if (enable)
			stream->state = running ? STATS_START_PENDING : STATS_ACTIVE;
		else
			stream->state = running ? STATS_STOP_PENDING : STATS_INACTIVE;
	}
	if (running) {
		rc = msm_isp_stats_wait_for_cfg_done(vfe_dev);
		if (rc)
			return rc;
	} else {
		ops->enable_module(vfe_dev, mask, enable);
		for (i = 0; i < groups; i++)
			if (comp[i])
				ops->cfg_comp_mask(vfe_dev, comp[i], i, enable);
	}
	if (enable)
		return 0;
	stats->num_active_stream -= hweight32(mask);
	prepared = mask;
	rc = 0;

put_prepared:
	for (i = 0; i < count; i++)
		if (prepared & BIT(i))
			msm_isp_put_ping_pong_buffers(vfe_dev, stats->stream_info[i].buf);
	if (ops->update_cgc_override)
		ops->update_cgc_override(vfe_dev, mask, 0);
	return rc;
}

int msm_isp_update_stats_stream(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0, i;
	struct msm_vfe_stats_stream *stream_info;
	struct msm_vfe_stats_shared_data *stats_data = &vfe_dev->stats_data;
	struct msm_vfe_axi_stream_update_cmd *update_cmd = arg;
	struct msm_vfe_axi_stream_cfg_update_info *update_info = NULL;

	if (update_cmd->num_streams > ARRAY_SIZE(update_cmd->update_info))
		return -EINVAL;
	/*validate request*/
	for (i = 0; i < update_cmd->num_streams; i++) {
		update_info = &update_cmd->update_info[i];
		/*check array reference bounds*/
		if (STATS_IDX(update_info->stream_handle)
			>= vfe_dev->hw_info->stats_hw_info->num_stats_type ||
		    (update_cmd->update_type == UPDATE_STREAM_STATS_FRAMEDROP_PATTERN &&
		     update_info->skip_pattern >= MAX_SKIP)) {
			pr_err("%s: stats idx %d out of bound!", __func__,
				STATS_IDX(update_info->stream_handle));
			return -EINVAL;
		}
	}

	for (i = 0; i < update_cmd->num_streams; i++) {
		update_info = &update_cmd->update_info[i];
		stream_info = &stats_data->stream_info[
				STATS_IDX(update_info->stream_handle)];
		if (stream_info->stream_handle !=
			update_info->stream_handle) {
			pr_err("%s: stats stream handle %x %x mismatch!\n",
				__func__, stream_info->stream_handle,
				update_info->stream_handle);
			continue;
		}

		switch (update_cmd->update_type) {
		case UPDATE_STREAM_STATS_FRAMEDROP_PATTERN: {
			uint32_t framedrop_period =
				msm_isp_get_framedrop_period(
				   update_info->skip_pattern);
			if (update_info->skip_pattern == SKIP_ALL)
				stream_info->framedrop_pattern = 0x0;
			else
				stream_info->framedrop_pattern = 0x1;
			stream_info->framedrop_period = framedrop_period - 1;
			if (stream_info->init_stats_frame_drop == 0)
				vfe_dev->hw_info->vfe_ops.stats_ops.cfg_wm_reg(
					vfe_dev, stream_info);
			break;
		}

		default:
			pr_err("%s: Invalid update type\n", __func__);
			return -EINVAL;
		}
	}
	return rc;
}
