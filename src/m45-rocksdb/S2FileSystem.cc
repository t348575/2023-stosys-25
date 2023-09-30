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

#include "S2FileSystem.h"
#include <iostream>
#include <string>
#include <sys/mman.h>

#include <stosys_debug.h>
#include <utils.h>

namespace ROCKSDB_NAMESPACE {
	SuperBlock::SuperBlock(const std::vector<char> &buffer) {
		block_map_size = u32_from_buffer(&buffer[0]);
		original_inode_reserved = u16_from_buffer(&buffer[4]);
	}

	void SuperBlock::serialize(std::vector<char> &output) {
		u32_to_buffer(block_map_size, &output[0]);
		u16_to_buffer(original_inode_reserved, &output[4]);
	}

	INode::INode(const std::vector<char> &buffer) {
		is_nested = (bool)buffer[0];
		inode_type = (Type)buffer[1];
		num_disk_pointers = u32_from_buffer(&buffer[2]);
		if (!is_nested) {
			mode = u16_from_buffer(&buffer[6]);
			uid = u16_from_buffer(&buffer[8]);
			gid = u16_from_buffer(&buffer[10]);
			links_count = u16_from_buffer(&buffer[12]);

			size = u32_from_buffer(&buffer[14]);
			time = u32_from_buffer(&buffer[18]);
			ctime = u32_from_buffer(&buffer[22]);
			mtime = u32_from_buffer(&buffer[26]);
			dtime = u32_from_buffer(&buffer[30]);
			blocks = u32_from_buffer(&buffer[34]);
			flags = u32_from_buffer(&buffer[38]);
			osd1 = u32_from_buffer(&buffer[42]);
			generation = u32_from_buffer(&buffer[46]);
			file_acl = u32_from_buffer(&buffer[50]);
			dir_acl = u32_from_buffer(&buffer[54]);
		}

		uint32_t pos = 0;
		uint64_t const read_upto = is_nested ? 25 : 13;
		for (uint64_t i = 58; pos < read_upto; i += 40, pos++) {
			disk_pointers[pos] = {.block = u32_from_buffer(&buffer[i]), .offset = (uint8_t)buffer[i + 4]};
		}

		next_pointer = {.block = u32_from_buffer(&buffer[153]), .offset = (uint8_t)buffer[157]};
	}

	void INode::serialize(std::vector<char> &output) const {
		output[0] = is_nested;
		output[1] = inode_type;
		u32_to_buffer(num_disk_pointers, &output[2]);

		if (!is_nested) {
			u16_to_buffer(mode, &output[6]);
			u16_to_buffer(uid, &output[8]);
			u16_to_buffer(gid, &output[10]);
			u16_to_buffer(links_count, &output[12]);

			u32_to_buffer(size, &output[14]);
			u32_to_buffer(time, &output[18]);
			u32_to_buffer(ctime, &output[22]);
			u32_to_buffer(mtime, &output[26]);
			u32_to_buffer(dtime, &output[30]);
			u32_to_buffer(blocks, &output[34]);
			u32_to_buffer(flags, &output[38]);
			u32_to_buffer(osd1, &output[42]);
			u32_to_buffer(generation, &output[46]);
			u32_to_buffer(file_acl, &output[50]);
			u32_to_buffer(dir_acl, &output[54]);
		}

		uint32_t pos = 0;
		uint64_t const read_upto = is_nested ? 25 : 13;
		for (uint64_t i = 58; pos < read_upto; i += 40, pos++) {
			u32_to_buffer(disk_pointers[pos].block, &output[i]);
			output[i + 4] = disk_pointers[pos].offset;
		}

		u32_to_buffer(next_pointer.block, &output[153]);
		u32_to_buffer(next_pointer.offset, &output[157]);
	}

