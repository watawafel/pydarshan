#!/bin/bash

# while file.out not empty 
# run again 


until [ -e *.err ] && [ -s *.out ] ; 
do	
	mv *.err error_logs/
	echo "waiting for new .out file"
	while [ ! -f *.err ]; do sleep 1; done
	qsub logs.pbs
	qsub logs.pbs	
done



#while [ ! -f *.err ] do sleep 1; done

#if [ -e *.err ] && [ -s *.err ]; then
#echo "exists and not empty"
#else
#echo "not exist or empty";
#fi


#if [ -e *.err ]
#then
#  if [ -s *.err ]
#    then 
#      echo "not empty"
#    else
#      echo "empty"
#   fi
#else
#echo "not exist"
#fi
