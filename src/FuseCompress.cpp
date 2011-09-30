/*
    (C) Copyright Milan Svoboda 2009.
    
    This file is part of FuseCompress.

    FuseCompress is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Foobar is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <sys/fsuid.h>
#include <dirent.h>
#include <sys/types.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <cstdlib>
#include <iostream>
#if defined(HAVE_ATTR_XATTR_H)
#  include <attr/xattr.h>
#elif defined(HAVE_SYS_XATTR_H)
#  include <sys/xattr.h>
#endif

#include "rlog/rlog.h"
#include "assert.h"

#include "FuseCompress.hpp"
#include "FileManager.hpp"

extern bool         g_DebugMode;
extern std::string  g_dirLower;
extern std::string  g_dirMount;
static DIR         *g_Dir;
FileManager        *g_FileManager;

FuseCompress::FuseCompress()
{
	memset(&m_ops, 0, sizeof m_ops);

	m_ops.init = FuseCompress::init;
	m_ops.destroy = FuseCompress::destroy;

	m_ops.readlink = FuseCompress::readlink;
	m_ops.getattr = FuseCompress::getattr;
	m_ops.readdir = FuseCompress::readdir;
	m_ops.mknod = FuseCompress::mknod;
	m_ops.mkdir = FuseCompress::mkdir;
	m_ops.rmdir = FuseCompress::rmdir;
	m_ops.unlink = FuseCompress::unlink;
	m_ops.symlink = FuseCompress::symlink;
	m_ops.rename = FuseCompress::rename;
	m_ops.link = FuseCompress::link;
	m_ops.chmod = FuseCompress::chmod;
	m_ops.chown = FuseCompress::chown;
	m_ops.truncate = FuseCompress::truncate;
	m_ops.utimens = FuseCompress::utimens;
	m_ops.open = FuseCompress::open;
	m_ops.read = FuseCompress::read;
	m_ops.write = FuseCompress::write;
	m_ops.flush = FuseCompress::flush;
	m_ops.release = FuseCompress::release;
	m_ops.fsync = FuseCompress::fsync;
	m_ops.statfs = FuseCompress::statfs;
	m_ops.setxattr = FuseCompress::setxattr;
	m_ops.getxattr = FuseCompress::getxattr;
	m_ops.listxattr = FuseCompress::listxattr;
	m_ops.removexattr = FuseCompress::removexattr;
}

FuseCompress::~FuseCompress()
{
}

int FuseCompress::Run(DIR *dir, int argc, const char **argv)
{
	g_Dir = dir;

	// Note: Remove const from argv to make a compiler
	// happy. Anyway, it's used as constant in fuse_main()
	// and the decalration of fuse_main  should be fixed
	// in the fuse package.
	//
	return fuse_main(argc, const_cast<char **> (argv),  &m_ops, NULL);
}

void *FuseCompress::init(struct fuse_conn_info *conn)
{
	if (fchdir(dirfd(g_Dir)) == -1)
	{
		rError("Failed to change directory");
		abort();
	}
	closedir(g_Dir);

	g_FileManager = new (std::nothrow) FileManager();
	if (!g_FileManager)
	{
		rError("No memory to allocate object of FileManager class");
		abort();
	}
	
	return NULL;
}

void FuseCompress::destroy(void *data)
{
	delete g_FileManager;
}

const char *FuseCompress::getpath(const char *path)
{
	assert(path[0] != '\0');

	if ((path[0] == '/') && (path[1] == '\0'))
		return ".";
	return ++path;
}

int FuseCompress::readlink(const char *name, char *buf, size_t size)
{
	int	 r;

	name = getpath(name);

	r = ::readlink(name, buf, size - 1);
	if (r == -1)
		return -errno;

	buf[r] = '\0';

	std::string path(buf);
	strcpy(buf, path.c_str());

	return 0;
}
	
int FuseCompress::getattr(const char *name, struct stat *st)
{
	int	 r = 0;
	CFile	*file;

	name = getpath(name);

	r = ::lstat(name, st);

	// Speed optimization: Fast path for '.' questions.
	//
	if ((name[0] == '.') && (name[1] == '\0'))
		return 0;

	// For symbolic links it's ok to stop here.
	// 
	if (S_ISLNK(st->st_mode))
		return 0;
	
	file = g_FileManager->Get(name);
	if (!file)
		return -errno;

	file->Lock();
	
	if (file->getattr(name, st) == -1)
		r = -errno;

	file->Unlock();

	g_FileManager->Put(file);
	
	return r;
}

int FuseCompress::readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	DIR           *dp;
	struct dirent *de;

	path = getpath(path);

	dp = ::opendir(path);
	if (dp == NULL)
		return -errno;

	while ((de = ::readdir(dp)) != NULL)
	{
		struct stat st;

		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0))
			break;
	}

	::closedir(dp);
	return 0;
}

int FuseCompress::mknod(const char *name, mode_t mode, dev_t rdev)
{
	int			 r = 0;
	uid_t			 uid;
	gid_t			 gid;
	struct stat		 st;
	struct fuse_context	*fc;
	
	name = getpath(name);
	rDebug("FuseCompress::mknod name: %s", name);
	
	fc = fuse_get_context();
	uid = setfsuid(fc->uid);
	gid = setfsgid(fc->gid);

	if (::mknod(name, mode | 0644, rdev) == -1)
		r = -errno;

	setfsuid(uid);
	setfsgid(gid);

	if (g_DebugMode)
	{
		lstat(name, &st); 
		rDebug("FuseCompress::mknod inode: %ld",
				(unsigned long int) st.st_ino);
	}
	
	return r;
}

int FuseCompress::mkdir(const char *path, mode_t mode)
{
	int			 r = 0;
	uid_t			 uid;
	gid_t			 gid;
	struct fuse_context	*fc;

	path = getpath(path);

	fc = fuse_get_context();
	uid = setfsuid(fc->uid);
	gid = setfsgid(fc->gid);
	
	if (::mkdir(path, mode) == -1)
		r = -errno;

	setfsuid(uid);
	setfsgid(gid);
	
	return r;
}

int FuseCompress::rmdir(const char *path)
{
	path = getpath(path);

	if (::rmdir(path) == -1)
		return -errno;

	return 0;
}

int FuseCompress::unlink(const char *path)
{
	int	 r = 0;
	CFile	*file;
	
	path = getpath(path);
	rDebug("FuseCompress::unlink %s", path);

	g_FileManager->Lock();

	file = g_FileManager->GetUnlocked(path, false);
	if (!file)
	{
		if (::unlink(path) == -1)
			r = -errno;

		g_FileManager->Unlock();
		return r;
	}

	g_FileManager->GetUnlocked(file);
	g_FileManager->Unlock();

	file->Lock();
	
	if (file->unlink(path) == -1)
		r = -errno;
	
	file->Unlock();

	g_FileManager->Put(file);

	return r;
}

int FuseCompress::symlink(const char *from, const char *to)
{
	to = getpath(to);

	if (::symlink(from, to) == -1)
		return -errno;

	return 0;
}

/**
 * DoubleLock is not used anywhere now. It is leaved here
 * to show the way how to lock two locks safely.
 *
void FuseCompress::DoubleLock(CFile *first, CFile *second)
{
	if (first->getInode() > second->getInode())
	{
		first->Lock();
		second->Lock();
	}
	else
	{
		second->Lock();
		first->Lock();
	}
}
 */

