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

    std::unique_ptr<rdb::FSWritableFile> wr_file;
    if (!z.NewWritableFile("/a/c", rdb::FileOptions(), &wr_file, nullptr).ok()) {
        std::cout << "Could not create writeable file" << std::endl;
    }

    auto const num_action = 58344;

    std::vector<uint8_t> buffer;
    for (uint32_t i = 0; i < num_action; i++) {
        buffer.push_back(i % 0xFF);
    }

    auto s = rdb::Slice((char*)&buffer[0], num_action);
    wr_file->Append(s, rdb::IOOptions(), nullptr);
    wr_file->Close(rdb::IOOptions(), nullptr);

    std::vector<char> temp_buffer;
    temp_buffer.resize(num_action);
    z.NewSequentialFile("/a/c", rdb::FileOptions(), &file, nullptr);

    rdb::Slice slice;
    if (!file->Read(num_action, rdb::IOOptions(), &slice, &temp_buffer[0], nullptr).ok()) {
        std::cout << "Could not read" << std::endl;
    }

    for (uint32_t i = 0; i < num_action; i++) {
        if ((uint8_t)*(slice.data() + i) != (uint8_t)(i % 0xFF)) {
            std::cout << "Not matching " << std::to_string(i) << " " << std::to_string((uint8_t)*(slice.data() + i)) << " " << std::to_string(i % 0xFF) << std::endl;
        }
    }
    std::cout << "All done!" << std::endl;
    return 0;
}