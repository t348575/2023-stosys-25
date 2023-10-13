/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

#ifndef STOSYS_PROJECT_S2FILESYSTEM_H
#define STOSYS_PROJECT_S2FILESYSTEM_H

#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/io_status.h"
#include "rocksdb/status.h"

#include "LockManager.h"

#include <ftl.h>
#include <iostream>
#include <vector>
#include <zns_device.h>

namespace ROCKSDB_NAMESPACE {
	const static uint8_t BLOCK_BIT_MASKS[4] = {0b00000011, 0b00001100, 0b00110000, 0b11000000};
	const static uint8_t RESET_BIT_MASKS[4] = {0b11111100, 0b11110011, 0b11001111, 0b00111111};
	const static uint64_t INODE_SIZE = 132;
	const static uint64_t NUM_DISK_POINTERS = 13;
	const static uint64_t NUM_NESTED_DISK_POINTERS = 14;
	const static uint64_t DIR_BLOCK_PLAIN_SIZE = 18;
	const static uint64_t DIR_SUPER_BLOCK_PLAIN_SIZE = 16;

	class SuperBlock {
	public:
		SuperBlock() = default;
		SuperBlock(const std::vector<uint8_t> &buffer);
		void serialize(std::vector<uint8_t> &output);
		uint64_t block_map_size;
		uint16_t original_inode_reserved;
	};

	struct INodePointer {
		uint64_t block;
		uint8_t offset;
	};

	enum S2EntryType : uint8_t { File = 0, Directory };

	class INode {
	public:
		INode() = default;
		INode(const uint8_t *buffer);
		void serialize(uint8_t *output) const;

		bool is_nested = false;
		S2EntryType inode_type;
		uint64_t num_disk_pointers = 0;
		uint64_t size = 0;
		uint64_t disk_pointers[NUM_NESTED_DISK_POINTERS]; // or NUM_DISK_POINTERS if normal size
		INodePointer next_pointer;
		bool use_next_pointer = false;
	};

	struct RichInode {
		INode inode;
		INodePointer ptr;
	};

	class DirectorySuperBlock {
	public:
		DirectorySuperBlock() = default;
		DirectorySuperBlock(const uint8_t *buffer);
		void serialize(uint8_t *output) const;

		uint64_t num_entries = 0;
		uint64_t name_len;
		std::string name;
	};

	class DirectoryEntry {
	public:
		DirectoryEntry() = default;
		DirectoryEntry(const uint8_t *buffer);
		void serialize(uint8_t *output) const;

		uint64_t block;
		uint8_t offset;
		S2EntryType type;
		uint64_t name_len;
		std::string name;
	};

	struct DeleteDirDfsItem {
		RichInode ri;
		std::string path;
	};

	class RocksWriteableFile;
	class S2FileSystem : public FileSystem {
	public:
		enum FsError : int { None = 0, CouldNotFindInode = -1 };
		enum BlockType : uint8_t { Unused = 0, Inode, Data };

		// No copying allowed
		S2FileSystem(std::string uri, bool debug);
		S2FileSystem(const S2FileSystem &) = delete;
		~S2FileSystem();

		IOStatus IsDirectory(const std::string &, const IOOptions &options, bool *is_dir, IODebugContext *) override;

		IOStatus NewSequentialFile(const std::string &fname, const FileOptions &file_opts,
								   std::unique_ptr<FSSequentialFile> *result,
								   __attribute__((unused)) IODebugContext *dbg);

		IOStatus NewRandomAccessFile(const std::string &fname, const FileOptions &file_opts,
									 std::unique_ptr<FSRandomAccessFile> *result,
									 __attribute__((unused)) IODebugContext *dbg);

		IOStatus NewWritableFile(const std::string &fname, const FileOptions &file_opts,
								 std::unique_ptr<FSWritableFile> *result, __attribute__((unused)) IODebugContext *dbg);

		IOStatus ReopenWritableFile(const std::string &, const FileOptions &, std::unique_ptr<FSWritableFile> *,
									IODebugContext *);

		IOStatus NewRandomRWFile(const std::string &, const FileOptions &, std::unique_ptr<FSRandomRWFile> *,
								 IODebugContext *);

		IOStatus NewMemoryMappedFileBuffer(const std::string &, std::unique_ptr<MemoryMappedFileBuffer> *);

		IOStatus NewDirectory(const std::string &name, const IOOptions &io_opts, std::unique_ptr<FSDirectory> *result,
							  __attribute__((unused)) IODebugContext *dbg);

		const char *Name() const;

		IOStatus GetFreeSpace(const std::string &, const IOOptions &, uint64_t *, IODebugContext *);

		IOStatus Truncate(const std::string &, size_t, const IOOptions &, IODebugContext *);

		IOStatus CreateDir(const std::string &dirname, const IOOptions &options,
						   __attribute__((unused)) IODebugContext *dbg);

		IOStatus CreateDirIfMissing(const std::string &dirname, const IOOptions &options,
									__attribute__((unused)) IODebugContext *dbg);

		IOStatus GetFileSize(const std::string &fname, const IOOptions &options, uint64_t *file_size,
							 __attribute__((unused)) IODebugContext *dbg);

