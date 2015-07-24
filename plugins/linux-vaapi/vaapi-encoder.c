#include <obs-module.h>
#include <util/darray.h>
#include <util/platform.h>

#include <va/va.h>
#include <va/va_x11.h>
#include <va/va_enc_h264.h>
#include <X11/Xlib.h>

#include "bitstream.h"
#include "surface-queue.h"
#include "vaapi-caps.h"
#include "vaapi-common.h"
#include "vaapi-encoder.h"

#define SPS_PROFILE_IDC_BASELINE             66
#define SPS_PROFILE_IDC_MAIN                 77
#define SPS_PROFILE_IDC_HIGH                 100

// HRD Constants (E.2.2)
enum {
	HRD_BITRATE_SCALE          = 6,  // E-37
	HRD_CPB_SIZE_SCALE         = 4,  // E-38
	HRD_INIT_CPB_REM_DELAY_LEN = 24,
	HRD_DPB_OUTPUT_DELAY_LEN   = 24,
	HRD_TIME_OFFSET_LEN        = 24
};

enum nal_unit_type_e
{
    NAL_UNKNOWN     = 0,
    NAL_SLICE       = 1,
    NAL_SLICE_DPA   = 2,
    NAL_SLICE_DPB   = 3,
    NAL_SLICE_DPC   = 4,
    NAL_SLICE_IDR   = 5,    /* ref_idc != 0 */
    NAL_SEI         = 6,    /* ref_idc == 0 */
    NAL_SPS         = 7,
    NAL_PPS         = 8,
    NAL_AUD         = 9,
    NAL_FILLER      = 12,
    /* ref_idc == 0 for 6,9,10,11,12 */
};

enum nal_priority_e
{
    NAL_PRIORITY_DISPOSABLE = 0,
    NAL_PRIORITY_LOW        = 1,
    NAL_PRIORITY_HIGH       = 2,
    NAL_PRIORITY_HIGHEST    = 3,
};

struct vaapi_encoder
{
	VADisplay display;
	VAConfigID config;
	VAContextID context;
	DARRAY(VASurfaceID) refpics;

	surface_queue_t *surfq;

	vaapi_profile_caps_t *caps;

	uint32_t bitrate;
	uint32_t bitrate_bits;
	bool cbr;
	uint32_t height;
	uint32_t width;
	uint32_t keyint;
	uint32_t framerate_num;
	uint32_t framerate_den;
	vaapi_format_t format;

	uint32_t intra_period;
	uint32_t cbp_window_ms;
	uint32_t cbp_size;
	uint32_t qp;

	VAEncSequenceParameterBufferH264 sps;
	VAEncPictureParameterBufferH264 pps;
	VAEncSliceParameterBufferH264 slice;

	uint64_t frame_cnt;
	uint32_t output_buf_size;

	uint32_t surface_cnt;
	void *coded_block_cb_opaque;
	vaapi_coded_block_cb coded_block_cb;
	DARRAY(uint8_t) extra_data;
};

typedef struct darray buffer_list_t;

void vaapi_encoder_destroy(vaapi_encoder_t *enc)
{
	if (enc != NULL) {
		vaDestroySurfaces(enc->display, enc->refpics.array,
				enc->refpics.num);

		da_free(enc->refpics);
		da_free(enc->extra_data);

		surface_queue_destroy(enc->surfq);

		vaDestroyConfig(enc->display, enc->config);
		vaDestroyContext(enc->display, enc->context);

		bfree(enc);
	}
}

static bool initialize_encoder(vaapi_encoder_t *enc)
{
	VAStatus status;

	CHECK_STATUS_FALSE(vaCreateConfig(enc->display, enc->caps->def.va,
			enc->caps->entrypoint, enc->caps->attribs,
			enc->caps->attribs_cnt, &enc->config));

	CHECK_STATUS_FAIL(vaCreateContext(enc->display, enc->config,
			enc->width, enc->height, VA_PROGRESSIVE, NULL, 0,
			&enc->context));

	CHECK_STATUS_FAILN(vaCreateSurfaces(enc->display, VA_RT_FORMAT_YUV420,
			enc->width, enc->height, enc->refpics.array,
			enc->refpics.num, NULL, 0), 1);

	return true;

fail1:
	vaDestroyContext(enc->display, enc->context);

fail:
	vaDestroyConfig(enc->display, enc->config);

	return false;
}

