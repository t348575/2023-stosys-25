#!/bin/bash

log_zones=("3" "8" "16")
sizes=("1048576" "2097152")

start=`date +%s.%N`
for s in "${sizes[@]}"
do
    for l in "${log_zones[@]}"
    do
    sudo ./bin/m2 -l $l -s $s | grep Failed
    done
done
end=`date +%s.%N`

echo Took $( echo "$end - $start" | bc -l ) 