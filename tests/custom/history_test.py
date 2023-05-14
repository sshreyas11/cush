#!/usr/bin/python
#
# Ctrl-C Test: Start a shell, send SIGINT, run a program, 
#              send SIGINT, then exit
#
# Requires the following commands to be implemented
# or otherwise usable:
#
#    sleep, ctrl-c control
#

# def test_cd():

#     #sendline("cd")
#     assert os.readlink(cwd_path) == '/home/ugrads/majors/rishikas/3214/cs3214-cush/src'
import sys, imp, atexit, pexpect, proc_check, signal, time, threading, subprocess
from testutils import *
console = setup_tests()
sendline("cd")
curr = os.getcwd()
sendline("pwd")
sendline("history")
expect("1 cd")
expect("2 pwd")
expect("3 history")
test_success()


# Run the 'history' command and capture its output
#result = subprocess.run(['history'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)