bool vaapi_encoder_set_cbp_window(vaapi_encoder_t *enc, uint32_t cbp_window_ms)
{
	enc->cbp_window_ms = cbp_window_ms;
	enc->cbp_size = (enc->bitrate_bits * enc->cbp_window_ms) / 1000;

	return true;
}

bool vaapi_encoder_set_bitrate(vaapi_encoder_t *enc, uint32_t bitrate)
{
	enc->bitrate = bitrate;
	enc->bitrate_bits = bitrate * 1000;
	enc->bitrate_bits &= ~((1U << HRD_BITRATE_SCALE) - 1);

	// recalculate window size
	vaapi_encoder_set_cbp_window(enc, enc->cbp_window_ms);

	return true;
}

static void init_sps(vaapi_encoder_t *enc)
{
	memset(&enc->sps, 0, sizeof(VAEncSequenceParameterBufferH264));

	int width_in_mbs, height_in_mbs;
	int frame_cropping_flag = 0;
	int frame_crop_bottom_offset = 0;

	width_in_mbs = (enc->width + 15) / 16;
	height_in_mbs = (enc->height + 15) / 16;

#define SPS enc->sps
	SPS.level_idc = 41;
	SPS.intra_period = enc->intra_period;
	SPS.bits_per_second = enc->bitrate_bits;
	SPS.max_num_ref_frames = 4;
	SPS.picture_width_in_mbs = width_in_mbs;
	SPS.picture_height_in_mbs = height_in_mbs;
	SPS.seq_fields.bits.frame_mbs_only_flag = 1;

	SPS.time_scale = enc->framerate_num;
	SPS.num_units_in_tick = enc->framerate_den;
	SPS.vui_fields.bits.timing_info_present_flag = true;

	if (height_in_mbs * 16 - enc->height > 0) {
		frame_cropping_flag = 1;
		frame_crop_bottom_offset =
				(height_in_mbs * 16 - enc->height) / 2;
	}

	SPS.frame_cropping_flag = frame_cropping_flag;
	SPS.frame_crop_bottom_offset = frame_crop_bottom_offset;

	SPS.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 2;

#undef SPS
}

static void init_pps(vaapi_encoder_t *enc)
{
#define PPS enc->pps
	PPS.pic_init_qp = enc->qp;

	PPS.pic_fields.bits.entropy_coding_mode_flag = 1;
	PPS.pic_fields.bits.deblocking_filter_control_present_flag = 1;
#undef PPS
}

static void initialize_defaults(vaapi_encoder_t *enc)
{
	float fps = (float)enc->framerate_num / enc->framerate_den;
	enc->intra_period = fps * enc->keyint;
	enc->qp = 0;
	enc->cbp_window_ms = 1500;

	vaapi_encoder_set_bitrate(enc, enc->bitrate);
	vaapi_encoder_set_cbp_window(enc, 1500);

	init_sps(enc);
	init_pps(enc);
}

