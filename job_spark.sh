#!bin/bash

qsub -I -l nodes=4:ppn=32,gres=shifter16,walltime=02:00:00 -v UDI="lgerhardt/spark-2.1.1:v1 -v /dsl/opt/bwpy:/opt/bwpy"
module use /mnt/a/u/eot/borcean2/apps/spark/2.1.1/modulefiles
module load spark-2.1.1
module show spark-2.1.1

alps-start-master.sh
wait 120
alps-start-slaves.sh

#jupyter-notebook --no-browser
