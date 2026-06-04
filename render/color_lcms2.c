#include <assert.h>
#include <lcms2.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/util/log.h>
#include <wlr/render/color.h>
#include "render/color.h"

static const cmsCIExyY srgb_whitepoint = { 0.3127, 0.3291, 1 };

static const cmsCIExyYTRIPLE srgb_primaries = {
	.Red = { 0.64, 0.33, 1 },
	.Green = { 0.3, 0.6, 1 },
	.Blue = { 0.15, 0.06, 1},
};

static void handle_lcms_error(cmsContext ctx, cmsUInt32Number code, const char *text) {
	wlr_log(WLR_ERROR, "[lcms] %s", text);
}

#define VCGT_LUT_SIZE 256

static uint16_t eval_vcgt_u16(cmsToneCurve *curve, float t) {
	float val = cmsEvalToneCurveFloat(curve, t);
	if (val <= 0.0f) {
		return 0;
	}
	if (val >= 1.0f) {
		return 65535;
	}
	return (uint16_t)(val * 65535.0f + 0.5f);
}

static bool sample_vcgt(cmsHPROFILE profile,
		uint16_t **r_out, uint16_t **g_out, uint16_t **b_out) {
	*r_out = NULL;
	*g_out = NULL;
	*b_out = NULL;

	cmsToneCurve **curves = (cmsToneCurve **)cmsReadTag(profile, cmsSigVcgtTag);
	if (curves == NULL || curves[0] == NULL
			|| curves[1] == NULL || curves[2] == NULL) {
		return false;
	}

	uint16_t *r = malloc(VCGT_LUT_SIZE * sizeof(uint16_t));
	uint16_t *g = malloc(VCGT_LUT_SIZE * sizeof(uint16_t));
	uint16_t *b = malloc(VCGT_LUT_SIZE * sizeof(uint16_t));
	if (r == NULL || g == NULL || b == NULL) {
		free(r);
		free(g);
		free(b);
		return false;
	}

	for (size_t i = 0; i < VCGT_LUT_SIZE; i++) {
		float t = (float)i / (float)(VCGT_LUT_SIZE - 1);
		r[i] = eval_vcgt_u16(curves[0], t);
		g[i] = eval_vcgt_u16(curves[1], t);
		b[i] = eval_vcgt_u16(curves[2], t);
	}

	*r_out = r;
	*g_out = g;
	*b_out = b;
	return true;
}

