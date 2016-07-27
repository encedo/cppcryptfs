/*
cppcryptfs : user-mode cryptographic virtual overlay filesystem.

Copyright (C) 2016 - Bailey Brown (github.com/bailey27/cppcryptfs)

cppcryptfs is based on the design of gocryptfs (github.com/rfjakob/gocryptfs)

The MIT License (MIT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "stdafx.h"
#include "cryptdefs.h"
#include "cryptio.h"
#include "cryptfile.h"
#include "fileutil.h"
#include "util.h"
#include "crypt.h"

CryptFile::CryptFile()
{
	m_handle = INVALID_HANDLE_VALUE;
	m_is_empty = false;
	m_con = NULL;
	m_real_file_size = (long long)-1;
	memset(&m_header, 0, sizeof(m_header));
}

BOOL
CryptFile::Associate(CryptContext *con, HANDLE hfile)
{

	static_assert(sizeof(m_header) == FILE_HEADER_LEN, "sizeof(m_header) != FILE_HEADER_LEN");

	m_handle = hfile;

	m_con = con;

	LARGE_INTEGER l;

	if (!GetFileSizeEx(hfile, &l)) {
		DbgPrint(L"ASSOCIATE: failed to get size of file\n");
		return FALSE;
	}

	m_real_file_size = l.QuadPart;

	if (l.QuadPart == 0) {
		m_header.version = CRYPT_VERSION;
		m_is_empty = true;
		return TRUE;
	} else if (l.QuadPart < FILE_HEADER_LEN) {
		DbgPrint(L"ASSOCIATE: missing file header\n");
		return FALSE;
	}

	l.QuadPart = 0;

	if (!SetFilePointerEx(hfile, l, NULL, FILE_BEGIN)) {
		DbgPrint(L"ASSOCIATE: failed to seek\n");
		return FALSE;
	}


	DWORD nread;

	if (!ReadFile(hfile, &m_header, sizeof(m_header), &nread, NULL)) {
		DbgPrint(L"ASSOCIATE: failed to read header\n");
		return FALSE;
	}

	if (nread != FILE_HEADER_LEN) {
		DbgPrint(L"ASSOCIATE: wrong number of bytes read when reading file header\n");
		return FALSE;
	}

	m_header.version = MakeBigEndianNative(m_header.version);

	if (m_header.version != CRYPT_VERSION) {
		DbgPrint(L"ASSOCIATE: file version mismatch\n");
		return FALSE;
	}

	static BYTE zerobytes[FILE_ID_LEN] = { 0 };

	if (!memcmp(m_header.fileid, zerobytes, sizeof(m_header.fileid))) {
		DbgPrint(L"ASSOCIATE: fileid is all zeroes\n");
		return FALSE;
	}

	return TRUE;
}


CryptFile::~CryptFile()
{
	// do not close handle
}

BOOL CryptFile::Read(unsigned char *buf, DWORD buflen, LPDWORD pNread, LONGLONG offset)
{

	if (m_real_file_size == (long long)-1)
		return FALSE;

	if (!pNread || !buf)
		return FALSE;

	*pNread = 0;

	LONGLONG bytesleft = buflen;

	unsigned char *p = buf;

	void *context = get_crypt_context(BLOCK_IV_LEN, AES_MODE_GCM);

	if (!context)
		return FALSE;

	BOOL bRet = TRUE;

	try {

		while (bytesleft > 0) {

			LONGLONG blockno = offset / PLAIN_BS;
			int blockoff = (int)(offset % PLAIN_BS);

			int advance;

			if (blockoff == 0 && bytesleft >= PLAIN_BS) {

				advance = read_block(m_con, m_handle, m_header.fileid, blockno, p, context);

				if (advance < 0)
					throw(-1);

				if (advance < 1)
					break;

			} else {

				unsigned char blockbuf[PLAIN_BS];

				int blockbytes = read_block(m_con, m_handle, m_header.fileid, blockno, blockbuf, context);

				if (blockbytes < 0)
					throw(-1);

				if (blockbytes < 1)
					break;

				int blockcpy = (int)min(bytesleft, blockbytes - blockoff);

				if (blockcpy < 1)
					break;

				memcpy(p, blockbuf + blockoff, blockcpy);

				advance = blockcpy;
			}

			p += advance;
			offset += advance;
			bytesleft -= advance;
			*pNread += advance;
		}
	} catch (...) {
		bRet = FALSE;
	}

	if (context)
		free_crypt_context(context);

	return bRet;
}


// write version and fileid to empty file before writing to it

BOOL CryptFile::WriteVersionAndFileId()
{
	if (m_real_file_size == (long long)-1)
		return FALSE;

	LARGE_INTEGER l;
	l.QuadPart = 0;

	if (!SetFilePointerEx(m_handle, l, NULL, FILE_BEGIN))
		return FALSE;

	if (!get_random_bytes(m_con, m_header.fileid, FILE_ID_LEN))
		return FALSE;

	unsigned short version = CRYPT_VERSION;

	m_header.version = MakeBigEndian(version);

	DWORD nWritten = 0;

	if (!WriteFile(m_handle, &m_header, sizeof(m_header), &nWritten, NULL)) {
		m_header.version = CRYPT_VERSION;
		return FALSE;
	}
	
	m_header.version = CRYPT_VERSION;

	m_real_file_size = FILE_HEADER_LEN;

	m_is_empty = false;

	return nWritten == FILE_HEADER_LEN;
}


BOOL CryptFile::Write(const unsigned char *buf, DWORD buflen, LPDWORD pNwritten, LONGLONG offset, BOOL bWriteToEndOfFile, BOOL bPagingIo)
{

	if (m_real_file_size == (long long)-1)
		return FALSE;

	BOOL bRet = TRUE;

	if (bWriteToEndOfFile) {
		LARGE_INTEGER l;
		l.QuadPart = m_real_file_size;
		if (!adjust_file_offset_down(l))
			return FALSE;
		offset = l.QuadPart;
	} else {
		if (bPagingIo) {
			LARGE_INTEGER l;
			l.QuadPart = m_real_file_size;
			if (!adjust_file_offset_down(l))
				return FALSE;

			if (offset >= l.QuadPart)
			{
				DbgPrint(L"CryptFile paging io past end of file, return\n");
				*pNwritten = 0;
				return true;
			}

			if ((offset + buflen) > l.QuadPart)
			{
				DbgPrint(L"CryptFile addjusting write length due to paging io\n");
				buflen = (DWORD)(l.QuadPart - offset);
			}
		}
	}

	if (!pNwritten || !buf)
		return FALSE;

	if (buflen < 1)
		return TRUE;

	if (m_is_empty) {
		if (!WriteVersionAndFileId())
			return FALSE;	
	} else {
		LARGE_INTEGER l;
		l.QuadPart = m_real_file_size;
		adjust_file_offset_down(l);
		// if creating a hole, call this->SetEndOfFile() to deal with last block if necessary
		if (offset != l.QuadPart && offset + buflen > l.QuadPart) {
			SetEndOfFile(offset + buflen, FALSE);
		}
	}

	*pNwritten = 0;

	LONGLONG bytesleft = buflen;

	const unsigned char *p = buf;

	void *context = get_crypt_context(BLOCK_IV_LEN, AES_MODE_GCM);

	if (!context)
		return FALSE;

	try {

		while (bytesleft > 0) {

			LONGLONG blockno = offset / PLAIN_BS;
			int blockoff = (int)(offset % PLAIN_BS);

			int advance;

			if (blockoff == 0 && bytesleft >= PLAIN_BS) { // overwriting whole blocks

				advance = write_block(m_con, m_handle, m_header.fileid, blockno, p, PLAIN_BS, context);

				if (advance != PLAIN_BS)
					throw(-1);

			} else { // else read-modify-write 

				unsigned char blockbuf[PLAIN_BS];

				memset(blockbuf, 0, sizeof(blockbuf));

				int blockbytes = read_block(m_con, m_handle, m_header.fileid, blockno, blockbuf, context);

				if (blockbytes < 0) {
					bRet = FALSE;
					break;
				}

				int blockcpy = (int)min(bytesleft, PLAIN_BS - blockoff);

				if (blockcpy < 1)
					break;

				memcpy(blockbuf + blockoff, p, blockcpy);

				int blockwrite = max(blockoff + blockcpy, blockbytes);

				int nWritten = write_block(m_con, m_handle, m_header.fileid, blockno, blockbuf, blockwrite, context);

				advance = blockcpy;

				if (nWritten != blockwrite)
					throw(-1);

			}

			p += advance;
			offset += advance;
			bytesleft -= advance;
			*pNwritten += advance;

		}
	} catch (...) {
		bRet = FALSE;
	}

	*pNwritten = min(*pNwritten, buflen);

	if (context)
		free_crypt_context(context);

	return bRet;
	
}

BOOL
CryptFile::LockFile(LONGLONG ByteOffset, LONGLONG Length)
{
	if (m_real_file_size == (long long)-1)
		return FALSE;

	long long start_block = ByteOffset / PLAIN_BS;

	long long end_block = (ByteOffset + Length + PLAIN_BS - 1) / PLAIN_BS;

	long long start_offset = CIPHER_FILE_OVERHEAD + start_block*CIPHER_BS;

	long long end_offset = CIPHER_FILE_OVERHEAD + end_block*CIPHER_BS;

	long long length = end_offset - start_offset;

	
	LARGE_INTEGER off, len;

	off.QuadPart = start_offset;

	len.QuadPart = length;

	return ::LockFile(m_handle, off.LowPart, off.HighPart, len.LowPart, len.HighPart);
}

BOOL
CryptFile::UnlockFile(LONGLONG ByteOffset, LONGLONG Length)
{
	if (m_real_file_size == (long long)-1)
		return FALSE;

	long long start_block = ByteOffset / PLAIN_BS;

	long long end_block = (ByteOffset + Length + PLAIN_BS - 1) / PLAIN_BS;

	long long start_offset = CIPHER_FILE_OVERHEAD + start_block*CIPHER_BS;

	long long end_offset = CIPHER_FILE_OVERHEAD + end_block*CIPHER_BS;

	long long length = end_offset - start_offset;


	LARGE_INTEGER off, len;

	off.QuadPart = start_offset;

	len.QuadPart = length;

	return ::UnlockFile(m_handle, off.LowPart, off.HighPart, len.LowPart, len.HighPart);
}


// used ONLY by CryptFile::SetEndOfFile()
static bool
adjust_file_offset_up_truncate_zero(LARGE_INTEGER& l)
{
	long long offset = l.QuadPart;

	if (offset < 0)
		return false;

	if (offset == 0) // truncate zero-length file to 0 bytes
		return true;

	long long blocks = (offset + PLAIN_BS - 1) / PLAIN_BS;
	offset += (blocks*CIPHER_BLOCK_OVERHEAD + CIPHER_FILE_OVERHEAD);
	if (offset < 1)
		return false;

	l.QuadPart = offset;

	return true;
}

// re-writes last block of necessary to account for file growing or shrinking
// if bSet is TRUE (the default), actually calls SetEndOfFile()
BOOL
CryptFile::SetEndOfFile(LONGLONG offset, BOOL bSet)
{

	if (m_real_file_size == (long long)-1)
		return FALSE;


	if (m_handle == NULL || m_handle == INVALID_HANDLE_VALUE)
		return FALSE;


	if (m_is_empty && offset != 0) {
		if (!WriteVersionAndFileId())
			return FALSE;
	}

	LARGE_INTEGER size_down;
	
	size_down.QuadPart = m_real_file_size;

	if (!adjust_file_offset_down(size_down)) {
		return FALSE;
	}

	LARGE_INTEGER up_off;
	up_off.QuadPart = offset;
	
	if (!adjust_file_offset_up_truncate_zero(up_off))
		return FALSE;

	long long last_block;
	int to_write;

	if (offset < size_down.QuadPart) {
		last_block = offset / PLAIN_BS;
		to_write = (int)(offset % PLAIN_BS);
	} else {
		last_block = size_down.QuadPart / PLAIN_BS;
		to_write = size_down.QuadPart % PLAIN_BS ? (int)min(PLAIN_BS, offset - size_down.QuadPart) : 0;
	}
	
	if (to_write == 0) { 
		if (bSet) {
			DbgPrint(L"setting end of file at %d\n", (int)up_off.QuadPart);
			if (!SetFilePointerEx(m_handle, up_off, NULL, FILE_BEGIN))
				return FALSE;
			return ::SetEndOfFile(m_handle);
		} else {
			return TRUE;
		}
	}
	
	// need to re-write truncated or expanded last block

	unsigned char buf[PLAIN_BS];

	memset(buf, 0, sizeof(buf));

	void *context = get_crypt_context(BLOCK_IV_LEN, AES_MODE_GCM);

	if (!context)
		return FALSE;

	int nread = read_block(m_con, m_handle, m_header.fileid, last_block, buf, context);

	if (nread < 0) {
		free_crypt_context(context);
		return FALSE;
	}

	if (nread < 1) { // shouldn't happen
		free_crypt_context(context);

		if (bSet) {
			if (!SetFilePointerEx(m_handle, up_off, NULL, FILE_BEGIN)) {
				return FALSE;
			}
			return ::SetEndOfFile(m_handle);
		} else {
			return TRUE;
		}
	}

	int nwritten = write_block(m_con, m_handle, m_header.fileid, last_block, buf, to_write, context);

	free_crypt_context(context);

	if (nwritten != to_write)
		return FALSE;

	if (bSet) {
		if (!SetFilePointerEx(m_handle, up_off, NULL, FILE_BEGIN))
			return FALSE;

		return ::SetEndOfFile(m_handle);
	} else {
		return TRUE;
	}

}
