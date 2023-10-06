#pragma once

#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <map>
#include <mutex>
#include <string>
#include <memory>

#include <utils.h>

class LockManager {
public:
	LockManager() = default;

    static LockManager *get_instance() {
		if (manager == nullptr) {
			manager = new LockManager();
		}
		return manager;
	}

	void Lock(const std::string path) {
        return PerformAction(path, Action::ActionLock);
	}

	void Unlock(const std::string path) {
        return PerformAction(path, Action::ActionUnlock);
	}

	class WriteGuard {
	public:
		WriteGuard(const std::string path) : path(path) {
            LockManager::get_instance()->Lock(path);
        }
		~WriteGuard() { LockManager::get_instance()->Unlock(path); }

	private:
		std::string path;
	};

    static LockManager *manager;

private:
	class InternalWriteGuard {
	public:
		InternalWriteGuard(std::recursive_mutex &mtx) : mtx(mtx) { mtx.lock(); }
		~InternalWriteGuard() { mtx.unlock(); }

	private:
		std::recursive_mutex &mtx;
	};

    // class PathLock {
    // public:
    //     enum LockState {
    //         Locked,
    //         LockedShared,
    //         Unlocked
    //     };
    //     PathLock(const std::string path): path(path) {}
    //     LockState GetState() {
    //         std::lock_guard<std::mutex> g(access_mtx);
    //         if (read_locks == 0 && !exclusive_locked) {
    //             return LockState::Unlocked;
    //         }

    //         if (read_locks > 0) {
    //             return LockState::LockedShared;
    //         }

    //         return LockState::Locked;
    //     }
    //     void Lock() {
    //         while (true) {
    //             std::lock_guard<std::mutex> g(access_mtx);
    //             if (read_locks > 0 || exclusive_locked) {
    //                 continue;
    //             }

    //             exclusive_locked = true;
    //             rw_lock.lock();
    //             break;
    //         }
    //     }
    //     void Unlock() {
    //         std::lock_guard<std::mutex> g(access_mtx);
    //         exclusive_locked = false;
    //         rw_lock.unlock();
    //     }
    //     void LockShared() {
    //         while (true) {
    //             std::lock_guard<std::mutex> g(access_mtx);
    //             if (exclusive_locked) {
    //                 continue;
    //             }

    //             read_locks++;
    //             rw_lock.lock_shared();
    //             break;
    //         }
    //     }
    //     void UnlockShared() {
    //         std::lock_guard<std::mutex> g(access_mtx);
    //         read_locks--;
    //         rw_lock.unlock_shared();
    //     }

    // private:
    //     const std::string path;
    //     bool exclusive_locked = false;
    //     uint64_t read_locks = 0;
    //     std::shared_mutex rw_lock;
    //     std::mutex access_mtx;
    // };

    enum Action {
        ActionLock,
        ActionUnlock,
    };

    void PerformAction(const std::string path, const Action action) {
        std::lock_guard<std::mutex> g(locked_paths_access);
        switch (action) {
            case Action::ActionLock: {
                locked_paths[path].lock();
                break;
            }
            case Action::ActionUnlock: {
                locked_paths[path].unlock();
                break;
            }
        }
    }
    
	std::mutex locked_paths_access;
	std::map<std::string, std::recursive_mutex> locked_paths;
};

#endif