bool pack_pps(vaapi_encoder_t *enc, bitstream_t *bs)
{
	bs_begin_nalu(bs, NAL_PPS, NAL_PRIORITY_HIGHEST);

#define PIC enc->pps
#define APPEND_PFB(c, x) bs_append_bits(bs, c, PIC.pic_fields.bits.x)

	// pic_parameter_set_id
	bs_append_ue(bs, PIC.pic_parameter_set_id);
	// seq_parameter_set_id
	bs_append_ue(bs, PIC.seq_parameter_set_id);

	APPEND_PFB(1, entropy_coding_mode_flag);

	/* pic_order_present_flag: 0 */
	bs_append_bool(bs, false);

	/* num_slice_groups_minus1 */
	bs_append_ue(bs, 0);

	// num_ref_idx_l0_active_minus1
	bs_append_ue(bs, PIC.num_ref_idx_l0_active_minus1);
	// num_ref_idx_l1_active_minus1
	bs_append_ue(bs, PIC.num_ref_idx_l1_active_minus1);

	// weighted_pred_flag
	APPEND_PFB(1, weighted_pred_flag);
	// weighted_bipred_idc
	APPEND_PFB(2, weighted_bipred_idc);

	// pic_init_qp_minus26
	bs_append_se(bs, PIC.pic_init_qp - 26);
	// pic_init_qs_minus26
	bs_append_se(bs, 0);
	// chroma_qp_index_offset
	bs_append_se(bs, 0);

	APPEND_PFB(1, deblocking_filter_control_present_flag);

	// constrained_intra_pred_flag,
	bs_append_bits(bs, 1, 0);
	// redundant_pic_cnt_present_flag
	bs_append_bits(bs, 1, 0);

	APPEND_PFB(1, transform_8x8_mode_flag);

	if (enc->caps->def.vaapi == VAAPI_PROFILE_HIGH) {
		APPEND_PFB(1, transform_8x8_mode_flag);
		// pic_scaling_matrix_present_flag
		bs_append_bool(bs, false);
		bs_append_se(bs, PIC.second_chroma_qp_index_offset);
	}

	bs_end_nalu(bs);

	return true;

#undef APPEND_PFB
#undef PIC
}

bool pack_sps(vaapi_encoder_t *enc, bitstream_t *bs)
{
	int profile_idc;
	bool constraint_set0_flag = false;
	bool constraint_set1_flag = false;
	bool constraint_set2_flag = false;
	bool constraint_set3_flag = false;
	bool constraint_set4_flag = false;
	bool constraint_set5_flag = false;

	switch(enc->caps->def.va) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Baseline:
		profile_idc = SPS_PROFILE_IDC_BASELINE;
		if (enc->caps->def.va == VAProfileH264ConstrainedBaseline)
			constraint_set0_flag = 1;
		break;
	case VAProfileH264Main:
		profile_idc = SPS_PROFILE_IDC_MAIN;
		constraint_set1_flag = 1;
		break;
	case VAProfileH264High:
		profile_idc = SPS_PROFILE_IDC_HIGH; break;
	default:
		VA_LOG(LOG_ERROR, "failed creating sps due to invalid profile");
		goto fail;
	}

#define SPS enc->sps

	bs_begin_nalu(bs, NAL_SPS, NAL_PRIORITY_HIGHEST);
	bs_append_bits(bs, 8, profile_idc);
	bs_append_bits(bs, 1, constraint_set0_flag);
	bs_append_bits(bs, 1, constraint_set1_flag);
	bs_append_bits(bs, 1, constraint_set2_flag);
	bs_append_bits(bs, 1, constraint_set3_flag);
	bs_append_bits(bs, 1, constraint_set4_flag);
	bs_append_bits(bs, 1, constraint_set5_flag);
	bs_append_bits(bs, 2, 0); // reserved 2 bits
	bs_append_bits(bs, 8, SPS.level_idc);
	bs_append_ue(bs, SPS.seq_parameter_set_id);

#define APPEND_SFB(x) bs_append_ue(bs, SPS.seq_fields.bits.x);
	APPEND_SFB(log2_max_frame_num_minus4);
	APPEND_SFB(pic_order_cnt_type);
	APPEND_SFB(log2_max_pic_order_cnt_lsb_minus4);
#undef APPEND_SFB

	bs_append_ue(bs, SPS.max_num_ref_frames);
	bs_append_bits(bs, 1, 0); // gaps_in_frame_num_value_allowed_flag

	// pic_width_in_mbs_minus1
	bs_append_ue(bs, SPS.picture_width_in_mbs - 1);
	// pic_height_in_map_units_minus1
	bs_append_ue(bs, SPS.picture_height_in_mbs - 1);

