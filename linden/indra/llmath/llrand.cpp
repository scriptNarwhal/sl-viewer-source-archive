/** 
 * @file llrand.cpp
 * @brief Global random generator.
 *
 * Copyright (c) 2000-2007, Linden Research, Inc.
 * 
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlife.com/developers/opensource/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at http://secondlife.com/developers/opensource/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 */

#include "linden_common.h"

#include "llrand.h"
#include "lluuid.h"

static LLRandLagFib2281 gRandomGenerator(LLUUID::getRandomSeed());

S32 ll_rand()
{
	return (S32)(gRandomGenerator() * RAND_MAX);
}

S32 ll_rand(S32 val)
{
	return (S32)(gRandomGenerator() * val);
}

F32 ll_frand()
{
	return (F32)gRandomGenerator();
}

F32 ll_frand(F32 val)
{
	return (F32)gRandomGenerator() * val;
}

F64 ll_drand()
{
	return gRandomGenerator();
}

F64 ll_drand(F64 val)
{
	return gRandomGenerator() * val;
}