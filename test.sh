#!/bin/sh
for x in test/??
do
	./tsvm $x <${x}i >/tmp/.tsvm.out
	cmp -s ${x}o /tmp/.tsvm.out
	if test $? != 0
	then
		echo "$x: failed"
		cat /tmp/.tsvm.out
	fi
done
