/** 
 * @file llvfs.cpp
 * @brief Implementation of virtual file system
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

#include "linden_common.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <set>
#include <map>
#if LL_WINDOWS
#include <share.h>
#else
#include <sys/file.h>
#endif
    
#include "llvfs.h"
#include "llstl.h"
    
const S32 FILE_BLOCK_MASK = 0x000003FF;	 // 1024-byte blocks
const S32 VFS_CLEANUP_SIZE = 5242880;  // how much space we free up in a single stroke
const S32 BLOCK_LENGTH_INVALID = -1;	// mLength for invalid LLVFSFileBlocks

LLVFS *gVFS = NULL;

// internal class definitions
class LLVFSBlock
{
public:
	LLVFSBlock() 
	{
		mLocation = 0;
		mLength = 0;
	}
    
	LLVFSBlock(U32 loc, S32 size)
	{
		mLocation = loc;
		mLength = size;
	}
    
	static BOOL insertFirstLL(LLVFSBlock *first, LLVFSBlock *second)
	{
		return first->mLocation != second->mLocation
			? first->mLocation < second->mLocation
			: first->mLength < second->mLength;

	}

public:
	U32 mLocation;
	S32	mLength;		// allocated block size
};
    
LLVFSFileSpecifier::LLVFSFileSpecifier()
:	mFileID(),
	mFileType( LLAssetType::AT_NONE )
{
}
    
LLVFSFileSpecifier::LLVFSFileSpecifier(const LLUUID &file_id, const LLAssetType::EType file_type)
{
	mFileID = file_id;
	mFileType = file_type;
}
    
bool LLVFSFileSpecifier::operator<(const LLVFSFileSpecifier &rhs) const
{
	return (mFileID == rhs.mFileID)
		? mFileType < rhs.mFileType
		: mFileID < rhs.mFileID;
}
    
bool LLVFSFileSpecifier::operator==(const LLVFSFileSpecifier &rhs) const
{
	return (mFileID == rhs.mFileID && 
			mFileType == rhs.mFileType);
}
    
    
class LLVFSFileBlock : public LLVFSBlock, public LLVFSFileSpecifier
{
public:
	LLVFSFileBlock() : LLVFSBlock(), LLVFSFileSpecifier()
	{
		init();
	}
    
	LLVFSFileBlock(const LLUUID &file_id, LLAssetType::EType file_type, U32 loc = 0, S32 size = 0)
		: LLVFSBlock(loc, size), LLVFSFileSpecifier( file_id, file_type )
	{
		init();
	}

	void init()
	{
		mSize = 0;
		mIndexLocation = -1;
		mAccessTime = (U32)time(NULL);

		for (S32 i = 0; i < (S32)VFSLOCK_COUNT; i++)
		{
			mLocks[(EVFSLock)i] = 0;
		}
	}

	#ifdef LL_LITTLE_ENDIAN
	inline void swizzleCopy(void *dst, void *src, int size) { memcpy(dst, src, size); }

	#else
	
	inline U32 swizzle32(U32 x)
	{
		return(((x >> 24) & 0x000000FF) | ((x >> 8)  & 0x0000FF00) | ((x << 8)  & 0x00FF0000) |((x << 24) & 0xFF000000));
	}
	
	inline U16 swizzle16(U16 x)
	{
		return(	((x >> 8)  & 0x000000FF) | ((x << 8)  & 0x0000FF00) );
	}
	
	inline void swizzleCopy(void *dst, void *src, int size) 
	{
		if(size == 4)
		{
			((U32*)dst)[0] = swizzle32(((U32*)src)[0]); 
		}
		else if(size == 2)
		{
			((U16*)dst)[0] = swizzle16(((U16*)src)[0]); 
		}
		else
		{
			// Perhaps this should assert...
			memcpy(dst, src, size);
		}
	}
	
	#endif

	void serialize(U8 *buffer)
	{
		swizzleCopy(buffer, &mLocation, 4);
		buffer += 4;
		swizzleCopy(buffer, &mLength, 4);
		buffer +=4;
		swizzleCopy(buffer, &mAccessTime, 4);
		buffer +=4;
		memcpy(buffer, &mFileID.mData, 16);
		buffer += 16;
		S16 temp_type = mFileType;
		swizzleCopy(buffer, &temp_type, 2);
		buffer += 2;
		swizzleCopy(buffer, &mSize, 4);
	}
    
	void deserialize(U8 *buffer, const S32 index_loc)
	{
		mIndexLocation = index_loc;
    
		swizzleCopy(&mLocation, buffer, 4);
		buffer += 4;
		swizzleCopy(&mLength, buffer, 4);
		buffer += 4;
		swizzleCopy(&mAccessTime, buffer, 4);
		buffer += 4;
		memcpy(&mFileID.mData, buffer, 16);
		buffer += 16;
		S16 temp_type;
		swizzleCopy(&temp_type, buffer, 2);
		mFileType = (LLAssetType::EType)temp_type;
		buffer += 2;
		swizzleCopy(&mSize, buffer, 4);
	}
    
	static BOOL insertLRU(LLVFSFileBlock* const& first,
						  LLVFSFileBlock* const& second)
	{
		return (first->mAccessTime == second->mAccessTime)
			? *first < *second
			: first->mAccessTime < second->mAccessTime;
	}
    
public:
	S32  mSize;
	S32  mIndexLocation; // location of index entry
	U32  mAccessTime;
	BOOL mLocks[VFSLOCK_COUNT]; // number of outstanding locks of each type
    
	static const S32 SERIAL_SIZE;
};

// Helper structure for doing lru w/ stl... is there a simpler way?
struct LLVFSFileBlock_less
{
	bool operator()(LLVFSFileBlock* const& lhs, LLVFSFileBlock* const& rhs) const
	{
		return (LLVFSFileBlock::insertLRU(lhs, rhs)) ? true : false;
	}
};


const S32 LLVFSFileBlock::SERIAL_SIZE = 34;
     
    
LLVFS::LLVFS(const char *index_filename, const char *data_filename, const BOOL read_only, const U32 presize, const BOOL remove_after_crash)
:	mRemoveAfterCrash(remove_after_crash)
{
	mDataMutex = new LLMutex(0);

	S32 i;
	for (i = 0; i < VFSLOCK_COUNT; i++)
	{
		mLockCounts[i] = 0;
	}
	mValid = VFSVALID_OK;
	mReadOnly = read_only;
	mIndexFilename = new char[strlen(index_filename) + 1];
	mDataFilename = new char[strlen(data_filename) + 1];
	strcpy(mIndexFilename, index_filename);
	strcpy(mDataFilename, data_filename);
    
	const char *file_mode = mReadOnly ? "rb" : "r+b";
    
	if (! (mDataFP = openAndLock(mDataFilename, file_mode, mReadOnly)))
	{
    	
		if (mReadOnly)
		{
			llwarns << "Can't find " << mDataFilename << " to open read-only VFS" << llendl;
			mValid = VFSVALID_BAD_CANNOT_OPEN_READONLY;
			return;
		}
    
		if((mDataFP = openAndLock(mDataFilename, "w+b", FALSE)))
		{
			// Since we're creating this data file, assume any index file is bogus
			// remove the index, since this vfs is now blank
			LLFile::remove(mIndexFilename);
		}
		else
		{
			llwarns << "Can't open VFS data file " << mDataFilename << " attempting to use alternate" << llendl;
    
			char *temp_index = new char[strlen(mIndexFilename) + 10];
			char *temp_data = new char[strlen(mDataFilename) + 10];

			for (U32 count = 0; count < 256; count++)
			{
				sprintf(temp_index, "%s.%u", mIndexFilename, count);
				sprintf(temp_data, "%s.%u", mDataFilename, count);
    
				// try just opening, then creating, each alternate
				if ((mDataFP = openAndLock(temp_data, "r+b", FALSE)))
				{
					break;
				}

				if ((mDataFP = openAndLock(temp_data, "w+b", FALSE)))
				{
					// we're creating the datafile, so nuke the indexfile
					LLFile::remove(temp_index);
					break;
				}
			}
    
			if (! mDataFP)
			{
				llwarns << "Couldn't open vfs data file after trying many alternates" << llendl;
				mValid = VFSVALID_BAD_CANNOT_CREATE;
				return;
			}

			delete[] mIndexFilename;
			delete[] mDataFilename;
    
			mIndexFilename = temp_index;
			mDataFilename = temp_data;
		}
    
		if (presize)
		{
			presizeDataFile(presize);
		}
	}

	// Did we leave this file open for writing last time?
	// If so, close it and start over.
	if (!mReadOnly && mRemoveAfterCrash)
	{
		llstat marker_info;
		char* marker = new char[strlen(mDataFilename) + strlen(".open") + 1];
		sprintf(marker, "%s.open", mDataFilename);
		if (!LLFile::stat(marker, &marker_info))
		{
			// marker exists, kill the lock and the VFS files
			unlockAndClose(mDataFP);
			mDataFP = NULL;

			llwarns << "VFS: File left open on last run, removing old VFS file " << mDataFilename << llendl;
			LLFile::remove(mIndexFilename);
			LLFile::remove(mDataFilename);
			LLFile::remove(marker);

			mDataFP = openAndLock(mDataFilename, "w+b", FALSE);
			if (!mDataFP)
			{
				llwarns << "Can't open VFS data file in crash recovery" << llendl;
				mValid = VFSVALID_BAD_CANNOT_CREATE;
				return;
			}

			if (presize)
			{
				presizeDataFile(presize);
			}
		}
		delete [] marker;
		marker = NULL;
	}

	// determine the real file size
	fseek(mDataFP, 0, SEEK_END);
	U32 data_size = ftell(mDataFP);

	// read the index file
	// make sure there's at least one file in it too
	// if not, we'll treat this as a new vfs
	llstat fbuf;
	if (! LLFile::stat(mIndexFilename, &fbuf) &&
		fbuf.st_size >= LLVFSFileBlock::SERIAL_SIZE &&
		(mIndexFP = openAndLock(mIndexFilename, file_mode, mReadOnly))
		)
	{	
		U8 *buffer = new U8[fbuf.st_size];
		fread(buffer, fbuf.st_size, 1, mIndexFP);
    
		U8 *tmp_ptr = buffer;
    
		LLLinkedList<LLVFSBlock> files_by_loc;
   		files_by_loc.setInsertBefore(LLVFSBlock::insertFirstLL);

		while (tmp_ptr < buffer + fbuf.st_size)
		{
			LLVFSFileBlock *block = new LLVFSFileBlock();
    
			block->deserialize(tmp_ptr, (S32)(tmp_ptr - buffer));
    
			// Do sanity check on this block.
			// Note that this skips zero size blocks, which helps VFS
			// to heal after some errors. JC
			if (block->mLength > 0 &&
				(U32)block->mLength <= data_size &&
				block->mLocation >= 0 &&
				block->mLocation < data_size &&
				block->mSize > 0 &&
				block->mSize <= block->mLength &&
				block->mFileType >= LLAssetType::AT_NONE &&
				block->mFileType < LLAssetType::AT_COUNT)
			{
				mFileBlocks.insert(fileblock_map::value_type(*block, block));
				files_by_loc.addDataSorted(block);
			}
			else
			if (block->mLength && block->mSize > 0)
			{
				// this is corrupt, not empty
				llwarns << "VFS corruption: " << block->mFileID << " (" << block->mFileType << ") at index " << block->mIndexLocation << " DS: " << data_size << llendl;
				llwarns << "Length: " << block->mLength << "\tLocation: " << block->mLocation << "\tSize: " << block->mSize << llendl;
				llwarns << "File has bad data - VFS removed" << llendl;

				delete[] buffer;
				delete block;

				unlockAndClose( mIndexFP );
				mIndexFP = NULL;
				LLFile::remove( mIndexFilename );

				unlockAndClose( mDataFP );
				mDataFP = NULL;
				LLFile::remove( mDataFilename );

				mValid = VFSVALID_BAD_CORRUPT;
				return;
			}
			else
			{
				// this is a null or bad entry, skip it
				S32 index_loc = (S32)(tmp_ptr - buffer);
				mIndexHoles.push_back(index_loc);
    
				delete block;
			}
    
			tmp_ptr += block->SERIAL_SIZE;
		}
		delete[] buffer;
    
		// discover all the free blocks
		LLVFSFileBlock *last_file_block = (LLVFSFileBlock*)files_by_loc.getFirstData();
    
		if (last_file_block)
		{
			// check for empty space at the beginning
			if (last_file_block->mLocation > 0)
			{
				LLVFSBlock *block = new LLVFSBlock(0, last_file_block->mLocation);
				addFreeBlock(block);
			}
    
			LLVFSFileBlock *cur_file_block;
			while ((cur_file_block = (LLVFSFileBlock*)files_by_loc.getNextData()))
			{
				if (cur_file_block->mLocation == last_file_block->mLocation
					&& cur_file_block->mLength == last_file_block->mLength)
				{
					llwarns << "VFS: removing duplicate entry"
						<< " at " << cur_file_block->mLocation 
						<< " length " << cur_file_block->mLength 
						<< " size " << cur_file_block->mSize
						<< " ID " << cur_file_block->mFileID 
						<< " type " << cur_file_block->mFileType 
						<< llendl;

					// Duplicate entries.  Nuke them both for safety.
					mFileBlocks.erase(*cur_file_block);	// remove ID/type entry
					if (cur_file_block->mLength > 0)
					{
						// convert to hole
						LLVFSBlock* block = new LLVFSBlock(cur_file_block->mLocation,
														   cur_file_block->mLength);
						addFreeBlock(block);
					}
					lockData();						// needed for sync()
					sync(cur_file_block, TRUE);		// remove first on disk
					sync(last_file_block, TRUE);	// remove last on disk
					unlockData();					// needed for sync()
					last_file_block = cur_file_block;
					continue;
				}

				U32 loc = last_file_block->mLocation + last_file_block->mLength;
				S32 length = cur_file_block->mLocation - loc;
    
				if (length < 0 || loc < 0 || loc > data_size)
				{
					// Invalid VFS
					unlockAndClose( mIndexFP );
					mIndexFP = NULL;
					LLFile::remove( mIndexFilename );

					unlockAndClose( mDataFP );
					mDataFP = NULL;
					LLFile::remove( mDataFilename );

					llwarns << "VFS: overlapping entries"
						<< " at " << cur_file_block->mLocation 
						<< " length " << cur_file_block->mLength 
						<< " ID " << cur_file_block->mFileID 
						<< " type " << cur_file_block->mFileType 
						<< llendl;
					mValid = VFSVALID_BAD_CORRUPT;
					return;
				}

				if (length > 0)
				{
					LLVFSBlock *block = new LLVFSBlock(loc, length);
					addFreeBlock(block);
				}
    
				last_file_block = cur_file_block;
			}
    
			// also note any empty space at the end
			U32 loc = last_file_block->mLocation + last_file_block->mLength;
			if (loc < data_size)
			{
				LLVFSBlock *block = new LLVFSBlock(loc, data_size - loc);
				addFreeBlock(block);
			}
		}
		else
		{
			LLVFSBlock *first_block = new LLVFSBlock(0, data_size);
			addFreeBlock(first_block);
		}
	}
	else
	{
		if (mReadOnly)
		{
			llwarns << "Can't find " << mIndexFilename << " to open read-only VFS" << llendl;
			mValid = VFSVALID_BAD_CANNOT_OPEN_READONLY;
			return;
		}
    
	
		mIndexFP = openAndLock(mIndexFilename, "w+b", FALSE);
		if (!mIndexFP)
		{
			llwarns << "Couldn't open an index file for the VFS, probably a sharing violation!" << llendl;

			unlockAndClose( mDataFP );
			mDataFP = NULL;
			LLFile::remove( mDataFilename );
			
			mValid = VFSVALID_BAD_CANNOT_CREATE;
			return;
		}
	
		// no index file, start from scratch w/ 1GB allocation
		LLVFSBlock *first_block = new LLVFSBlock(0, data_size ? data_size : 0x40000000);
		addFreeBlock(first_block);
	}

	// Open marker file to look for bad shutdowns
	if (!mReadOnly && mRemoveAfterCrash)
	{
		char* marker = new char[strlen(mDataFilename) + strlen(".open") + 1];
		sprintf(marker, "%s.open", mDataFilename);
		FILE* marker_fp = LLFile::fopen(marker, "w");
		if (marker_fp)
		{
			fclose(marker_fp);
			marker_fp = NULL;
		}
		delete [] marker;
		marker = NULL;
	}

	llinfos << "VFS: Using index file " << mIndexFilename << " and data file " << mDataFilename << llendl;

	mValid = VFSVALID_OK;
}
    
LLVFS::~LLVFS()
{
	if (mDataMutex->isLocked())
	{
		llerrs << "LLVFS destroyed with mutex locked" << llendl;
	}
	
	unlockAndClose(mIndexFP);
	mIndexFP = NULL;

	fileblock_map::const_iterator it;
	for (it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
	{
		delete (*it).second;
	}
	mFileBlocks.clear();
	
	mFreeBlocksByLength.clear();

	for_each(mFreeBlocksByLocation.begin(), mFreeBlocksByLocation.end(), DeletePairedPointer());
    
	unlockAndClose(mDataFP);
	mDataFP = NULL;
    
	// Remove marker file
	if (!mReadOnly && mRemoveAfterCrash)
	{
		char* marker_file = new char[strlen(mDataFilename) + strlen(".open") + 1];
		sprintf(marker_file, "%s.open", mDataFilename);
		LLFile::remove(marker_file);
		delete [] marker_file;
		marker_file = NULL;
	}

	delete[] mIndexFilename;
	mIndexFilename = NULL;
	delete[] mDataFilename;
	mDataFilename = NULL;

	delete mDataMutex;
}

void LLVFS::presizeDataFile(const U32 size)
{
	if (!mDataFP)
	{
		llerrs << "LLVFS::presizeDataFile() with no data file open" << llendl;
	}

	// we're creating this file for the first time, size it
	fseek(mDataFP, size-1, SEEK_SET);
	S32 tmp = 0;
	tmp = (S32)fwrite(&tmp, 1, 1, mDataFP);
	// fflush(mDataFP);

	// also remove any index, since this vfs is now blank
	LLFile::remove(mIndexFilename);

	if (tmp)
	{
		llinfos << "Pre-sized VFS data file to " << ftell(mDataFP) << " bytes" << llendl;
	}
	else
	{
		llwarns << "Failed to pre-size VFS data file" << llendl;
	}
}

BOOL LLVFS::getExists(const LLUUID &file_id, const LLAssetType::EType file_type)
{
	LLVFSFileBlock *block = NULL;
		
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}

	lockData();
	
	LLVFSFileSpecifier spec(file_id, file_type);
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		block = (*it).second;
		block->mAccessTime = (U32)time(NULL);
	}

	BOOL res = (block && block->mLength > 0) ? TRUE : FALSE;
	
	unlockData();
	
	return res;
}
    
S32	 LLVFS::getSize(const LLUUID &file_id, const LLAssetType::EType file_type)
{
	S32 size = 0;
	
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;

	}

	lockData();
	
	LLVFSFileSpecifier spec(file_id, file_type);
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;

		block->mAccessTime = (U32)time(NULL);
		size = block->mSize;
	}

	unlockData();
	
	return size;
}
    
S32  LLVFS::getMaxSize(const LLUUID &file_id, const LLAssetType::EType file_type)
{
	S32 size = 0;
	
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}

	lockData();
	
	LLVFSFileSpecifier spec(file_id, file_type);
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;

		block->mAccessTime = (U32)time(NULL);
		size = block->mLength;
	}

	unlockData();

	return size;
}

BOOL LLVFS::checkAvailable(S32 max_size)
{
	blocks_length_map_t::iterator iter = mFreeBlocksByLength.lower_bound(max_size); // first entry >= size
	return (iter == mFreeBlocksByLength.end()) ? FALSE : TRUE;
}

BOOL LLVFS::setMaxSize(const LLUUID &file_id, const LLAssetType::EType file_type, S32 max_size)
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	if (mReadOnly)
	{
		llerrs << "Attempt to write to read-only VFS" << llendl;
	}
	if (max_size <= 0)
	{
		llwarns << "VFS: Attempt to assign size " << max_size << " to vfile " << file_id << llendl;
		return FALSE;
	}

	lockData();
	
	LLVFSFileSpecifier spec(file_id, file_type);
	LLVFSFileBlock *block = NULL;
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		block = (*it).second;
	}
    
	// round all sizes upward to KB increments
	if (max_size & FILE_BLOCK_MASK)
	{
		max_size += FILE_BLOCK_MASK;
		max_size &= ~FILE_BLOCK_MASK;
	}
    
	if (block && block->mLength > 0)
	{    
		block->mAccessTime = (U32)time(NULL);
    
		if (max_size == block->mLength)
		{
			unlockData();
			return TRUE;
		}
		else if (max_size < block->mLength)
		{
			// this file is shrinking
			LLVFSBlock *free_block = new LLVFSBlock(block->mLocation + max_size, block->mLength - max_size);

			addFreeBlock(free_block);
    
			block->mLength = max_size;
    
			if (block->mLength < block->mSize)
			{
				// JC: Was a warning, but Ian says it's bad.
				llerrs << "Truncating virtual file " << file_id << " to " << block->mLength << " bytes" << llendl;
				block->mSize = block->mLength;
			}
    
			sync(block);
			//mergeFreeBlocks();

			unlockData();
			return TRUE;
		}
		else if (max_size > block->mLength)
		{
			// this file is growing
			// first check for an adjacent free block to grow into
			S32 size_increase = max_size - block->mLength;

			// Find the first free block with and addres > block->mLocation
			LLVFSBlock *free_block;
			blocks_location_map_t::iterator iter = mFreeBlocksByLocation.upper_bound(block->mLocation);
			if (iter != mFreeBlocksByLocation.end())
			{
				free_block = iter->second;
			
				if (free_block->mLocation == block->mLocation + block->mLength &&
					free_block->mLength >= size_increase)
				{
					// this free block is at the end of the file and is large enough

					// Must call useFreeSpace before sync(), as sync()
					// unlocks data structures.
					useFreeSpace(free_block, size_increase);
					block->mLength += size_increase;
					sync(block);

					unlockData();
					return TRUE;
				}
			}
			
			// no adjecent free block, find one in the list
			free_block = findFreeBlock(max_size, block);
    
			if (free_block)
			{
				if (block->mLength > 0)
				{
					// create a new free block where this file used to be
					LLVFSBlock *new_free_block = new LLVFSBlock(block->mLocation, block->mLength);

					addFreeBlock(new_free_block);
    
					if (block->mSize > 0)
					{
						// move the file into the new block
						U8 *buffer = new U8[block->mSize];
						fseek(mDataFP, block->mLocation, SEEK_SET);
						fread(buffer, block->mSize, 1, mDataFP);
						fseek(mDataFP, free_block->mLocation, SEEK_SET);
						fwrite(buffer, block->mSize, 1, mDataFP);
						// fflush(mDataFP);
    
						delete[] buffer;
					}
				}
    
				block->mLocation = free_block->mLocation;
    
				block->mLength = max_size;

				// Must call useFreeSpace before sync(), as sync()
				// unlocks data structures.
				useFreeSpace(free_block, max_size);

				sync(block);

				unlockData();
				return TRUE;
			}
			else
			{
				llwarns << "VFS: No space (" << max_size << ") to resize existing vfile " << file_id << llendl;
				//dumpMap();
				unlockData();
				dumpStatistics();
				return FALSE;
			}
		}
	}
	else
	{
		// find a free block in the list
		LLVFSBlock *free_block = findFreeBlock(max_size);
    
		if (free_block)
		{        
			if (block)
			{
				block->mLocation = free_block->mLocation;
				block->mLength = max_size;
			}
			else
			{
				// this file doesn't exist, create it
				block = new LLVFSFileBlock(file_id, file_type, free_block->mLocation, max_size);
				mFileBlocks.insert(fileblock_map::value_type(spec, block));
			}

			// Must call useFreeSpace before sync(), as sync()
			// unlocks data structures.
			useFreeSpace(free_block, max_size);
			block->mAccessTime = (U32)time(NULL);

			sync(block);
		}
		else
		{
			llwarns << "VFS: No space (" << max_size << ") for new virtual file " << file_id << llendl;
			//dumpMap();
			unlockData();
			dumpStatistics();
			return FALSE;
		}
	}
	unlockData();
	return TRUE;
}


// WARNING: HERE BE DRAGONS!
// rename is the weirdest VFS op, because the file moves but the locks don't!
void LLVFS::renameFile(const LLUUID &file_id, const LLAssetType::EType file_type,
					   const LLUUID &new_id, const LLAssetType::EType &new_type)
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	if (mReadOnly)
	{
		llerrs << "Attempt to write to read-only VFS" << llendl;
	}

	lockData();
	
	LLVFSFileSpecifier new_spec(new_id, new_type);
	LLVFSFileSpecifier old_spec(file_id, file_type);
	
	fileblock_map::iterator it = mFileBlocks.find(old_spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *src_block = (*it).second;

		// this will purge the data but leave the file block in place, w/ locks, if any
		// WAS: removeFile(new_id, new_type); NOW uses removeFileBlock() to avoid mutex lock recursion
		fileblock_map::iterator new_it = mFileBlocks.find(new_spec);
		if (new_it != mFileBlocks.end())
		{
			LLVFSFileBlock *new_block = (*new_it).second;
			removeFileBlock(new_block);
		}
		
		// if there's something in the target location, remove it but inherit its locks
		it = mFileBlocks.find(new_spec);
		if (it != mFileBlocks.end())
		{
			LLVFSFileBlock *dest_block = (*it).second;

			for (S32 i = 0; i < (S32)VFSLOCK_COUNT; i++)
			{
				if(dest_block->mLocks[i])
				{
					llerrs << "Renaming VFS block to a locked file." << llendl;
				}
				dest_block->mLocks[i] = src_block->mLocks[i];
			}
			
			mFileBlocks.erase(new_spec);
			delete dest_block;
		}

		src_block->mFileID = new_id;
		src_block->mFileType = new_type;
		src_block->mAccessTime = (U32)time(NULL);
   
		mFileBlocks.erase(old_spec);
		mFileBlocks.insert(fileblock_map::value_type(new_spec, src_block));

		sync(src_block);
	}
	else
	{
		llwarns << "VFS: Attempt to rename nonexistent vfile " << file_id << ":" << file_type << llendl;
	}
	unlockData();
}

// mDataMutex must be LOCKED before calling this
void LLVFS::removeFileBlock(LLVFSFileBlock *fileblock)
{
	// convert this into an unsaved, dummy fileblock to preserve locks
	// a more rubust solution would store the locks in a seperate data structure
	sync(fileblock, TRUE);
	
	if (fileblock->mLength > 0)
	{
		// turn this file into an empty block
		LLVFSBlock *free_block = new LLVFSBlock(fileblock->mLocation, fileblock->mLength);
		
		addFreeBlock(free_block);
	}
	
	fileblock->mLocation = 0;
	fileblock->mSize = 0;
	fileblock->mLength = BLOCK_LENGTH_INVALID;
	fileblock->mIndexLocation = -1;

	//mergeFreeBlocks();
}

void LLVFS::removeFile(const LLUUID &file_id, const LLAssetType::EType file_type)
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	if (mReadOnly)
	{
		llerrs << "Attempt to write to read-only VFS" << llendl;
	}

    lockData();
	
	LLVFSFileSpecifier spec(file_id, file_type);
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;
		removeFileBlock(block);
	}
	else
	{
		llwarns << "VFS: attempting to remove nonexistent file " << file_id << " type " << file_type << llendl;
	}

	unlockData();
}
    
    
S32 LLVFS::getData(const LLUUID &file_id, const LLAssetType::EType file_type, U8 *buffer, S32 location, S32 length)
{
	S32 bytesread = 0;
	
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	llassert(location >= 0);
	llassert(length >= 0);

	BOOL do_read = FALSE;
	
    lockData();
	
	LLVFSFileSpecifier spec(file_id, file_type);
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;

		block->mAccessTime = (U32)time(NULL);
    
		if (location > block->mSize)
		{
			llwarns << "VFS: Attempt to read location " << location << " in file " << file_id << " of length " << block->mSize << llendl;
		}
		else
		{
			if (length > block->mSize - location)
			{
				length = block->mSize - location;
			}
			location += block->mLocation;
			do_read = TRUE;
		}
	}

	unlockData();

	if (do_read)
	{
		fseek(mDataFP, location, SEEK_SET);
		bytesread = (S32)fread(buffer, 1, length, mDataFP);
	}
	
	return bytesread;
}
    
S32 LLVFS::storeData(const LLUUID &file_id, const LLAssetType::EType file_type, const U8 *buffer, S32 location, S32 length)
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	if (mReadOnly)
	{
		llerrs << "Attempt to write to read-only VFS" << llendl;
	}
    
	llassert(length > 0);

    lockData();
    
	LLVFSFileSpecifier spec(file_id, file_type);
	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;

		S32 in_loc = location;
		if (location == -1)
		{
			location = block->mSize;
		}
		llassert(location >= 0);
		
		block->mAccessTime = (U32)time(NULL);
    
		if (block->mLength == BLOCK_LENGTH_INVALID)
		{
			// Block was removed, ignore write
			llwarns << "VFS: Attempt to write to invalid block"
					<< " in file " << file_id 
					<< " location: " << in_loc
					<< " bytes: " << length
					<< llendl;
			unlockData();
			return length;
		}
		else if (location > block->mLength)
		{
			llwarns << "VFS: Attempt to write to location " << location 
					<< " in file " << file_id 
					<< " type " << S32(file_type)
					<< " of size " << block->mSize
					<< " block length " << block->mLength
					<< llendl;
			unlockData();
			return length;
		}
		else
		{
			if (length > block->mLength - location )
			{
				llwarns << "VFS: Truncating write to virtual file " << file_id << " type " << S32(file_type) << llendl;
				length = block->mLength - location;
			}
			U32 file_location = location + block->mLocation;
			
			unlockData();
			
			fseek(mDataFP, file_location, SEEK_SET);
			S32 write_len = (S32)fwrite(buffer, 1, length, mDataFP);
			if (write_len != length)
			{
				llwarns << llformat("VFS Write Error: %d != %d",write_len,length) << llendl;
			}
			// fflush(mDataFP);
			
			lockData();
			if (location + length > block->mSize)
			{
				block->mSize = location + write_len;
				sync(block);
			}
			unlockData();
			
			return write_len;
		}
	}
	else
	{
		unlockData();
		return 0;
	}
}
 
void LLVFS::incLock(const LLUUID &file_id, const LLAssetType::EType file_type, EVFSLock lock)
{
	lockData();

	LLVFSFileSpecifier spec(file_id, file_type);
	LLVFSFileBlock *block;
	
 	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		block = (*it).second;
	}
	else
	{
		// Create a dummy block which isn't saved
		block = new LLVFSFileBlock(file_id, file_type, 0, BLOCK_LENGTH_INVALID);
    	block->mAccessTime = (U32)time(NULL);
		mFileBlocks.insert(fileblock_map::value_type(spec, block));
	}

	block->mLocks[lock]++;
	mLockCounts[lock]++;
	
	unlockData();
}

void LLVFS::decLock(const LLUUID &file_id, const LLAssetType::EType file_type, EVFSLock lock)
{
	lockData();

	LLVFSFileSpecifier spec(file_id, file_type);
 	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;

		if (block->mLocks[lock] > 0)
		{
			block->mLocks[lock]--;
		}
		else
		{
			llwarns << "VFS: Decrementing zero-value lock " << lock << llendl;
		}
		mLockCounts[lock]--;
	}

	unlockData();
}

BOOL LLVFS::isLocked(const LLUUID &file_id, const LLAssetType::EType file_type, EVFSLock lock)
{
	lockData();
	
	BOOL res = FALSE;
	
	LLVFSFileSpecifier spec(file_id, file_type);
 	fileblock_map::iterator it = mFileBlocks.find(spec);
	if (it != mFileBlocks.end())
	{
		LLVFSFileBlock *block = (*it).second;
		res = (block->mLocks[lock] > 0);
	}

	unlockData();

	return res;
}

//============================================================================
// protected
//============================================================================

void LLVFS::eraseBlockLength(LLVFSBlock *block)
{
	// find the corresponding map entry in the length map and erase it
	S32 length = block->mLength;
	blocks_length_map_t::iterator iter = mFreeBlocksByLength.lower_bound(length);
	blocks_length_map_t::iterator end = mFreeBlocksByLength.end();
	while(iter != end)
	{
		LLVFSBlock *tblock = iter->second;
		llassert(tblock->mLength == length); // there had -better- be an entry with our length!
		if (tblock == block)
		{
			mFreeBlocksByLength.erase(iter);
			break;
		}
		++iter;
	}
	if (iter == end)
	{
		llerrs << "eraseBlock could not find block" << llendl;
	}
}


// Remove block from both free lists (by location and by length).
void LLVFS::eraseBlock(LLVFSBlock *block)
{
	eraseBlockLength(block);
	// find the corresponding map entry in the location map and erase it	
	U32 location = block->mLocation;
	llverify(mFreeBlocksByLocation.erase(location) == 1); // we should only have one entry per location.
}


// Add the region specified by block location and length to the free lists.
// Also incrementally defragment by merging with previous and next free blocks.
void LLVFS::addFreeBlock(LLVFSBlock *block)
{
#if LL_DEBUG
	size_t dbgcount = mFreeBlocksByLocation.count(block->mLocation);
	if(dbgcount > 0)
	{
		llerrs << "addFreeBlock called with block already in list" << llendl;
	}
#endif

	// Get a pointer to the next free block (by location).
	blocks_location_map_t::iterator next_free_it = mFreeBlocksByLocation.lower_bound(block->mLocation);

	// We can merge with previous if it ends at our requested location.
	LLVFSBlock* prev_block = NULL;
	bool merge_prev = false;
	if (next_free_it != mFreeBlocksByLocation.begin())
	{
		blocks_location_map_t::iterator prev_free_it = next_free_it;
		--prev_free_it;
		prev_block = prev_free_it->second;
		merge_prev = (prev_block->mLocation + prev_block->mLength == block->mLocation);
	}

	// We can merge with next if our block ends at the next block's location.
	LLVFSBlock* next_block = NULL;
	bool merge_next = false;
	if (next_free_it != mFreeBlocksByLocation.end())
	{
		next_block = next_free_it->second;
		merge_next = (block->mLocation + block->mLength == next_block->mLocation);
	}

	if (merge_prev && merge_next)
	{
		// llinfos << "VFS merge BOTH" << llendl;
		// Previous block is changing length (a lot), so only need to update length map.
		// Next block is going away completely. JC
		eraseBlockLength(prev_block);
		eraseBlock(next_block);
		prev_block->mLength += block->mLength + next_block->mLength;
		mFreeBlocksByLength.insert(blocks_length_map_t::value_type(prev_block->mLength, prev_block));
		delete block;
		block = NULL;
		delete next_block;
		next_block = NULL;
	}
	else if (merge_prev)
	{
		// llinfos << "VFS merge previous" << llendl;
		// Previous block is maintaining location, only changing length,
		// therefore only need to update the length map. JC
		eraseBlockLength(prev_block);
		prev_block->mLength += block->mLength;
		mFreeBlocksByLength.insert(blocks_length_map_t::value_type(prev_block->mLength, prev_block)); // multimap insert
		delete block;
		block = NULL;
	}
	else if (merge_next)
	{
		// llinfos << "VFS merge next" << llendl;
		// Next block is changing both location and length,
		// so both free lists must update. JC
		eraseBlock(next_block);
		next_block->mLocation = block->mLocation;
		next_block->mLength += block->mLength;
		// Don't hint here, next_free_it iterator may be invalid.
		mFreeBlocksByLocation.insert(blocks_location_map_t::value_type(next_block->mLocation, next_block)); // multimap insert
		mFreeBlocksByLength.insert(blocks_length_map_t::value_type(next_block->mLength, next_block)); // multimap insert			
		delete block;
		block = NULL;
	}
	else
	{
		// Can't merge with other free blocks.
		// Hint that insert should go near next_free_it.
 		mFreeBlocksByLocation.insert(next_free_it, blocks_location_map_t::value_type(block->mLocation, block)); // multimap insert
 		mFreeBlocksByLength.insert(blocks_length_map_t::value_type(block->mLength, block)); // multimap insert
	}
}

// Superceeded by new addFreeBlock which does incremental free space merging.
// Incremental is faster overall.
//void LLVFS::mergeFreeBlocks()
//{
// 	if (!isValid())
// 	{
// 		llerrs << "Attempting to use invalid VFS!" << llendl;
// 	}
// 	// TODO: could we optimize this with hints from the calling code?
// 	blocks_location_map_t::iterator iter = mFreeBlocksByLocation.begin();	
// 	blocks_location_map_t::iterator end = mFreeBlocksByLocation.end();	
// 	LLVFSBlock *first_block = iter->second;
// 	while(iter != end)
// 	{
// 		blocks_location_map_t::iterator first_iter = iter; // save for if we do a merge
// 		if (++iter == end)
// 			break;
// 		LLVFSBlock *second_block = iter->second;
// 		if (first_block->mLocation + first_block->mLength == second_block->mLocation)
// 		{
// 			// remove the first block from the length map
// 			eraseBlockLength(first_block);
// 			// merge first_block with second_block, since they're adjacent
// 			first_block->mLength += second_block->mLength;
// 			// add the first block to the length map (with the new size)
// 			mFreeBlocksByLength.insert(blocks_length_map_t::value_type(first_block->mLength, first_block)); // multimap insert
//
// 			// erase and delete the second block
// 			eraseBlock(second_block);
// 			delete second_block;
//
// 			// reset iterator
// 			iter = first_iter; // haven't changed first_block, so corresponding iterator is still valid
// 			end = mFreeBlocksByLocation.end();
// 		}
// 		first_block = second_block;
// 	}
//}
	

void LLVFS::useFreeSpace(LLVFSBlock *free_block, S32 length)
{
	if (free_block->mLength == length)
	{
		eraseBlock(free_block);
		delete free_block;
	}
	else
	{
		eraseBlock(free_block);
  		
		free_block->mLocation += length;
		free_block->mLength -= length;

		addFreeBlock(free_block);
	}
}

// NOTE! mDataMutex must be LOCKED before calling this
// sync this index entry out to the index file
// we need to do this constantly to avoid corruption on viewer crash
void LLVFS::sync(LLVFSFileBlock *block, BOOL remove)
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	if (mReadOnly)
	{
		llwarns << "Attempt to sync read-only VFS" << llendl;
		return;
	}
	if (block->mLength == BLOCK_LENGTH_INVALID)
	{
		// This is a dummy file, don't save
		return;
	}
	if (block->mLength == 0)
	{
		llerrs << "VFS syncing zero-length block" << llendl;
	}

    BOOL set_index_to_end = FALSE;
	S32 seek_pos = block->mIndexLocation;
		
	if (-1 == seek_pos)
	{
		if (!mIndexHoles.empty())
		{
			seek_pos = mIndexHoles.front();
			mIndexHoles.pop_front();
		}
		else
		{
			set_index_to_end = TRUE;
		}
	}

    if (set_index_to_end)
	{
		// Need fseek/ftell to update the seek_pos and hence data
		// structures, so can't unlockData() before this.
		fseek(mIndexFP, 0, SEEK_END);
		seek_pos = ftell(mIndexFP);
	}
	    
	block->mIndexLocation = seek_pos;
	if (remove)
	{
		mIndexHoles.push_back(seek_pos);
	}

	U8 buffer[LLVFSFileBlock::SERIAL_SIZE];
	if (remove)
	{
		memset(buffer, 0, LLVFSFileBlock::SERIAL_SIZE);
	}
	else
	{
		block->serialize(buffer);
	}

	unlockData();

	// If set_index_to_end, file pointer is already at seek_pos
	// and we don't need to do anything.  Only seek if not at end.
	if (!set_index_to_end)
	{
		fseek(mIndexFP, seek_pos, SEEK_SET);
	}

	fwrite(buffer, LLVFSFileBlock::SERIAL_SIZE, 1, mIndexFP);
	// fflush(mIndexFP);
	
	lockData();
	
	return;
}

// mDataMutex must be LOCKED before calling this
// Can initiate LRU-based file removal to make space.
// The immune file block will not be removed.
LLVFSBlock *LLVFS::findFreeBlock(S32 size, LLVFSFileBlock *immune)
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}

	LLVFSBlock *block = NULL;
	BOOL have_lru_list = FALSE;
	
	typedef std::set<LLVFSFileBlock*, LLVFSFileBlock_less> lru_set;
	lru_set lru_list;
    
	LLTimer timer;

	while (! block)
	{
		// look for a suitable free block
		blocks_length_map_t::iterator iter = mFreeBlocksByLength.lower_bound(size); // first entry >= size
		if (iter != mFreeBlocksByLength.end())
			block = iter->second;
    	
		// no large enough free blocks, time to clean out some junk
		if (! block)
		{
			// create a list of files sorted by usage time
			// this is far faster than sorting a linked list
			if (! have_lru_list)
			{
				for (fileblock_map::iterator it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
				{
					LLVFSFileBlock *tmp = (*it).second;

					if (tmp != immune &&
						tmp->mLength > 0 &&
						! tmp->mLocks[VFSLOCK_READ] &&
						! tmp->mLocks[VFSLOCK_APPEND] &&
						! tmp->mLocks[VFSLOCK_OPEN])
					{
						lru_list.insert(tmp);
					}
				}
				
				have_lru_list = TRUE;
			}

			if (lru_list.size() == 0)
			{
				// No more files to delete, and still not enough room!
				llwarns << "VFS: Can't make " << size << " bytes of free space in VFS, giving up" << llendl;
				break;
			}

			// is the oldest file big enough?  (Should be about half the time)
			lru_set::iterator it = lru_list.begin();
			LLVFSFileBlock *file_block = *it;
			if (file_block->mLength >= size && file_block != immune)
			{
				// ditch this file and look again for a free block - should find it
				// TODO: it'll be faster just to assign the free block and break
				llinfos << "LRU: Removing " << file_block->mFileID << ":" << file_block->mFileType << llendl;
				lru_list.erase(it);
				removeFileBlock(file_block);
				file_block = NULL;
				continue;
			}

			
			llinfos << "VFS: LRU: Aggressive: " << (S32)lru_list.size() << " files remain" << llendl;
			dumpLockCounts();
			
			// Now it's time to aggressively make more space
			// Delete the oldest 5MB of the vfs or enough to hold the file, which ever is larger
			// This may yield too much free space, but we'll use it up soon enough
			U32 cleanup_target = (size > VFS_CLEANUP_SIZE) ? size : VFS_CLEANUP_SIZE;
			U32 cleaned_up = 0;
		   	for (it = lru_list.begin();
				 it != lru_list.end() && cleaned_up < cleanup_target;
				 )
			{
				file_block = *it;
				
				// TODO: it would be great to be able to batch all these sync() calls
				// llinfos << "LRU2: Removing " << file_block->mFileID << ":" << file_block->mFileType << " last accessed" << file_block->mAccessTime << llendl;

				cleaned_up += file_block->mLength;
				lru_list.erase(it++);
				removeFileBlock(file_block);
				file_block = NULL;
			}
			//mergeFreeBlocks();
		}
	}
    
	F32 time = timer.getElapsedTimeF32();
	if (time > 0.5f)
	{
		llwarns << "VFS: Spent " << time << " seconds in findFreeBlock!" << llendl;
	}

	return block;
}

//============================================================================
// public
//============================================================================

void LLVFS::pokeFiles()
{
	if (!isValid())
	{
		llerrs << "Attempting to use invalid VFS!" << llendl;
	}
	U32 word;
	
	// only write data if we actually read 4 bytes
	// otherwise we're writing garbage and screwing up the file
	fseek(mDataFP, 0, SEEK_SET);
	if (fread(&word, 1, 4, mDataFP) == 4)
	{
		fseek(mDataFP, 0, SEEK_SET);
		fwrite(&word, 1, 4, mDataFP);
		fflush(mDataFP);
	}

	fseek(mIndexFP, 0, SEEK_SET);
	if (fread(&word, 1, 4, mIndexFP) == 4)
	{
		fseek(mIndexFP, 0, SEEK_SET);
		fwrite(&word, 1, 4, mIndexFP);
		fflush(mIndexFP);
	}
}

    
void LLVFS::dumpMap()
{
	llinfos << "Files:" << llendl;
	for (fileblock_map::iterator it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
	{
		LLVFSFileBlock *file_block = (*it).second;
		llinfos << "Location: " << file_block->mLocation << "\tLength: " << file_block->mLength << "\t" << file_block->mFileID << "\t" << file_block->mFileType << llendl;
	}
    
	llinfos << "Free Blocks:" << llendl;
	for (blocks_location_map_t::iterator iter = mFreeBlocksByLocation.begin(),
			 end = mFreeBlocksByLocation.end();
		 iter != end; iter++)
	{
		LLVFSBlock *free_block = iter->second;
		llinfos << "Location: " << free_block->mLocation << "\tLength: " << free_block->mLength << llendl;
	}
}
    
// verify that the index file contents match the in-memory file structure
// Very slow, do not call routinely. JC
void LLVFS::audit()
{
	// Lock the mutex through this whole function.
	LLMutexLock lock_data(mDataMutex);
	
	fflush(mIndexFP);

	fseek(mIndexFP, 0, SEEK_END);
	S32 index_size = ftell(mIndexFP);
	fseek(mIndexFP, 0, SEEK_SET);
    
	U8 *buffer = new U8[index_size];
	fread(buffer, index_size, 1, mIndexFP);
    
	U8 *tmp_ptr = buffer;
    
	std::map<LLVFSFileSpecifier, LLVFSFileBlock*>	found_files;
	U32 cur_time = (U32)time(NULL);

	BOOL vfs_corrupt = FALSE;
	
	std::vector<LLVFSFileBlock*> audit_blocks;
	while (tmp_ptr < buffer + index_size)
	{
		LLVFSFileBlock *block = new LLVFSFileBlock();
		audit_blocks.push_back(block);
		
		block->deserialize(tmp_ptr, (S32)(tmp_ptr - buffer));
		tmp_ptr += block->SERIAL_SIZE;
    
		// do sanity check on this block
		if (block->mLength >= 0 &&
			block->mLocation >= 0 &&
			block->mSize >= 0 &&
			block->mSize <= block->mLength &&
			block->mFileType >= LLAssetType::AT_NONE &&
			block->mFileType < LLAssetType::AT_COUNT &&
			block->mAccessTime <= cur_time &&
			block->mFileID != LLUUID::null)
		{
			if (mFileBlocks.find(*block) == mFileBlocks.end())
			{
				llwarns << "VFile " << block->mFileID << ":" << block->mFileType << " on disk, not in memory, loc " << block->mIndexLocation << llendl;
			}
			else if (found_files.find(*block) != found_files.end())
			{
				std::map<LLVFSFileSpecifier, LLVFSFileBlock*>::iterator it;
				it = found_files.find(*block);
				LLVFSFileBlock* dupe = it->second;
				// try to keep data from being lost
				unlockAndClose(mIndexFP);
				mIndexFP = NULL;
				unlockAndClose(mDataFP);
				mDataFP = NULL;
				llwarns << "VFS: Original block index " << block->mIndexLocation
					<< " location " << block->mLocation 
					<< " length " << block->mLength 
					<< " size " << block->mSize 
					<< " id " << block->mFileID
					<< " type " << block->mFileType
					<< llendl;
				llwarns << "VFS: Duplicate block index " << dupe->mIndexLocation
					<< " location " << dupe->mLocation 
					<< " length " << dupe->mLength 
					<< " size " << dupe->mSize 
					<< " id " << dupe->mFileID
					<< " type " << dupe->mFileType
					<< llendl;
				llwarns << "VFS: Index size " << index_size << llendl;
				llwarns << "VFS: INDEX CORRUPT" << llendl;
				vfs_corrupt = TRUE;
				break;
			}
			else
			{
				found_files[*block] = block;
			}
		}
		else
		{
			if (block->mLength)
			{
				llwarns << "VFile " << block->mFileID << ":" << block->mFileType << " corrupt on disk" << llendl;
			}
			// else this is just a hole
		}
	}
    
	delete[] buffer;

	if (vfs_corrupt)
	{
		for (std::vector<LLVFSFileBlock*>::iterator iter = audit_blocks.begin();
			 iter != audit_blocks.end(); ++iter)
		{
			delete *iter;
		}
		audit_blocks.clear();
		return;
	}
	
	for (fileblock_map::iterator it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
	{
		LLVFSFileBlock* block = (*it).second;

		if (block->mSize > 0)
		{
			if (! found_files.count(*block))
			{
				llwarns << "VFile " << block->mFileID << ":" << block->mFileType << " in memory, not on disk, loc " << block->mIndexLocation<< llendl;
				fseek(mIndexFP, block->mIndexLocation, SEEK_SET);
				U8 buf[LLVFSFileBlock::SERIAL_SIZE];
				fread(buf, LLVFSFileBlock::SERIAL_SIZE, 1, mIndexFP);
    			
				LLVFSFileBlock disk_block;
				disk_block.deserialize(buf, block->mIndexLocation);
				
				llwarns << "Instead found " << disk_block.mFileID << ":" << block->mFileType << llendl;
			}
			else
			{
				block = found_files.find(*block)->second;
				found_files.erase(*block);
				delete block;
			}
		}
	}
    
	for (std::map<LLVFSFileSpecifier, LLVFSFileBlock*>::iterator iter = found_files.begin();
		 iter != found_files.end(); iter++)
	{
		LLVFSFileBlock* block = iter->second;
		llwarns << "VFile " << block->mFileID << ":" << block->mFileType << " szie:" << block->mSize << " leftover" << llendl;
	}
    
	llinfos << "VFS: audit OK" << llendl;
	// mutex released by LLMutexLock() destructor.
}
    
    
// quick check for uninitialized blocks
// Slow, do not call in release.
void LLVFS::checkMem()
{
	lockData();
	
	for (fileblock_map::iterator it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
	{
		LLVFSFileBlock *block = (*it).second;
		llassert(block->mFileType >= LLAssetType::AT_NONE &&
				 block->mFileType < LLAssetType::AT_COUNT &&
				 block->mFileID != LLUUID::null);
    
		for (std::deque<S32>::iterator iter = mIndexHoles.begin();
			 iter != mIndexHoles.end(); ++iter)
		{
			S32 index_loc = *iter;
			if (index_loc == block->mIndexLocation)
			{
				llwarns << "VFile block " << block->mFileID << ":" << block->mFileType << " is marked as a hole" << llendl;
			}
		}
	}
    
	llinfos << "VFS: mem check OK" << llendl;

	unlockData();
}

void LLVFS::dumpLockCounts()
{
	S32 i;
	for (i = 0; i < VFSLOCK_COUNT; i++)
	{
		llinfos << "LockType: " << i << ": " << mLockCounts[i] << llendl;
	}
}

void LLVFS::dumpStatistics()
{
	lockData();
	
	// Investigate file blocks.
	std::map<S32, S32> size_counts;
	std::map<U32, S32> location_counts;
	std::map<LLAssetType::EType, std::pair<S32,S32> > filetype_counts;

	S32 max_file_size = 0;
	S32 total_file_size = 0;
	S32 invalid_file_count = 0;
	for (fileblock_map::iterator it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
	{
		LLVFSFileBlock *file_block = (*it).second;
		if (file_block->mLength == BLOCK_LENGTH_INVALID)
		{
			invalid_file_count++;
		}
		else if (file_block->mLength <= 0)
		{
			llinfos << "Bad file block at: " << file_block->mLocation << "\tLength: " << file_block->mLength << "\t" << file_block->mFileID << "\t" << file_block->mFileType << llendl;
			size_counts[file_block->mLength]++;
			location_counts[file_block->mLocation]++;
		}
		else
		{
			total_file_size += file_block->mLength;
		}

		if (file_block->mLength > max_file_size)
		{
			max_file_size = file_block->mLength;
		}

		filetype_counts[file_block->mFileType].first++;
		filetype_counts[file_block->mFileType].second += file_block->mLength;
	}
    
	for (std::map<S32,S32>::iterator it = size_counts.begin(); it != size_counts.end(); ++it)
	{
		S32 size = it->first;
		S32 size_count = it->second;
		llinfos << "Bad files size " << size << " count " << size_count << llendl;
	}
	for (std::map<U32,S32>::iterator it = location_counts.begin(); it != location_counts.end(); ++it)
	{
		U32 location = it->first;
		S32 location_count = it->second;
		llinfos << "Bad files location " << location << " count " << location_count << llendl;
	}

	// Investigate free list.
	S32 max_free_size = 0;
	S32 total_free_size = 0;
	std::map<S32, S32> free_length_counts;
	for (blocks_location_map_t::iterator iter = mFreeBlocksByLocation.begin(),
			 end = mFreeBlocksByLocation.end();
		 iter != end; iter++)
	{
		LLVFSBlock *free_block = iter->second;
		if (free_block->mLength <= 0)
		{
			llinfos << "Bad free block at: " << free_block->mLocation << "\tLength: " << free_block->mLength << llendl;
		}
		else
		{
			llinfos << "Block: " << free_block->mLocation
					<< "\tLength: " << free_block->mLength
					<< "\tEnd: " << free_block->mLocation + free_block->mLength
					<< llendl;
			total_free_size += free_block->mLength;
		}

		if (free_block->mLength > max_free_size)
		{
			max_free_size = free_block->mLength;
		}

		free_length_counts[free_block->mLength]++;
	}

	// Dump histogram of free block sizes
	for (std::map<S32,S32>::iterator it = free_length_counts.begin(); it != free_length_counts.end(); ++it)
	{
		llinfos << "Free length " << it->first << " count " << it->second << llendl;
	}

	llinfos << "Invalid blocks: " << invalid_file_count << llendl;
	llinfos << "File blocks:    " << mFileBlocks.size() << llendl;

	S32 length_list_count = (S32)mFreeBlocksByLength.size();
	S32 location_list_count = (S32)mFreeBlocksByLocation.size();
	if (length_list_count == location_list_count)
	{
		llinfos << "Free list lengths match, free blocks: " << location_list_count << llendl;
	}
	else
	{
		llwarns << "Free list lengths do not match!" << llendl;
		llwarns << "By length: " << length_list_count << llendl;
		llwarns << "By location: " << location_list_count << llendl;
	}
	llinfos << "Max file: " << max_file_size/1024 << "K" << llendl;
	llinfos << "Max free: " << max_free_size/1024 << "K" << llendl;
	llinfos << "Total file size: " << total_file_size/1024 << "K" << llendl;
	llinfos << "Total free size: " << total_free_size/1024 << "K" << llendl;
	llinfos << "Sum: " << (total_file_size + total_free_size) << " bytes" << llendl;
	llinfos << llformat("%.0f%% full",((F32)(total_file_size)/(F32)(total_file_size+total_free_size))*100.f) << llendl;

	llinfos << " " << llendl;
	for (std::map<LLAssetType::EType, std::pair<S32,S32> >::iterator iter = filetype_counts.begin();
		 iter != filetype_counts.end(); ++iter)
	{
		llinfos << "Type: " << LLAssetType::getDesc(iter->first)
				<< " Count: " << iter->second.first
				<< " Bytes: " << (iter->second.second>>20) << " MB" << llendl;
	}
	
	// Look for potential merges 
	{
 		blocks_location_map_t::iterator iter = mFreeBlocksByLocation.begin();	
 		blocks_location_map_t::iterator end = mFreeBlocksByLocation.end();	
 		LLVFSBlock *first_block = iter->second;
 		while(iter != end)
 		{
 			if (++iter == end)
 				break;
 			LLVFSBlock *second_block = iter->second;
 			if (first_block->mLocation + first_block->mLength == second_block->mLocation)
 			{
				llinfos << "Potential merge at " << first_block->mLocation << llendl;
 			}
 			first_block = second_block;
 		}
	}
	unlockData();
}

// Debug Only!
#include "llapr.h"
void LLVFS::dumpFiles()
{
	lockData();
	
	for (fileblock_map::iterator it = mFileBlocks.begin(); it != mFileBlocks.end(); ++it)
	{
		LLVFSFileSpecifier file_spec = it->first;
		LLVFSFileBlock *file_block = it->second;
		S32 length = file_block->mLength;
		S32 size = file_block->mSize;
		if (length != BLOCK_LENGTH_INVALID && size > 0)
		{
			LLUUID id = file_spec.mFileID;
			LLAssetType::EType type = file_spec.mFileType;
			U8* buffer = new U8[size];

			unlockData();
			getData(id, type, buffer, 0, size);
			lockData();
			
			LLString extention = ".data";
			switch(type)
			{
			  case LLAssetType::AT_TEXTURE:
				extention = ".jp2"; // ".j2c"; // IrfanView recognizes .jp2 -sjb
				break;
			  default:
				break;
			}
			LLString filename = id.getString() + extention;
			llinfos << " Writing " << filename << llendl;
			apr_file_t* file = ll_apr_file_open(filename, LL_APR_WB);
			ll_apr_file_write(file, buffer, size);
			apr_file_close(file);
			delete[] buffer;
		}
	}
	
	unlockData();
}

//============================================================================
// protected
//============================================================================

// static
FILE *LLVFS::openAndLock(const char *filename, const char *mode, BOOL read_lock)
{
#if LL_WINDOWS
    	
	return LLFile::_fsopen(filename, mode, (read_lock ? _SH_DENYWR : _SH_DENYRW));
    	
#else

	FILE *fp;
	int fd;
	
	// first test the lock in a non-destructive way
	if (strstr(mode, "w"))
	{
		fp = LLFile::fopen(filename, "rb");
		if (fp)
		{
			fd = fileno(fp);
			if (flock(fd, (read_lock ? LOCK_SH : LOCK_EX) | LOCK_NB) == -1)
			{
				fclose(fp);
				return NULL;
			}
		  
			fclose(fp);
		}
	}

	// now actually open the file for use
	fp = LLFile::fopen(filename, mode);
	if (fp)
	{
		fd = fileno(fp);
		if (flock(fd, (read_lock ? LOCK_SH : LOCK_EX) | LOCK_NB) == -1)
		{
			fclose(fp);
			fp = NULL;
		}
	}

	return fp;
    	
#endif
}
    
// static
void LLVFS::unlockAndClose(FILE *fp)
{
	if (fp)
	{
	// IW: we don't actually want to unlock on linux
	// this is because a forked process can kill the parent's lock
	// with an explicit unlock
	// however, fclose() will implicitly remove the lock
	// but only once both parent and child have closed the file
    /*	
	  #if !LL_WINDOWS
	  int fd = fileno(fp);
	  flock(fd, LOCK_UN);
	  #endif
    */
    
		fclose(fp);
	}
}