#!/bin/bash
sudo rm -rf /tmp/shadowdb
sudo nvme zns reset-zone /dev/nvme0n1 -a
sudo rm write write_confirm