#define APPEND_SFB_BIT(x) bs_append_bits(bs, 1, SPS.seq_fields.bits.x);
	APPEND_SFB_BIT(frame_mbs_only_flag);
	APPEND_SFB_BIT(direct_8x8_inference_flag);
#undef APPEND_SFB_BIT

	bs_append_bits(bs, 1, SPS.frame_cropping_flag);
	if (SPS.frame_cropping_flag) {
		bs_append_ue(bs, SPS.frame_crop_left_offset);
		bs_append_ue(bs, SPS.frame_crop_right_offset);
		bs_append_ue(bs, SPS.frame_crop_top_offset);
		bs_append_ue(bs, SPS.frame_crop_bottom_offset);
	}

	// vui_parameters_present_flag
	bs_append_bits(bs, 1, 1);

	// aspect_ratio_info_present_flag
	bs_append_bits(bs, 1, 0);
	// overscan_info_present_flag
	bs_append_bits(bs, 1, 0);

	// video_signal_type_present_flag
	bs_append_bits(bs, 1, 0);
	// chroma_loc_info_present_flag
	bs_append_bits(bs, 1, 0);

	// timing_info_present_flag
	bs_append_bits(bs, 1, SPS.vui_fields.bits.timing_info_present_flag);
	{
		bs_append_bits(bs, 32, SPS.num_units_in_tick);
		bs_append_bits(bs, 32, SPS.time_scale * 2);

		// fixed_frame_rate_flag
		bs_append_bool(bs, true);
	}

	bool nal_hrd_parameters_present_flag = SPS.bits_per_second > 0;

	// nal_hrd_parameters_present_flag
	bs_append_bool(bs, nal_hrd_parameters_present_flag);
	if (nal_hrd_parameters_present_flag) {

		int cpb_cnt_minus1 = 0;
		bs_append_ue(bs, cpb_cnt_minus1);
		// bit_rate_scale
		bs_append_bits(bs, 4, 0 /*default*/);
		// cpb_size_scale
		bs_append_bits(bs, 4, 0 /*default*/);
		for(int i = 0; i <= cpb_cnt_minus1; i++) {
			int bit_rate_scale = enc->bitrate_bits;
			bit_rate_scale >>= HRD_BITRATE_SCALE;
			bs_append_ue(bs, bit_rate_scale - 1);
			int cbp_size_scale = enc->cbp_size;
			cbp_size_scale >>= HRD_CPB_SIZE_SCALE;
			bs_append_ue(bs, cbp_size_scale - 1);
			bs_append_bool(bs, enc->cbr);
		}
		// initial_cpb_removal_delay_length_minus1
		bs_append_bits(bs, 5, 23);
		// cpb_removal_delay_length_minus1
		bs_append_bits(bs, 5, 23);
		// dpb_output_delay_length_minus1
		bs_append_bits(bs, 5, 23);
		// time_offset_length
		bs_append_bits(bs, 5, 23);
	}

	bool vcl_hrd_parameters_present_flag = false;

	// vcl_hrd_parameters_present_flag
	bs_append_bool(bs, vcl_hrd_parameters_present_flag);

	if (nal_hrd_parameters_present_flag ||
	    vcl_hrd_parameters_present_flag) {
		// low_delay_hrd_flag
		bs_append_bool(bs, false);
	}

	// pic_struct_present_flag
	bs_append_bool(bs, false);

	bool bitstream_restriction_flag = true;

	// bitstream_restriction_flag
	bs_append_bool(bs, bitstream_restriction_flag);

	if (bitstream_restriction_flag) {
		// motion_vectors_over_pic_boundaries_flag
		bs_append_bool(bs, false);
		// max_bytes_per_pic_denom
		bs_append_ue(bs, 2);
		// max_bits_per_mb_denom
		bs_append_ue(bs, 1);
		// log2_max_mv_length_horizontal
		bs_append_ue(bs, 16);
		// log2_max_mv_length_vertical
		bs_append_ue(bs, 16);
		// disable B slices
		// max_num_reorder_frames
		bs_append_ue(bs, 0);
		// max_num_ref_frame
		bs_append_ue(bs, SPS.max_num_ref_frames);
	}

	bs_end_nalu(bs);

	return true;

