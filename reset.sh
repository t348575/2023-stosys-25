#!/bin/bash
sudo rm -rf /tmp/shadowdb /tmp/testdb
sudo nvme zns reset-zone /dev/nvme0n1 -a