struct wlr_color_transform *wlr_color_transform_init_linear_to_icc(
		const void *data, size_t size) {
	struct wlr_color_transform_lcms2 *tx = NULL;
	uint16_t *vcgt_r = NULL, *vcgt_g = NULL, *vcgt_b = NULL;

	cmsContext ctx = cmsCreateContext(NULL, NULL);
	if (ctx == NULL) {
		wlr_log(WLR_ERROR, "cmsCreateContext failed");
		return NULL;
	}

	cmsSetLogErrorHandlerTHR(ctx, handle_lcms_error);

	cmsHPROFILE icc_profile = cmsOpenProfileFromMemTHR(ctx, data, size);
	if (icc_profile == NULL) {
		wlr_log(WLR_ERROR, "cmsOpenProfileFromMemTHR failed");
		goto error_ctx;
	}

	if (cmsGetDeviceClass(icc_profile) != cmsSigDisplayClass) {
		wlr_log(WLR_ERROR, "ICC profile must have the Display device class");
		goto error_icc_profile;
	}

	bool has_vcgt = sample_vcgt(icc_profile, &vcgt_r, &vcgt_g, &vcgt_b);

	cmsToneCurve *linear_tone_curve = cmsBuildGamma(ctx, 1);
	if (linear_tone_curve == NULL) {
		wlr_log(WLR_ERROR, "cmsBuildGamma failed");
		goto error_vcgt;
	}

	cmsToneCurve *linear_tf[] = {
		linear_tone_curve,
		linear_tone_curve,
		linear_tone_curve,
	};
	cmsHPROFILE srgb_profile = cmsCreateRGBProfileTHR(ctx, &srgb_whitepoint,
		&srgb_primaries, linear_tf);
	cmsFreeToneCurve(linear_tone_curve);
	if (srgb_profile == NULL) {
		wlr_log(WLR_ERROR, "cmsCreateRGBProfileTHR failed");
		goto error_vcgt;
	}

	cmsHTRANSFORM lcms_tr = cmsCreateTransformTHR(ctx,
		srgb_profile, TYPE_RGB_FLT, icc_profile, TYPE_RGB_FLT,
		INTENT_RELATIVE_COLORIMETRIC, 0);
	cmsCloseProfile(srgb_profile);
	cmsCloseProfile(icc_profile);
	if (lcms_tr == NULL) {
		wlr_log(WLR_ERROR, "cmsCreateTransformTHR failed");
		goto error_vcgt_closed;
	}

	tx = calloc(1, sizeof(*tx));
	if (!tx) {
		cmsDeleteTransform(lcms_tr);
		goto error_vcgt_closed;
	}
	wlr_color_transform_init(&tx->base, COLOR_TRANSFORM_LCMS2);
	tx->ctx = ctx;
	tx->lcms = lcms_tr;

	struct wlr_color_transform *eval_transform = &tx->base;

	if (has_vcgt) {
		struct wlr_color_transform *vcgt_transform = wlr_color_transform_init_lut_3x1d(
			VCGT_LUT_SIZE, vcgt_r, vcgt_g, vcgt_b);
		if (vcgt_transform != NULL) {
			struct wlr_color_transform *transforms[] = { &tx->base, vcgt_transform };
			struct wlr_color_transform *pipeline =
				wlr_color_transform_init_pipeline(transforms, 2);
			wlr_color_transform_unref(vcgt_transform);
			if (pipeline != NULL) {
				wlr_color_transform_unref(&tx->base);
				eval_transform = pipeline;
			}
		}
	}
	free(vcgt_r);
	free(vcgt_g);
	free(vcgt_b);

	size_t dim_len = 33;
	float *lut_3d = calloc(3 * dim_len * dim_len * dim_len, sizeof(float));
	if (lut_3d == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		wlr_color_transform_unref(eval_transform);
		return NULL;
	}

	float factor = 1.0f / (dim_len - 1);
	for (size_t b_index = 0; b_index < dim_len; b_index++) {
		for (size_t g_index = 0; g_index < dim_len; g_index++) {
			for (size_t r_index = 0; r_index < dim_len; r_index++) {
				float rgb_in[3] = {
					r_index * factor,
					g_index * factor,
					b_index * factor,
				};
				float rgb_out[3];
				wlr_color_transform_eval(eval_transform, rgb_out, rgb_in);
				size_t offset = 3 * (r_index + dim_len * g_index + dim_len * dim_len * b_index);
				lut_3d[offset] = rgb_out[0];
				lut_3d[offset + 1] = rgb_out[1];
				lut_3d[offset + 2] = rgb_out[2];
			}
		}
	}
	wlr_color_transform_unref(eval_transform);

	struct wlr_color_transform_lut3d *result = calloc(1, sizeof(*result));
	if (!result) {
		free(lut_3d);
		return NULL;
	}
	wlr_color_transform_init(&result->base, COLOR_TRANSFORM_LUT_3D);
	result->dim_len = dim_len;
	result->lut_3d = lut_3d;
	return &result->base;

error_vcgt_closed:
	free(vcgt_r);
	free(vcgt_g);
	free(vcgt_b);
	goto error_ctx;
error_vcgt:
	free(vcgt_r);
	free(vcgt_g);
	free(vcgt_b);
error_icc_profile:
	cmsCloseProfile(icc_profile);
error_ctx:
	cmsDeleteContext(ctx);
	return NULL;
}

void color_transform_lcms2_finish(struct wlr_color_transform_lcms2 *tr) {
	cmsDeleteTransform(tr->lcms);
	cmsDeleteContext(tr->ctx);
}

struct wlr_color_transform_lcms2 *color_transform_lcms2_from_base(
		struct wlr_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_LCMS2);
	struct wlr_color_transform_lcms2 *lcms2 = wl_container_of(tr, lcms2, base);
	return lcms2;
}

void color_transform_lcms2_eval(struct wlr_color_transform_lcms2 *tr,
		float out[static 3], const float in[static 3]) {
	cmsDoTransform(tr->lcms, in, out, 1);
}
