#include "RocksClasses.h"

#include <VisualProfiler.hpp>
#include <utils.h>

namespace ROCKSDB_NAMESPACE {
	RocksSequentialFile::RocksSequentialFile(const INode root_inode, const struct user_zns_device *zns_dev,
											 S2FileSystem *fs)
		: root_inode(root_inode), fs(fs), zns_dev(zns_dev) {
		buffer.resize(zns_dev->lba_size_bytes);
	}

	IOStatus RocksSequentialFile::Read(size_t n, const IOOptions &options, Slice *result, char *scratch,
									   IODebugContext *dbg) {
		UNUSED(options);
		UNUSED(dbg);
		PROFILE_SCOPE("fs", "RocksSequentialFile::Read");
		uint64_t left = n;
		while (left > 0) {
			if (ptr == root_inode.size) {
				*result = Slice(scratch, n - left);
				ptr++;
				return IOStatus::OK();
			}

			if (ptr > root_inode.size) {
				*result = Slice(scratch, 0);
				return IOStatus::OK();
			}

			if (ptr == 0) {
				curr_inode = root_inode;
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

			auto ret = fs->ReadBlock(curr_inode.disk_pointers[disk_ptr], &buffer[0]);
			if (!ret.ok()) {
				return ret;
			}

			const auto op_size = std::min(std::min((uint64_t)zns_dev->lba_size_bytes, left), root_inode.size - ptr);
			memcpy((uint8_t *)scratch + n - left, &buffer[0], op_size);
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

	RocksWriteableFile::RocksWriteableFile(RichInode root_inode, S2FileSystem *fs, struct user_zns_device *zns_dev, bool preallocate)
		: root_inode(root_inode), current_inode(&this->root_inode), fs(fs), zns_dev(zns_dev), preallocate(preallocate) {
		buffer.resize(zns_dev->lba_size_bytes);
		disk_ptr_idx = 0;
		num_disk_ptrs = current_inode->inode.num_disk_pointers;
	}

	RocksWriteableFile::~RocksWriteableFile() {
		Close(IOOptions(), nullptr);
		if (past_root) {
			delete current_inode;
		}
	}

	IOStatus RocksWriteableFile::Append(const Slice &data, const IOOptions &options, IODebugContext *dbg) {
		UNUSED(options);
		UNUSED(dbg);
		PROFILE_SCOPE("fs", "RocksWriteableFile::Append");
		uint64_t left = data.size();
		while (left > 0) {
			const uint64_t last_block_leftover = root_inode.inode.size % zns_dev->lba_size_bytes;
			uint64_t op_size = std::min((uint64_t)zns_dev->lba_size_bytes, left);

			if (last_block_leftover != 0) {
				if (last_block_leftover + op_size > zns_dev->lba_size_bytes) {
					op_size = zns_dev->lba_size_bytes - last_block_leftover;
				}
			}

			if (num_disk_ptrs == 0 || ((root_inode.inode.size + op_size > num_disk_ptrs * zns_dev->lba_size_bytes) && (!preallocate || (!current_inode->inode.is_nested && disk_ptr_idx + 1 == NUM_DISK_POINTERS) || (current_inode->inode.is_nested && disk_ptr_idx + 1 == NUM_NESTED_DISK_POINTERS)))) {
				RichInode *tmp = nullptr;
				fs->AddDiskPointer(preallocate, current_inode, &tmp, disk_ptr_idx, num_disk_ptrs);
				if (tmp != nullptr) {
					if (past_root) {
						delete current_inode;
					}
					past_root = true;
					current_inode = tmp;
				}
			} else if (last_block_leftover == 0) {
				disk_ptr_idx++;
			}

			auto current_disk_ptr = current_inode->inode.disk_pointers[disk_ptr_idx];
			if (last_block_leftover != 0) {
				if (!has_read_last_block) {
					has_read_last_block = true;
					auto ret = fs->ReadBlock(current_disk_ptr, &buffer[0]);
					if (!ret.ok()) {
						return ret;
					}
				}

				memcpy(&buffer[last_block_leftover], data.data() + data.size() - left, op_size);
				auto ret = fs->WriteBlock(current_disk_ptr, &buffer[0]);
				if (!ret.ok()) {
					return ret;
				}
			} else {
				has_read_last_block = false;
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
		PROFILE_SCOPE("fs", "RocksWriteableFile::Flush");
		return fs->WriteINode(root_inode);
	}
	IOStatus RocksWriteableFile::Close(const IOOptions &options, IODebugContext *dbg) { return Flush(options, dbg); }
	IOStatus RocksWriteableFile::Sync(const IOOptions &options, IODebugContext *dbg) { return Flush(options, dbg); }

	RocksRandomAccessFile::RocksRandomAccessFile(const INode root_inode, const struct user_zns_device *zns_dev,
												 S2FileSystem *fs, IOStatus &init_status)
		: root_inode(root_inode), fs(fs), zns_dev(zns_dev) {
		PROFILE_SCOPE("fs", "RocksRandomAccessFile::RocksRandomAccessFile");
		init_status = IOStatus::OK();
		RichInode current_ri;
		current_ri.inode = root_inode;
		const uint64_t total_disk_pointers = closest_upper_multiple(zns_dev->lba_size_bytes, root_inode.size) / zns_dev->lba_size_bytes;
		uint64_t passed_disk_pointers = 0;
		while (true) {
			const uint64_t to_use = std::min(std::min(passed_disk_pointers + current_ri.inode.num_disk_pointers, total_disk_pointers), passed_disk_pointers + (current_ri.inode.is_nested ? NUM_NESTED_DISK_POINTERS : NUM_DISK_POINTERS)) - passed_disk_pointers;
			disk_pointers.insert(disk_pointers.end(), current_ri.inode.disk_pointers,
								 current_ri.inode.disk_pointers + to_use);

			passed_disk_pointers += to_use;
			if (to_use != current_ri.inode.num_disk_pointers) {
				break;
			}

			if (!current_ri.inode.use_next_pointer) {
				break;
			}

			init_status = fs->ReadINode(current_ri.inode.next_pointer, current_ri.inode);
			if (!init_status.ok()) {
				break;
			}
		}
	}

	IOStatus RocksRandomAccessFile::Read(uint64_t offset, size_t n, const IOOptions &options, Slice *result,
										 char *scratch, IODebugContext *dbg) const {
		UNUSED(options);
		UNUSED(dbg);
		PROFILE_SCOPE("fs", "RocksRandomAccessFile::Read");
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);

		uint64_t output_ptr = 0, size_left = n;
		uint64_t op_size;
		if (offset + n > root_inode.size) {
			op_size = root_inode.size - offset;
		} else {
			op_size = n;
		}

		auto const start_block = closest_lower_multiple(zns_dev->lba_size_bytes, offset) / zns_dev->lba_size_bytes;
		auto const end_block =
				closest_upper_multiple(zns_dev->lba_size_bytes, offset + op_size) / zns_dev->lba_size_bytes;

		for (uint64_t i = start_block; i < end_block; i++) {
			auto ret = fs->ReadBlock(disk_pointers[i], &buffer[0]);
			if (!ret.ok()) {
				return ret;
			}

			uint64_t copy_from = 0;
			uint64_t copy_size = std::min((uint64_t)zns_dev->lba_size_bytes, size_left);
			if (i == start_block) {
				copy_from = offset - closest_lower_multiple(zns_dev->lba_size_bytes, offset);
				copy_size = std::min(zns_dev->lba_size_bytes - copy_from, size_left);
			} else if (i == end_block) {
				copy_size = std::min((offset + n) % zns_dev->lba_size_bytes, size_left);
			}

			memcpy(scratch + output_ptr, &buffer[copy_from], copy_size);
			output_ptr += copy_size;
			size_left -= copy_size;
		}

		*result = Slice(scratch, op_size);
		return IOStatus::OK();
	}
} // namespace ROCKSDB_NAMESPACE
