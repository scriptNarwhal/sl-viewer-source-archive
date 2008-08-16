/** 
 * @file llpanelevent.h
 * @brief Display for events in the finder
 *
 * Copyright (c) 2004-2007, Linden Research, Inc.
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

#ifndef LL_LLPANELEVENT_H
#define LL_LLPANELEVENT_H

#include "llpanel.h"

#include "lleventinfo.h"
#include "lluuid.h"
#include "v3dmath.h"

class LLTextBox;
class LLTextEditor;
class LLButton;
class LLMessageSystem;

class LLPanelEvent : public LLPanel
{
public:
	LLPanelEvent();
	/*virtual*/ ~LLPanelEvent();

	/*virtual*/ BOOL postBuild();
	/*virtual*/ void draw();

	void setEventID(const U32 event_id);
	void sendEventInfoRequest();

	static void processEventInfoReply(LLMessageSystem *msg, void **);

	U32 getEventID() { return mEventID; }

protected:
	void resetInfo();

	static void onClickTeleport(void*);
	static void onClickMap(void*);
	//static void onClickLandmark(void*);
	static void onClickCreateEvent(void*);
	static void onClickNotify(void*);

	static void callbackCreateEventWebPage(S32 options, void* data);

protected:
	U32				mEventID;
	LLEventInfo		mEventInfo;

	LLTextBox*		mTBName;
	LLTextBox*		mMatureText;
	LLTextBox*		mTBCategory;
	LLTextBox*		mTBDate;
	LLTextBox*		mTBDuration;
	LLTextEditor*	mTBDesc;

	LLTextBox*		mTBRunBy;
	LLTextBox*		mTBLocation;
	LLTextBox*		mTBCover;

	LLButton*		mTeleportBtn;
	LLButton*		mMapBtn;
	//LLButton*		mLandmarkBtn;
	LLButton*		mCreateEventBtn;
	LLButton*		mNotifyBtn;

	static LLLinkedList<LLPanelEvent> sAllPanels;
};

#endif // LL_LLPANELEVENT_H