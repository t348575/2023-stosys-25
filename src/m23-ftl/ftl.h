#pragma once

#ifndef FTL_H
#define FTL_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <vector>
#include <thread>

#include "../common/unused.h"
#include "zns_device.h"

#define PREFER_SEQ_LOG
#define USE_CACHE
#define READ_CACHE_SIZE 64

enum status_flag : uint8_t { Valid = 0, Invalid = 2 };

struct ftl_record {
	uint64_t l_address, p_address, base_address;
	status_flag flag;
};

struct count_base_address_sums {
	uint64_t base_address, p_address, count;
};

class LogRecord {
public:
	LogRecord(const uint64_t p_address, const uint64_t zone_size) : p_address(p_address), write_ptr(p_address) {
		logs.reserve(zone_size);
	};
	std::vector<struct count_base_address_sums> count_base_addresses();
	std::vector<ftl_record> remove_base_address(const uint64_t base_address, const bool simulate);

	uint64_t p_address, zone_size = 0, write_ptr, base_address;
	bool is_seq = false;
	std::vector<ftl_record> logs;
};

class DataRecord {
public:
	DataRecord(const ftl_record record) : record(record) {}

	ftl_record record;
};

class DataMap {
public:
	static DataMap *get_instance() {
		if (records.get() == nullptr) {
			records.reset(new DataMap());
		}
		return records.get();
	}

	~DataMap() {
		auto const records = DataMap::get_instance();
		records->should_run_gc = false;
		while (!records->gc_exited) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		write_metadata();
		free(device->_private);
		free(device);
	}

	void init(struct user_zns_device *_device, const struct device_data *_dd);
	bool find_in_log(const uint64_t address, ftl_record &record);
	bool find_in_data(const uint64_t address, ftl_record &record);
	void add_or_update(const uint64_t idx, const ftl_record record);
	uint64_t get_free_log_zone(const uint64_t address, int &index);
	uint64_t count_free_log_pages();
	bool get_data_zone(const uint64_t l_address, ftl_record &record, uint64_t &idx);
	bool remove_unused_data_zone(const uint64_t base_address, ftl_record &record);
	void set_data_zone(const uint64_t idx, const ftl_record record);
	void delete_from_log_if_exists(const uint64_t address);
	void write_metadata();
	void read_metadata();
	std::vector<struct count_base_address_sums> count_base_addresses();

	std::vector<LogRecord> log_zones;
	std::vector<DataRecord> data_zones;
	struct user_zns_device *device;
	const struct device_data *dd;
	std::shared_mutex data_mtx, log_mtx;
	std::atomic<uint64_t> write_counter;
	bool gc_running = false;
	bool should_run_gc = true;
	bool gc_exited = false;
	static std::shared_ptr<DataMap> records;

	struct ReadCacheEntry {
		std::vector<uint8_t> buffer;
		uint64_t address;
		uint64_t access_count = 0;
		bool is_valid = false;
	};
	std::array<ReadCacheEntry, READ_CACHE_SIZE> read_cache;
	void init_cache() {
		for (uint32_t i = 0; i < READ_CACHE_SIZE; i++) {
			ReadCacheEntry r = {.buffer = std::vector<uint8_t>(), .address = 0, .is_valid = false};
			r.buffer.resize(device->lba_size_bytes);
			read_cache[i] = r;
		}
	}
	void cache(const uint64_t address, const char *buffer) {
#ifdef USE_CACHE
		for (uint32_t i = 0; i < READ_CACHE_SIZE; i++) {
			if (read_cache[i].is_valid && read_cache[i].address == address) {
				memcpy(&read_cache[i].buffer[0], buffer, device->lba_size_bytes);
				return;
			}
		}

		uint32_t i = 0;
		uint32_t smallest_index = 0;
		uint64_t smallest = std::string::npos;
		for (; i < READ_CACHE_SIZE; i++) {
			if (read_cache[i].access_count < smallest) {
				smallest = read_cache[i].access_count;
				smallest_index = i;
			}

			if (!read_cache[i].is_valid) {
				read_cache[i].address = address;
				read_cache[i].access_count = 0;
				read_cache[i].is_valid = true;
				memcpy(&read_cache[i].buffer[0], buffer, device->lba_size_bytes);
				return;
			}
		}

		read_cache[smallest_index].address = address;
		read_cache[smallest_index].access_count = 0;
		memcpy(&read_cache[smallest_index].buffer[0], buffer, device->lba_size_bytes);
#endif
	}
	bool get_from_cache(const uint64_t address, char *buffer) {
#ifdef USE_CACHE
		for (uint32_t i = 0; i < READ_CACHE_SIZE; i++) {
			if (read_cache[i].is_valid && read_cache[i].address == address) {
				memcpy(buffer, &read_cache[i].buffer[0], device->lba_size_bytes);
				return true;
			}
		}
#endif
		return false;
	}
};

#endif