/**
 * Inode number is the same for newly created file `to` as `from` file.
 * This allows us to simple call ::rename and everything just works.
 */
int FuseCompress::rename(const char *from, const char *to)
{
	int	 r = 0;
	CFile	*file_from;
	CFile	*file_to;
	
	from = getpath(from);
	to = getpath(to);
	rDebug("FuseCompress::rename from: %s, to: %s", from, to);

	g_FileManager->Lock();
	
	file_from = g_FileManager->GetUnlocked(from, false);
	file_to = g_FileManager->GetUnlocked(to, false);
	if (file_to)
	{
		// This is most important command. We need to delete cached
		// content of the file_to.
		//
		file_to->Lock();
		r = file_to->unlink(to);
		file_to->Unlock();

		if (r == -1)
		{
			r = -errno;
			goto error;
		}
	}

	// Rename file 'from' to file 'to'. This changes name of the inode file_from.
	// Thus file_from is also the new file named 'to'.
	// 
	if (::rename(from, to) == -1)
	{
		r = -errno;
		goto error;
	}

	if (file_from)
	{
		// Physically change name of the inode pointed to by file_from...
		// 
		file_from->Lock();
		file_from->m_name = to;
		file_from->Unlock();
	}
error:
	g_FileManager->Unlock();
	return r;
}

int FuseCompress::link(const char *from, const char *to)
{
	from = getpath(from);
	to = getpath(to);

	if (::link(from, to) == -1)
		return -errno;
	
	return 0;
}

int FuseCompress::chmod(const char *path, mode_t mode)
{
	path = getpath(path);

	if (::chmod(path, mode) == -1)
		return -errno;
	
	return 0;
}

int FuseCompress::chown(const char *path, uid_t uid, gid_t gid)
{
	path = getpath(path);

	if (::lchown(path, uid, gid) == -1)
		return -errno;

	return 0;
}

int FuseCompress::truncate(const char *name, off_t size)
{
	int	 r = 0;
	CFile	*file;

	name = getpath(name);
	rDebug("FuseCompress::truncate file %s, to size: %llx", name,
			(long long int) size);

	file = g_FileManager->Get(name);
	if (!file)
		return -errno;

	file->Lock();
	
	if (file->truncate(name, size) == -1)
		r = -errno;

	file->Unlock();

	g_FileManager->Put(file);
	
	return r;
}

