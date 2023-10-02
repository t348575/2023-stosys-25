#include "RocksClasses.h"

namespace ROCKSDB_NAMESPACE {
	SeqFile::SeqFile(const INode inode, struct user_zns_device *zns_dev) : inode(inode), zns_dev(zns_dev) {
		buffer.resize(zns_dev->lba_size_bytes);
	}

	IOStatus SeqFile::Read(size_t n, const IOOptions &options, Slice *result, char *scratch, IODebugContext *dbg) {
		UNUSED(options);
		UNUSED(dbg);
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
			int ret = zns_udevice_read(zns_dev, curr_inode.next_pointer.block * zns_dev->lba_size_bytes, &buffer[0],
									   zns_dev->lba_size_bytes);
			if (ret != 0) {
				return IOStatus::IOError();
			}

			curr_inode = INode((uint8_t *)&buffer[(curr_inode.next_pointer.offset * INODE_SIZE) + 1]);
		}

		const int ret = zns_udevice_read(zns_dev, curr_inode.disk_pointers[disk_ptr] * zns_dev->lba_size_bytes, scratch,
										 zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		const auto op_size = std::min(zns_dev->lba_size_bytes, (uint32_t)n - ptr);
		*result = Slice(scratch, op_size);
		ptr += op_size;
		disk_ptr++;
		return IOStatus::OK();
	}

	IOStatus SeqFile::Skip(uint64_t n) {
		ptr += n;
		return IOStatus::OK();
	}
} // namespace ROCKSDB_NAMESPACE