#pragma once

#include <obs-module.h>
#include <util/darray.h>

#include <va/va.h>

#define VA_LOG(level, format, ...) \
	blog(level, "[VAAPI encoder]: " format, ##__VA_ARGS__)

#define VA_LOG_STATUS(level, x, status) \
	VA_LOG(LOG_ERROR, "%s: %s", #x, vaErrorStr(status));

#define CHECK_STATUS_(x, y) \
	do { \
		status = (x); \
		if (status != VA_STATUS_SUCCESS) { \
			VA_LOG_STATUS(LOG_ERROR, #x, status); \
			y; \
		} \
	} while (false)

#define CHECK_STATUS_FAIL(x) \
	CHECK_STATUS_(x, goto fail)

#define CHECK_STATUS_FAILN(x, n) \
	CHECK_STATUS_(x, goto fail ## n)

#define CHECK_STATUS_FALSE(x) \
	CHECK_STATUS_(x, return false)

static inline size_t round_up_to_power_of_2(size_t value, size_t alignment)
{
	return ((value + (alignment - 1)) & ~(alignment - 1));
}

typedef struct darray buffer_list_t;

enum vaapi_slice_type
{
	VAAPI_SLICE_TYPE_P,
	VAAPI_SLICE_TYPE_B,
	VAAPI_SLICE_TYPE_I
};
typedef enum vaapi_slice_type vaapi_slice_type_t;

struct coded_block_entry {
	DARRAY(uint8_t) data;
	uint64_t pts;
	vaapi_slice_type_t type;
};
typedef struct coded_block_entry coded_block_entry_t;

VADisplay vaapi_get_display();
