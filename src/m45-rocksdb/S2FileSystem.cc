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

#include "RocksClasses.h"
#include <stosys_debug.h>
#include <utils.h>

namespace ROCKSDB_NAMESPACE {
	SuperBlock::SuperBlock(const std::vector<uint8_t> &buffer) {
		block_map_size = u32_from_buffer(&buffer[0]);
		original_inode_reserved = u16_from_buffer(&buffer[4]);
	}

	void SuperBlock::serialize(std::vector<uint8_t> &output) {
		u32_to_buffer(block_map_size, &output[0]);
		u16_to_buffer(original_inode_reserved, &output[4]);
	}

	INode::INode(const uint8_t *buffer) {
		is_nested = (bool)buffer[0];
		inode_type = (S2EntryType)buffer[1];
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
		uint64_t const read_upto = is_nested ? NUM_NESTED_DISK_POINTERS : NUM_DISK_POINTERS;
		for (uint64_t i = 58; pos < read_upto; i += 4, pos++) {
			disk_pointers[pos] = u32_from_buffer(&buffer[i]);
		}

		next_pointer = {.block = u32_from_buffer(&buffer[110]), .offset = (uint8_t)buffer[114]};
		use_next_pointer = buffer[115];
	}

	void INode::serialize(uint8_t *output) const {
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
		uint64_t const read_upto = is_nested ? NUM_NESTED_DISK_POINTERS : NUM_DISK_POINTERS;
		for (uint64_t i = 58; pos < read_upto; i += 4, pos++) {
			u32_to_buffer(disk_pointers[pos], &output[i]);
		}

		u32_to_buffer(next_pointer.block, &output[110]);
		output[114] = next_pointer.offset;
		output[115] = use_next_pointer;
	}

	DirectoryEntry::DirectoryEntry(const uint8_t *buffer) {
		block = u32_from_buffer(&buffer[0]);
		offset = buffer[4];
		type = (S2EntryType)buffer[5];
		name_len = u32_from_buffer(&buffer[6]);
		name.assign((char *)&buffer[DIR_BLOCK_PLAIN_SIZE], name_len);
	}

	void DirectoryEntry::serialize(uint8_t *output) const {
		u32_to_buffer(block, &output[0]);
		output[4] = offset;
		output[5] = (S2EntryType)type;
		u32_to_buffer(name_len, &output[6]);
		memcpy(&output[DIR_BLOCK_PLAIN_SIZE], name.c_str(), name_len);
	}

	S2FileSystem::S2FileSystem(std::string uri_db_path, bool debug) {
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
		params.log_zones = 32;
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

		inodes_per_block = zns_dev->lba_size_bytes / INODE_SIZE;

		// setup super block
		super_block.block_map_size = zns_dev->lba_size_bytes * 2;
		super_block.original_inode_reserved = 16;
		WriteSuperBlock();
		block_map_offset = 3;

		// setup inode & data block map
		this->block_map.resize(super_block.block_map_size);
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
		output[0] = 1;
		root.serialize(&output[1]);
		WriteBlock(0, &output[0]);

		// write directory to data block
		std::vector<uint8_t> dir_buffer;
		dir_buffer.resize(zns_dev->lba_size_bytes);

		DirectorySuperBlock dir_sb;
		dir_sb.name_len = 0;
		dir_sb.num_entries = 0;
		dir_sb.serialize(&dir_buffer[0]);

		WriteBlock(super_block.original_inode_reserved, &dir_buffer[0]);
	}

	void S2FileSystem::SetBlockType(const uint16_t num, const BlockType block_type) {
		block_map[num / 4] &= RESET_BIT_MASKS[num % 4];
		block_map[num / 4] |= BLOCK_BIT_MASKS[num % 4] & block_type << (num % 4) * 2;
	}

	S2FileSystem::BlockType S2FileSystem::GetBlockType(const uint32_t num) const {
		return (S2FileSystem::BlockType)(BLOCK_BIT_MASKS[0] & block_map[num / 4] >> (num % 4) * 2);
	}

