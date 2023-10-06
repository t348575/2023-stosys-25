#include "RocksClasses.h"

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
		// std::cout << "Reading " << std::to_string(n) << " bytes from " << path << std::endl;
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

			const auto op_size = std::min(std::min(zns_dev->lba_size_bytes, (uint32_t)left), root_inode.size);
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

	RocksWriteableFile::RocksWriteableFile(RichInode root_inode, S2FileSystem *fs, struct user_zns_device *zns_dev)
		: root_inode(root_inode), current_inode(&this->root_inode), fs(fs), zns_dev(zns_dev) {
		buffer.resize(zns_dev->lba_size_bytes);
	}

	RocksWriteableFile::~RocksWriteableFile() {
		if (past_root) {
			delete current_inode;
		}
		Close(IOOptions(), nullptr);
	}

	IOStatus RocksWriteableFile::Append(const Slice &data, const IOOptions &options, IODebugContext *dbg) {
		UNUSED(options);
		UNUSED(dbg);
		uint64_t left = data.size();
		// std::cout << "Writing " << std::to_string(data.size()) << " bytes from " << path << std::endl;
		while (left > 0) {
			const auto last_block_leftover = root_inode.inode.size % zns_dev->lba_size_bytes;
			const auto offset = zns_dev->lba_size_bytes - last_block_leftover;
			uint64_t op_size = std::min(std::min(offset, zns_dev->lba_size_bytes), (uint32_t)left);

			if (last_block_leftover == 0 || current_inode->inode.num_disk_pointers == 0) {
				RichInode *tmp = nullptr;
				fs->AddDiskPointer(current_inode, &tmp);
				if (tmp != nullptr) {
					if (past_root) {
						delete current_inode;
					}
					past_root = true;
					current_inode = tmp;
				}
			}

			auto current_disk_ptr = current_inode->inode.disk_pointers[current_inode->inode.num_disk_pointers - 1];
			if (last_block_leftover != 0) {
				auto ret = fs->ReadBlock(current_disk_ptr, &buffer[0]);
				if (!ret.ok()) {
					return ret;
				}

				if (offset + op_size > zns_dev->lba_size_bytes) {
					op_size = zns_dev->lba_size_bytes - offset;
				}

				// std::cout << offset << " " << data.size() 	<< " " << data.size() - left << " " << op_size << " " <<
				// left << " " << last_block_leftover << std::endl;
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
		// std::cout << "Done writing " << std::to_string(data.size()) << " bytes from " << path << std::endl;
		return IOStatus::OK();
	}

	IOStatus RocksWriteableFile::Flush(const IOOptions &options, IODebugContext *dbg) {
		return fs->WriteINode({root_inode.inode, root_inode.ptr}, false);
	}
	IOStatus RocksWriteableFile::Close(const IOOptions &options, IODebugContext *dbg) { return Flush(options, dbg); }
	IOStatus RocksWriteableFile::Sync(const IOOptions &options, IODebugContext *dbg) { return Flush(options, dbg); }

	RocksRandomAccessFile::RocksRandomAccessFile(const INode root_inode, const struct user_zns_device *zns_dev,
												 S2FileSystem *fs, IOStatus &init_status)
		: root_inode(root_inode), fs(fs), zns_dev(zns_dev) {
		init_status = IOStatus::OK();
		RichInode current_ri;
		current_ri.inode = root_inode;
		while (true) {
			disk_pointers.insert(disk_pointers.end(), current_ri.inode.disk_pointers,
								 current_ri.inode.disk_pointers + current_ri.inode.num_disk_pointers);
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
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);

		uint32_t output_ptr = 0, size_left = n;
		auto const start_block = closest_lower_multiple(zns_dev->lba_size_bytes, offset) / zns_dev->lba_size_bytes;
		auto const end_block = closest_upper_multiple(zns_dev->lba_size_bytes, offset + n) / zns_dev->lba_size_bytes;
		uint32_t op_size;
		if (offset + n > root_inode.size) {
			op_size = root_inode.size - offset;
		} else {
			op_size = n;
		}

		for (uint32_t i = start_block; i < end_block; i++) {
			auto ret = fs->ReadBlock(disk_pointers[i], &buffer[0]);
			if (!ret.ok()) {
				return ret;
			}

			uint32_t copy_from = 0;
			uint32_t copy_size = std::min(zns_dev->lba_size_bytes, size_left);
			if (i == start_block) {
				copy_from = offset - closest_lower_multiple(zns_dev->lba_size_bytes, offset);
			} else if (i == end_block) {
				copy_from = (offset + n) - closest_lower_multiple(zns_dev->lba_size_bytes, offset + n);
			}

			memcpy(scratch + output_ptr, &buffer[copy_from], copy_size);
			output_ptr += copy_size;
			size_left -= copy_size;
		}

		*result = Slice(scratch, op_size);
		return IOStatus::OK();
	}
} // namespace ROCKSDB_NAMESPACE