	S2FileSystem::S2FileSystem(std::string uri_db_path, bool debug) {
		FileSystem::Default();
		std::string sdelimiter = ":";
		std::string edelimiter = "://";
		this->_uri = uri_db_path;
		struct zdev_init_params params;
		std::string device =
				uri_db_path.substr(uri_db_path.find(sdelimiter) + sdelimiter.size(),
								   uri_db_path.find(edelimiter) - (uri_db_path.find(sdelimiter) + sdelimiter.size()));
		// make sure to setup these parameters properly and check the forced reset flag for M5
		params.name = strdup(device.c_str());
		params.log_zones = 3;
		params.gc_wmark = 1;
		params.force_reset = false;
		int ret = init_ss_zns_device(&params, &this->zns_dev);
		if (ret != 0) {
			std::cout << "Error: " << uri_db_path << " failed to open the device " << device.c_str() << "\n";
			std::cout << "Error: ret " << ret << "\n";
		}
		assert(ret == 0);
		assert(this->zns_dev->lba_size_bytes != 0);
		assert(this->zns_dev->capacity_bytes != 0);
		ss_dprintf(DBG_FS_1, "device %s is opened and initialized, reported LBA size is %u and capacity %lu \n",
				   device.c_str(), this->zns_dev->lba_size_bytes, this->zns_dev->capacity_bytes);

		// ftl device initialization
		this->device->init(zns_dev, (device_data *)zns_dev->_private);

		inodes_per_block = (zns_dev->lba_size_bytes / 158) - 1;

		// setup super block
		super_block.block_map_size = zns_dev->lba_size_bytes * 2;
		super_block.original_inode_reserved = 16;
		FlushSuperBlock();
		block_map_offset = 3;

		// setup inode & data block map
		this->block_map.resize(super_block.block_map_size);
		SetBlockType(0, BlockType::Inode); // root directory pointer
		FlushBlockMap();

		// setup root directory
		INode root;
		root.is_nested = false;
		root.inode_type = INode::Type::Directory;
		root.num_disk_pointers = 1;
		root.disk_pointers[0].block = super_block.original_inode_reserved;
		root.disk_pointers[0].offset = super_block.original_inode_reserved;

		inodes.reserve(inodes_per_block * super_block.original_inode_reserved);
		inodes.push_back({.inode = root, .block = 0});
		inode_super_blocks.reserve(16 * inodes_per_block);
		inode_super_blocks.push_back({.block = 0, .num_inodes = 1});

		FlushInodeBlock(0);
	}

	void S2FileSystem::SetBlockType(const uint16_t num, const BlockType block_type) {
		auto const base_index = (num - 1) / 4;
		auto const bit_index = ((num - 1) % 4) * 2;
		block_map[base_index] &= BLOCK_BIT_MASKS[bit_index] & block_type;
	}

	int S2FileSystem::FlushBlockMap() {
		return zns_udevice_write(zns_dev, zns_dev->lba_size_bytes, &block_map[0], super_block.block_map_size);
	}

	int S2FileSystem::FlushSuperBlock() {
		std::vector<char> output;
		output.resize(zns_dev->lba_size_bytes);
		super_block.serialize(output);
		return zns_udevice_write(zns_dev, 0, &output[0], zns_dev->lba_size_bytes);
	}

	int S2FileSystem::FlushInodeBlock(const uint32_t block) {
		std::vector<char> output;
		output.reserve(zns_dev->lba_size_bytes);

		bool found = false;
		uint32_t num_inodes = 0;
		for (const auto &t : inode_super_blocks) {
			if (t.block == block) {
				found = true;
				num_inodes = t.num_inodes;
				break;
			}
		}

		if (!found) {
			return FsError::CouldNotFindInode;
		}
		output.push_back(num_inodes);

		for (const auto &t : inodes) {
			if (t.block == block) {
				t.inode.serialize(output);
			}
		}

		output.resize(zns_dev->lba_size_bytes);
		return zns_udevice_write(zns_dev, (block_map_offset + block) * zns_dev->lba_size_bytes, &output[0],
								 zns_dev->lba_size_bytes);
	}

	S2FileSystem::~S2FileSystem() {}

