/*
 *  This file is part of nzbget. See <http://nzbget.net>.
 *
 *  Copyright (C) 2016 Andrey Prygunkov <hugbug@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "nzbget.h"

#include "RarRenamer.h"
#include "Log.h"
#include "Options.h"
#include "Util.h"
#include "FileSystem.h"

// RAR3 constants

static const uint16 RAR3_MAIN_VOLUME = 0x0001;
static const uint16 RAR3_MAIN_NEWNUMBERING = 0x0010;
static const uint16 RAR3_MAIN_PASSWORD = 0x0080;

static const uint8 RAR3_BLOCK_MAIN = 0x73; // s
static const uint8 RAR3_BLOCK_FILE = 0x74; // t
static const uint8 RAR3_BLOCK_ENDARC = 0x7b; // {

static const uint16 RAR3_BLOCK_ADDSIZE = 0x8000;

static const uint16 RAR3_FILE_ADDSIZE = 0x0100;
static const uint16 RAR3_FILE_SPLITBEFORE = 0x0001;
static const uint16 RAR3_FILE_SPLITAFTER = 0x0002;

static const uint16 RAR3_ENDARC_NEXTVOL = 0x0001;
static const uint16 RAR3_ENDARC_DATACRC = 0x0002;
static const uint16 RAR3_ENDARC_VOLNUMBER = 0x0008;

// RAR5 constants

static const uint8 RAR5_BLOCK_MAIN = 1;
static const uint8 RAR5_BLOCK_FILE = 2;
//static const uint8 RAR5_BLOCK_ENCRYPTION = 4;
static const uint8 RAR5_BLOCK_ENDARC = 5;

static const uint8 RAR5_BLOCK_EXTRADATA = 0x01;
static const uint8 RAR5_BLOCK_DATAAREA = 0x02;
static const uint8 RAR5_BLOCK_SPLITBEFORE = 0x08;
static const uint8 RAR5_BLOCK_SPLITAFTER = 0x10;

static const uint8 RAR5_MAIN_ISVOL = 0x01;
static const uint8 RAR5_MAIN_VOLNR = 0x02;

static const uint8 RAR5_FILE_TIME = 0x02;
static const uint8 RAR5_FILE_CRC = 0x04;
static const uint8 RAR5_FILE_EXTRATIME = 0x03;
static const uint8 RAR5_FILE_EXTRATIMEUNIXFORMAT = 0x01;

static const uint8 RAR5_ENDARC_NEXTVOL = 0x01;

//TODO: delete debug-function
#undef debug
void debug(const char* format, ...)
{
	BString<1024> tmp2;

	va_list ap;
	va_start(ap, format);
	tmp2.FormatV(format, ap);
	va_end(ap);

	printf("%s\n", *tmp2);
}

void RarRenamer::Cancel()
{
	m_cancelled = true;
}

void RarRenamer::Execute()
{
	m_progressLabel.Format("Checking renamed rar-files for %s", *m_infoName);
	m_stageProgress = 0;
	UpdateProgress();

	BuildDirList(m_destDir);

	for (CString& destDir : m_dirList)
	{
		debug("Checking %s", *destDir);
		CheckFiles(destDir);
	}

	if (m_cancelled)
	{
		PrintMessage(Message::mkWarning, "Renaming cancelled for %s", *m_infoName);
	}
	else if (m_renamedCount > 0)
	{
		PrintMessage(Message::mkInfo, "Successfully renamed %i rar-file(s) for %s", m_renamedCount, *m_infoName);
		m_status = rsSuccess;
	}
	else
	{
		PrintMessage(Message::mkInfo, "No renamed rar-files found for %s", *m_infoName);
	}

	Completed();
}

void RarRenamer::BuildDirList(const char* destDir)
{
	m_dirList.push_back(destDir);

	DirBrowser dirBrowser(destDir);

	while (const char* filename = dirBrowser.Next())
	{
		if (!m_cancelled)
		{
			BString<1024> fullFilename("%s%c%s", destDir, PATH_SEPARATOR, filename);
			if (FileSystem::DirectoryExists(fullFilename))
			{
				BuildDirList(fullFilename);
			}
			else
			{
				m_fileCount++;
			}
		}
	}
}

void RarRenamer::CheckFiles(const char* destDir)
{
	DirBrowser dir(destDir);
	while (const char* filename = dir.Next())
	{
		if (!m_cancelled)
		{
			BString<1024> fullFilename("%s%c%s", destDir, PATH_SEPARATOR, filename);

			if (!FileSystem::DirectoryExists(fullFilename))
			{
				m_progressLabel.Format("Checking file %s", filename);
				m_stageProgress = m_fileCount > 0 ? m_curFile * 1000 / m_fileCount : 1000;
				UpdateProgress();
				m_curFile++;

				CheckRegularFile(destDir, fullFilename);
			}
		}
	}

	LogDebugInfo();
}

void RarRenamer::CheckRegularFile(const char* destDir, const char* filename)
{
	debug("Checking file %s", filename);

	DiskFile file;
	if (!file.Open(filename, DiskFile::omRead))
	{
		PrintMessage(Message::mkError, "Could not open file %s", filename);
		return;
	}

	int version = DetectRarVersion(file);
	file.Seek(0);

	RarVolume volume(version, filename);
	bool ok = false;

	switch (version)
	{
		case 3:
			ok = ReadRar3Volume(file, volume);
			break;

		case 5:
			ok = ReadRar5Volume(file, volume);
			break;
	}

	if (ok)
	{
		m_volumes.push_back(std::move(volume));
	}

	file.Close();
}

int RarRenamer::DetectRarVersion(DiskFile& file)
{
	static char RAR3_SIGNATURE[] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00 };
	static char RAR5_SIGNATURE[] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00 };

	char fileSignature[8];

	int cnt = 0;
	cnt = (int)file.Read(fileSignature, sizeof(fileSignature));

	bool rar5 = cnt == sizeof(fileSignature) && !strcmp(RAR5_SIGNATURE, fileSignature);
	bool rar3 = !rar5 && cnt == sizeof(fileSignature) && !strcmp(RAR3_SIGNATURE, fileSignature);

	return rar3 ? 3 : rar5 ? 5 : 0;
}

bool RarRenamer::Seek(DiskFile& file, RarBlock* block, int64 relpos)
{
	if (!file.Seek(relpos, DiskFile::soCur)) return false;
	if (block)
	{
		block->trailsize -= relpos;
	}
	return true;
}

bool RarRenamer::Read(DiskFile& file, RarBlock* block, void* buffer, int64 size)
{
	if (file.Read(buffer, size) != size) return false;
	if (block)
	{
		block->trailsize -= size;
	}
	return true;
}

bool RarRenamer::Read16(DiskFile& file, RarBlock* block, uint16* result)
{
	uint8 buf[2];
	if (file.Read(buf, sizeof(buf)) != sizeof(buf)) return false;
	if (block)
	{
		block->trailsize -= sizeof(buf);
	}
	*result = ((uint16)buf[1] << 8) + buf[0];
	return true;
}

bool RarRenamer::Read32(DiskFile& file, RarBlock* block, uint32* result)
{
	uint8 buf[4];
	if (file.Read(buf, sizeof(buf)) != sizeof(buf)) return false;
	if (block)
	{
		block->trailsize -= sizeof(buf);
	}
	*result = ((uint32)buf[3] << 24) + ((uint32)buf[2] << 16) + ((uint32)buf[1] << 8) + buf[0];
	return true;
}

bool RarRenamer::ReadV(DiskFile& file, RarBlock* block, uint64* result)
{
	*result = 0;
	uint8 val;
	uint8 bits = 0;
	do
	{
		if (file.Read(&val, sizeof(val)) != sizeof(val)) return false;
		*result += (uint64)(val & 0x7f) << bits;
		bits += 7;
		if (block)
		{
			block->trailsize -= 1;
		}
	} while (val & 0x80);

	return true;
}

bool RarRenamer::ReadRar3Volume(DiskFile& file, RarVolume& volume)
{
	debug("Reading rar3-file %s", *volume.m_filename);

	while (!file.Eof())
	{
		RarBlock block = ReadRar3Block(file);
		if (!block.type)
		{
			return false;
		}

		if (block.type == RAR3_BLOCK_MAIN)
		{
			if (block.flags & RAR3_MAIN_PASSWORD)
			{
				// no support for encrypted headers
				return false;
			}
			volume.m_newNaming = block.flags & RAR3_MAIN_NEWNUMBERING;
			volume.m_multiVolume = block.flags & RAR3_MAIN_VOLUME;
		}

		else if (block.type == RAR3_BLOCK_FILE)
		{
			RarFile innerFile;
			if (!ReadRar3File(file, volume, block, innerFile)) return false;
			volume.m_files.push_back(std::move(innerFile));
		}

		else if (block.type == RAR3_BLOCK_ENDARC)
		{
			if (block.flags & RAR3_ENDARC_DATACRC)
			{
				if (!Seek(file, &block, 4)) return false;
			}
			if (block.flags & RAR3_ENDARC_VOLNUMBER)
			{
				if (!Read32(file, &block, &volume.m_volumeNo)) return false;
				volume.m_hasNextVolume = (block.flags & RAR3_ENDARC_NEXTVOL) != 0;
			}
			break;
		}

		if (!file.Seek(block.trailsize, DiskFile::soCur))
		{
			return false;
		}
	}

	return true;
}

RarRenamer::RarBlock RarRenamer::ReadRar3Block(DiskFile& file)
{
	RarBlock block {0};
	uint8 buf[7];

	if (file.Read(&buf, sizeof(buf)) != sizeof(buf))
	{
		debug("Bad read at: %lli", file.Position());
		return {0};
	}
	block.crc = ((uint16)buf[1] << 8) + buf[0];
	block.type = buf[2];
	block.flags = ((uint16)buf[4] << 8) + buf[3];
	uint16 size = ((uint16)buf[6] << 8) + buf[5];

	uint32 blocksize = size;
	block.trailsize = blocksize - sizeof(buf);

	uint8 addbuf[4];
	if ((block.flags & RAR3_BLOCK_ADDSIZE) && file.Read(&addbuf, sizeof(addbuf)) != sizeof(addbuf))
	{
		return {0};
	}
	block.addsize = ((uint32)addbuf[3] << 24) + ((uint32)addbuf[2] << 16) + ((uint32)addbuf[1] << 8) + addbuf[0];

	if (block.flags & RAR3_BLOCK_ADDSIZE)
	{
		blocksize += (uint32)block.addsize;
		block.trailsize = blocksize - sizeof(buf) - 4;
	}

	static int num = 0;
	debug("%i) %llu, %i, %i, %i, %u, %llu", ++num, (long long)block.crc, (int)block.type, (int)block.flags, (int)size, (int)block.addsize, (long long)block.trailsize);

	return block;
}

bool RarRenamer::ReadRar3File(DiskFile& file, RarVolume& volume, RarBlock& block, RarFile& innerFile)
{
	innerFile.m_splitBefore = block.flags & RAR3_FILE_SPLITBEFORE;
	innerFile.m_splitAfter = block.flags & RAR3_FILE_SPLITAFTER;

	uint16 namelen;

	uint32 size;
	if (!Read32(file, &block, &size)) return false;
	innerFile.m_size = size;

	if (!Seek(file, &block, 1)) return false;
	if (!Seek(file, &block, 4)) return false;
	if (!Read32(file, &block, &innerFile.m_time)) return false;
	if (!Seek(file, &block, 2)) return false;
	if (!Read16(file, &block, &namelen)) return false;
	if (!Read32(file, &block, &innerFile.m_attr)) return false;

	if (block.flags & RAR3_FILE_ADDSIZE)
	{
		uint32 highsize;
		if (!Read32(file, &block, &highsize)) return false;
		block.trailsize += (uint64)highsize << 32;

		if (!Read32(file, &block, &highsize)) return false;
		innerFile.m_size += (uint64)highsize << 32;
	}

	if (namelen > 8192) return false; // an error
	CharBuffer name;
	name.Reserve(namelen + 1);
	if (!Read(file, &block, (char*)name, namelen)) return false;
	name[namelen] = '\0';
	innerFile.m_filename = name;
	debug("%i, %i, %s", (int)block.trailsize, (int)namelen, (const char*)name);

	return true;
}

bool RarRenamer::ReadRar5Volume(DiskFile& file, RarVolume& volume)
{
	debug("Reading rar5-file %s", *volume.m_filename);

	if (!Seek(file, nullptr, 8)) return false;

	while (!file.Eof())
	{
		RarBlock block = ReadRar5Block(file);
		if (!block.type)
		{
			return false;
		}

		if (block.type == RAR5_BLOCK_MAIN)
		{
			uint64 arcflags;
			if (!ReadV(file, &block, &arcflags)) return false;
			if (arcflags & RAR5_MAIN_VOLNR)
			{
				uint64 volnr;
				if (!ReadV(file, &block, &volnr)) return false;
				volume.m_volumeNo = (uint32)volnr;
			}
			volume.m_newNaming = true;
			volume.m_multiVolume = (arcflags & RAR5_MAIN_ISVOL) != 0;
		}

		else if (block.type == RAR5_BLOCK_FILE)
		{
			RarFile innerFile;
			if (!ReadRar5File(file, volume, block, innerFile)) return false;
			volume.m_files.push_back(std::move(innerFile));
		}

		else if (block.type == RAR5_BLOCK_ENDARC)
		{
			uint64 endflags;
			if (!ReadV(file, &block, &endflags)) return false;
			volume.m_hasNextVolume = (endflags & RAR5_ENDARC_NEXTVOL) != 0;
			break;
		}

		if (!file.Seek(block.trailsize, DiskFile::soCur))
		{
			return false;
		}
	}

	return true;
}

RarRenamer::RarBlock RarRenamer::ReadRar5Block(DiskFile& file)
{
	RarBlock block{ 0 };
	uint64 buf = 0;

	if (!Read32(file, nullptr, &block.crc)) return{ 0 };

	if (!ReadV(file, nullptr, &buf)) return{ 0 };
	uint32 size = (uint32)buf;
	block.trailsize = size;

	if (!ReadV(file, &block, &buf)) return{ 0 };
	block.type = (uint8)buf;

	if (!ReadV(file, &block, &buf)) return{ 0 };
	block.flags = (uint16)buf;

	block.addsize = 0;
	if ((block.flags & RAR5_BLOCK_EXTRADATA) && !ReadV(file, &block, &block.addsize)) return{ 0 };

	uint64 datasize = 0;
	if ((block.flags & RAR5_BLOCK_DATAAREA) && !ReadV(file, &block, &datasize)) return{ 0 };
	block.trailsize += datasize;

	static int num = 0;
	debug("%i) %llu, %i, %i, %i, %u, %llu", ++num, (long long)block.crc, (int)block.type, (int)block.flags, (int)size, (int)block.addsize, (long long)block.trailsize);

	return block;
}

bool RarRenamer::ReadRar5File(DiskFile& file, RarVolume& volume, RarBlock& block, RarFile& innerFile)
{
	innerFile.m_splitBefore = block.flags & RAR5_BLOCK_SPLITBEFORE;
	innerFile.m_splitAfter = block.flags & RAR5_BLOCK_SPLITAFTER;

	uint64 val;

	uint64 fileflags;
	if (!ReadV(file, &block, &fileflags)) return false;

	if (fileflags & 1)
	{
		if (!ReadV(file, &block, &val)) return false;
		volume.m_volumeNo = (uint32)val;
	}

	if (!ReadV(file, &block, &val)) return false; // skip
	innerFile.m_size = (int64)val;

	if (!ReadV(file, &block, &val)) return false;
	innerFile.m_attr = (uint32)val;

	if (fileflags & RAR5_FILE_TIME && !Read32(file, &block, &innerFile.m_time)) return false;
	if (fileflags & RAR5_FILE_CRC && !Seek(file, &block, 4)) return false;

	if (!ReadV(file, &block, &val)) return false; // skip
	if (!ReadV(file, &block, &val)) return false; // skip

	uint64 namelen;
	if (!ReadV(file, &block, &namelen)) return false;
	if (namelen > 8192) return false; // an error
	CharBuffer name;
	name.Reserve((uint32)namelen + 1);
	if (!Read(file, &block, (char*)name, namelen)) return false;
	name[namelen] = '\0';
	innerFile.m_filename = name;

	// reading extra headers to find file time
	if (block.flags & RAR5_BLOCK_EXTRADATA)
	{
		uint64 remsize = block.addsize;
		while (remsize > 0)
		{
			uint64 trailsize = block.trailsize;

			uint64 len;
			if (!ReadV(file, &block, &len)) return false;
			remsize -= trailsize - block.trailsize + len;
			trailsize = block.trailsize;

			uint64 type;
			if (!ReadV(file, &block, &type)) return false;

			if (type == RAR5_FILE_EXTRATIME)
			{
				uint64 flags;
				if (!ReadV(file, &block, &flags)) return false;
				if (flags & RAR5_FILE_EXTRATIMEUNIXFORMAT)
				{
					if (!Read32(file, &block, &innerFile.m_time)) return false;
				}
				else
				{
					uint32 timelow, timehigh;
					if (!Read32(file, &block, &timelow)) return false;
					if (!Read32(file, &block, &timehigh)) return false;
					uint64 wintime = ((uint64)timehigh << 32) + timelow;
					innerFile.m_time = (uint32)(wintime / 10000000 - 11644473600LL);
				}
			}

			len -= trailsize - block.trailsize;

			if (!Seek(file, &block, len)) return false;
		}
	}

	debug("%llu, %i, %s", (long long)block.trailsize, (int)namelen, (const char*)name);

	return true;
}

void RarRenamer::RenameFile(const char* srcFilename, const char* destFileName)
{
	PrintMessage(Message::mkInfo, "Renaming %s to %s", FileSystem::BaseFileName(srcFilename), FileSystem::BaseFileName(destFileName));
	if (!FileSystem::MoveFile(srcFilename, destFileName))
	{
		PrintMessage(Message::mkError, "Could not rename %s to %s: %s", srcFilename, destFileName,
			*FileSystem::GetLastErrorMessage());
		return;
	}

	m_renamedCount++;

	// notify about new file name
	RegisterRenamedFile(FileSystem::BaseFileName(srcFilename), FileSystem::BaseFileName(destFileName));
}

void RarRenamer::LogDebugInfo()
{
	debug("Dumping volumes:");
	for (RarVolume& volume : m_volumes)
	{
		debug("Volume: version:%i, multi:%i, vol-no:%i, new-naming:%i, has-next:%i, file-count:%i, [%s]",
			(int)volume.m_version, (int)volume.m_multiVolume, volume.m_volumeNo,
			(int)volume.m_newNaming, (int)volume.m_hasNextVolume, (int)volume.m_files.size(),
			FileSystem::BaseFileName(volume.m_filename));

		for (RarFile& file : volume.m_files)
		{
			debug("  time:%i, size:%lli, attr:%i, split-before:%i, split-after:%i, [%s]",
				(int)file.m_time, (long long)file.m_size, (int)file.m_attr,
				(int)file.m_splitBefore, (int)file.m_splitAfter, *file.m_filename);
		}
	}
}