int FuseCompress::utimens(const char *name, const struct timespec tv[2])
{
	int	 r = 0;
	CFile	*file;

	name = getpath(name);

	g_FileManager->Lock();

	file = g_FileManager->GetUnlocked(name, false);
	if (!file)
	{
		if (::utimensat(AT_FDCWD, name, tv, AT_SYMLINK_NOFOLLOW) == -1)
			r = -errno;

		g_FileManager->Unlock();
		return r;
	}

	g_FileManager->GetUnlocked(file);
	g_FileManager->Unlock();

	file->Lock();
	
	if (file->utimens(name, tv) == -1)
		r = -errno;
	
	file->Unlock();

	g_FileManager->Put(file);
	
	return r;
}

/**
 * Note:
 *       fi->flags contains open flags supplied with user. Fuse lib and kernel
 *       take care about correct read / write (append) behaviour. We don't need to
 *       care about this. Fuse also takes care about permissions.
 */
int FuseCompress::open(const char *name, struct fuse_file_info *fi)
{
	int	 r = 0;
	CFile	*file;
	
	name = getpath(name);

	file = g_FileManager->Get(name);

	rDebug("FuseCompress::open %p name: %s", (void *) file, name);

	if (!file)
		return -errno;

	file->Lock();
	
	if (file->open(name, fi->flags) == -1)
		r = -errno;

	fi->fh = (long) file;

	file->Unlock();

	if (r != 0)
	{
		// Error occured during open. We must put file to keep
		// reference number in sync. (Release won't be called if
		// open returns error code.)
		//
		g_FileManager->Put(file);
	}
	
	// There is no g_FileManager->Put(file), because the file is
	// opened and is beeing used...
	// 
	return r;
}

int FuseCompress::read(const char *name, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int	 r;
	CFile	*file = reinterpret_cast<CFile *> (fi->fh);

	rDebug("FuseCompress::read(B) %p name: %s, size: 0x%x, offset: 0x%llx",
			(void *) file, getpath(name), (unsigned int) size, (long long int) offset);

	file->Lock();
	
	r = file->read(buf, size, offset);
	if (r == -1)
		r = -errno;

	file->Unlock();

	rDebug("FuseCompress::read(E) %p name: %s, size: 0x%x, offset: 0x%llx, returned: 0x%x",
			(void *) file, getpath(name), (unsigned int) size, (long long int) offset, r);

	sched_yield();

	return r;
}

int FuseCompress::write(const char *name, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int	 r;
	CFile	*file = reinterpret_cast<CFile *> (fi->fh);

	rDebug("FuseCompress::write %p name: %s, size: 0x%x, offset: 0x%llx",
			(void *) file, getpath(name), (unsigned int) size, (long long int) offset);

	file->Lock();
	
	r = file->write(buf, size, offset);
	if (r == -1)
		r = -errno;

	file->Unlock();

	sched_yield();

	return r;
}

int FuseCompress::flush(const char *name, struct fuse_file_info *fi)
{
	int	 r = 0;
	CFile	*file = reinterpret_cast<CFile *> (fi->fh);

	name = getpath(name);

	file->Lock();
	
	if (file->flush(name) == -1)
		r = -errno;

	file->Unlock();

	return r;
}

int FuseCompress::fsync(const char *name, int isdatasync,
                          struct fuse_file_info *fi)
{
	int	 r = 0;
	CFile	*file = reinterpret_cast<CFile *> (fi->fh);

	name = getpath(name);

	file->Lock();
	
	if (isdatasync)
	{
		if (file->fdatasync(name) == -1)
			r = -errno;
	}
	else
		if (file->fsync(name) == -1)
			r = -errno;

	file->Unlock();

	return r;
}

/**
 * Counterpart to the open, for each open there will be exactly
 * one call for release.
 */
int FuseCompress::release(const char *name, struct fuse_file_info *fi)
{
	int	 r = 0;
	CFile	*file = reinterpret_cast<CFile *> (fi->fh);

	name = getpath(name);
	rDebug("FuseCompress::release %p name: %s", (void *) file, name);

	file->Lock();
	
	if (file->release(name) == -1)
		r = -errno;

	file->Unlock();

	g_FileManager->Put(file);
	
	return r;
}

int FuseCompress::statfs(const char *name, struct statvfs *buf)
{
	int r = statvfs(getpath(name), buf);

	// Milan's TODO: Maybe increase the number of free blocks.
	// We are compressed filesystem, aren't we? :-)
	
	// buf->f_blocks *= 2;
	// buf->f_bavail *= 2;

	return r;
}

int FuseCompress::setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
	path = getpath(path);

	if (-1 == ::lsetxattr(path, name, value, size, flags))
		return -errno;
	return 0;
}

int FuseCompress::getxattr(const char *path, const char *name, char *value, size_t size)
{
	path = getpath(path);

	int r;
	if (-1 == (r = ::lgetxattr(path, name, value, size)))
		return -errno;
	return r;
}

int FuseCompress::listxattr(const char *path, char *list, size_t size)
{
	path = getpath(path);

	int r;
	if (-1 == (r = ::llistxattr(path, list, size)))
		return -errno;
	return r;
}

int FuseCompress::removexattr(const char *path, const char *name)
{
	path = getpath(path);

	if (-1 == ::lremovexattr(path, name))
		return -errno;
	return 0;
}