	// Create a brand new sequentially-readable file with the specified name.
	// On success, stores a pointer to the new file in *result and returns OK.
	// On failure stores nullptr in *result and returns non-OK.  If the file does
	// not exist, returns a non-OK status.
	//
	// The returned file will only be accessed by one thread at a time.
	IOStatus S2FileSystem::NewSequentialFile(const std::string &fname, const FileOptions &file_opts,
											 std::unique_ptr<FSSequentialFile> *result,
											 __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::IsDirectory(const std::string &, const IOOptions &options, bool *is_dir, IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Create a brand new random access read-only file with the
	// specified name.  On success, stores a pointer to the new file in
	// *result and returns OK.  On failure stores nullptr in *result and
	// returns non-OK.  If the file does not exist, returns a non-OK
	// status.
	//
	// The returned file may be concurrently accessed by multiple threads.
	IOStatus S2FileSystem::NewRandomAccessFile(const std::string &fname, const FileOptions &file_opts,
											   std::unique_ptr<FSRandomAccessFile> *result,
											   __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	const char *S2FileSystem::Name() const { return "S2FileSytem"; }

	// Create an object that writes to a new file with the specified
	// name.  Deletes any existing file with the same name and creates a
	// new file.  On success, stores a pointer to the new file in
	// *result and returns OK.  On failure stores nullptr in *result and
	// returns non-OK.
	//
	// The returned file will only be accessed by one thread at a time.
	IOStatus S2FileSystem::NewWritableFile(const std::string &fname, const FileOptions &file_opts,
										   std::unique_ptr<FSWritableFile> *result,
										   __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::ReopenWritableFile(const std::string &, const FileOptions &,
											  std::unique_ptr<FSWritableFile> *, IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::NewRandomRWFile(const std::string &, const FileOptions &, std::unique_ptr<FSRandomRWFile> *,
										   IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::NewMemoryMappedFileBuffer(const std::string &, std::unique_ptr<MemoryMappedFileBuffer> *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Create an object that represents a directory. Will fail if directory
	// doesn't exist. If the directory exists, it will open the directory
	// and create a new Directory object.
	//
	// On success, stores a pointer to the new Directory in
	// *result and returns OK. On failure stores nullptr in *result and
	// returns non-OK.
	IOStatus S2FileSystem::NewDirectory(const std::string &name, const IOOptions &io_opts,
										std::unique_ptr<FSDirectory> *result,
										__attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetFreeSpace(const std::string &, const IOOptions &, uint64_t *, IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::Truncate(const std::string &, size_t, const IOOptions &, IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Create the specified directory. Returns error if directory exists.
	IOStatus S2FileSystem::CreateDir(const std::string &dirname, const IOOptions &options,
									 __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Creates directory if missing. Return Ok if it exists, or successful in
	// Creating.
	IOStatus S2FileSystem::CreateDirIfMissing(const std::string &dirname, const IOOptions &options,
											  __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetFileSize(const std::string &fname, const IOOptions &options, uint64_t *file_size,
									   __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::DeleteDir(const std::string &dirname, const IOOptions &options,
									 __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetFileModificationTime(const std::string &fname, const IOOptions &options,
												   uint64_t *file_mtime, __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetAbsolutePath(const std::string &db_path, const IOOptions &options,
										   std::string *output_path, __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::DeleteFile(const std::string &fname, const IOOptions &options,
									  __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::NewLogger(const std::string &fname, const IOOptions &io_opts,
									 std::shared_ptr<Logger> *result, __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetTestDirectory(const IOOptions &options, std::string *path,
											__attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Release the lock acquired by a previous successful call to LockFile.
	// REQUIRES: lock was returned by a successful LockFile() call
	// REQUIRES: lock has not already been unlocked.
	IOStatus S2FileSystem::UnlockFile(FileLock *lock, const IOOptions &options,
									  __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Lock the specified file.  Used to prevent concurrent access to
	// the same db by multiple processes.  On failure, stores nullptr in
	// *lock and returns non-OK.
	//
	// On success, stores a pointer to the object that represents the
	// acquired lock in *lock and returns OK.  The caller should call
	// UnlockFile(*lock) to release the lock.  If the process exits,
	// the lock will be automatically released.
	//
	// If somebody else already holds the lock, finishes immediately
	// with a failure.  I.e., this call does not wait for existing locks
	// to go away.
	//
	// May create the named file if it does not already exist.
	IOStatus S2FileSystem::LockFile(const std::string &fname, const IOOptions &options, FileLock **lock,
									__attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::AreFilesSame(const std::string &, const std::string &, const IOOptions &, bool *,
										IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::NumFileLinks(const std::string &, const IOOptions &, uint64_t *, IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::LinkFile(const std::string &, const std::string &, const IOOptions &, IODebugContext *) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::RenameFile(const std::string &src, const std::string &target, const IOOptions &options,
									  __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetChildrenFileAttributes(const std::string &dir, const IOOptions &options,
													 std::vector<FileAttributes> *result,
													 __attribute__((unused)) IODebugContext *dbg) {
		return FileSystem::GetChildrenFileAttributes(dir, options, result, dbg);
	}

	// Store in *result the names of the children of the specified directory.
	// The names are relative to "dir".
	// Original contents of *results are dropped.
	// Returns OK if "dir" exists and "*result" contains its children.
	//         NotFound if "dir" does not exist, the calling process does not have
	//                  permission to access "dir", or if "dir" is invalid.
	//         IOError if an IO Error was encountered
	IOStatus S2FileSystem::GetChildren(const std::string &dir, const IOOptions &options,
									   std::vector<std::string> *result, __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	// Returns OK if the named file exists.
	//         NotFound if the named file does not exist,
	//                  the calling process does not have permission to determine
	//                  whether this file exists, or if the path is invalid.
	//         IOError if an IO Error was encountered
	IOStatus S2FileSystem::FileExists(const std::string &fname, const IOOptions &options,
									  __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::ReuseWritableFile(const std::string &fname, const std::string &old_fname,
											 const FileOptions &file_opts, std::unique_ptr<FSWritableFile> *result,
											 __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}
} // namespace ROCKSDB_NAMESPACE