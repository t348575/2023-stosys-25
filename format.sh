#!/bin/bash

files=("zns_device.cpp" "zns_device.h" "ftl.cpp" "ftl.h")
for item in "${files[@]}"
do
    echo "formatting $item"
    clang-format -i --style=file src/m23-ftl/$item
done