fail:
	return false;

#undef SPS
}

static bool create_buffer(vaapi_encoder_t *enc, VABufferType type,
		unsigned int size, unsigned int num_elements, void *data,
		struct darray *list)
{
	VABufferID buf;
	VAStatus status;

	CHECK_STATUS_FALSE(vaCreateBuffer(enc->display, enc->context,
			type, size, num_elements, data, &buf));
	if (buf == VA_INVALID_ID) {
		VA_LOG(LOG_ERROR, "failed to create buffer");
		return false;
	}

	darray_push_back(sizeof(VABufferID), list, &buf);

	return true;
}

static VABufferID get_last_buffer(buffer_list_t *list)
{
	if (list->num > 0)
		return *((VABufferID *)darray_item(sizeof(VABufferID), list,
				list->num - 1));
	return VA_INVALID_ID;
}

static void destroy_last_buffer(vaapi_encoder_t *enc, buffer_list_t *list)
{
	if (list->num > 0) {
		VABufferID *buf = darray_item(sizeof(VABufferID), list,
				list->num - 1);
		if (*buf != VA_INVALID_ID) {
			vaDestroyBuffer(enc->display, *buf);
		}
		darray_pop_back(sizeof(VABufferID), list);
	}
}

static void destroy_buffers(vaapi_encoder_t *enc, buffer_list_t *list)
{
	for(size_t i = 0; i < list->num; i++) {
		VABufferID *buf = darray_item(sizeof(VABufferID), list, i);
		if (*buf != VA_INVALID_ID) {
			vaDestroyBuffer(enc->display, *buf);
		}
	}
	darray_resize(sizeof(VABufferID), list, 0);
}

static bool create_seq_buffer(vaapi_encoder_t *enc, buffer_list_t *list)
{
	return create_buffer(enc, VAEncSequenceParameterBufferType,
			sizeof(enc->sps), 1, &enc->sps, list);
}

static bool create_slice_buffer(vaapi_encoder_t *enc, buffer_list_t *list,
		vaapi_slice_type_t slice_type)
{
	int width_in_mbs = (enc->width + 15) / 16;
	int height_in_mbs = (enc->height + 15) / 16;

	memset(&enc->slice, 0, sizeof(enc->slice));

	enc->slice.num_macroblocks = width_in_mbs * height_in_mbs;
	enc->slice.slice_type = slice_type;

	enc->slice.slice_alpha_c0_offset_div2 = 2;
	enc->slice.slice_beta_offset_div2 = 2;

	if (!create_buffer(enc, VAEncSliceParameterBufferType,
			sizeof(enc->slice), 1, &enc->slice, list)) {
		VA_LOG(LOG_ERROR, "failed to create "
				"VAEncSliceParameterBufferType buffer");
		return false;
	}

	return true;
}

static bool create_misc_buffer(vaapi_encoder_t *enc,
		VAEncMiscParameterType type, size_t size, void *data,
		buffer_list_t *list)
{
	VAStatus status;
	VABufferID buffer;
	void *buffer_data;

	if (!create_buffer(enc, VAEncMiscParameterBufferType,
			sizeof(VAEncMiscParameterBufferType) + size,
			1, data, list))
		return false;

	buffer = get_last_buffer(list);

	CHECK_STATUS_FAIL(vaMapBuffer(enc->display, buffer, &buffer_data));

	VAEncMiscParameterBuffer* misc_param =
			(VAEncMiscParameterBuffer*)buffer_data;

	misc_param->type = type;
	memcpy(misc_param->data, data, size);

	CHECK_STATUS_FAIL(vaUnmapBuffer(enc->display, buffer));

	return true;

fail:
	destroy_last_buffer(enc, list);

	return false;
}

