/*
 * \brief  File-system directory node
 * \author Norman Feske
 * \date   2012-04-11
 */

/*
 * Copyright (C) 2012-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _INCLUDE__RAM_FS__DIRECTORY_H_
#define _INCLUDE__RAM_FS__DIRECTORY_H_

/* Genode includes */
#include <file_system/util.h>

/* local includes */
#include "node.h"
#include "file.h"
#include "symlink.h"

namespace Ram_fs { class Directory; }


class Ram_fs::Directory : public Node
{
	private:

		List<Node> _entries;
		size_t     _num_entries;

		Node *_entry_unsynchronized(size_t index)
		{
			Node *node = _entries.first();
			for (unsigned i = 0; i < index && node; node = node->next(), i++);
			return node;
		}

	public:

		Directory(char const *name) : _num_entries(0) { Node::name(name); }

		bool has_sub_node_unsynchronized(char const *name) const override
		{
			Node const *sub_node = _entries.first();
			for (; sub_node; sub_node = sub_node->next())
				if (strcmp(sub_node->name(), name) == 0)
					return true;

			return false;
		}

		void adopt_unsynchronized(Node *node) override
		{
			/*
			 * XXX inc ref counter
			 */
			_entries.insert(node);
			_num_entries++;

			mark_as_updated();
		}

		void discard(Node *node) override
		{
			_entries.remove(node);
			_num_entries--;

			mark_as_updated();
		}

		Node *lookup(char const *path, bool return_parent = false) override
		{
			if (strcmp(path, "") == 0) {
				return this;
			}

			if (!path || path[0] == '/')
				throw File_system::Lookup_failed();

			/* find first path delimiter */
			unsigned i = 0;
			for (; path[i] && path[i] != '/'; i++);

			/*
			 * If no path delimiter was found, we are the parent of the
			 * specified path.
			 */
			if (path[i] == 0 && return_parent) {
				return this;
			}

			/*
			 * The offset 'i' corresponds to the end of the first path
			 * element, which can be either the end of the string or the
			 * first '/' character.
			 */

			/* try to find entry that matches the first path element */
			Node *sub_node = _entries.first();
			for (; sub_node; sub_node = sub_node->next())
				if ((strlen(sub_node->name()) == i) &&
					(strcmp(sub_node->name(), path, i) == 0))
					break;

			if (!sub_node)
				throw File_system::Lookup_failed();

			if (!File_system::contains_path_delimiter(path)) {

				/*
				 * Because 'path' is a basename that corresponds to an
				 * existing sub_node, we have found what we were looking
				 * for.
				 */
				return sub_node;
			}

			/*
			 * As 'path' contains one or more path delimiters, traverse
			 * into the sub directory names after the first path element.
			 */

			/*
			 * We cannot traverse into anything other than a directory.
			 *
			 * XXX we might follow symlinks here
			 */
			Directory *sub_dir = dynamic_cast<Directory *>(sub_node);
			if (!sub_dir)
				throw File_system::Lookup_failed();

			return sub_dir->lookup(path + i + 1, return_parent);
		}

		Directory *lookup_dir(char const *path)
		{
			Node *node = lookup(path);

			Directory *dir = dynamic_cast<Directory *>(node);
			if (dir)
				return dir;

			throw File_system::Lookup_failed();
		}

		File *lookup_file(char const *path) override
		{
			Node *node = lookup(path);

			File *file = dynamic_cast<File *>(node);
			if (file)
				return file;

			throw File_system::Lookup_failed();
		}

		Symlink *lookup_symlink(char const *path)
		{
			Node *node = lookup(path);

			Symlink *symlink = dynamic_cast<Symlink *>(node);
			if (symlink)
				return symlink;

			throw File_system::Lookup_failed();
		}

		/**
		 * Lookup parent directory of the specified path
		 *
		 * \throw File_system::Lookup_failed
		 */
		Directory *lookup_parent(char const *path)
		{
			return static_cast<Directory *>(lookup(path, true));
		}

		size_t read(char *dst, size_t len, seek_off_t seek_offset) override
		{
			using File_system::Directory_entry;

			if (len < sizeof(Directory_entry)) {
				Genode::error("read buffer too small for directory entry");
				return 0;
			}

			seek_off_t index = seek_offset / sizeof(Directory_entry);

			if (seek_offset % sizeof(Directory_entry)) {
				Genode::error("seek offset not alighed to sizeof(Directory_entry)");
				return 0;
			}

			Node *node = _entry_unsynchronized(index);

			/* index out of range */
			if (!node)
				return 0;

			Directory_entry *e = (Directory_entry *)(dst);

			e->inode = node->inode();

			if (dynamic_cast<File      *>(node)) e->type = Directory_entry::TYPE_FILE;
			if (dynamic_cast<Directory *>(node)) e->type = Directory_entry::TYPE_DIRECTORY;
			if (dynamic_cast<Symlink   *>(node)) e->type = Directory_entry::TYPE_SYMLINK;

			strncpy(e->name, node->name(), sizeof(e->name));

			return sizeof(Directory_entry);
		}

		size_t write(char const *src, size_t len, seek_off_t seek_offset) override
		{
			/* writing to directory nodes is not supported */
			return 0;
		}

		Status status() override
		{
			Status s;
			s.inode = inode();
			s.size = _num_entries * sizeof(File_system::Directory_entry);
			s.mode = File_system::Status::MODE_DIRECTORY;
			return s;
		}
};

#endif /* _INCLUDE__RAM_FS__DIRECTORY_H_ */
