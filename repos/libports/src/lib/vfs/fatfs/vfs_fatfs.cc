/*
 * \brief  FatFS VFS plugin
 * \author Christian Prochaska
 * \author Emery Hemingway
 * \date   2016-05-22
 *
 * See http://www.elm-chan.org/fsw/ff/00index_e.html
 * or documents/00index_e.html in the FatFS source.
 */

/*
 * Copyright (C) 2016-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <vfs/file_system_factory.h>
#include <vfs/file_system.h>
#include <vfs/vfs_handle.h>
#include <os/path.h>
#include <timer_session/connection.h>

/* Genode block backend */
#include <fatfs/block.h>

namespace Fatfs {

/* FatFS includes */
#include <fatfs/ff.h>

	using namespace Vfs;
	using namespace Fatfs;
	struct File_system;

};


class Fatfs::File_system : public Vfs::File_system
{
	private:

		typedef Genode::Path<FF_MAX_LFN> Path;

		struct Fatfs_file_handle;
		typedef Genode::List<Fatfs_file_handle> Fatfs_file_handles;

		/**
		 * The FatFS library does not support opening a file
		 * for writing twice, so this plugin manages a tree of
		 * open files shared across open VFS handles.
		 */

		struct File : Genode::Avl_node<File>
		{
			Path               path;
			Fatfs::FIL         fil;
			Fatfs_file_handles handles;

			/************************
			 ** Avl node interface **
			 ************************/

			bool higher(File *other) {
				return (Genode::strcmp(other->path.base(), path.base()) > 0); }

			File *lookup(char const *path_str)
			{
				int const cmp = Genode::strcmp(path_str, path.base());
				if (cmp == 0)
					return this;

				File *f = Genode::Avl_node<File>::child(cmp);
				return f ? f->lookup(path_str) : nullptr;
			}
		};

		struct Fatfs_handle : Vfs_handle
		{
			using Vfs_handle::Vfs_handle;

			virtual Read_result complete_read(char *buf,
			                                  file_size buf_size,
			                                  file_size &out_count) = 0;
		};

		struct Fatfs_file_handle : Fatfs_handle, Fatfs_file_handles::Element
		{
			File *file = nullptr;

			Fatfs_file_handle(File_system &fs, Allocator &alloc, int status_flags)
			: Fatfs_handle(fs, fs, alloc, status_flags) { }

			Read_result complete_read(char *buf,
			                          file_size buf_size,
			                          file_size &out_count) override
			{
				if (!file) {
					Genode::error("READ_ERR_INVALID");
					return READ_ERR_INVALID;
				}
				if ((status_flags()&OPEN_MODE_ACCMODE) == OPEN_MODE_WRONLY)
					return READ_ERR_INVALID;

				FRESULT fres;
				FIL *fil = &file->fil;

				fres = f_lseek(fil, seek());
				if (fres == FR_OK) {
					UINT bw = 0;
					fres = f_read(fil, buf, buf_size, &bw);
					out_count = bw;
				}

				switch (fres) {
				case FR_OK:             return READ_OK;
				case FR_INVALID_OBJECT: return READ_ERR_INVALID;
				case FR_TIMEOUT:        return READ_ERR_WOULD_BLOCK;
				case FR_DISK_ERR:       return READ_ERR_IO;
				case FR_INT_ERR:        return READ_ERR_IO;
				case FR_DENIED:         return READ_ERR_IO;
				default:                return READ_ERR_IO;
				}
			}
		};

		struct Fatfs_dir_handle : Fatfs_handle
		{
			DIR dir;

			Fatfs_dir_handle(File_system &fs, Allocator &alloc)
			: Fatfs_handle(fs, fs, alloc, 0) { }

