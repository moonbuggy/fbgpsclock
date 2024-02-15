#! /bin/sh

make && (sudo killall fbgpsclock; sudo dd if=/dev/zero of=/dev/fb1; sleep 1; sudo ./fbgpsclock)
