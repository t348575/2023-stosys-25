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
#include <stack>
#include <string>
#include <sys/mman.h>

#include "RocksClasses.h"
#include <VisualProfiler.hpp>
#include <stosys_debug.h>
#include <utils.h>

#define PROFILE_FS_FUNCTION() PROFILE_FUNCTION_CATEGORY("fs")

namespace ROCKSDB_NAMESPACE {
	SuperBlock::SuperBlock(const std::vector<uint8_t> &buffer) {
		PROFILE_FS_FUNCTION();
		block_map_size = u64_from_buffer(&buffer[0]);
		original_inode_reserved = u16_from_buffer(&buffer[8]);
	}

	void SuperBlock::serialize(std::vector<uint8_t> &output) {
		PROFILE_FS_FUNCTION();
		u64_to_buffer(block_map_size, &output[0]);
		u16_to_buffer(original_inode_reserved, &output[8]);
	}

	INode::INode(const uint8_t *buffer) {
		PROFILE_FS_FUNCTION();
		is_nested = (bool)buffer[0];
		inode_type = (S2EntryType)buffer[1];
		num_disk_pointers = u64_from_buffer(&buffer[2]);
		if (!is_nested) {

			size = u64_from_buffer(&buffer[10]);
		}

		uint64_t pos = 0;
		uint64_t const read_upto = is_nested ? NUM_NESTED_DISK_POINTERS : NUM_DISK_POINTERS;
		uint64_t const from = is_nested ? 10 : 18;
		for (uint64_t i = from; pos < read_upto; i += 8, pos++) {
			disk_pointers[pos] = u64_from_buffer(&buffer[i]);
		}

		next_pointer = {.block = u64_from_buffer(&buffer[122]), .offset = (uint8_t)buffer[130]};
		use_next_pointer = buffer[131];
	}

	void INode::serialize(uint8_t *output) const {
		PROFILE_FS_FUNCTION();
		output[0] = is_nested;
		output[1] = inode_type;
		u64_to_buffer(num_disk_pointers, &output[2]);

		if (!is_nested) {

			u64_to_buffer(size, &output[10]);
		}

		uint64_t pos = 0;
		uint64_t const read_upto = is_nested ? NUM_NESTED_DISK_POINTERS : NUM_DISK_POINTERS;
		uint64_t const from = is_nested ? 10 : 18;
		for (uint64_t i = from; pos < read_upto; i += 8, pos++) {
			u64_to_buffer(disk_pointers[pos], &output[i]);
		}

		u64_to_buffer(next_pointer.block, &output[122]);
		output[130] = next_pointer.offset;
		output[131] = use_next_pointer;
	}

	DirectoryEntry::DirectoryEntry(const uint8_t *buffer) {
		PROFILE_FS_FUNCTION();
		block = u64_from_buffer(&buffer[0]);
		offset = buffer[8];
		type = (S2EntryType)buffer[9];
		name_len = u64_from_buffer(&buffer[10]);
		name.assign((char *)&buffer[DIR_BLOCK_PLAIN_SIZE], name_len);
	}

	void DirectoryEntry::serialize(uint8_t *output) const {
		PROFILE_FS_FUNCTION();
		u64_to_buffer(block, &output[0]);
		output[8] = offset;
		output[9] = (S2EntryType)type;
		u64_to_buffer(name_len, &output[10]);
		memcpy(&output[DIR_BLOCK_PLAIN_SIZE], name.c_str(), name_len);
	}

	S2FileSystem::S2FileSystem(std::string uri_db_path, bool debug) {
		PROFILE_FS_FUNCTION();
		UNUSED(debug);
		std::string sdelimiter = ":";
		std::string edelimiter = "://";
		this->_uri = uri_db_path;
		struct zdev_init_params params;
		std::string device =
				uri_db_path.substr(uri_db_path.find(sdelimiter) + sdelimiter.size(),
								   uri_db_path.find(edelimiter) - (uri_db_path.find(sdelimiter) + sdelimiter.size()));
		// make sure to setup these parameters properly and check the forced reset flag for M5
		params.name = (char *)device.c_str();
		params.log_zones = 4;
		params.gc_wmark = 0;
		params.force_reset = true;
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

		inodes_per_block = (zns_dev->lba_size_bytes / INODE_SIZE) - 1;
		block_map_size = ((zns_dev->capacity_bytes / zns_dev->lba_size_bytes) / 4);		

		if (is_fresh_start()) {
			// setup super block
			auto const blocks_needed_for_bitmap = closest_upper_multiple(zns_dev->lba_size_bytes, (zns_dev->capacity_bytes / zns_dev->lba_size_bytes) / 4) / zns_dev->lba_size_bytes;
			block_map_offset = closest_upper_multiple(4, blocks_needed_for_bitmap);
			super_block.block_map_size = zns_dev->lba_size_bytes * blocks_needed_for_bitmap;
			super_block.original_inode_reserved = 16 + block_map_offset;
			WriteSuperBlock();

			// setup inode & data block map
			block_map.resize(super_block.block_map_size);
			SetBlockType(0, BlockType::Inode); // root directory pointer
			WriteBlockMap();

			// setup root directory inode
			INode root;
			root.is_nested = false;
			root.inode_type = S2EntryType::Directory;
			root.num_disk_pointers = 1;
			root.size = DIR_SUPER_BLOCK_PLAIN_SIZE;
			root.disk_pointers[0] = super_block.original_inode_reserved;
			SetBlockType(super_block.original_inode_reserved, BlockType::Data);
			WriteBlockMap();

			// setup super inode block
			std::vector<uint8_t> output;
			output.resize(zns_dev->lba_size_bytes);
			for (uint8_t i = 0; i < inodes_per_block; i++) {
				output[i] = false;
			}
			output[0] = true;
			root.serialize(&output[inodes_per_block]);
			WriteBlock(0, &output[0]);

			// write directory to data block
			std::vector<uint8_t> dir_buffer;
			dir_buffer.resize(zns_dev->lba_size_bytes);

			DirectorySuperBlock dir_sb;
			dir_sb.name_len = 0;
			dir_sb.num_entries = 0;
			dir_sb.serialize(&dir_buffer[0]);

			WriteBlock(super_block.original_inode_reserved, &dir_buffer[0]);
		} else {
			std::vector<uint8_t> buffer;
			buffer.resize(zns_dev->lba_size_bytes);
			int ret = zns_udevice_read(zns_dev, 0, (void *)&buffer[0], zns_dev->lba_size_bytes);
			if (ret != 0) {
				std::cerr << "Could not read super block" << std::endl;
			}

			super_block = SuperBlock(buffer);
			block_map.resize(super_block.block_map_size);
			block_map_offset = super_block.original_inode_reserved - 16;
			ret = zns_udevice_read(zns_dev, zns_dev->lba_size_bytes, (void *)&block_map[0], super_block.block_map_size);
			if (ret != 0) {
				std::cerr << "Could not read block map" << std::endl;
			}
		}
	}

