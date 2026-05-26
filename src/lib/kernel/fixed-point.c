#include <fixed-point.h>

/* P.Q fixed-point real arithmetic with Q == 14 */
#define F (2 << 14)

struct fix_t
i32_to_fixed(int32_t n)
{
	return (struct fix_t){
		.val = n * F,
	};
}

int32_t
fixed_to_i32_rounding_zero(struct fix_t x)
{
	return x.val / F;
}

int32_t
fixed_to_i32_rounding_nearest(struct fix_t x)
{
	return x.val >= 0 ? (x.val + F / 2) / F : (x.val - F / 2) / F;
}

struct fix_t
add_fixed_fixed(struct fix_t x, struct fix_t y)
{
	return (struct fix_t){
		.val = x.val + y.val,
	};
}

struct fix_t
sub_fixed_fixed(struct fix_t x, struct fix_t y)
{
	return (struct fix_t){
		.val = x.val - y.val,
	};
}

struct fix_t
mul_fixed_fixed(struct fix_t x, struct fix_t y)
{
	return (struct fix_t){
		.val = ((int64_t)x.val) * y.val / F,
	};
}

struct fix_t
div_fixed_fixed(struct fix_t x, struct fix_t y)
{
	return (struct fix_t){
		.val = ((int64_t)x.val) * F / y.val,
	};
}

struct fix_t
add_fixed_i32(struct fix_t x, int32_t n)
{
	return add_fixed_fixed(x, i32_to_fixed(n));
}

struct fix_t
sub_fixed_i32(struct fix_t x, int32_t n)
{
	return sub_fixed_fixed(x, i32_to_fixed(n));
}

struct fix_t
mul_fixed_i32(struct fix_t x, int32_t n)
{
	return (struct fix_t){
		.val = x.val * n,
	};
}

struct fix_t
div_fixed_i32(struct fix_t x, int32_t n)
{
	return (struct fix_t){
		.val = x.val / n,
	};
}

#undef F