			Read_result complete_read(char *buf,
			                          file_size buf_size,
			                          file_size &out_count) override
			{
				/* not very efficient, just N calls to f_readdir */

				out_count = 0;

				if (buf_size < sizeof(Dirent))
					return READ_ERR_INVALID;

				file_size dir_index = seek() / sizeof(Dirent);

				Dirent *vfs_dir = (Dirent*)buf;

				FILINFO info;
				FRESULT res;
				vfs_dir->fileno = 1; /* inode 0 is a pending unlink */

				do {
					res = f_readdir (&dir, &info);
					if ((res != FR_OK) || (!info.fname[0])) {
						vfs_dir->type    = DIRENT_TYPE_END;
						vfs_dir->name[0] = '\0';
						out_count = sizeof(Dirent);
						return READ_OK;
					}
				} while (dir_index-- > 0);

				vfs_dir->type = (info.fattrib & AM_DIR) ?
					DIRENT_TYPE_DIRECTORY : DIRENT_TYPE_FILE;
				Genode::strncpy(vfs_dir->name, (const char*)info.fname,
				                sizeof(vfs_dir->name));

				out_count = sizeof(Dirent);
				return READ_OK;
			}
		};

		Genode::Env &_env;
		Genode::Allocator &_alloc;

		FATFS _fatfs;

		/* Tree of open FatFS file objects */
		Genode::Avl_tree<File> _open_files;

		/* Pre-allocated FIL */
		File *_next_file = nullptr;

		/**
		 * Return an open FatFS file matching path or null.
		 */
		File *_opened_file(char const *path)
		{
			return _open_files.first() ?
				_open_files.first()->lookup(path) : nullptr;
		}

		/**
		 * Close an open FatFS file
		 */
		void _close(File &file)
		{
			/* close file */
			_open_files.remove(&file);
			f_close(&file.fil);

			if (_next_file == nullptr) {
				/* reclaim heap space */
				file.path.import("");
				_next_file = &file;
			} else {
				destroy(_alloc, &file);
			}
		}

		/**
		 * Invalidate all handles on a FatFS file
		 * and close the file
		 */
		void _close_all(File &file)
		{
			/* invalidate handles */
			for (Fatfs_file_handle *handle = file.handles.first();
			     handle; handle = file.handles.first())
			{
				handle->file = nullptr;
				file.handles.remove(handle);
			}
			_close(file);
		}

	public:

		File_system(Genode::Env       &env,
		            Genode::Allocator &alloc,
		            Genode::Xml_node   config)
		: _env(env), _alloc(alloc)
		{
			{
				static unsigned codepage = 0;
				unsigned const cp = config.attribute_value<unsigned>(
					"codepage", 0);

				if (codepage != 0 && codepage != cp) {
					Genode::error(
						"cannot reinitialize codepage for FAT library, please "
						"use additional VFS instances for additional codepages");
					throw ~0;
				}

				if (f_setcp(cp) != FR_OK) {
					Genode::error("invalid OEM code page '", cp, "'");
					throw FR_INVALID_PARAMETER;
				}
				codepage = cp;
			}

			auto const drive_num = config.attribute_value(
				"drive", Genode::String<4>("0"));

#if _USE_MKFS == 1
			if (config.attribute_value("format", false)) {
				Genode::log("formatting drive ", drive_num, "...");
				if (f_mkfs((const TCHAR*)drive_num.string(), 1, 0) != FR_OK) {
					Genode::error("format of drive ", drive_num, " failed");
					throw ~0;
				}
			}
#endif

			/* mount the file system */
			switch (f_mount(&_fatfs, (const TCHAR*)drive_num.string(), 1)) {
			case FR_OK: {
				TCHAR label[24] = { '\0' };
				f_getlabel((const TCHAR*)drive_num.string(), label, nullptr);
				Genode::log("FAT file system \"", (char const *)label, "\" mounted");
				return;
			}
			case FR_INVALID_DRIVE:
				Genode::error("invalid drive ", drive_num);           throw ~0;
			case FR_DISK_ERR:
				Genode::error("drive ", drive_num, " disk error");    throw ~0;
			case FR_NOT_READY:
				Genode::error("drive ", drive_num, " not ready");     throw ~0;
			case FR_NO_FILESYSTEM:
				Genode::error("no file system on drive ", drive_num); throw ~0;
			default:
				Genode::error("failed to mount drive ", drive_num);   throw ~0;
			}
		}