	void S2FileSystem::SetBlockType(const uint16_t num, const BlockType block_type) {
		PROFILE_FS_FUNCTION();
		std::lock_guard<std::recursive_mutex> g(mtx);
		block_map[num / 4] &= RESET_BIT_MASKS[num % 4];
		block_map[num / 4] |= BLOCK_BIT_MASKS[num % 4] & block_type << (num % 4) * 2;
	}

	IOStatus S2FileSystem::WriteBlockMap() {
		PROFILE_FS_FUNCTION();
		std::lock_guard<std::recursive_mutex> g(mtx);
		auto ret = zns_udevice_write(zns_dev, zns_dev->lba_size_bytes, &block_map[0], super_block.block_map_size);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::WriteSuperBlock() {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> output;
		output.resize(zns_dev->lba_size_bytes);
		super_block.serialize(output);
		auto ret = zns_udevice_write(zns_dev, 0, &output[0], zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::WriteBlock(const uint64_t block, const uint8_t *data) {
		auto ret = zns_udevice_write(zns_dev, (block + block_map_offset) * zns_dev->lba_size_bytes, (void *)&data[0],
									 zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::ReadBlock(const uint64_t block, uint8_t *data) {
		auto ret = zns_udevice_read(zns_dev, (block + block_map_offset) * zns_dev->lba_size_bytes, (void *)&data[0],
									zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	DirectorySuperBlock::DirectorySuperBlock(const uint8_t *buffer) {
		PROFILE_FS_FUNCTION();
		num_entries = u64_from_buffer(&buffer[0]);
		name_len = u64_from_buffer(&buffer[8]);
		name.assign((char *)&buffer[DIR_SUPER_BLOCK_PLAIN_SIZE], name_len);
	}

	void DirectorySuperBlock::serialize(uint8_t *output) const {
		PROFILE_FS_FUNCTION();
		u64_to_buffer(num_entries, &output[0]);
		u64_to_buffer(name_len, &output[8]);
		memcpy(&output[DIR_SUPER_BLOCK_PLAIN_SIZE], name.c_str(), name_len);
	}

	IOStatus S2FileSystem::ReadINode(const INodePointer ptr, INode &inode) {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		auto ret = ReadBlock(ptr.block, &buffer[0]);
		if (!ret.ok()) {
			return ret;
		}

		if (!buffer[ptr.offset]) {
			return IOStatus::NotFound();
		}

		inode = INode(&buffer[inodes_per_block + (ptr.offset * INODE_SIZE)]);
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::ReadDirectory(const INode inode, DirectorySuperBlock &dir_super_block,
										 std::vector<DirectoryEntry> &directory_entries, uint64_t &buffer_size) {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> buffer;
		const auto ret = ReadEntireFile(inode, buffer);
		if (!ret.ok()) {
			return ret;
		}
		buffer_size = buffer.size();

		uint64_t j = 0, buffer_idx = 0;
		dir_super_block = DirectorySuperBlock(&buffer[0]);
		buffer_idx = DIR_SUPER_BLOCK_PLAIN_SIZE + dir_super_block.name_len;
		for (; j < dir_super_block.num_entries && buffer_idx < zns_dev->lba_size_bytes; j++) {
			directory_entries.push_back(DirectoryEntry(&buffer[buffer_idx]));
			buffer_idx += directory_entries.back().name_len + DIR_BLOCK_PLAIN_SIZE;
		}

		return IOStatus::OK();
	}

	IOStatus S2FileSystem::FindInode(const std::string &path, RichInode &inode) {
		RichInode base_ri;
		std::string base_path;
		return FindInodeWithBase(path, inode, base_ri, base_path);
	}

	IOStatus S2FileSystem::FindInodeWithBase(const std::string &path, RichInode &inode, RichInode &base_inode,
											 std::string &base_path) {
		FindInodeDirs dirs;
		return FindInodeWithBaseWithDirs(path, inode, base_inode, base_path, dirs);
	}

	IOStatus S2FileSystem::FindInodeWithBaseWithDirs(const std::string &path, RichInode &inode, RichInode &base_inode,
													 std::string &base_path, FindInodeDirs &dirs) {
		PROFILE_FS_FUNCTION();
		std::string current_path;
		bool continue_searching = true;
		inode.ptr.block = 0;
		inode.ptr.offset = 0;
		auto status = ReadINode({.block = inode.ptr.block, .offset = inode.ptr.offset}, inode.inode);
		if (!status.ok()) {
			return status;
		}
		base_inode = inode;
		base_path = "";

		while (continue_searching) {
			continue_searching = false;
			if (inode.inode.inode_type == S2EntryType::File) {
				break;
			}

			dirs.dir_entries.clear();
			status = ReadDirectory(inode.inode, dirs.sb, dirs.dir_entries, dirs.buffer_size);
			if (!status.ok()) {
				return status;
			}

			if (current_path + "/" + dirs.sb.name == path) {
				return IOStatus::OK();
			}

			for (const auto &entry : dirs.dir_entries) {
				auto const is_path_eq = current_path + "/" + entry.name == path;
				if (is_path_eq ||
					(entry.type == S2EntryType::Directory && path.find(current_path + "/" + entry.name) == 0)) {
					inode.ptr.block = entry.block;
					inode.ptr.offset = entry.offset;
					auto status = ReadINode({.block = entry.block, .offset = entry.offset}, inode.inode);
					if (!status.ok()) {
						return status;
					}

					if (is_path_eq) {
						return IOStatus::OK();
					} else {
						base_inode = inode;
						current_path += "/" + entry.name;
						base_path = current_path;
						continue_searching = true;
						break;
					}
				}
			}
		}

		return IOStatus::NotFound("find_inode: " + path);
	}

	IOStatus S2FileSystem::PlaceNewINodeBlock(const INode inode, INodePointer &ptr, bool flush) {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> buffer;
		std::lock_guard<std::recursive_mutex> g(mtx);
		buffer.resize(zns_dev->lba_size_bytes);
		for (uint64_t i = 0; i < block_map_size; i++) {
			for (uint8_t j = 0; j < 4; j++) {
				auto const block_num = (i * 4) + j;
				BlockType const block_type =
						(S2FileSystem::BlockType)(BLOCK_BIT_MASKS[0] & block_map[block_num / 4] >> (block_num % 4) * 2);
				switch (block_type) {
				case BlockType::Inode: {
					auto ret = ReadBlock(block_num, &buffer[0]);
					if (!ret.ok()) {
						return ret;
					}

					for (uint8_t k = 0; k < inodes_per_block; k++) {
						if (!buffer[k]) {
							ret = WriteINode({inode, {.block = block_num, .offset = k}});
							if (!ret.ok()) {
								return ret;
							}
							ptr = {.block = block_num, .offset = k};
							return IOStatus::OK();
						}
					}
					break;
				}
				case BlockType::Unused: {
					SetBlockType(block_num, BlockType::Inode);
					if (flush) {
						auto ret = WriteBlockMap();
						if (!ret.ok()) {
							return ret;
						}
					}
					for (uint8_t k = 0; k < inodes_per_block; k++) {
						buffer[k] = false;
					}
					buffer[0] = true;
					inode.serialize(&buffer[inodes_per_block]);
					auto ret = WriteBlock(block_num, &buffer[0]);
					if (!ret.ok()) {
						return ret;
					}
					ptr = {.block = block_num, .offset = 0};
					return IOStatus::OK();
				}
				default: {
					break;
				}
				}
			}
		}
		return IOStatus::NoSpace();
	}

	IOStatus S2FileSystem::PlaceNewDataBlock(uint64_t &block_num, bool flush) {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		std::lock_guard<std::recursive_mutex> g(mtx);
		for (uint64_t i = super_block.original_inode_reserved / 4; i < block_map_size; i++) {
			for (uint8_t j = 0; j < 4; j++) {
				auto const t_block_num = (i * 4) + j;
				BlockType const block_type = (S2FileSystem::BlockType)(
						BLOCK_BIT_MASKS[0] & block_map[t_block_num / 4] >> (t_block_num % 4) * 2);
				if (block_type == BlockType::Unused) {
					SetBlockType(t_block_num, BlockType::Data);
					if (flush) {
						auto ret = WriteBlockMap();
						if (!ret.ok()) {
							return ret;
						}
					}
					block_num = t_block_num;
					return IOStatus::OK();
				}
			}
		}
		return IOStatus::NoSpace();
	}

	IOStatus S2FileSystem::CreateItem(const std::string &name, const S2EntryType item, const RichInode base_inode,
									  FindInodeDirs &dirs, RichInode &inode) {
		PROFILE_FS_FUNCTION();
		inode.inode.is_nested = false;
		inode.inode.inode_type = item;
		inode.inode.num_disk_pointers = 0;
		inode.inode.use_next_pointer = false;
		inode.inode.size = 0;

		DirectoryEntry entry;
		entry.name = name;
		entry.name_len = entry.name.length();
		entry.type = item;
		return AddEntryToDirectory(base_inode, dirs, entry, inode);
	}

	IOStatus S2FileSystem::WriteINode(const RichInode ri) {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		auto ret = ReadBlock(ri.ptr.block, &buffer[0]);
		if (!ret.ok()) {
			return ret;
		}

		buffer[ri.ptr.offset] = true;
		ri.inode.serialize(&buffer[inodes_per_block + (ri.ptr.offset * INODE_SIZE)]);
		return WriteBlock(ri.ptr.block, &buffer[0]);
	}

	IOStatus S2FileSystem::ReadEntireFile(const INode inode, std::vector<uint8_t> &output) {
		PROFILE_FS_FUNCTION();
		output.resize(closest_upper_multiple(zns_dev->lba_size_bytes, inode.size));
		INode use_inode = inode;
		uint64_t buffer_idx = 0;
		while (true) {
			// TODO: optimize to sequential read later
			for (uint64_t i = 0; i < use_inode.num_disk_pointers; i++) {
				auto ret = ReadBlock(use_inode.disk_pointers[i], &output[buffer_idx]);
				if (!ret.ok()) {
					return ret;
				}
				buffer_idx += zns_dev->lba_size_bytes;
				output.resize(output.size() + zns_dev->lba_size_bytes);
			}

			if (use_inode.use_next_pointer) {
				const auto status = ReadINode(use_inode.next_pointer, use_inode);
				if (!status.ok()) {
					return status;
				}
			} else {
				break;
			}
		}

		output.resize(inode.size);
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::AddEntryToDirectory(const RichInode base_inode, FindInodeDirs &dirs, DirectoryEntry entry,
											   RichInode &inode) {
		PROFILE_FS_FUNCTION();
		if (entry.type == S2EntryType::Directory) {
			auto ret = PlaceNewDataBlock(inode.inode.disk_pointers[0], false);
			if (!ret.ok()) {
				return ret;
			}
			inode.inode.num_disk_pointers = 1;

			DirectorySuperBlock new_dir_sb;
			new_dir_sb.num_entries = 0;
			new_dir_sb.name_len = entry.name_len;
			new_dir_sb.name = entry.name;
			inode.inode.size += DIR_SUPER_BLOCK_PLAIN_SIZE + entry.name_len;

			std::vector<uint8_t> buffer;
			buffer.resize(zns_dev->lba_size_bytes);
			new_dir_sb.serialize(&buffer[0]);
			ret = WriteBlock(inode.inode.disk_pointers[0], &buffer[0]);
			if (!ret.ok()) {
				return ret;
			}
		}

		auto ret = PlaceNewINodeBlock(inode.inode, inode.ptr, false);
		if (!ret.ok()) {
			return ret;
		}

		std::vector<uint8_t> buffer;
		entry.block = inode.ptr.block;
		entry.offset = inode.ptr.offset;

		dirs.dir_entries.push_back(entry);

		buffer.resize(dirs.buffer_size + DIR_BLOCK_PLAIN_SIZE + entry.name_len);
		uint64_t buffer_index = DIR_SUPER_BLOCK_PLAIN_SIZE + dirs.sb.name_len;
		dirs.sb.num_entries++;
		dirs.sb.serialize(&buffer[0]);

		for (auto &t : dirs.dir_entries) {
			t.serialize(&buffer[buffer_index]);
			buffer_index += DIR_BLOCK_PLAIN_SIZE + t.name_len;
		}

		ret = OverWriteEntireFile(base_inode, buffer);
		if (!ret.ok()) {
			return ret;
		}

		return IOStatus::OK();
	}

	IOStatus S2FileSystem::AddDiskPointer(RichInode *ri_node, RichInode **output) {
		PROFILE_FS_FUNCTION();
		if ((!ri_node->inode.is_nested && ri_node->inode.num_disk_pointers == NUM_DISK_POINTERS) ||
			(ri_node->inode.is_nested && ri_node->inode.num_disk_pointers == NUM_NESTED_DISK_POINTERS)) {
			RichInode *new_inode = new RichInode;
			new_inode->inode.is_nested = true;
			new_inode->inode.num_disk_pointers = 1;

			auto ret = PlaceNewDataBlock(new_inode->inode.disk_pointers[0]);
			if (!ret.ok()) {
				return ret;
			}
			ret = PlaceNewINodeBlock(new_inode->inode, new_inode->ptr);
			if (!ret.ok()) {
				return ret;
			}

			ri_node->inode.next_pointer = new_inode->ptr;
			ri_node->inode.use_next_pointer = true;
			ret = WriteINode({ri_node->inode, ri_node->ptr});
			if (!ret.ok()) {
				return ret;
			}
			*output = new_inode;
			return IOStatus::OK();
		}

		ri_node->inode.num_disk_pointers++;
		auto ret = PlaceNewDataBlock(ri_node->inode.disk_pointers[ri_node->inode.num_disk_pointers - 1]);
		if (!ret.ok()) {
			return ret;
		}
		return WriteINode({ri_node->inode, ri_node->ptr});
	}

	IOStatus S2FileSystem::RemoveInode(const INodePointer ptr) {
		PROFILE_FS_FUNCTION();
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		auto ret = ReadBlock(ptr.block, &buffer[0]);
		if (!ret.ok()) {
			return ret;
		}

		buffer[ptr.offset] = false;

		bool block_still_in_use = false;
		for (uint8_t i = 0; i < inodes_per_block; i++) {
			if (buffer[i]) {
				block_still_in_use = true;
				break;
			}
		}

		if (!block_still_in_use) {
			SetBlockType(ptr.block, BlockType::Unused);
			return WriteBlockMap();
		}

		return WriteBlock(ptr.block, &buffer[0]);
	}

	S2FileSystem::~S2FileSystem() {
		// uint64_t empty = 0;
		// uint64_t data = 0;
		// uint64_t inode = 0;
		// for (uint64_t i = 0; i < block_map_size; i++) {
		// 	// std::cout << std::to_string(i) << " ";
		// 	// std::cout << std::to_string(block_map[i]) << " ";
		// 	for (uint8_t j = 0; j < 4; j++) {
		// 		auto const block_num = (i * 4) + j;
		// 		BlockType const block_type =
		// 				(S2FileSystem::BlockType)(BLOCK_BIT_MASKS[0] & block_map[block_num / 4] >> (block_num % 4) * 2);
		// 		// std::cout << std::to_string(block_type) << " ";
		// 		switch (block_type) {
		// 			case S2FileSystem::BlockType::Data: {
		// 				data++;
		// 				break;
		// 			}
		// 			case S2FileSystem::BlockType::Inode: {
		// 				inode++;
		// 				break;
		// 			}
		// 			case S2FileSystem::BlockType::Unused: {
		// 				empty++;
		// 				break;
		// 			}
		// 		}
		// 	}
		// 	// std::cout << std::endl;
		// }
		// std::cout << "Free blocks " << std::to_string(empty) << std::endl;
		// std::cout << "Data blocks " << std::to_string(data) << std::endl;
		// std::cout << "Inode blocks " << std::to_string(inode) << std::endl;
		WriteBlockMap();
	}

	// Create a brand new sequentially-readable file with the specified name.
	// On success, stores a pointer to the new file in *result and returns OK.
	// On failure stores nullptr in *result and returns non-OK.  If the file does
	// not exist, returns a non-OK status.
	//
	// The returned file will only be accessed by one thread at a time.
	IOStatus S2FileSystem::NewSequentialFile(const std::string &fname, const FileOptions &file_opts,
											 std::unique_ptr<FSSequentialFile> *result,
											 __attribute__((unused)) IODebugContext *dbg) {
		PROFILE_FS_FUNCTION();
		RichInode base, inode;
		std::string base_path;
		auto const _fname = SanitizePath(fname);
		auto g = LockManager::WriteGuard(_fname);
		FindInodeDirs dirs;
		auto ret = FindInodeWithBaseWithDirs(_fname, inode, base, base_path, dirs);
		if (ret.code() == Status::Code::kNotFound) {
			ret = CreateItem(_fname.substr(base_path.length() + 1), S2EntryType::File, base, dirs, inode);
			if (!ret.ok()) {
				return ret;
			}
		} else if (!ret.ok()) {
			return IOStatus::NotFound("newsequentialfile: " + fname);
		}

		auto r = new RocksSequentialFile(inode.inode, zns_dev, this);
		r->path = _fname;
		result->reset(r);
		return IOStatus::OK();
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
		PROFILE_FS_FUNCTION();
		RichInode inode;
		auto const _fname = SanitizePath(fname);
		auto g = LockManager::WriteGuard(_fname);
		auto ret = FindInode(_fname, inode);
		if (!ret.ok()) {
			return ret;
		}

		IOStatus init_status;
		result->reset(new RocksRandomAccessFile(inode.inode, zns_dev, this, init_status));
		return init_status;
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
		PROFILE_FS_FUNCTION();
		RichInode inode, base;
		auto const _fname = SanitizePath(fname);
		auto g = LockManager::WriteGuard(_fname);
		std::string base_path;
		FindInodeDirs dirs;
		auto ret = FindInodeWithBaseWithDirs(_fname, inode, base, base_path, dirs);
		if (ret.code() == Status::Code::kNotFound) {
			ret = CreateItem(_fname.substr(base_path.length() + 1), S2EntryType::File, base, dirs, inode);
			if (!ret.ok()) {
				return ret;
			}
		} else if (!ret.ok()) {
			return IOStatus::NotFound("newwritablefile" + fname);
		} else if (ret.ok()) {
			ret = RemoveItem(_fname, base_path, inode);
			if (!ret.ok()) {
				return ret;
			}
		}

		auto r = new RocksWriteableFile(inode, this, zns_dev);
		r->path = fname;
		result->reset(r);
		return IOStatus::OK();
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
		PROFILE_FS_FUNCTION();
		auto const _name = SanitizePath(name);
		auto g = LockManager::WriteGuard(_name);
		RichInode inode;
		const auto ret = FindInode(_name, inode);
		if (!ret.ok()) {
			return ret;
		}

		result->reset(new RocksDirectory());
		return IOStatus::OK();
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
		PROFILE_FS_FUNCTION();

		RichInode inode, base;
		const auto _dirname = SanitizePath(dirname);
		auto g = LockManager::WriteGuard(_dirname);
		std::string base_path;
		FindInodeDirs dirs;
		auto ret = FindInodeWithBaseWithDirs(_dirname, inode, base, base_path, dirs);
		if (ret.ok()) {
			return IOStatus::InvalidArgument();
		} else if (ret.code() != Status::kNotFound) {
			return ret;
		}

		return CreateItem(_dirname.substr(base_path.length() + 1), S2EntryType::Directory, base, dirs, inode);
	}

	// Creates directory if missing. Return Ok if it exists, or successful in
	// Creating.
	IOStatus S2FileSystem::CreateDirIfMissing(const std::string &dirname, const IOOptions &options,
											  __attribute__((unused)) IODebugContext *dbg) {
		PROFILE_FS_FUNCTION();
		RichInode inode, base;
		const auto _dirname = SanitizePath(dirname);
		std::string base_path = _dirname;
		auto g = LockManager::WriteGuard(_dirname);
		auto ret = FindInodeWithBase(_dirname, inode, base, base_path);
		if (ret.code() == Status::Code::kNotFound) {
			std::stack<std::string> dirs_to_create;
			dirs_to_create.push(_dirname);

			if (base_path.length() == 0) {
				base_path = get_base_path(_dirname);
			}

			std::string base_copy = base_path;
			while (true) {
				ret = FindInodeWithBase(base_copy, inode, base, base_path);
				if (ret.code() == Status::Code::kNotFound) {
					dirs_to_create.push(base_copy);
					base_copy = get_base_path(base_copy);
				} else if (!ret.ok()) {
					return ret;
				} else {
					break;
				}
			}

			while (dirs_to_create.size() > 0) {
				CreateDir(dirs_to_create.top(), options, dbg);
				dirs_to_create.pop();
			}
		} else if (!ret.ok()) {
			return IOStatus::NotFound("createdirifmissing: " + dirname);
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::GetFileSize(const std::string &fname, const IOOptions &options, uint64_t *file_size,
									   __attribute__((unused)) IODebugContext *dbg) {
		PROFILE_FS_FUNCTION();
		RichInode inode;
		const auto _fname = SanitizePath(fname);
		auto g = LockManager::WriteGuard(_fname);
		auto ret = FindInode(_fname, inode);
		if (!ret.ok()) {
			return ret;
		}
		*file_size = inode.inode.size;
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::DeleteDir(const std::string &dirname, const IOOptions &options,
									 __attribute__((unused)) IODebugContext *dbg) {
		PROFILE_FS_FUNCTION();
		RichInode inode;
		const auto _dirname = SanitizePath(dirname);
		auto g = LockManager::WriteGuard(_dirname);
		auto ret = FindInode(_dirname, inode);
		if (!ret.ok()) {
			return ret;
		}

		if (inode.inode.inode_type != S2EntryType::Directory) {
			return IOStatus::InvalidArgument();
		}

		std::stack<DeleteDirDfsItem> dfs_stack, delete_stack;
		dfs_stack.push({inode, _dirname});
		while (dfs_stack.size() > 0) {
			DeleteDirDfsItem current = dfs_stack.top();
			dfs_stack.pop();

			if (current.ri.inode.inode_type == S2EntryType::Directory) {
				delete_stack.push(current);

				uint64_t temp;
				DirectorySuperBlock dir_sb;
				std::vector<DirectoryEntry> dir_entries;
				auto ret = ReadDirectory(current.ri.inode, dir_sb, dir_entries, temp);
				if (!ret.ok()) {
					return ret;
				}

				for (const auto &entry : dir_entries) {
					INode temp_inode;
					INodePointer ptr = {.block = entry.block, .offset = entry.offset};
					ret = ReadINode(ptr, temp_inode);
					if (!ret.ok()) {
						return ret;
					}
					dfs_stack.push({.ri = {temp_inode, ptr}, .path = current.path + "/" + entry.name});
				}
			} else {

				ret = RemoveItem(current.path, get_base_path(current.path), current.ri);
				if (!ret.ok()) {
					return ret;
				}
			}
		}

		while (delete_stack.size() > 0) {
			DeleteDirDfsItem current = delete_stack.top();
			delete_stack.pop();
			ret = RemoveItem(current.path, get_base_path(current.path), current.ri);
			if (!ret.ok()) {
				return ret;
			}
		}

		return IOStatus::OK();
	}

	IOStatus S2FileSystem::GetFileModificationTime(const std::string &fname, const IOOptions &options,
												   uint64_t *file_mtime, __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	IOStatus S2FileSystem::GetAbsolutePath(const std::string &db_path, const IOOptions &options,
										   std::string *output_path, __attribute__((unused)) IODebugContext *dbg) {
		*output_path = SanitizePath(db_path);
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::DeleteFile(const std::string &fname, const IOOptions &options,
									  __attribute__((unused)) IODebugContext *dbg) {
		PROFILE_FS_FUNCTION();
		RichInode inode, base_inode;
		const auto _fname = SanitizePath(fname);
		auto g = LockManager::WriteGuard(fname);
		std::string base_path;
		auto ret = FindInodeWithBase(_fname, inode, base_inode, base_path);
		if (!ret.ok()) {
			return ret;
		}

		if (inode.inode.inode_type != S2EntryType::File) {
			return IOStatus::InvalidArgument();
		}

		return RemoveItem(_fname, base_path, inode);
	}

	IOStatus S2FileSystem::NewLogger(const std::string &fname, const IOOptions &io_opts,
									 std::shared_ptr<Logger> *result, __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::OK();
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
		return IOStatus::OK();
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
		return IOStatus::OK();
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
		PROFILE_FS_FUNCTION();
		RichInode src_ri, src_base_ri, inode, base;

		auto const _src = SanitizePath(src);
		auto const _target = SanitizePath(target);
		auto g_src = LockManager::WriteGuard(_src);
		auto g_target = LockManager::WriteGuard(_target);

		std::string src_base_path, target_base_path;
		auto ret = FindInodeWithBase(_src, src_ri, src_base_ri, src_base_path);
		if (!ret.ok()) {
			return ret;
		}

		ret = FindInodeWithBase(_target, inode, base, target_base_path);
		if (ret.code() != Status::Code::kNotFound) {
			if (ret.ok()) {
				ret = RemoveItem(_target, target_base_path, inode);
				if (!ret.ok()) {
					return ret;
				}
			} else {
				return ret;
			}
		}

		DirectorySuperBlock directory_super_block;
		std::vector<DirectoryEntry> directory_entry;
		uint64_t buffer_size;
		ret = ReadDirectory(src_base_ri.inode, directory_super_block, directory_entry, buffer_size);
		if (!ret.ok()) {
			return ret;
		}

		std::string src_name = _src.substr(src_base_path.length() + 1);
		std::string target_name = _target.substr(target_base_path.length() + 1);
		for (auto &entry : directory_entry) {
			if (entry.name == src_name) {
				entry.name = target_name;
				entry.name_len = target_name.length();
				entry.block = src_ri.ptr.block;
				entry.offset = src_ri.ptr.offset;
				break;
			}
		}

		if (src_ri.inode.inode_type == S2EntryType::Directory) {
			DirectorySuperBlock src_sb;
			std::vector<DirectoryEntry> src_entries;
			uint64_t src_buffer;
			ret = ReadDirectory(src_ri.inode, src_sb, src_entries, src_buffer);
			if (!ret.ok()) {
				return ret;
			}

			src_sb.name = target_name;
			src_sb.name_len = target_name.length();
			std::vector<uint8_t> buffer;
			buffer.resize(src_buffer + (src_name.length() - target_name.length()));
			uint64_t buffer_index = DIR_SUPER_BLOCK_PLAIN_SIZE + src_sb.name_len;
			src_sb.serialize(&buffer[0]);

			for (auto &entry : src_entries) {
				entry.serialize(&buffer[buffer_index]);
				buffer_index += DIR_BLOCK_PLAIN_SIZE + entry.name_len;
			}

			buffer.resize(buffer_index + 1);
			ret = OverWriteEntireFile(src_ri, buffer);
			if (!ret.ok()) {
				return ret;
			}
		}

		std::vector<uint8_t> buffer;
		buffer.resize(buffer_size + (src_name.length() - target_name.length()));
		uint64_t buffer_index = DIR_SUPER_BLOCK_PLAIN_SIZE + directory_super_block.name_len;
		directory_super_block.serialize(&buffer[0]);

		for (auto &entry : directory_entry) {
			entry.serialize(&buffer[buffer_index]);
			buffer_index += DIR_BLOCK_PLAIN_SIZE + entry.name_len;
		}

		buffer.resize(buffer_index + 1);
		return OverWriteEntireFile(src_base_ri, buffer);
	}

	IOStatus S2FileSystem::RemoveItem(const std::string path, const std::string base_path, const RichInode ri) {
		PROFILE_FS_FUNCTION();
		RichInode ri_c = ri;
		auto ret = RemoveInode(ri_c.ptr);
		if (!ret.ok()) {
			return ret;
		}

		std::string path_name = path.substr((base_path.length() == 1 ? 0 : base_path.length()) + 1);
		RichInode base;
		ret = FindInode(base_path, base);
		if (!ret.ok()) {
			return ret;
		}

		uint64_t buffer_size;
		DirectorySuperBlock dir_sb;
		std::vector<DirectoryEntry> dir_entries;
		ret = ReadDirectory(base.inode, dir_sb, dir_entries, buffer_size);
		if (!ret.ok()) {
			return ret;
		}

		std::vector<uint8_t> buffer;
		buffer.resize(buffer_size - DIR_BLOCK_PLAIN_SIZE - path_name.length());
		uint64_t buffer_index = DIR_SUPER_BLOCK_PLAIN_SIZE + dir_sb.name_len;
		dir_sb.num_entries--;
		dir_sb.serialize(&buffer[0]);

		for (auto &entry : dir_entries) {
			if (entry.name == path_name) {
				continue;
			}
			entry.serialize(&buffer[buffer_index]);
			buffer_index += DIR_BLOCK_PLAIN_SIZE + entry.name_len;
		}
		buffer.resize(buffer_index + 1);
		ret = OverWriteEntireFile(base, buffer);
		if (!ret.ok()) {
			return ret;
		}

		INode inode_tmp = ri_c.inode;
		while (true) {
			for (uint64_t i = 0; i < inode_tmp.num_disk_pointers; i++) {
				SetBlockType(inode_tmp.disk_pointers[i], BlockType::Unused);
			}

			if (!inode_tmp.use_next_pointer) {
				break;
			}

			IOStatus ret = ReadINode(ri_c.inode.next_pointer, inode_tmp);
			if (!ret.ok()) {
				return ret;
			}

			ret = RemoveInode(ri_c.inode.next_pointer);
			if (!ret.ok()) {
				return ret;
			}
			ri_c.inode = inode_tmp;
		}

		return WriteBlockMap();
	}

	IOStatus S2FileSystem::OverWriteEntireFile(const RichInode inode, const std::vector<uint8_t> &buffer) {
		PROFILE_FS_FUNCTION();
		uint64_t num_to_delete =
				closest_upper_multiple(zns_dev->lba_size_bytes, inode.inode.size) / zns_dev->lba_size_bytes;

		INode inode_current = inode.inode;
		std::vector<RichInode> traversed_inode_vector;
		traversed_inode_vector.push_back(inode);

		while (inode_current.use_next_pointer) {
			INode inode_tmp;
			IOStatus ret = ReadINode(inode_current.next_pointer, inode_tmp);

			if (!ret.ok()) {
				return ret;
			}

			traversed_inode_vector.push_back({inode_tmp, inode_current.next_pointer});
			inode_current = inode_tmp;
		}

		while (traversed_inode_vector.size() > 0 && num_to_delete > 0) { // remove the inode
			uint64_t nr_for_delete = std::min(
					num_to_delete,
					traversed_inode_vector.back().inode.num_disk_pointers); // how many to delete from the current inode
			num_to_delete -= nr_for_delete;
			traversed_inode_vector.back().inode.num_disk_pointers -= nr_for_delete;

			for (uint64_t i = 0; i < nr_for_delete; i++) {
				SetBlockType(traversed_inode_vector.back().inode.disk_pointers[i], BlockType::Unused);
			}

			if (traversed_inode_vector.back().inode.num_disk_pointers == 0) {
				if (traversed_inode_vector.size() < 2) {
					auto ret = WriteINode(traversed_inode_vector.back());
					if (!ret.ok()) {
						return ret;
					}
					traversed_inode_vector.pop_back();
					continue;
				}

				IOStatus ret = RemoveInode(traversed_inode_vector.back().ptr);
				if (!ret.ok()) {
					return ret;
				}

				traversed_inode_vector[traversed_inode_vector.size() - 1].inode.use_next_pointer = false;

				ret = WriteINode(traversed_inode_vector[traversed_inode_vector.size() - 1]);
				if (!ret.ok()) {
					return ret;
				}
			} else {
				auto ret = WriteINode(traversed_inode_vector.back());

				if (!ret.ok()) {
					return ret;
				}
			}
			traversed_inode_vector.pop_back();
		}
		auto ret = WriteBlockMap();
		if (!ret.ok()) {
			return ret;
		}

		INode inode_c;
		ret = ReadINode(inode.ptr, inode_c);
		if (!ret.ok()) {
			return ret;
		}
		inode_c.size = 0;
		ret = WriteINode({inode_c, inode.ptr});
		if (!ret.ok()) {
			return ret;
		}

		Slice slice((char *)&buffer[0], buffer.size());
		RocksWriteableFile file = NewWritableFileByINode({inode_c, inode.ptr});
		return file.Append(slice, IOOptions(), nullptr);
	}

	RocksWriteableFile S2FileSystem::NewWritableFileByINode(const RichInode rich_inode) {
		return RocksWriteableFile({rich_inode.inode, rich_inode.ptr}, this, zns_dev);
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
		UNUSED(options);
		PROFILE_FS_FUNCTION();
		RichInode inode;
		const auto _dirname = SanitizePath(dir);
		auto g = LockManager::WriteGuard(_dirname);
		auto ret = FindInode(_dirname, inode);
		if (!ret.ok()) {
			return ret;
		}
		std::vector<DirectoryEntry> dir_list;
		DirectorySuperBlock dir_sb;
		uint64_t temp;
		ret = ReadDirectory(inode.inode, dir_sb, dir_list, temp);
		if (!ret.ok()) {
			return ret;
		}
		for (const auto &entry : dir_list) {
			result->push_back(entry.name);
		}
		return IOStatus::OK();
	}

	// Returns OK if the named file exists.
	//         NotFound if the named file does not exist,
	//                  the calling process does not have permission to determine
	//                  whether this file exists, or if the path is invalid.
	//         IOError if an IO Error was encountered
	IOStatus S2FileSystem::FileExists(const std::string &fname, const IOOptions &options,
									  __attribute__((unused)) IODebugContext *dbg) {
		PROFILE_FS_FUNCTION();
		RichInode inode;
		auto const _fname = SanitizePath(fname);
		auto g = LockManager::WriteGuard(_fname);
		return FindInode(_fname, inode);
	}

	IOStatus S2FileSystem::ReuseWritableFile(const std::string &fname, const std::string &old_fname,
											 const FileOptions &file_opts, std::unique_ptr<FSWritableFile> *result,
											 __attribute__((unused)) IODebugContext *dbg) {
		return IOStatus::IOError(__FUNCTION__);
	}

	std::string S2FileSystem::SanitizePath(const std::string input) {
		std::string output = input;
		size_t start_pos = 0;
		while ((start_pos = output.find("//", start_pos)) != std::string::npos) {
			output.replace(start_pos, 2, "/");
			start_pos += 1;
		}

		if (output.length() > 1 && output.back() == '/') {
			output.pop_back();
		}
		return output;
	}
} // namespace ROCKSDB_NAMESPACE