static bool create_misc_rc_buffer(vaapi_encoder_t *enc,
		buffer_list_t *list)
{
	VAEncMiscParameterRateControl rc = {0};

	rc.bits_per_second = enc->bitrate_bits;
	rc.target_percentage = 90;
	rc.window_size = enc->cbp_size;
	rc.initial_qp = enc->qp;
	rc.min_qp = 1;
	rc.basic_unit_size = 0;
	rc.rc_flags.bits.disable_frame_skip = false;

	if (!create_misc_buffer(enc, VAEncMiscParameterTypeRateControl,
			sizeof(rc), &rc, list)) {
		VA_LOG(LOG_ERROR, "failed to create "
				"VAEncMiscParameterBufferType RC buffer");
		return false;
	}

	return true;
}

static bool create_misc_hdr_buffer(vaapi_encoder_t *enc,
		buffer_list_t *list)
{
	VAEncMiscParameterHRD hrd = {0};

	hrd.initial_buffer_fullness = enc->cbp_size / 2;
	hrd.buffer_size = enc->cbp_size;

	if (!create_misc_buffer(enc, VAEncMiscParameterTypeHRD,
			sizeof(hrd), &hrd, list)) {
		VA_LOG(LOG_ERROR, "failed to create "
				"VAEncMiscParameterBufferType HRD buffer");
		return false;
	}

	return true;
}

static bool create_pic_buffer(vaapi_encoder_t *enc, buffer_list_t *list,
		VABufferID output_buf)
{
	VASurfaceID curr_pic, pic0;

#define PPS enc->pps

	curr_pic = enc->refpics.array[enc->frame_cnt % 2];
	pic0 = enc->refpics.array[(enc->frame_cnt + 1) % 2];

	PPS.CurrPic.picture_id = curr_pic;
	PPS.CurrPic.frame_idx = enc->frame_cnt;
	PPS.CurrPic.flags = 0;

	PPS.CurrPic.TopFieldOrderCnt = enc->frame_cnt * 2;
	PPS.CurrPic.BottomFieldOrderCnt = PPS.CurrPic.TopFieldOrderCnt;

	PPS.ReferenceFrames[0].picture_id = pic0;
	PPS.ReferenceFrames[1].picture_id = enc->refpics.array[2];
	PPS.ReferenceFrames[2].picture_id = VA_INVALID_ID;

	PPS.coded_buf = output_buf;
	PPS.frame_num = enc->frame_cnt;
	PPS.pic_init_qp = enc->qp;

	PPS.pic_fields.bits.idr_pic_flag = (enc->frame_cnt == 0);
	PPS.pic_fields.bits.reference_pic_flag = 1;

	if (!create_buffer(enc, VAEncPictureParameterBufferType,
				sizeof(VAEncPictureParameterBufferH264), 1,
				&PPS, list)) {
		VA_LOG(LOG_ERROR, "failed to create "
				"VAEncPictureParameterBufferH264 buffer");
		return false;
	}

	return true;
}

static bool create_packed_header_buffers(vaapi_encoder_t *enc,
		buffer_list_t *list, VAEncPackedHeaderType type,
		bitstream_t *bs)
{
	VAEncPackedHeaderParameterBuffer header;

	header.type = type;
	header.bit_length = bs_size(bs) * 8;
	header.has_emulation_bytes = 0;

	if (!create_buffer(enc,
			VAEncPackedHeaderParameterBufferType,
			sizeof(header), 1, &header, list)) {
		VA_LOG(LOG_ERROR, "failed to create "
				"VAEncPackedHeaderParameterBufferType buffer");
		goto fail;
	}
	if (!create_buffer(enc, VAEncPackedHeaderDataBufferType,
			bs_size(bs), 1, bs_data(bs), list)) {
		VA_LOG(LOG_ERROR, "failed to create "
				"VAEncPackedHeaderDataBufferType buffer");
		goto fail1;
	}

	return true;

fail1:
	destroy_last_buffer(enc, list);

fail:
	return false;
}

