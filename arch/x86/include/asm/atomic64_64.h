/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_ATOMIC64_64_H
#define _ASM_X86_ATOMIC64_64_H

#include <linux/types.h>
#include <asm/alternative.h>
#include <asm/cmpxchg.h>

/* The 64-bit atomic type */

#define ATOMIC64_INIT(i)	{ (i) }

static __always_inline s64 arch_atomic64_read(const atomic64_t *v)
{
	return __READ_ONCE((v)->counter);
}

static __always_inline void arch_atomic64_set(atomic64_t *v, s64 i)
{
	__WRITE_ONCE(v->counter, i);
}

static __always_inline void arch_atomic64_add(s64 i, atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "addq %1, %0"
		     : "=m" (v->counter)
		     : "er" (i), "m" (v->counter) : "memory");
}

static __always_inline void arch_atomic64_sub(s64 i, atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "subq %1, %0"
		     : "=m" (v->counter)
		     : "er" (i), "m" (v->counter) : "memory");
}

static __always_inline bool arch_atomic64_sub_and_test(s64 i, atomic64_t *v)
{
	return GEN_BINARY_RMWcc(LOCK_PREFIX "subq", v->counter, e, "er", i);
}
#define arch_atomic64_sub_and_test arch_atomic64_sub_and_test

static __always_inline void arch_atomic64_inc(atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "incq %0"
		     : "=m" (v->counter)
		     : "m" (v->counter) : "memory");
}
#define arch_atomic64_inc arch_atomic64_inc

static __always_inline void arch_atomic64_dec(atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "decq %0"
		     : "=m" (v->counter)
		     : "m" (v->counter) : "memory");
}
#define arch_atomic64_dec arch_atomic64_dec

static __always_inline bool arch_atomic64_dec_and_test(atomic64_t *v)
{
	return GEN_UNARY_RMWcc(LOCK_PREFIX "decq", v->counter, e);
}
#define arch_atomic64_dec_and_test arch_atomic64_dec_and_test

static __always_inline bool arch_atomic64_inc_and_test(atomic64_t *v)
{
	return GEN_UNARY_RMWcc(LOCK_PREFIX "incq", v->counter, e);
}
#define arch_atomic64_inc_and_test arch_atomic64_inc_and_test

static __always_inline bool arch_atomic64_add_negative(s64 i, atomic64_t *v)
{
	return GEN_BINARY_RMWcc(LOCK_PREFIX "addq", v->counter, s, "er", i);
}
#define arch_atomic64_add_negative arch_atomic64_add_negative

static __always_inline s64 arch_atomic64_add_return(s64 i, atomic64_t *v)
{
	return i + xadd(&v->counter, i);
}
#define arch_atomic64_add_return arch_atomic64_add_return

#define arch_atomic64_sub_return(i, v) arch_atomic64_add_return(-(i), v)

static __always_inline s64 arch_atomic64_fetch_add(s64 i, atomic64_t *v)
{
	return xadd(&v->counter, i);
}
#define arch_atomic64_fetch_add arch_atomic64_fetch_add

#define arch_atomic64_fetch_sub(i, v) arch_atomic64_fetch_add(-(i), v)

static __always_inline s64 arch_atomic64_cmpxchg(atomic64_t *v, s64 old, s64 new)
{
	return arch_cmpxchg(&v->counter, old, new);
}
#define arch_atomic64_cmpxchg arch_atomic64_cmpxchg

static __always_inline bool arch_atomic64_try_cmpxchg(atomic64_t *v, s64 *old, s64 new)
{
	return arch_try_cmpxchg(&v->counter, old, new);
}
#define arch_atomic64_try_cmpxchg arch_atomic64_try_cmpxchg

static __always_inline s64 arch_atomic64_xchg(atomic64_t *v, s64 new)
{
	return arch_xchg(&v->counter, new);
}
#define arch_atomic64_xchg arch_atomic64_xchg

static __always_inline void arch_atomic64_and(s64 i, atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "andq %1, %0"
			: "+m" (v->counter)
			: "er" (i)
			: "memory");
}

static __always_inline s64 arch_atomic64_fetch_and(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read(v);

	do {
	} while (!arch_atomic64_try_cmpxchg(v, &val, val & i));
	return val;
}
#define arch_atomic64_fetch_and arch_atomic64_fetch_and

static __always_inline void arch_atomic64_or(s64 i, atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "orq %1, %0"
			: "+m" (v->counter)
			: "er" (i)
			: "memory");
}

static __always_inline s64 arch_atomic64_fetch_or(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read(v);

	do {
	} while (!arch_atomic64_try_cmpxchg(v, &val, val | i));
	return val;
}
#define arch_atomic64_fetch_or arch_atomic64_fetch_or

static __always_inline void arch_atomic64_xor(s64 i, atomic64_t *v)
{
	asm_inline volatile(LOCK_PREFIX "xorq %1, %0"
			: "+m" (v->counter)
			: "er" (i)
			: "memory");
}

static __always_inline s64 arch_atomic64_fetch_xor(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read(v);

	do {
	} while (!arch_atomic64_try_cmpxchg(v, &val, val ^ i));
	return val;
}
#define arch_atomic64_fetch_xor arch_atomic64_fetch_xor

#endif /* _ASM_X86_ATOMIC64_64_H */
