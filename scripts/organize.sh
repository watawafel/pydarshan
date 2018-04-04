#!/bin/bash

	num_dirs=$(find . -maxdepth 1 -type d | wc -l)
num_dirs=$(echo $(( num_dirs - 1 )))

#echo "Number of Directories: $num_dirs"

part=$(($num_dirs / 4))
#echo $part

node_rank=$((ALPS_APP_PE % 4))

#echo $node_rank

#for d in */ ; do
#    echo "$d"
#done


#ls -1 -d */ | split --lines=1

array=( $( ls  -d */ ) )

#	for d in  */; 
#	do  
	if (($node_rank == 0));
	then
#		for d in ${array[@]::$part}; do echo $d "0" ;done
	for d in ${array[@]::$part}; do cd $d; find . -name '*.tar.gz' -exec tar -xzvf {} \; ;cd ..; done
# 	find . -name '*.tar.gz' -exec tar -xzvf {} \;
	fi


	if (($node_rank== 1));
	then

#		for d in ${array[@]:$part:$part}; do echo $d "1"; done
	for d in ${array[@]:$part:$part}; do cd $d; find . -name '*.tar.gz' -exec tar -xzvf {} \; ;cd ..; done
#	find . -name '*.tar.gz' -exec tar -xzvf {} \;
	fi

	if (($node_rank == 2));
	then
#		for d in ${array[@]:$(($part * 2)):$part}; do echo $d "2"; done
	for d in ${array[@]:$(($part * 2)):$part}; do cd $d; find . -name '*.tar.gz' -exec tar -xzvf {} \; ;cd ..; done
#       find . -name '*.tar.gz' -exec tar -xzvf {} \;

	fi


	if (($node_rank== 3));
	then
#		for d in ${array[@]:$(($part * 3))}; do echo $d "3"; done
	for d in ${array[@]:$(($part * 3))}; do cd $d; find . -name '*.tar.gz' -exec tar -xzvf {} \; ;cd ..; done
#       find . -name '*.tar.gz' -exec tar -xzvf {} \;

	fi


