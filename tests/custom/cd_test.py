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
import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *
console = setup_tests()
curr_dir = os.getcwd()
sendline("mkdir test")
sendline("cd "+ curr_dir + "/test/")
sendline("pwd")
expect(curr_dir+"/test/")
test_success()


# store the cwd into a var, sendline mkdir, cd into that directory, check that cwd var = cwd var + /hw/