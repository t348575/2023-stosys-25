#pragma once

#ifndef FTL_H
#define FTL_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "../common/unused.h"
#include "zns_device.h"

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
	LogRecord(const uint64_t p_address, const uint64_t zone_size, const bool is_seq)
		: p_address(p_address), write_ptr(p_address), is_seq(is_seq) {
		logs.reserve(zone_size);
	};
	std::vector<struct count_base_address_sums> count_base_addresses();
	std::vector<ftl_record> remove_base_address(const uint64_t base_address, const bool simulate);

	uint64_t p_address, zone_size = 0, write_ptr;
	bool is_seq;
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
		if (records == nullptr) {
			records = new DataMap();
		}
		return records;
	}

	void init(const struct user_zns_device *_device, const struct device_data *_dd);
	bool find_in_log(const uint64_t address, ftl_record &record);
	bool find_in_data(const uint64_t address, ftl_record &record);
	void add_or_update(const uint64_t idx, const ftl_record record);
	uint64_t get_free_log_zone(int &index);
	uint64_t count_free_log_pages();
	bool get_data_zone(const uint64_t l_address, ftl_record &record, uint64_t &idx);
	bool remove_unused_data_zone(const uint64_t base_address, ftl_record &record);
	void set_data_zone(const uint64_t idx, const ftl_record record);
	void delete_from_log_if_exists(const uint64_t address);
	std::vector<struct count_base_address_sums> count_base_addresses();

	std::vector<LogRecord> log_zones;
	std::vector<DataRecord> data_zones;
	const struct user_zns_device *device;
	const struct device_data *dd;
	std::shared_mutex data_mtx, log_mtx;
	std::atomic<uint64_t> write_counter;
	bool should_run_gc = true;
	bool gc_exited = false;
	static DataMap *records;
};

#endif