static VABufferID create_output_buffer(vaapi_encoder_t *enc)
{
	VABufferID output_buf;
	VAStatus status;

	CHECK_STATUS_FAIL(vaCreateBuffer(enc->display, enc->context,
				VAEncCodedBufferType, enc->output_buf_size,
				1, NULL, &output_buf));
	return output_buf;

fail:
	return VA_INVALID_ID;
}

static void encode_nalu_to_extra_data(vaapi_encoder_t *enc, bitstream_t *bs)
{
	size_t size = bs_size(bs);
	uint8_t *data = bs_data(bs);
	int zero_cnt = 0;
	// add header manually
	da_push_back_array(enc->extra_data, data, 5);
	// add EP to rbsp
	for(uint8_t *d = (data + 5); d < (data + size - 1); d++) {
		if (zero_cnt == 2 && *d <= 0x03) {
			static uint8_t EP_3 = 0x03;
			da_push_back(enc->extra_data, &EP_3);
			zero_cnt = 0;
		}

		if (*d == 0x00)
			zero_cnt++;
		else
			zero_cnt = 0;

		da_push_back(enc->extra_data, d);
	}
	da_push_back(enc->extra_data, (data + size - 1));
}

static bool create_packed_sps_pps_buffers(vaapi_encoder_t *enc,
		buffer_list_t *list)
{
	bitstream_t *bs = bs_create();

	pack_sps(enc, bs);
	create_packed_header_buffers(enc, list, VAEncPackedHeaderSequence,
			bs);
	encode_nalu_to_extra_data(enc, bs);

	bs_reset(bs);

	pack_pps(enc, bs);
	create_packed_header_buffers(enc, list, VAEncPackedHeaderPicture,
			bs);
	encode_nalu_to_extra_data(enc, bs);

	bs_free(bs);

	return true;
}

bool vaapi_encoder_extra_data(vaapi_encoder_t *enc,
		uint8_t **extra_data, size_t *extra_data_size)
{
	if (enc->extra_data.num == 0)
		return false;

	*extra_data = enc->extra_data.array;
	*extra_data_size = enc->extra_data.num;

	return true;
}

static bool render_picture(vaapi_encoder_t *enc, buffer_list_t *list,
		VASurfaceID input)
{
	VAStatus status;

	CHECK_STATUS_FALSE(vaBeginPicture(enc->display, enc->context, input));
	CHECK_STATUS_FALSE(vaRenderPicture(enc->display, enc->context,
			(VABufferID *)list->array, list->num));
	CHECK_STATUS_FALSE(vaEndPicture(enc->display, enc->context));
	CHECK_STATUS_FALSE(vaSyncSurface(enc->display, input));

	return true;
}

bool encode_surface(vaapi_encoder_t *enc, VASurfaceID input)
{
	DARRAY(VABufferID) buffers = {0};
	VABufferID output_buffer;
	vaapi_slice_type_t slice_type;

	// todo: implement b frame handling
	if ((enc->frame_cnt % enc->intra_period) == 0)
		slice_type = VAAPI_SLICE_TYPE_I;
	else
		slice_type = VAAPI_SLICE_TYPE_P;

	// todo: pool buffers
	if (!create_seq_buffer(enc, &buffers.da))
		goto fail;
	if (!create_misc_hdr_buffer(enc, &buffers.da))
		goto fail;
	if (!create_misc_rc_buffer(enc, &buffers.da))
		goto fail;
	if (!create_slice_buffer(enc, &buffers.da, slice_type))
		goto fail;

	if (enc->frame_cnt == 0)
		if (!create_packed_sps_pps_buffers(enc, &buffers.da))
			goto fail;

	// can we reuse output buffers?
	output_buffer = create_output_buffer(enc);
	if (!create_pic_buffer(enc, &buffers.da, output_buffer))
		goto fail;

	surface_entry_t e = {
		.surface = input,
		.output = output_buffer,
		.list = buffers.da,
		.pts = enc->frame_cnt,
		.type = slice_type
	};

	if (!surface_queue_push_and_render(enc->surfq, &e))
		goto fail;

	enc->frame_cnt++;
	return true;

fail:
	destroy_buffers(enc, &buffers.da);
	da_free(buffers);
	return false;
}

