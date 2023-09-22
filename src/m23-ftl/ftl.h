#pragma once

#ifndef FTL_H
#define FTL_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <vector>

#include "../common/unused.h"
#include "zns_device.h"

template <class T> class RecursiveRwLock {
public:
	RecursiveRwLock() {}
	RecursiveRwLock(const RecursiveRwLock<T> &from) { UNUSED(from); }

	void lock() {
		mtx.lock();
		if (write_locks == 0) {
			rw_lock.lock();
		}
		write_locks++;
	}

	void unlock() {
		mtx.unlock();
		if (write_locks == 1) {
			rw_lock.unlock();
		}
		write_locks--;
	}

	void lock_shared() {
		std::lock_guard<std::recursive_mutex> lk(mtx);
		rw_lock.lock_shared();
	}

	void unlock_shared() {
		std::lock_guard<std::recursive_mutex> lk(mtx);
		rw_lock.unlock_shared();
	}

private:
	std::shared_mutex rw_lock;
	uint64_t write_locks = 0;
	std::recursive_mutex mtx;
};

template <class T, typename = std::enable_if_t<std::is_base_of_v<RecursiveRwLock<T>, T>>> class WriteGuard {
public:
	WriteGuard(T &_data) : data(_data) { data.lock(); };
	~WriteGuard() { data.unlock(); }

private:
	T &data;
};

template <class T, typename = std::enable_if_t<std::is_base_of_v<RecursiveRwLock<T>, T>>> class ReadGuard {
public:
	ReadGuard(T &_data) : data(_data) { data.lock_shared(); };
	~ReadGuard() { data.unlock_shared(); }

private:
	T &data;
};

enum status_flag { Valid = 0, Invalid = 2 };

struct ftl_record {
	uint64_t l_address, p_address, base_address;
	status_flag flag;
};

struct sums {
	uint64_t base_address, p_address, count;
};

class LogRecord : public RecursiveRwLock<LogRecord> {
public:
	LogRecord(const uint64_t p_address, const uint64_t zone_size, const struct device_data *dd)
		: p_address(p_address), dd(dd) {
		logs.reserve(zone_size);
	};
	uint64_t get_p_address() { return p_address; }
	std::vector<struct sums> count_base_addresses();
	std::vector<ftl_record> remove_base_address(const uint64_t base_address);

	uint64_t p_address;
	std::vector<ftl_record> logs;
	const struct device_data *dd;
};

class DataRecord : public RecursiveRwLock<DataRecord> {
public:
	DataRecord(const ftl_record record) : record(record) {}

	ftl_record record;
};

class DataMap {
public:
	DataMap() = default;
	DataMap(const struct user_zns_device *_device, const struct device_data *_dd);

	bool find(const uint64_t address, ftl_record &record);
	void add_or_update(const uint64_t idx, const ftl_record record);
	uint64_t get_free_log_zone(int &index);
	bool get_data_zone(const uint64_t l_address, ftl_record &record, bool &reset_zone, uint64_t &idx);
	uint64_t get_log_p_address(uint64_t idx);

	std::vector<LogRecord> log_zones;
	std::vector<DataRecord> data_zones;
	const struct user_zns_device *device;
	const struct device_data *dd;
};

#endif