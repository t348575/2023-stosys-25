#!/bin/bash

files=("m23-ftl/zns_device" "m23-ftl/ftl" "common/utils")
for item in "${files[@]}"
do
    echo "formatting $item"
    clang-format -i --style=file src/$item.cpp
    clang-format -i --style=file src/$item.h
done

files=("S2FileSystem" "RocksClasses")
for item in "${files[@]}"
do
    echo "formatting $item"
    clang-format -i --style=file src/m45-rocksdb/$item.cc
    clang-format -i --style=file src/m45-rocksdb/$item.h
done