		IOStatus DeleteDir(const std::string &dirname, const IOOptions &options,
						   __attribute__((unused)) IODebugContext *dbg);

		IOStatus GetFileModificationTime(const std::string &fname, const IOOptions &options, uint64_t *file_mtime,
										 __attribute__((unused)) IODebugContext *dbg);

		IOStatus GetAbsolutePath(const std::string &db_path, const IOOptions &options, std::string *output_path,
								 __attribute__((unused)) IODebugContext *dbg);

		IOStatus DeleteFile(const std::string &fname, const IOOptions &options, IODebugContext *dbg);

		IOStatus NewLogger(const std::string &fname, const IOOptions &io_opts, std::shared_ptr<Logger> *result,
						   __attribute__((unused)) IODebugContext *dbg);

		IOStatus GetTestDirectory(const IOOptions &options, std::string *path,
								  __attribute__((unused)) IODebugContext *dbg);

		IOStatus UnlockFile(FileLock *lock, const IOOptions &options, __attribute__((unused)) IODebugContext *dbg);

		IOStatus LockFile(const std::string &fname, const IOOptions &options, FileLock **lock,
						  __attribute__((unused)) IODebugContext *dbg);

		IOStatus AreFilesSame(const std::string &, const std::string &, const IOOptions &, bool *, IODebugContext *);

		IOStatus NumFileLinks(const std::string &, const IOOptions &, uint64_t *, IODebugContext *);

		IOStatus LinkFile(const std::string &, const std::string &, const IOOptions &, IODebugContext *);

		IOStatus RenameFile(const std::string &src, const std::string &target, const IOOptions &options,
							__attribute__((unused)) IODebugContext *dbg);

		IOStatus GetChildrenFileAttributes(const std::string &dir, const IOOptions &options,
										   std::vector<FileAttributes> *result,
										   __attribute__((unused)) IODebugContext *dbg);

		IOStatus GetChildren(const std::string &dir, const IOOptions &options, std::vector<std::string> *result,
							 __attribute__((unused)) IODebugContext *dbg);

		IOStatus FileExists(const std::string &fname, const IOOptions &options,
							__attribute__((unused)) IODebugContext *dbg);

		IOStatus ReuseWritableFile(const std::string &fname, const std::string &old_fname, const FileOptions &file_opts,
								   std::unique_ptr<FSWritableFile> *result,
								   __attribute__((unused)) IODebugContext *dbg);

		static std::string SanitizePath(const std::string input);
		void SetBlockType(const uint16_t num, const BlockType block_type);
		IOStatus WriteBlockMap();
		IOStatus WriteSuperBlock();
		IOStatus WriteBlock(const uint64_t block, const uint8_t *data);
		IOStatus ReadBlock(const uint64_t block, uint8_t *data);
		IOStatus RemoveInode(const INodePointer ptr);

		IOStatus PlaceNewINodeBlock(const INode inode, INodePointer &ptr, bool flush = true);
		IOStatus PlaceNewDataBlock(uint64_t &block_num, bool flush = true);
		IOStatus ReadINode(const INodePointer ptr, INode &inode);
		IOStatus WriteINode(const RichInode ri, bool increment_count);

		struct FindInodeDirs {
			DirectorySuperBlock sb;
			std::vector<DirectoryEntry> dir_entries;
			uint64_t buffer_size;
		};

		IOStatus FindInode(const std::string &path, RichInode &inode);
		IOStatus FindInodeWithBase(const std::string &path, RichInode &inode, RichInode &base_inode,
								   std::string &base_path);
		IOStatus FindInodeWithBaseWithDirs(const std::string &path, RichInode &inode, RichInode &base_inode,
										   std::string &base_path, FindInodeDirs &dirs);
		IOStatus AddDiskPointer(RichInode *inode, RichInode **output);

		IOStatus CreateItem(const std::string &name, const S2EntryType item, const RichInode base_inode,
							FindInodeDirs &dirs, RichInode &inode);
		IOStatus ReadEntireFile(const INode inode, std::vector<uint8_t> &output);
		IOStatus OverWriteEntireFile(const RichInode inode, const std::vector<uint8_t> &buffer);
		IOStatus ReadDirectory(const INode inode, DirectorySuperBlock &dir_super_block,
							   std::vector<DirectoryEntry> &directory, uint64_t &buffer_size);
		IOStatus AddEntryToDirectory(const RichInode base_inode, FindInodeDirs &dirs, DirectoryEntry entry,
									 RichInode &inode);
		IOStatus RemoveItem(const std::string path, const std::string base_path, const RichInode ri);
		RocksWriteableFile NewWritableFileByINode(const RichInode rich_inode);

	private:
		struct user_zns_device *zns_dev;
		std::string _uri;
		const std::string _fs_delimiter = "/";

		// indexes offset after super block and block_map
		std::vector<uint8_t> block_map; // bitmap of inode and data
		uint64_t block_map_offset;		// end of data bitmap block
		uint64_t inodes_per_block;
		SuperBlock super_block;
		DataMap *device = DataMap::get_instance();
		std::recursive_mutex mtx;
	};
} // namespace ROCKSDB_NAMESPACE

#endif // STOSYS_PROJECT_S2FILESYSTEM_H