	IOStatus S2FileSystem::WriteBlockMap() {
		auto ret = zns_udevice_write(zns_dev, zns_dev->lba_size_bytes, &block_map[0], super_block.block_map_size);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::WriteSuperBlock() {
		std::vector<uint8_t> output;
		output.resize(zns_dev->lba_size_bytes);
		super_block.serialize(output);
		auto ret = zns_udevice_write(zns_dev, 0, &output[0], zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::WriteBlock(const uint32_t block, const uint8_t *data) {
		auto ret = zns_udevice_write(zns_dev, (block + block_map_offset) * zns_dev->lba_size_bytes, (void *)&data[0],
									 zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::ReadBlock(const uint32_t block, uint8_t *data) {
		auto ret = zns_udevice_read(zns_dev, (block + block_map_offset) * zns_dev->lba_size_bytes, (void *)&data[0],
									zns_dev->lba_size_bytes);
		if (ret != 0) {
			return IOStatus::IOError();
		}
		return IOStatus::OK();
	}

	DirectorySuperBlock::DirectorySuperBlock(const uint8_t *buffer) {
		num_entries = u32_from_buffer(&buffer[0]);
		name_len = u32_from_buffer(&buffer[4]);
		name.assign((char *)&buffer[DIR_SUPER_BLOCK_PLAIN_SIZE], name_len);
	}

	void DirectorySuperBlock::serialize(uint8_t *output) const {
		u32_to_buffer(num_entries, &output[0]);
		u32_to_buffer(name_len, &output[4]);
		memcpy(&output[DIR_SUPER_BLOCK_PLAIN_SIZE], name.c_str(), name_len);
	}

	IOStatus S2FileSystem::ReadINode(const INodePointer ptr, INode &inode) {
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		auto ret = ReadBlock(ptr.block, &buffer[0]);
		if (!ret.ok()) {
			return ret;
		}

		inode = INode(&buffer[1 + (ptr.offset * INODE_SIZE)]);
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::ReadDirectory(const INode inode, DirectorySuperBlock &dir_super_block,
										 std::vector<DirectoryEntry> &directory_entries, uint32_t &buffer_size) {
		std::vector<uint8_t> buffer;
		const auto ret = ReadEntireFile(inode, buffer);
		if (!ret.ok()) {
			return ret;
		}
		buffer_size = buffer.size();

		uint32_t j = 0, buffer_idx = 0;
		dir_super_block = DirectorySuperBlock(&buffer[0]);
		buffer_idx = DIR_SUPER_BLOCK_PLAIN_SIZE + dir_super_block.name_len;
		for (; j < dir_super_block.num_entries && buffer_idx < zns_dev->lba_size_bytes; j++) {
			directory_entries.push_back(DirectoryEntry(&buffer[buffer_idx]));
			buffer_idx += directory_entries.back().name_len + DIR_BLOCK_PLAIN_SIZE;
		}

		return IOStatus::OK();
	}

	IOStatus S2FileSystem::FindInode(const std::string &path, INode &inode, INodePointer &ptr) {
		std::string _path = SanitizePath(path);
		std::string current_path;
		bool continue_searching = true;
		ptr.block = 0;
		ptr.offset = 0;
		auto status = ReadINode({.block = ptr.block, .offset = ptr.offset}, inode);
		if (!status.ok()) {
			return status;
		}

		while (continue_searching) {
			continue_searching = false;
			if (inode.inode_type == S2EntryType::File) {
				break;
			}

			std::vector<DirectoryEntry> dir_list;
			DirectorySuperBlock dir_super_block;
			uint32_t temp;
			status = ReadDirectory(inode, dir_super_block, dir_list, temp);
			if (!status.ok()) {
				return status;
			}

			if (current_path + "/" + dir_super_block.name == _path) {
				return IOStatus::OK();
			}

			for (const auto &entry : dir_list) {
				auto const is_path_eq = current_path + "/" + entry.name == _path;
				if (is_path_eq ||
					(entry.type == S2EntryType::Directory && _path.find(current_path + "/" + entry.name) == 0)) {
					ptr.block = entry.block;
					ptr.offset = entry.offset;
					auto status = ReadINode({.block = entry.block, .offset = entry.offset}, inode);
					if (!status.ok()) {
						return status;
					}

					if (is_path_eq) {
						return IOStatus::OK();
					} else {
						current_path += "/" + entry.name;
						continue_searching = true;
						break;
					}
				}
			}
		}

		return IOStatus::NotFound();
	}

	IOStatus S2FileSystem::PlaceNewINodeBlock(const INode inode, INodePointer &ptr) {
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		for (uint32_t i = 0; i < block_map.size(); i++) {
			for (uint8_t j = 0; j < 4; j++) {
				auto const block_num = (i * 4) + j;
				BlockType const block_type = GetBlockType(block_num);
				switch (block_type) {
				case BlockType::Inode: {
					auto ret = ReadBlock(block_num, &buffer[0]);
					if (!ret.ok()) {
						return ret;
					}

					if (buffer[0] < inodes_per_block) {
						ret = WriteINode({inode, {.block = block_num, .offset = buffer[0]}}, true);
						if (!ret.ok()) {
							return ret;
						}
						ptr = {.block = block_num, .offset = (buffer[0])};
						return IOStatus::OK();
					}
					break;
				}
				case BlockType::Unused: {
					SetBlockType(block_num, BlockType::Inode);
					auto ret = WriteBlockMap();
					if (!ret.ok()) {
						return ret;
					}
					buffer[0] = 1;
					inode.serialize(&buffer[1]);
					ret = WriteBlock(block_num, &buffer[0]);
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

	IOStatus S2FileSystem::PlaceNewDataBlock(uint32_t &block_num) {
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		for (uint32_t i = super_block.original_inode_reserved / 4; i < block_map.size(); i++) {
			for (uint8_t j = 0; j < 4; j++) {
				auto const t_block_num = (i * 4) + j;
				BlockType const block_type = GetBlockType(t_block_num);
				if (block_type == BlockType::Unused) {
					SetBlockType(t_block_num, BlockType::Data);
					auto ret = WriteBlockMap();
					if (!ret.ok()) {
						return ret;
					}
					block_num = t_block_num;
					return IOStatus::OK();
				}
			}
		}
		return IOStatus::NoSpace();
	}

	IOStatus S2FileSystem::CreateItem(const std::string &path, const S2EntryType item, INode &inode,
									  INodePointer &ptr) {
		std::string _path = SanitizePath(path);
		auto const idx = _path.find_last_of("/");
		std::string base_path;
		if (idx == std::string::npos) {
			base_path = _path;
		} else {
			base_path = _path.substr(0, idx + 1);
		}

		if (base_path.length() > 1 && base_path.back() == '/') {
			base_path.pop_back();
		}
		INode base_node;
		INodePointer base_ptr;
		auto const ret = FindInode(base_path, base_node, base_ptr);
		if (!ret.ok()) {
			return ret;
		}

		inode.is_nested = false;
		inode.inode_type = item;
		inode.num_disk_pointers = 0;
		inode.use_next_pointer = false;
		inode.size = 0;

		DirectoryEntry entry;
		entry.name = _path.substr(idx + 1, _path.length());
		entry.name_len = entry.name.length();
		entry.type = item;
		return AddEntryToDirectory(base_node, base_ptr, entry, inode, ptr);
	}

	IOStatus S2FileSystem::WriteINode(const RichInode ri, bool increment_count) {
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		auto ret = ReadBlock(ri.ptr.block, &buffer[0]);
		if (!ret.ok()) {
			return ret;
		}

		if (increment_count) {
			buffer[0]++;
		}

		ri.inode.serialize(&buffer[1 + (ri.ptr.offset * INODE_SIZE)]);
		return WriteBlock(ri.ptr.block, &buffer[0]);
	}

	IOStatus S2FileSystem::ReadEntireFile(const INode inode, std::vector<uint8_t> &output) {
		output.resize(closest_upper_multiple(zns_dev->lba_size_bytes, inode.size));
		INode use_inode = inode;
		uint32_t buffer_idx = 0;
		while (true) {
			// TODO: optimize to sequential read later
			for (uint32_t i = 0; i < use_inode.num_disk_pointers; i++) {
				auto ret = ReadBlock(use_inode.disk_pointers[i], &output[buffer_idx]);
				if (!ret.ok()) {
					return ret;
				}
				buffer_idx += zns_dev->lba_size_bytes;
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

	IOStatus S2FileSystem::FindAllNodes(const INode inode, FindAllNodesResult &result, const bool recursive) {
		// TODO: add recursive to get dir items
		INode use_inode = inode;
		while (true) {
			result.data_nodes.insert(result.data_nodes.end(), use_inode.disk_pointers,
									 use_inode.disk_pointers + use_inode.num_disk_pointers);

			if (use_inode.use_next_pointer) {
				result.inodes.push_back(use_inode.next_pointer);
				const auto status = ReadINode(use_inode.next_pointer, use_inode);
				if (!status.ok()) {
					return status;
				}
			} else {
				break;
			}
		}

		return IOStatus::OK();
	}

	IOStatus S2FileSystem::AddEntryToDirectory(const INode base_node, const INodePointer base_ptr, DirectoryEntry entry,
											   INode &node, INodePointer &ptr) {
		// TODO: remove dsb from every page
		if (entry.type == S2EntryType::Directory) {
			auto ret = PlaceNewDataBlock(node.disk_pointers[0]);
			if (!ret.ok()) {
				return ret;
			}
			node.num_disk_pointers = 1;

			DirectorySuperBlock new_dir_sb;
			new_dir_sb.num_entries = 0;
			new_dir_sb.name_len = entry.name_len;
			new_dir_sb.name = entry.name;
			node.size += DIR_SUPER_BLOCK_PLAIN_SIZE + entry.name_len;

			std::vector<uint8_t> buffer;
			buffer.resize(zns_dev->lba_size_bytes);
			new_dir_sb.serialize(&buffer[0]);
			ret = WriteBlock(node.disk_pointers[0], &buffer[0]);
			if (!ret.ok()) {
				return ret;
			}
		}

		auto ret = PlaceNewINodeBlock(node, ptr);
		if (!ret.ok()) {
			return ret;
		}

		std::vector<uint8_t> buffer;
		entry.block = ptr.block;
		entry.offset = ptr.offset;

		DirectorySuperBlock dir_super_block;
		std::vector<DirectoryEntry> dir_list;
		uint32_t buffer_size;
		ret = ReadDirectory(base_node, dir_super_block, dir_list, buffer_size);
		if (!ret.ok()) {
			return ret;
		}
		dir_list.push_back(entry);

		buffer.resize(buffer_size + DIR_BLOCK_PLAIN_SIZE + entry.name_len);
		uint32_t buffer_index = DIR_SUPER_BLOCK_PLAIN_SIZE + dir_super_block.name_len;
		dir_super_block.num_entries++;
		dir_super_block.serialize(&buffer[0]);

		for (auto &t : dir_list) {
			t.serialize(&buffer[buffer_index]);
			buffer_index += DIR_BLOCK_PLAIN_SIZE + t.name_len;
		}

		return WriteEntireFile({base_node, base_ptr}, buffer);
		// INode use_node = base_node;
		// INode base_node_c = base_node;
		// while (true) {
		// 	if (use_node.use_next_pointer) {
		// 		const auto status = ReadINode(use_node.next_pointer, use_node);
		// 		if (!status.ok()) {
		// 			return status;
		// 		}
		// 		continue;
		// 	}

		// 	uint32_t buffer_idx = 0;
		// 	auto ret = ReadBlock(use_node.disk_pointers[use_node.num_disk_pointers - 1], &buffer[0]);
		// 	if (!ret.ok()) {
		// 		return ret;
		// 	}

		// 	dir_super_block = DirectorySuperBlock(&buffer[0]);
		// 	buffer_idx = DIR_SUPER_BLOCK_PLAIN_SIZE + dir_super_block.name_len;

		// 	for (; j < dir_super_block.num_entries && buffer_idx < zns_dev->lba_size_bytes; j++) {
		// 		buffer_idx += DirectoryEntry(&buffer[buffer_idx]).name_len + DIR_BLOCK_PLAIN_SIZE;
		// 	}

		// 	base_node_c.size += space_needed;
		// 	ret = WriteINode({base_node_c, base_ptr}, false);
		// 	if (!ret.ok()) {
		// 		return ret;
		// 	}

		// 	if (buffer_idx + space_needed > zns_dev->lba_size_bytes) {
		// 		// TODO: create another disk pointer or inode as needed
		// 	}

		// 	entry.serialize(&buffer[buffer_idx]);
		// 	dir_super_block.num_entries++;
		// 	dir_super_block.serialize(&buffer[0]);
		// 	ret = WriteBlock(use_node.disk_pointers[use_node.num_disk_pointers - 1], &buffer[0]);
		// 	if (!ret.ok()) {
		// 		return ret;
		// 	}
		// 	break;
		// }
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::AddDiskPointer(RichInode **ri_node, std::function<void()> clear_callback) {
		if ((!(*ri_node)->inode.is_nested && (*ri_node)->inode.num_disk_pointers == NUM_DISK_POINTERS) ||
			((*ri_node)->inode.is_nested && (*ri_node)->inode.num_disk_pointers == NUM_NESTED_DISK_POINTERS)) {
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

			(*ri_node)->inode.next_pointer = new_inode->ptr;
			(*ri_node)->inode.use_next_pointer = true;
			ret = WriteINode({(*ri_node)->inode, (*ri_node)->ptr}, false);
			if (!ret.ok()) {
				return ret;
			}
			clear_callback();
			*ri_node = new_inode;
			return IOStatus::OK();
		}

		(*ri_node)->inode.num_disk_pointers++;
		auto ret = PlaceNewDataBlock((*ri_node)->inode.disk_pointers[(*ri_node)->inode.num_disk_pointers - 1]);
		if (!ret.ok()) {
			return ret;
		}
		return WriteINode({(*ri_node)->inode, (*ri_node)->ptr}, false);
	}

	IOStatus S2FileSystem::RemoveInode(const INodePointer ptr) {
		std::vector<uint8_t> buffer;
		buffer.resize(zns_dev->lba_size_bytes);
		auto ret = ReadBlock(ptr.block, &buffer[0]);
		if (!ret.ok()) {
			return ret;
		}

		uint8_t num_inodes = buffer[0];
		for (uint32_t i = 0; i < num_inodes; i++) {
			if (i < ptr.offset) {
				continue;
			}

			memcpy(&buffer[1 + (ptr.offset * INODE_SIZE)], &buffer[1 + (i * INODE_SIZE)], INODE_SIZE);
		}
		buffer[0]--;
		return WriteBlock(ptr.block, &buffer[0]);
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
		INode inode;
		INodePointer ptr;
		auto ret = FindInode(fname, inode, ptr);
		if (ret.code() == Status::Code::kNotFound) {
			ret = CreateItem(fname, S2EntryType::File, inode, ptr);
			if (!ret.ok()) {
				return ret;
			}
		} else if (!ret.ok()) {
			return IOStatus::NotFound();
		}

		result->reset(new RocksSequentialFile(inode, zns_dev, this));
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
		// TODO: delete old file
		INode inode;
		INodePointer ptr;
		auto ret = FindInode(fname, inode, ptr);
		if (ret.code() == Status::Code::kNotFound) {
			ret = CreateItem(fname, S2EntryType::File, inode, ptr);
			if (!ret.ok()) {
				return ret;
			}
		} else if (!ret.ok()) {
			return IOStatus::NotFound();
		}

		result->reset(new RocksWriteableFile({inode, ptr}, this, zns_dev));
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
		const auto ret = CreateDir(name, io_opts, dbg);
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
		INode inode;
		INodePointer ptr;
		auto ret = FindInode(dirname, inode, ptr);
		if (ret.ok()) {
			return IOStatus::InvalidArgument();
		} else if (ret.code() != Status::kNotFound) {
			return ret;
		}

		return CreateItem(dirname, S2EntryType::Directory, inode, ptr);
	}

	// Creates directory if missing. Return Ok if it exists, or successful in
	// Creating.
	IOStatus S2FileSystem::CreateDirIfMissing(const std::string &dirname, const IOOptions &options,
											  __attribute__((unused)) IODebugContext *dbg) {
		INode inode;
		INodePointer ptr;
		auto ret = FindInode(dirname, inode, ptr);
		if (ret.code() == Status::Code::kNotFound) {
			ret = CreateDir(dirname, options, dbg);
			if (!ret.ok()) {
				return ret;
			}
		} else if (!ret.ok()) {
			return IOStatus::NotFound();
		}
		return IOStatus::OK();
	}

	IOStatus S2FileSystem::GetFileSize(const std::string &fname, const IOOptions &options, uint64_t *file_size,
									   __attribute__((unused)) IODebugContext *dbg) {
		INode inode;
		INodePointer ptr;
		auto ret = FindInode(fname, inode, ptr);
		if (!ret.ok()) {
			return ret;
		}
		*file_size = inode.size;
		return IOStatus::OK();
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
		*output_path = db_path;
		return IOStatus::OK();
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
		INode inode, src_inode;
		INodePointer ptr;
		auto const idx = src.find_last_of("/");

		auto ret = FindInode(src, src_inode, ptr);

		if (!ret.ok()) {
			return ret;
		}

		ret = FindInode(target, inode, ptr);

		if (ret.code() != Status::Code::kNotFound) {
			if (ret.ok()) {
				return IOStatus::InvalidArgument();
			}

			return ret;
		}

		std::string base_path;

		if (idx == std::string::npos) {
			base_path = src;
		} else {
			base_path = src.substr(0, idx + 1);
		}

		if (base_path.length() > 1 && base_path.back() == '/') {
			base_path.pop_back();
		}

		ret = FindInode(base_path, inode, ptr);

		if (!ret.ok()) {
			return ret;
		}

		DirectorySuperBlock directory_super_block;
		std::vector<DirectoryEntry> directory_entry;
		uint32_t buffer_size;

		ret = ReadDirectory(inode, directory_super_block, directory_entry, buffer_size);

		std::string src_name = src;
		if (src_name.back() == '/') {
			src_name.pop_back();
		}
		src_name = src_name.substr(src_name.find_last_of('/') + 1);

		std::string target_name = target;
		if (target_name.back() == '/') {
			target_name.pop_back();
		}
		target_name = target_name.substr(target_name.find_last_of('/') + 1);
		for (auto &entry : directory_entry) {
			if (entry.name == src_name) {
				entry.name = target_name;
				entry.name_len = target_name.length();
				break;
			}
		}

		if (src_inode.inode_type == S2EntryType::Directory) {
			// TODO: rename super block
		}

		std::vector<uint8_t> buffer;
		buffer.resize(buffer_size + (src_name.length() - target_name.length()));
		uint32_t buffer_index = DIR_SUPER_BLOCK_PLAIN_SIZE + directory_super_block.name_len;
		directory_super_block.serialize(&buffer[0]);

		for (auto &entry : directory_entry) {
			entry.serialize(&buffer[buffer_index]);
			buffer_index += DIR_BLOCK_PLAIN_SIZE + entry.name_len;
		}

		buffer.resize(buffer_index + 1);
		return WriteEntireFile({inode, ptr}, buffer);
	}

	IOStatus S2FileSystem::WriteEntireFile(const RichInode inode, const std::vector<uint8_t> &buffer) {
		uint32_t num_to_delete =
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
		}

		while (traversed_inode_vector.size() > 0 && num_to_delete > 0) { // remove the inode
			uint32_t nr_for_delete = std::min(
					num_to_delete,
					traversed_inode_vector.back().inode.num_disk_pointers); // how many to delete from the current inode
			num_to_delete -= nr_for_delete;
			traversed_inode_vector.back().inode.num_disk_pointers -= nr_for_delete;

			for (uint32_t i = 0; i < nr_for_delete; i++) {
				SetBlockType(traversed_inode_vector.back().inode.disk_pointers[i], BlockType::Unused);
			}

			if (traversed_inode_vector.back().inode.num_disk_pointers == 0) {
				if (traversed_inode_vector.size() < 2) {
					auto ret = WriteINode(traversed_inode_vector.back(), false);
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

				ret = WriteINode(traversed_inode_vector[traversed_inode_vector.size() - 1], false);
				if (!ret.ok()) {
					return ret;
				}
			} else {
				auto ret = WriteINode(traversed_inode_vector.back(), false);

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
		inode_c.size = buffer.size();
		ret = WriteINode({inode_c, inode.ptr}, false);
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
		INode inode;
		INodePointer ptr;
		auto ret = FindInode(dir, inode, ptr);
		if (!ret.ok()) {
			return ret;
		}
		std::vector<DirectoryEntry> dir_list;
		DirectorySuperBlock dir_sb;
		uint32_t temp;
		ret = ReadDirectory(inode, dir_sb, dir_list, temp);
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
		INode inode;
		INodePointer ptr;
		return FindInode(fname, inode, ptr);
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
		return output;
	}
} // namespace ROCKSDB_NAMESPACE