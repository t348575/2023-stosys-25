#pragma once

#ifndef FS_IMPL_H
#define FS_IMPL_H

#include <rocksdb/file_system.h>

#include "S2FileSystem.h"

namespace ROCKSDB_NAMESPACE {
	class SeqFile : public FSSequentialFile {
	public:
		SeqFile(const INode inode, struct user_zns_device *zns_dev);
		~SeqFile() = default;

		IOStatus Read(size_t n, const IOOptions &options, Slice *result, char *scratch, IODebugContext *dbg);

		IOStatus Skip(uint64_t n);

	private:
		INode inode;
		INode curr_inode;
		uint32_t ptr = 0;
		uint32_t disk_ptr = 0;
		std::vector<char> buffer;
		struct user_zns_device *zns_dev;
	};
} // namespace ROCKSDB_NAMESPACE

#endif