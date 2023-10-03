#include "RocksClasses.h"

namespace ROCKSDB_NAMESPACE {
	RocksSequentialFile::RocksSequentialFile(const INode inode, const struct user_zns_device *zns_dev, S2FileSystem *fs)
		: inode(inode), fs(fs), zns_dev(zns_dev) {}

	IOStatus RocksSequentialFile::Read(size_t n, const IOOptions &options, Slice *result, char *scratch,
									   IODebugContext *dbg) {
		UNUSED(options);
		UNUSED(dbg);
		uint64_t left = n;
		while (left > 0) {
			if (ptr > inode.size) {
				*result = Slice(scratch, 0);
				return IOStatus::OK();
			}

			if (ptr == 0) {
				curr_inode = inode;
			}

			if ((!curr_inode.is_nested && disk_ptr == NUM_DISK_POINTERS) ||
				(curr_inode.is_nested && disk_ptr == NUM_NESTED_DISK_POINTERS)) {
				disk_ptr = 0;
				if (!curr_inode.use_next_pointer) {
					return IOStatus::Corruption();
				}

				auto ret = fs->ReadINode(curr_inode.next_pointer, curr_inode);
				if (!ret.ok()) {
					return ret;
				}
			}

			auto ret = fs->ReadBlock(curr_inode.disk_pointers[disk_ptr], (uint8_t*)scratch + n - left);
			if (!ret.ok()) {
				return ret;
			}
			const auto op_size = std::min(zns_dev->lba_size_bytes, (uint32_t)left);
			ptr += op_size;
			disk_ptr++;
			left -= op_size;
		}
		*result = Slice(scratch, n - left);
		return IOStatus::OK();
	}

	IOStatus RocksSequentialFile::Skip(uint64_t n) {
		ptr += n;
		return IOStatus::OK();
	}

	RocksWriteableFile::RocksWriteableFile(RichInode root_inode, S2FileSystem *fs, struct user_zns_device *zns_dev)
		: root_inode(root_inode), fs(fs), zns_dev(zns_dev) {
		current_inode = &this->root_inode;
		buffer.resize(zns_dev->lba_size_bytes);
		current_inode_address = (uint64_t)&root_inode;
	}

	RocksWriteableFile::~RocksWriteableFile() {
		if (current_inode_address != (uint64_t)current_inode) {
			delete current_inode;
		}
		Close(IOOptions(), nullptr);
	}

	IOStatus RocksWriteableFile::Append(const Slice &data, const IOOptions &options, IODebugContext *dbg) {
		UNUSED(options);
		UNUSED(dbg);
		uint64_t left = data.size();
		while (left > 0) {
			const auto last_block_leftover = root_inode.inode.size % zns_dev->lba_size_bytes;
			const auto offset = zns_dev->lba_size_bytes - last_block_leftover;
			uint64_t op_size = std::min(std::min(offset, zns_dev->lba_size_bytes), (uint32_t)left);

			if (last_block_leftover == 0) {
				fs->AddDiskPointer(&current_inode);
			}

			auto current_disk_ptr = current_inode->inode.disk_pointers[current_inode->inode.num_disk_pointers - 1];
			if (last_block_leftover != 0) {
				auto ret = fs->ReadBlock(current_disk_ptr, &buffer[0]);
				if (!ret.ok()) {
					return ret;
				}

				memcpy(&buffer[offset], data.data() + data.size() - left, op_size);
				ret = fs->WriteBlock(current_disk_ptr, &buffer[0]);
				if (!ret.ok()) {
					return ret;
				}
			} else {
				uint8_t *buffer_to_use;
				if (op_size < zns_dev->lba_size_bytes) {
					memcpy(&buffer[0], (uint8_t *)data.data() + data.size() - left, op_size);
					buffer_to_use = &buffer[0];
				} else {
					buffer_to_use = (uint8_t *)data.data() + data.size() - left;
				}
				auto ret = fs->WriteBlock(current_disk_ptr, buffer_to_use);
				if (!ret.ok()) {
					return ret;
				}
			}
			left -= op_size;
			root_inode.inode.size += op_size;
		}
		return IOStatus::OK();
	}

	IOStatus RocksWriteableFile::Flush(const IOOptions &options, IODebugContext *dbg) {
		return fs->WriteINode(root_inode.inode, root_inode.ptr, false);
	}
	IOStatus RocksWriteableFile::Close(const IOOptions &options, IODebugContext *dbg) { return Flush(options, dbg); }
	IOStatus RocksWriteableFile::Sync(const IOOptions &options, IODebugContext *dbg) { return Flush(options, dbg); }
} // namespace ROCKSDB_NAMESPACE