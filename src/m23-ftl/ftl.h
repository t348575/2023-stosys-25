#include <shared_mutex>
#include <functional>
#include <vector>

#ifndef FTL_H
#define FTL_H

enum status_flag { Valid = 0, Erased = 1, Invalid = 2 };

struct ftl_record {
	uint64_t l_address, p_address, write_ptr;
	status_flag flag;
};

template <class T, class S> class FtlTable {
public:
	void reset();
	void push_back(T record);
	void push_back(uint index, S record);
	bool has(uint64_t address);
	void reserve(uint64_t size);
	void set_zone_info(uint32_t zone_size) { this->zone_size = zone_size; }
	void set_invalid(int index);
	void clear_index(int index);
	void remove_index(int index);
	uint64_t update_write_ptr(int index);
	void reset_write_ptr(uint index);
	int64_t insertion_pos(uint index, uint64_t l_address);
	uint64_t size_at(uint index);
	uint32_t get_zone_size() { return zone_size; }
	uint32_t table_index_size(uint index);
	void record_transaction(std::function<void(std::vector<T> &table)> f);

	typename std::vector<T>::iterator end() {
		auto guard = ReadGuard(*this);
		return table.end();
	}
	typename std::vector<T>::iterator begin() {
		auto guard = ReadGuard(*this);
		return table.end();
	}
	int get_unused_block();
	void set(const uint index, const T record);
	T get(const uint index);
	int get_record(const int index, const uint64_t l_address, S &result, uint &result_index);
	int does_base_address_exist(const uint64_t &address, const struct user_zns_device *my_dev);

private:
	class WriteGuard {
	public:
		WriteGuard(FtlTable &ftl) : ftl(ftl) { ftl.rw_lock.lock(); };
		~WriteGuard() { ftl.rw_lock.unlock(); }

	private:
		FtlTable &ftl;
	};

	class ReadGuard {
	public:
		ReadGuard(FtlTable &ftl) : ftl(ftl) { ftl.rw_lock.lock_shared(); };
		~ReadGuard() { ftl.rw_lock.unlock_shared(); }

	private:
		FtlTable &ftl;
	};

	std::vector<T> table;
	uint32_t zone_size;
	std::shared_mutex rw_lock;
};

#endif