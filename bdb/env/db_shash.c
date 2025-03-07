/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: db_shash.c,v 1.1 2006/01/28 00:09:27 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>
#endif

#include "db_int.h"

/*
 * Table of good hash values.  Up to ~250,000 buckets, we use powers of 2.
 * After that, we slow the rate of increase by half.  For each choice, we
 * then use a nearby prime number as the hash value.
 *
 * If a terabyte is the maximum cache we'll see, and we assume there are
 * 10 1K buckets on each hash chain, then 107374182 is the maximum number
 * of buckets we'll ever need.
 */
static const struct {
	u_int32_t power;
	u_int32_t prime;
} list[] = {
	{	 32,		37},		/* 2^5 */
	{	 64,		67},		/* 2^6 */
	{	128,	       131},		/* 2^7 */
	{	256,	       257},		/* 2^8 */
	{	512,	       521},		/* 2^9 */
	{      1024,	      1031},		/* 2^10 */
	{      2048,	      2053},		/* 2^11 */
	{      4096,	      4099},		/* 2^12 */
	{      8192,	      8191},		/* 2^13 */
	{     16384,	     16381},		/* 2^14 */
	{     32768,	     32771},		/* 2^15 */
	{     65536,	     65537},		/* 2^16 */
	{    131072,	    131071},		/* 2^17 */
	{    262144,	    262147},		/* 2^18 */
	{    393216,	    393209},		/* 2^18 + 2^18/2 */
	{    524288,	    524287},		/* 2^19 */
	{    786432,	    786431},		/* 2^19 + 2^19/2 */
	{   1048576,	   1048573},		/* 2^20 */
	{   1572864,	   1572869},		/* 2^20 + 2^20/2 */
	{   2097152,	   2097169},		/* 2^21 */
	{   3145728,	   3145721},		/* 2^21 + 2^21/2 */
	{   4194304,	   4194301},		/* 2^22 */
	{   6291456,	   6291449},		/* 2^22 + 2^22/2 */
	{   8388608,	   8388617},		/* 2^23 */
	{  12582912,	  12582917},		/* 2^23 + 2^23/2 */
	{  16777216,	  16777213},		/* 2^24 */
	{  25165824,	  25165813},		/* 2^24 + 2^24/2 */
	{  33554432,	  33554393},		/* 2^25 */
	{  50331648,	  50331653},		/* 2^25 + 2^25/2 */
	{  67108864,	  67108859},		/* 2^26 */
	{ 100663296,	 100663291},		/* 2^26 + 2^26/2 */
	{ 134217728,	 134217757},		/* 2^27 */
	{ 201326592,	 201326611},		/* 2^27 + 2^27/2 */
	{ 268435456,	 268435459},		/* 2^28 */
	{ 402653184,	 402653189},		/* 2^28 + 2^28/2 */
	{ 536870912,	 536870909},		/* 2^29 */
	{ 805306368,	 805306357},		/* 2^29 + 2^29/2 */
	{1073741824,	1073741827},		/* 2^30 */
	{0,		0}
};

/*
 * __db_tablesize --
 *	Choose a size for the hash table.
 *
 * PUBLIC: int __db_tablesize __P((u_int32_t));
 */
int
__db_tablesize(n_buckets)
	u_int32_t n_buckets;
{
	int i;

	/*
	 * We try to be clever about how big we make the hash tables.  Use a
	 * prime number close to the "suggested" number of elements that will
	 * be in the hash table.  Use 64 as the minimum hash table size.
	 *
	 * Ref: Sedgewick, Algorithms in C, "Hash Functions"
	 */
	if (n_buckets < 32)
		n_buckets = 32;

	for (i = 0;; ++i) {
		if (list[i].power == 0) {
			--i;
			break;
		}
		if (list[i].power >= n_buckets)
			break;
	}
	return (list[i].prime);
}

/*
 * __db_hashinit --
 *	Initialize a hash table that resides in shared memory.
 *
 * PUBLIC: void __db_hashinit __P((void *, u_int32_t));
 */
void
__db_hashinit(begin, nelements)
	void *begin;
	u_int32_t nelements;
{
	u_int32_t i;
	SH_TAILQ_HEAD(hash_head) *headp;

	headp = (struct hash_head *)begin;

	for (i = 0; i < nelements; i++, headp++)
		SH_TAILQ_INIT(headp);
}