		/***************************
		 ** File_system interface **
		 ***************************/

		char const *type() override { return "fatfs"; }


		/*********************************
		 ** Directory service interface **
		 *********************************/

		Open_result open(char const *path, unsigned vfs_mode,
		                 Vfs_handle **vfs_handle,
		                 Allocator  &alloc) override
		{
			Fatfs_file_handle *handle;

			File *file = _opened_file(path);
			bool create = vfs_mode & OPEN_MODE_CREATE;

			if (file && create) {
				Genode::error("OPEN_ERR_EXISTS");
				return OPEN_ERR_EXISTS;
			}

			if (file && f_error(&file->fil)) {
				Genode::error("FatFS: hard error on file '", path, "'");
				return OPEN_ERR_NO_PERM;
			};

			/* attempt allocation before modifying blocks */
			if (!_next_file)
				_next_file = new (_alloc) File();
			handle = new (alloc) Fatfs_file_handle(*this, alloc, vfs_mode);

			if (!file) {
				file = _next_file;
				FRESULT fres = f_open(
					&_next_file->fil, (TCHAR const *)path,
					FA_READ | FA_WRITE | (create ? FA_CREATE_NEW : FA_OPEN_EXISTING));
				if (fres != FR_OK) {
					destroy(alloc, handle);
					switch(fres) {
					case FR_NO_FILE:
					case FR_NO_PATH:      return OPEN_ERR_UNACCESSIBLE;
					case FR_EXIST:        return OPEN_ERR_EXISTS;
					case FR_INVALID_NAME: return OPEN_ERR_NAME_TOO_LONG;
					default:              return OPEN_ERR_NO_PERM;
					}
				}

				file->path.import(path);
				_open_files.insert(file);
				_next_file = nullptr;
			}

			file->handles.insert(handle);
			handle->file = file;
			*vfs_handle = handle;
			return OPEN_OK;
		}

		Opendir_result opendir(char const *path, bool create,
		                       Vfs_handle **vfs_handle,
		                       Allocator &alloc) override
		{
			Fatfs_dir_handle *handle;

			/* attempt allocation before modifying blocks */
			handle = new (alloc) Fatfs_dir_handle(*this, alloc);

			if (create) {
				FRESULT res = f_mkdir((const TCHAR*)path);
				if (res != FR_OK) {
					destroy(alloc, handle);
					switch (res) {
					case FR_EXIST:        return OPENDIR_ERR_NODE_ALREADY_EXISTS;
					case FR_NO_PATH:      return OPENDIR_ERR_LOOKUP_FAILED;
					case FR_INVALID_NAME: return OPENDIR_ERR_NAME_TOO_LONG;
					default:              return OPENDIR_ERR_PERMISSION_DENIED;
					}
				}
			}

			FRESULT res = f_opendir(&handle->dir, (const TCHAR*)path);
			if (res != FR_OK) {
				destroy(alloc, handle);
				switch (res) {
				case FR_NO_PATH: return OPENDIR_ERR_LOOKUP_FAILED;
				default:         return OPENDIR_ERR_PERMISSION_DENIED;
				}
			}

			*vfs_handle = handle;

			return OPENDIR_OK;
		}

		void close(Vfs_handle *vfs_handle) override
		{
			{
				Fatfs_file_handle *handle;

				handle = dynamic_cast<Fatfs_file_handle *>(vfs_handle);

				if (handle) {
					File *file = handle->file;
					if (file) {
						file->handles.remove(handle);
						if (!file->handles.first())
							_close(*file);
						else
							f_sync(&file->fil);
					}
					destroy(handle->alloc(), handle);
					return;
				}
			}

			{
				Fatfs_dir_handle *handle;

				handle = dynamic_cast<Fatfs_dir_handle *>(vfs_handle);

				if (handle) {
					f_closedir(&handle->dir);
					destroy(handle->alloc(), handle);
				}
			}
		}

