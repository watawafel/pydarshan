#!/bin/bash
module use /mnt/a/u/eot/borcean2/apps/spark/2.1.1/modulefiles

module load spark-2.1.1

module show spark-2.1.1

alps-start-master.sh
sleep 120
alps-start-slaves.sh
