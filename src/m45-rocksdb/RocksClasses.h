#pragma once

#ifndef ROCKS_CLASSES_H
#define ROCKS_CLASSES_H

#include <rocksdb/file_system.h>

#include "S2FileSystem.h"

namespace ROCKSDB_NAMESPACE {
	class RocksSequentialFile : public FSSequentialFile {
	public:
		RocksSequentialFile(const INode inode, const struct user_zns_device *zns_dev, S2FileSystem *fs);

		IOStatus Read(size_t n, const IOOptions &options, Slice *result, char *scratch, IODebugContext *dbg);
		IOStatus Skip(uint64_t n);

		std::string path;

	private:
		INode root_inode;
		INode curr_inode;
		uint64_t ptr = 0;
		uint64_t disk_ptr = 0;
		S2FileSystem *fs;
		std::vector<uint8_t> buffer;
		const struct user_zns_device *zns_dev;
	};

	class RocksDirectory : public FSDirectory {
	public:
		RocksDirectory() = default;

		IOStatus Fsync(const IOOptions &options, IODebugContext *dbg) {
			UNUSED(options);
			UNUSED(dbg);
			return IOStatus::OK();
		}
	};

	class RocksWriteableFile : public FSWritableFile {
	public:
		RocksWriteableFile() = default;
		RocksWriteableFile(RichInode root_inode, S2FileSystem *fs, struct user_zns_device *zns_dev, bool preallocate);
		~RocksWriteableFile();

		IOStatus Append(const Slice &data, const IOOptions &options, IODebugContext *dbg);
		IOStatus Close(const IOOptions &options, IODebugContext *dbg);
		IOStatus Flush(const IOOptions &options, IODebugContext *dbg);
		IOStatus Sync(const IOOptions &options, IODebugContext *dbg);
		void DeleteCurrentNode();

		std::string path;

	private:
		RichInode root_inode, *current_inode;
		std::vector<uint8_t> buffer;
		S2FileSystem *fs;
		struct user_zns_device *zns_dev;
		bool past_root = false;
		bool has_read_last_block = false;
		bool preallocate = true;
		uint64_t disk_ptr_idx, num_disk_ptrs;
	};

	class RocksRandomAccessFile : public FSRandomAccessFile {
	public:
		RocksRandomAccessFile(const INode inode, const struct user_zns_device *zns_dev, S2FileSystem *fs,
							  IOStatus &init_status);
		IOStatus Read(uint64_t offset, size_t n, const IOOptions &options, Slice *result, char *scratch,
					  IODebugContext *dbg) const;

	private:
		std::vector<uint64_t> disk_pointers;
		INode root_inode;
		S2FileSystem *fs;
		const struct user_zns_device *zns_dev;
	};
} // namespace ROCKSDB_NAMESPACE

#endif