		Genode::Dataspace_capability dataspace(char const *path) override
		{
			Genode::warning(__func__, " not implemented in FAT plugin");
			return Genode::Dataspace_capability();
		}

		void release(char const *path,
		             Genode::Dataspace_capability ds_cap) override { }

		file_size num_dirent(char const *path) override
		{
			DIR       dir;
			FILINFO   fno;
			file_size count = 0;

			if (f_opendir(&dir, (const TCHAR*)path) != FR_OK) return 0;

			fno.fname[0] = 0xFF;
			while ((f_readdir (&dir, &fno) == FR_OK) && fno.fname[0])
				++count;
			f_closedir(&dir);
			return count;
		}

		bool directory(char const *path) override
		{
			FILINFO fno;

			return f_stat((const TCHAR*)path, &fno) == FR_OK ?
				(fno.fattrib & AM_DIR) : false;
		}

		char const *leaf_path(char const *path) override
		{
			if (_opened_file(path)) {
				return path;
			} else {
				FILINFO fno;
				return (f_stat((const TCHAR*)path, &fno) == FR_OK) ?
					path : 0;
			}
		}

		Stat_result stat(char const *path, Stat &stat) override
		{
			stat = Stat();

			FILINFO info;

			FRESULT const err = f_stat((const TCHAR*)path, &info);
			switch (err) {
			case FR_OK:
				stat.inode = 1;
				stat.device = (Genode::addr_t)this;
				stat.mode = (info.fattrib & AM_DIR) ?
					STAT_MODE_DIRECTORY : STAT_MODE_FILE;
				/* XXX: size in f_stat is always zero */
				if ((stat.mode == STAT_MODE_FILE) && (info.fsize == 0)) {
					File *file = _opened_file(path);
					if (file) {
						stat.size = f_size(&file->fil);
					} else {
						FIL fil;
						if (f_open(&fil, (TCHAR const *)path, FA_READ) == FR_OK) {
							stat.size = f_size(&fil);
							f_close(&fil);
						}
					}
				} else {
					stat.size = info.fsize;
				}
				return STAT_OK;

			case FR_NO_FILE:
			case FR_NO_PATH:
				return STAT_ERR_NO_ENTRY;

			default:
				Genode::error("unhandled FatFS::f_stat error ", (int)err);
				return STAT_ERR_NO_PERM;
			}
			return STAT_ERR_NO_PERM;
		}

		Unlink_result unlink(char const *path) override
		{
			/* close the file if it is open */
			if (File *file = _opened_file(path))
				_close_all(*file);

			switch (f_unlink((const TCHAR*)path)) {
			case FR_OK: return UNLINK_OK;
			case FR_NO_FILE:
			case FR_NO_PATH: return UNLINK_ERR_NO_ENTRY;
			default:         return UNLINK_ERR_NO_PERM;
			}
		}

		Rename_result rename(char const *from, char const *to) override
		{
			if (File *to_file = _opened_file(to)) {
				_close_all(*to_file);
				f_unlink((TCHAR const *)to);
			} else {
				FILINFO info;
				if (FR_OK == f_stat((TCHAR const *)to, &info)) {
					if (info.fattrib & AM_DIR) {
						return RENAME_ERR_NO_PERM;
					} else {
						f_unlink((TCHAR const *)to);
					}
				}
			}

			if (File *from_file = _opened_file(from))
				_close_all(*from_file);

			switch (f_rename((const TCHAR*)from, (const TCHAR*)to)) {
			case FR_OK:      return RENAME_OK;
			case FR_NO_FILE:
			case FR_NO_PATH: return RENAME_ERR_NO_ENTRY;
			default:         return RENAME_ERR_NO_PERM;
			}
		}


		/*******************************
		 ** File io service interface **
		 *******************************/

