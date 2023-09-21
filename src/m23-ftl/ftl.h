#pragma once

#ifndef FTL_H
#define FTL_H

#include <cstdint>
#include <shared_mutex>
#include <type_traits>
#include <vector>

#include "../common/unused.h"
#include "zns_device.h"

template <class T> class RwLockedClass {
public:
	RwLockedClass() {}
	RwLockedClass(const RwLockedClass<T> &from) { UNUSED(from); }
	std::shared_mutex rw_lock;
};

template <class T, typename = std::enable_if_t<std::is_base_of_v<RwLockedClass<T>, T>>> class WriteGuard {
public:
	WriteGuard(T &_data) : data(_data) { data.rw_lock.lock(); };
	~WriteGuard() { data.rw_lock.unlock(); }

private:
	T &data;
};

template <class T, typename = std::enable_if_t<std::is_base_of_v<RwLockedClass<T>, T>>> class ReadGuard {
public:
	ReadGuard(T &_data) : data(_data) { data.rw_lock.lock_shared(); };
	~ReadGuard() { data.rw_lock.unlock_shared(); }

private:
	T &data;
};

enum status_flag { Valid = 0, Invalid = 2 };

struct ftl_record {
	uint64_t l_address, p_address;
	status_flag flag;
};

class LogRecord : public RwLockedClass<LogRecord> {
public:
	LogRecord(const uint64_t base_address, const uint64_t zone_size, const bool is_data_zone)
		: base_address(base_address), is_data_zone(is_data_zone) {
		logs.reserve(zone_size);
	};
	uint64_t base_address;
	bool is_data_zone;
	std::vector<ftl_record> logs;
};

class DataMap {
public:
	DataMap() = default;
	DataMap(const struct user_zns_device *_device, const struct device_data *_dd);

	bool find(const uint64_t address, ftl_record &record);
	void add_or_update(const ftl_record record);
	bool check_for_free_zones(ftl_record &record)

	std::vector<LogRecord> data;
	const struct user_zns_device *device;
	const struct device_data *dd;
};

#endif