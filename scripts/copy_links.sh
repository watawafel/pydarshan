#/bin/bash

#!/bin/bash.exe

for link in $(find /u/eot/borcean2/apps/pydarshan/scripts/ -type l)
do
  loc=$(dirname $link)
  dir=$(readlink $link)
  mv $dir $loc
  rm $link
done
