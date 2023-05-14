Student Information
-------------------

Rishika Surineni <rishikas>
Shreyas Sakhalkar <shreyas7>

How to execute the shell
------------------------
<describe how to execute from the command line>
Important Notes
---------------
<Any important notes about your system>
Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:

jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

\ ^C : When the user hits ctrl+C a the status of that job throws a WIFSIGNALED signal
which is dealt with in handle_child_status function. We print to the stderr the strsignal which
basically helps in printing the message of the process when stopped. We also reduce the num_processes_alive 
of the job struct as the job is no longer running and is interrupted or stopped.

\ ^Z :  When the user hits ctrl+Z a the status of that job throws a WIFSTOPPED signal
which is dealt with in handle_child_status function. The job_status sends a SIGSTP signal and in that
we change the status of the job to STOPPED. We save the termstate of the job so that if we bring it to foreground
it can keep on running from where it was stoppd. We print the job stopped and finally we return the terminal
back to shell so that it puts the job to background while stopped and we can continue working on 
the terminal.

exit: If the command is 'exit', the program will exit cush.

jobs : If the command is 'jobs', the program traverses
through the list of jobs and prints the current job if it exists 
and if the status is not foreground.

stop: If the command is 'stop', the program first converts the second
agrument in the commands pipeline to an integer. Then we used the provided
get_job_from_jib(int n) method to get the job of the command. If a job 
exists, the job will be stopped using SIGSTOP, and otherwise, print a 
'job not found' line.

kill : If the command is 'kill', we will again, get the integer representation
of the second argument to get the job from the jid. Similar to kill, we 
make sure a job exists, and if yes - then terminate the job with SIGTERM, 
and if not - print job not found message.

bg : Similar to the other builtins, we use the integer conversion of the 
command to get the current job in the job list. Then we change the status
of the current job to BACKGROUND. Then we print the job in them background,
and continue the job in the background and save the terminal state for the job.

fg : After making a job from the given command line prompt, we change the status
to FOREGROUND. Then, we print the current job pipeline. We save the pdig 
into list of pids. Then we continue the job, and give the terminal background
back to the shell after the job is complete.



Description of Extended Functionality
-------------------------------------
<describe your IMPLEMENTATION of the following functionality:
I/O, Pipes, Exclusive Access > 
--IO--
IO, piping and exclusivae access is a part of the posix spawn. For IO redirections we
the pipeline's iored_input for not being NULL and then we read from the iored_input
We also check if the count = 0 as that tells us that it is the first time we 
are running the non-built in command. The count helps us keep track of the first iteration.
IO uses addopen function to open the file and gives it read only permissions. 
For output we check the iored_output for not being null and the count to be at the last command 
in the pipeline or the last iteration. Then we try to append the data if the file is open otherwise
we Truncate a new file and output the data to a new file.

--Piping and exclusive acess--
For piping we initially make the pipe array before we loop through the commands in the 
pipeline. Then in posix_spawn we check the size to be greater than 1. The size variable stores the size 
of the commands list in each pipeline. This is done so that piping is ignored if a singular command is 
passed. We then check that count_pipes function to check if it is in the first pipe, last pipe or one of the 
middle ones. This is done to connect the pipe terminals correctly so that the input and output from one pipe 
to another is seemless. We use the addup2 function to connect the pipe ends.
Finally we increment the count_pipes everytime we close a pipe to keep track of the pipes,
We close all the pipes outside the for loop for commands list in the pipes.




List of Additional Builtins Implemented
---------------------------------------
(Written by Your Team)
cd : The first additional builtin command we implemented was 'cd'.

The cd command changes the directory that the user is currently in. 
Our approach: when the command is 'cd', the program checks the second argument 
given. If there is no second argument, the directory changes into the home
directory using the "chdir" command. This command is the c call for cd. 
If there is an argument given, we chdir into whichever argument that was given.

history : The second additional builtin command we implemented was 'history'.

The history command prints an array of commands in order of first used to most 
recently used. Our implementation: we defined and initialized a 2d array of 
commands that have each command entered by the user and the max length of a 
single command. We create a helper method to add each command to the array. 
Then, when we check to see if a command is 'history' we iterate through the array 
and print each command plus it's position in the array.
message.txt
4 KB