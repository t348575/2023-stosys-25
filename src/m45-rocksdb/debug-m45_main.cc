#include <memory>
#include <iostream>

#include "S2FileSystem.h"

namespace rdb = ROCKSDB_NAMESPACE;

int main() {
    rdb::S2FileSystem z = rdb::S2FileSystem("s2fs:nvme0n1:///tmp/testdb/", true);
    std::unique_ptr<rdb::FSSequentialFile> file;
    z.CreateDirIfMissing("/a", rdb::IOOptions(), nullptr);
    z.CreateDir("/a", rdb::IOOptions(), nullptr);
    z.CreateDirIfMissing("/b", rdb::IOOptions(), nullptr);
    z.CreateDirIfMissing("/a/b", rdb::IOOptions(), nullptr);
    z.NewSequentialFile("/a/c", rdb::FileOptions(), &file, nullptr);
    z.NewSequentialFile("/a/b/c", rdb::FileOptions(), &file, nullptr);
    return 0;
}