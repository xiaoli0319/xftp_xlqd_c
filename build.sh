#!/bin/bash
set -e
gcc main.c $(pkg-config --cflags --libs ncursesw libssh2) -lgcrypt -o xftp_xlqd
strip xftp_xlqd
echo "Binary size: $(stat -c%s xftp_xlqd) bytes"
