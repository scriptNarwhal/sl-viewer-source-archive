/** 
 * @file llpanelaudioprefs.h
 * @brief Audio preference definitions
 *
 * Copyright (c) 2001-2007, Linden Research, Inc.
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

#ifndef LL_LLPANELAUDIOPREFS_H
#define LL_LLPANELAUDIOPREFS_H

#include "llpanel.h"

class LLSpinCtrl;
class LLSlider;
class LLSliderCtrl;
class LLCheckBoxCtrl;
class LLTextBox;
class LLRadioGroup;

class LLPanelAudioPrefs : public LLPanel
{
public:
	LLPanelAudioPrefs();
	virtual ~LLPanelAudioPrefs();

	void apply();	// Apply the changed values.
	void cancel();	// Cancel the changed values.

	void enable(BOOL b);
	virtual BOOL postBuild();
	
	static void restartCallback(S32 option, void *userdata);
	static void onMuteAudio(LLUICtrl* ctrl, void* userdata);

protected:


	F32             mPreviousVolume;
	F32             mPreviousMediaVolume;
	F32             mPreviousUI;
	F32             mPreviousFootsteps;
	F32             mPreviousWind;
	F32             mPreviousDoppler;
	F32             mPreviousDistance;
	F32             mPreviousRolloff;

	S32             mPreviousBitrate;

	F32             mPreviousMoneyThreshold;
	F32             mPreviousHealthThreshold;

	BOOL			mPreviousStreamingMusic;
	BOOL			mPreviousStreamingVideo;
	BOOL            mPreviousMuteAudio;
	BOOL            mPreviousMuteWhenMinimized;
};

#endif