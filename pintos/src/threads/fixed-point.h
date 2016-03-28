#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#define FIXED_POINT_Q 14
#define FIXED_POINT_F (1 << FIXED_POINT_Q)

typedef int fixed_point;

static inline fixed_point
fp (int n)
{
  return n * FIXED_POINT_F;
}

static inline int
fp_floor (fixed_point x)
{
  return x / FIXED_POINT_F;
}

static inline int
fp_round (fixed_point x)
{
  if (x >= 0)
    return (x + FIXED_POINT_F / 2) / FIXED_POINT_F;
  else
    return (x - FIXED_POINT_F / 2) / FIXED_POINT_F;
}

static inline fixed_point
fp_add (fixed_point x, fixed_point y)
{
  return x + y;
}

static inline fixed_point
fp_add_int (fixed_point x, int n)
{
  return x + n * FIXED_POINT_F;
}

static inline fixed_point
fp_subtract (fixed_point x, fixed_point y)
{
  return x - y;
}

static inline fixed_point
fp_subtract_int (fixed_point x, int n)
{
  return x - n * FIXED_POINT_F;
}


static inline fixed_point
fp_multiply (fixed_point x, fixed_point y)
{
  return ((int64_t) x) * y / FIXED_POINT_F;
}

static inline fixed_point
fp_multiply_int (fixed_point x, int y)
{
  return x * y;
}

static inline fixed_point
fp_divide (fixed_point x, fixed_point y)
{
  ASSERT(y != 0);
  return ((int64_t) x) * FIXED_POINT_F / y;
}

static inline fixed_point
fp_divide_int (fixed_point x, int n)
{
  ASSERT(n != 0);
  return x / n;
}

#endif /* threads/fixed-point.h */ 

