/** 
 * @file llgivemoney.cpp
 * @author Aaron Brashears, Kelly Washington, James Cook
 * @brief Implementation of the LLFloaterPay class.
 *
 * Copyright (c) 2002-2007, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"

#include "llgivemoney.h"

#include "message.h"

#include "llagent.h"
#include "llresmgr.h"
#include "lltextbox.h"
#include "lllineeditor.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llbutton.h"
#include "llselectmgr.h"
#include "lltransactiontypes.h"
#include "llvieweruictrlfactory.h"

///----------------------------------------------------------------------------
/// Local function declarations, constants, enums, and typedefs
///----------------------------------------------------------------------------

const char* GIVE_MONEY_TITLE = "";	// Dialog contains text "Pay"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLGiveMoneyInfo
//
// A small class used to track callback information
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

struct LLGiveMoneyInfo
{
	LLFloaterPay* mFloater;
	S32 mAmount;
	LLGiveMoneyInfo(LLFloaterPay* floater, S32 amount) :
		mFloater(floater), mAmount(amount){}
};

///----------------------------------------------------------------------------
/// Class LLFloaterPay
///----------------------------------------------------------------------------

S32 LLFloaterPay::sLastAmount = 0;
const S32 MAX_AMOUNT_LENGTH = 10;
const S32 FASTPAY_BUTTON_WIDTH = 80;

// Default constructor
LLFloaterPay::LLFloaterPay(const std::string& name, 
						   money_callback callback,
						   const LLUUID& uuid,
						   BOOL target_is_object) :
	LLFloater(name, "FloaterPayRectB", GIVE_MONEY_TITLE, RESIZE_NO,
				DEFAULT_MIN_WIDTH, DEFAULT_MIN_HEIGHT, DRAG_ON_TOP,
				MINIMIZE_NO, CLOSE_YES),
	mCallbackData(),
	mCallback(callback),
	mObjectNameText(NULL),
	mTargetUUID(uuid),
	mTargetIsObject(target_is_object),
	mTargetIsGroup(FALSE)
{
	if (target_is_object)
	{
		gUICtrlFactory->buildFloater(this,"floater_pay_object.xml");
	}
	else 
	{
		gUICtrlFactory->buildFloater(this,"floater_pay.xml");
	}

	
	S32 i =0;

	LLGiveMoneyInfo* info = new LLGiveMoneyInfo(this, PAY_BUTTON_DEFAULT_0);
	mCallbackData.push_back(info);

	childSetAction("fastpay 1",&LLFloaterPay::onGive,info);
	childSetVisible("fastpay 1", FALSE);

	mQuickPayButton[i] = LLUICtrlFactory::getButtonByName(this, "fastpay 1");
	mQuickPayInfo[i] = info;
	++i;

	info = new LLGiveMoneyInfo(this, PAY_BUTTON_DEFAULT_1);
	mCallbackData.push_back(info);

	childSetAction("fastpay 5",&LLFloaterPay::onGive,info);
	childSetVisible("fastpay 5", FALSE);

	mQuickPayButton[i] = LLUICtrlFactory::getButtonByName(this, "fastpay 5");
	mQuickPayInfo[i] = info;
	++i;

	info = new LLGiveMoneyInfo(this, PAY_BUTTON_DEFAULT_2);
	mCallbackData.push_back(info);

	childSetAction("fastpay 10",&LLFloaterPay::onGive,info);
	childSetVisible("fastpay 10", FALSE);

	mQuickPayButton[i] = LLUICtrlFactory::getButtonByName(this, "fastpay 10");
	mQuickPayInfo[i] = info;
	++i;

	info = new LLGiveMoneyInfo(this, PAY_BUTTON_DEFAULT_3);
	mCallbackData.push_back(info);

	childSetAction("fastpay 20",&LLFloaterPay::onGive,info);
	childSetVisible("fastpay 20", FALSE);

	mQuickPayButton[i] = LLUICtrlFactory::getButtonByName(this, "fastpay 20");
	mQuickPayInfo[i] = info;
	++i;

	
    childSetVisible("amount text", FALSE);	

	LLString last_amount;
	if(sLastAmount > 0)
	{
		last_amount = llformat("%d", sLastAmount);
	}

    childSetVisible("amount", FALSE);
	
	childSetKeystrokeCallback("amount", &LLFloaterPay::onKeystroke, this);
	childSetText("amount", last_amount);
	childSetPrevalidate("desc", LLLineEditor::prevalidatePositiveS32);

	info = new LLGiveMoneyInfo(this, 0);
	mCallbackData.push_back(info);

	childSetAction("pay btn",&LLFloaterPay::onGive,info);
	setDefaultBtn("pay btn");
	childSetVisible("pay btn", FALSE);
	childSetEnabled("pay btn", (sLastAmount > 0));

	childSetAction("cancel btn",&LLFloaterPay::onCancel,this);

	center();
	open();
}

// Destroys the object
LLFloaterPay::~LLFloaterPay()
{
	std::for_each(mCallbackData.begin(), mCallbackData.end(), DeletePointer());
}

// static
void LLFloaterPay::processPayPriceReply(LLMessageSystem* msg, void **userdata)
{
	LLFloaterPay* self = (LLFloaterPay*)userdata;
	if (self)
	{
		S32 price;
		LLUUID target;

		msg->getUUIDFast(_PREHASH_ObjectData,_PREHASH_ObjectID,target);
		if (target != self->mTargetUUID)
		{
			// This is a message for a different object's pay info
			return;
		}

		msg->getS32Fast(_PREHASH_ObjectData,_PREHASH_DefaultPayPrice,price);
		
		if (PAY_PRICE_HIDE == price)
		{
			self->childSetVisible("amount", FALSE);
			self->childSetVisible("pay btn", FALSE);
			self->childSetVisible("amount text", FALSE);
		}
		else if (PAY_PRICE_DEFAULT == price)
		{			
			self->childSetVisible("amount", TRUE);
			self->childSetVisible("pay btn", TRUE);
			self->childSetVisible("amount text", TRUE);
		}
		else
		{
			// PAY_PRICE_HIDE and PAY_PRICE_DEFAULT are negative values
			// So we take the absolute value here after we have checked for those cases
			
			self->childSetVisible("amount", TRUE);
			self->childSetVisible("pay btn", TRUE);
			self->childSetEnabled("pay btn", TRUE);
			self->childSetVisible("amount text", TRUE);

			self->childSetText("amount", llformat("%d", llabs(price)));
		}

		S32 num_blocks = msg->getNumberOfBlocksFast(_PREHASH_ButtonData);
		S32 i = 0;
		if (num_blocks > MAX_PAY_BUTTONS) num_blocks = MAX_PAY_BUTTONS;

		S32 max_pay_amount = 0;
		S32 padding_required = 0;

		for (i=0;i<num_blocks;++i)
		{
			S32 pay_button;
			msg->getS32Fast(_PREHASH_ButtonData,_PREHASH_PayButton,pay_button,i);
			if (pay_button > 0)
			{
				LLString button_str;
				gResMgr->getMonetaryString( button_str, pay_button );

				self->mQuickPayButton[i]->setLabelSelected(button_str.c_str());
				self->mQuickPayButton[i]->setLabelUnselected(button_str.c_str());
				self->mQuickPayButton[i]->setVisible(TRUE);
				self->mQuickPayInfo[i]->mAmount = pay_button;
				self->childSetVisible("fastpay text",TRUE);

				if ( pay_button > max_pay_amount )
				{
					max_pay_amount = pay_button;
				}
			}
			else
			{
				self->mQuickPayButton[i]->setVisible(FALSE);
			}
		}

		// build a string containing the maximum value and calc nerw button width from it.
		LLString balance_str;
		gResMgr->getMonetaryString( balance_str, max_pay_amount );
		const LLFontGL* font = gResMgr->getRes(LLFONT_SANSSERIF);
		S32 new_button_width = font->getWidth( LLString(balance_str));
		new_button_width += ( 12 + 12 );	// padding

		// dialong is sized for 2 digit pay amounts - larger pay values need to be scaled
		const S32 threshold = 100000;
		if ( max_pay_amount >= threshold )
		{
			S32 num_digits_threshold = (S32)log10((double)threshold) + 1;
			S32 num_digits_max = (S32)log10((double)max_pay_amount) + 1;
				
			// calculate the extra width required by 2 buttons with max amount and some commas
			padding_required = ( num_digits_max - num_digits_threshold + ( num_digits_max / 3 ) ) * font->getWidth( "0" );
		};

		// change in button width
		S32 button_delta = new_button_width - FASTPAY_BUTTON_WIDTH;
		if ( button_delta < 0 ) 
			button_delta = 0;

		// now we know the maximum amount, we can resize all the buttons to be 
		for (i=0;i<num_blocks;++i)
		{
			LLRect r;
			r = self->mQuickPayButton[i]->getRect();

			// RHS button colum needs to move further because LHS changed too
			if ( i % 2 )
			{
				r.setCenterAndSize( r.getCenterX() + ( button_delta * 3 ) / 2 , 
					r.getCenterY(), 
						r.getWidth() + button_delta, 
							r.getHeight() ); 
			}
			else
			{
				r.setCenterAndSize( r.getCenterX() + button_delta / 2, 
					r.getCenterY(), 
						r.getWidth() + button_delta, 
						r.getHeight() ); 
			}
			self->mQuickPayButton[i]->setRect( r );
		}

		for (i=num_blocks;i<MAX_PAY_BUTTONS;++i)
		{
			self->mQuickPayButton[i]->setVisible(FALSE);
		}

		self->reshape( self->mRect.getWidth() + padding_required, self->mRect.getHeight(), FALSE );
	}
	msg->setHandlerFunc("PayPriceReply",NULL,NULL);
}

// static
void LLFloaterPay::payViaObject(money_callback callback, const LLUUID& object_id)
{
	LLViewerObject* object = gObjectList.findObject(object_id);
	if (!object) return;
	
	LLSelectNode* node = gSelectMgr->getFirstRootNode();
	if (!node) return;
	
	LLFloaterPay *floater = new LLFloaterPay("Give Money", callback, object_id, TRUE);
	if (!floater) return;
	
	LLHost target_region = object->getRegion()->getHost();
	
	LLMessageSystem* msg = gMessageSystem;
	msg->newMessageFast(_PREHASH_RequestPayPrice);
	msg->nextBlockFast(_PREHASH_ObjectData);
	msg->addUUIDFast(_PREHASH_ObjectID, object_id);
	msg->sendReliable(target_region);
	msg->setHandlerFuncFast(_PREHASH_PayPriceReply, processPayPriceReply,(void **)floater);
	
	LLUUID owner_id;
	BOOL is_group = FALSE;
	node->mPermissions->getOwnership(owner_id, is_group);
	
	floater->childSetText("object_name_text",node->mName);

	floater->finishPayUI(owner_id, is_group);
}

void LLFloaterPay::payDirectly(money_callback callback, 
							   const LLUUID& target_id,
							   BOOL is_group)
{
	LLFloaterPay *floater = new LLFloaterPay("Give Money", callback, target_id, FALSE);
	if (!floater) return;
	
	floater->childSetVisible("amount", TRUE);
	floater->childSetVisible("pay btn", TRUE);
	floater->childSetVisible("amount text", TRUE);

	floater->childSetVisible("fastpay text",TRUE);
	for(S32 i=0;i<MAX_PAY_BUTTONS;++i)
	{
		floater->mQuickPayButton[i]->setVisible(TRUE);
	}
	
	floater->finishPayUI(target_id, is_group);
}
	
void LLFloaterPay::finishPayUI(const LLUUID& target_id, BOOL is_group)
{
	gCacheName->get(target_id, is_group, onCacheOwnerName, (void*)this);
	
	// Make sure the amount field has focus

	childSetFocus("amount", TRUE);
	
	LLLineEditor* amount = LLUICtrlFactory::getLineEditorByName(this, "amount");
	amount->selectAll();
	mTargetIsGroup = is_group;
}

// static
void LLFloaterPay::onCacheOwnerName(const LLUUID& owner_id,
									const char* firstname,
									const char* lastname,
									BOOL is_group,
									void* userdata)
{
	LLFloaterPay* self = (LLFloaterPay*)userdata;
	if (!self) return;
	
	if (is_group)
	{
		self->childSetVisible("payee_group",true);
		self->childSetVisible("payee_resident",false);
	}
	else
	{
		self->childSetVisible("payee_group",false);
		self->childSetVisible("payee_resident",true);
	}
	
	self->childSetTextArg("payee_name", "[FIRST]", firstname);
	self->childSetTextArg("payee_name", "[LAST]", lastname);
}

// static
void LLFloaterPay::onCancel(void* data)
{
	LLFloaterPay* self = reinterpret_cast<LLFloaterPay*>(data);
	if(self)
	{
		if (self->mTargetIsObject)
		{
			gSelectMgr->deselectAll();
		}
		self->close();
	}
}

// static
void LLFloaterPay::onKeystroke(LLLineEditor*, void* data)
{
	LLFloaterPay* self = reinterpret_cast<LLFloaterPay*>(data);
	if(self)
	{
		if (!self->childGetText("amount").empty())
		{
			self->childSetEnabled("pay btn", TRUE);
		}
		else
		{
			self->childSetEnabled("pay btn", FALSE);
		}
	}
}

// static
void LLFloaterPay::onGive(void* data)
{
	LLGiveMoneyInfo* info = reinterpret_cast<LLGiveMoneyInfo*>(data);
	if(info && info->mFloater)
	{
		info->mFloater->give(info->mAmount);
		info->mFloater->close();
	}
}

void LLFloaterPay::give(S32 amount)
{
	if(mCallback)
	{
		// if the amount is 0, that menas that we should use the
		// text field.
		if(amount == 0)
		{
			amount = atoi(childGetText("amount").c_str());
		}
		sLastAmount = amount;

		// Try to pay an object.
		if (mTargetIsObject)
		{
			LLViewerObject* dest_object = gObjectList.findObject(mTargetUUID);
			if(dest_object)
			{
				LLViewerRegion* region = dest_object->getRegion();
				if (region)
				{
					// Find the name of the root object
					LLSelectNode* node = gSelectMgr->getFirstRootNode();
					LLString object_name;
					if (node)
					{
						object_name = node->mName;
					}
					S32 tx_type = TRANS_PAY_OBJECT;
					if(dest_object->isAvatar()) tx_type = TRANS_GIFT;
					mCallback(mTargetUUID, region, amount, FALSE, tx_type, object_name);
					gSelectMgr->deselectAll();
				}
			}
		}
		else
		{
			// just transfer the money
			mCallback(mTargetUUID, gAgent.getRegion(), amount, mTargetIsGroup, TRANS_GIFT, LLString::null);
		}
	}
}

///----------------------------------------------------------------------------
/// Local function definitions
///----------------------------------------------------------------------------
