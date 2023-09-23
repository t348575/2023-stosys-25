#pragma once

#ifndef FTL_H
#define FTL_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <vector>

#include "../common/unused.h"
#include "zns_device.h"

enum status_flag { Valid = 0, Invalid = 2 };

struct ftl_record {
	uint64_t l_address, p_address, base_address;
	status_flag flag;
};

struct count_base_address_sums {
	uint64_t base_address, p_address, count;
};

class LogRecord {
public:
	LogRecord(const uint64_t p_address, const uint64_t zone_size, const struct device_data *dd)
		: p_address(p_address), dd(dd) {
		logs.reserve(zone_size);
	};
	uint64_t get_p_address() { return p_address; }
	std::vector<struct count_base_address_sums> count_base_addresses();
	std::vector<ftl_record> remove_base_address(const uint64_t base_address);

	uint64_t p_address, zone_size = 0;
	std::vector<ftl_record> logs;
	const struct device_data *dd;
};

class DataRecord {
public:
	DataRecord(const ftl_record record) : record(record) {}

	ftl_record record;
};

class DataMap {
public:
	void init(const struct user_zns_device *_device, const struct device_data *_dd);
	bool find(const uint64_t address, ftl_record &record);
	void add_or_update(const uint64_t idx, const ftl_record record);
	uint64_t get_free_log_zone(int &index);
	bool get_data_zone(const uint64_t l_address, ftl_record &record, uint64_t &idx);
	void set_data_zone(const uint64_t idx, const ftl_record record);
	void delete_if_exists(const uint64_t address);
	uint64_t get_log_p_address(uint64_t idx);

	std::vector<LogRecord> log_zones;
	std::vector<DataRecord> data_zones;
	const struct user_zns_device *device;
	const struct device_data *dd;
	std::mutex mtx;
	std::atomic<bool> in_action;
};

#endif