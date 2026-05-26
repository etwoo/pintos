#ifndef __LIB_KERNEL_FIXED_POINT_H
#define __LIB_KERNEL_FIXED_POINT_H

#include <inttypes.h>

/* represent fixed-point value as struct for type safety at callsites */
struct fix_t {
	int32_t val;
};

struct fix_t i32_to_fixed(int32_t n);
int32_t fixed_to_i32_rounding_zero(struct fix_t x);
int32_t fixed_to_i32_rounding_nearest(struct fix_t x);

struct fix_t add_fixed_fixed(struct fix_t x, struct fix_t y);
struct fix_t sub_fixed_fixed(struct fix_t x, struct fix_t y);
struct fix_t mul_fixed_fixed(struct fix_t x, struct fix_t y);
struct fix_t div_fixed_fixed(struct fix_t x, struct fix_t y);

struct fix_t add_fixed_i32(struct fix_t x, int32_t n);
struct fix_t sub_fixed_i32(struct fix_t x, int32_t n);
struct fix_t mul_fixed_i32(struct fix_t x, int32_t n);
struct fix_t div_fixed_i32(struct fix_t x, int32_t n);

#endif