		Write_result write(Vfs_handle *vfs_handle,
		                   char const *buf, file_size buf_size,
		                   file_size &out_count) override
		{
			Fatfs_file_handle *handle = static_cast<Fatfs_file_handle *>(vfs_handle);
			if (!handle->file)
				return WRITE_ERR_INVALID;
			if ((handle->status_flags()&OPEN_MODE_ACCMODE) == OPEN_MODE_RDONLY)
				return WRITE_ERR_INVALID;

			FRESULT fres = FR_OK;
			FIL *fil = &handle->file->fil;
			FSIZE_t const wpos = handle->seek();

			/* seek file pointer */
			if (f_tell(fil) != wpos) {
				/*
				 * seeking beyond the EOF will expand the file size
				 * and is not the expected behavior
				 */
				if (f_size(fil) < wpos)
					return WRITE_ERR_INVALID;

				fres = f_lseek(fil, wpos);
				/* check the seek again */
				if (f_tell(fil) != handle->seek())
					return WRITE_ERR_IO;
			}

			if (fres == FR_OK) {
				UINT bw = 0;
				fres = f_write(fil, buf, buf_size, &bw);
				f_sync(fil);
				out_count = bw;
			}

			switch (fres) {
			case FR_OK:             return WRITE_OK;
			case FR_INVALID_OBJECT: return WRITE_ERR_INVALID;
			case FR_TIMEOUT:        return WRITE_ERR_WOULD_BLOCK;
			default:                return WRITE_ERR_IO;
			}
		}

		Read_result complete_read(Vfs_handle *vfs_handle, char *buf,
		                          file_size buf_size,
		                          file_size &out_count) override
		{
			Fatfs_file_handle *handle = static_cast<Fatfs_file_handle *>(vfs_handle);
			return handle->complete_read(buf, buf_size, out_count);
		}

		Ftruncate_result ftruncate(Vfs_handle *vfs_handle, file_size len) override
		{
			Fatfs_file_handle *handle = static_cast<Fatfs_file_handle *>(vfs_handle);

			if (!handle->file)
				return FTRUNCATE_ERR_NO_PERM;
			if ((handle->status_flags()&OPEN_MODE_ACCMODE) == OPEN_MODE_RDONLY)
				return FTRUNCATE_ERR_NO_PERM;

			FIL *fil = &handle->file->fil;
			FRESULT res = FR_OK;

			/* f_lseek will expand a file... */
			res = f_lseek(fil, len);
			if (f_tell(fil) != len)
				return f_size(fil) < len ?
					FTRUNCATE_ERR_NO_SPACE : FTRUNCATE_ERR_NO_PERM;

			/* ... otherwise truncate will shorten to the seek position */
			if ((res == FR_OK) && (len < f_size(fil))) {
				res = f_truncate(fil);
				if (res == FR_OK && len < handle->seek())
					handle->seek(len);
			}

			return res == FR_OK ?
				FTRUNCATE_OK : FTRUNCATE_ERR_NO_PERM;
		}

		bool read_ready(Vfs_handle *) override { return true; }
};


struct Fatfs_factory : Vfs::File_system_factory
{
	struct Inner : Vfs::File_system_factory
	{
		Inner(Genode::Env &env, Genode::Allocator &alloc) {
			Fatfs::block_init(env, alloc); }

		Vfs::File_system *create(Genode::Env       &env,
		                         Genode::Allocator &alloc,
		                         Genode::Xml_node   node,
		                         Vfs::Io_response_handler &) override
		{ 
			return new (alloc)
				Fatfs::File_system(env, alloc, node);
		}
	};

	Vfs::File_system *create(Genode::Env       &env,
	                         Genode::Allocator &alloc,
	                         Genode::Xml_node   node,
	                         Vfs::Io_response_handler &io_handler) override
	{
		static Inner factory(env, alloc);
		return factory.create(env, alloc, node, io_handler);
	}
};


extern "C" Vfs::File_system_factory *vfs_file_system_factory(void)
{
	static Fatfs_factory factory;
	return &factory;
}