bool upload_frame_to_surface(vaapi_encoder_t *enc, struct encoder_frame *frame,
		VASurfaceID surface)
{
	VAStatus status;
	VAImage image;
	void *data;

	CHECK_STATUS_FALSE(vaDeriveImage(enc->display, surface, &image));

	CHECK_STATUS_FAIL(vaMapBuffer(enc->display, image.buf, &data));
	for(uint32_t i = 0; i < 2; i++) {
		uint8_t *d_in = frame->data[i];
		uint8_t *d_out = data + image.offsets[i];
		for(uint32_t j = 0; j < enc->height / (i+1); j++) {
			memcpy(d_out, d_in, enc->width);
			d_in += frame->linesize[i];
			d_out += image.pitches[i];
		}
	}
	CHECK_STATUS_FAIL(vaUnmapBuffer(enc->display, image.buf));

	vaDestroyImage(enc->display, image.image_id);

	return true;

fail:
	vaDestroyImage(enc->display, image.image_id);

	return false;
}

bool vaapi_encoder_encode(vaapi_encoder_t *enc, struct encoder_frame *frame)
{
	VASurfaceID input_surface;

	if (!surface_queue_pop_available(enc->surfq, &input_surface)) {
		VA_LOG(LOG_ERROR, "unable to aquire input surface");
		goto fail;
	}

	// todo: add profiling

	if (!upload_frame_to_surface(enc, frame, input_surface)) {
		VA_LOG(LOG_ERROR, "unable to upload frame to input surface");
		goto fail;
	}

	if (!encode_surface(enc, input_surface)) {
		VA_LOG(LOG_ERROR, "unable to encode frame");
		goto fail;
	}

	coded_block_entry_t c;
	bool success;
	if (!surface_queue_pop_finished(enc->surfq, &c, &success)) {
		VA_LOG(LOG_ERROR, "unable to pop finished frame");
		goto fail;
	}

	if (success)
		enc->coded_block_cb(enc->coded_block_cb_opaque, &c);

	return true;

fail:
	vaDestroySurfaces(enc->display, &input_surface, 1);

	return false;
}

vaapi_encoder_t *vaapi_encoder_create(vaapi_encoder_attribs_t *attribs)
{
	vaapi_encoder_t *enc;

	enc = bzalloc(sizeof(vaapi_encoder_t));

	enc->display = vaapi_get_display();
	if (enc->display == NULL)
		goto fail;

	enc->caps = vaapi_caps_from_profile(attribs->profile);
	if (enc->caps == NULL) {
		VA_LOG(LOG_ERROR, "failed to find any valid profiles for this "
				" hardware");
		goto fail;
	}

	enc->width                 = attribs->width;
	enc->height                = attribs->height;

	// bitrate internally is bits, not kbits
	enc->bitrate               = attribs->bitrate;
	enc->cbr                   = attribs->cbr;

	enc->framerate_num         = attribs->framerate_num;
	enc->framerate_den         = attribs->framerate_den;
	enc->keyint                = attribs->keyint;
	enc->format                = attribs->format;

	enc->surface_cnt         = attribs->surface_cnt;
	enc->coded_block_cb_opaque = attribs->coded_block_cb_opaque;
	enc->coded_block_cb        = attribs->coded_block_cb;

	enc->output_buf_size = enc->width * enc->height;

	da_resize(enc->refpics, attribs->refpic_cnt);

	if (!initialize_encoder(enc)) {
		VA_LOG(LOG_ERROR, "failed to initialize encoder for profile %s",
				enc->caps->def.name);
		goto fail;
	}

	initialize_defaults(enc);

	enc->surfq = surface_queue_create(enc->display, enc->context,
			enc->surface_cnt, enc->width, enc->height);

	return enc;

fail:
	vaapi_encoder_destroy(enc);

	return NULL;
}
