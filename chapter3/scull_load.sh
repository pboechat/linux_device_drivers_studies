#!/bin/sh

/sbin/insmod ./scull.ko $* || exit 1

rm -f /dev/scull[0-3]

major=$(awk "\$2==\"scull\" {print \$1}" /proc/devices)

echo MAJOR=$major

mknod /dev/scull0 c $major 0
mknod /dev/scull1 c $major 1
mknod /dev/scull2 c $major 2
mknod /dev/scull3 c $major 3

group="staff"
grep -q "^staff:" /etc/group || group="wheel"

chgrp $group /dev/scull[0-3]
chmod 666 /dev/scull[0-3]