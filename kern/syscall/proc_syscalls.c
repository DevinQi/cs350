#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>
#include <test.h>

	/* this implementation of sys__exit does not do anything with the exit code */
	/* this needs to be fixed to get exit() and waitpid() working properly */

#if OPT_A3
void _sys__exit(int exitcode) {
#else
void sys__exit(int exitcode) {
#endif // OPT_A3

	struct addrspace *as;
	struct proc *p = curproc;
	/* for now, just include this to keep the compiler from complaining about
		 an unused variable */
	(void)exitcode;

	DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

	KASSERT(curproc->p_addrspace != NULL);
	as_deactivate();
	/*
	 * clear p_addrspace before calling as_destroy. Otherwise if
	 * as_destroy sleeps (which is quite possible) when we
	 * come back we'll be calling as_activate on a
	 * half-destroyed address space. This tends to be
	 * messily fatal.
	 */
	as = curproc_setas(NULL);
	as_destroy(as);

#if OPT_A2
	DEBUG(DB_PROCSYS, "Syscall: _exit (Code %d)\n",exitcode);
	KASSERT(curproc->p_data != NULL);

	lock_acquire(procdata_lock);

	procdata_t *p_data = curproc->p_data;

	DEBUG(DB_PROCSYS, "Free PID (%d)\n", p_data->p_pid);
	//Free process id
	pid_use[p_data->p_pid] = false;

	procdata_t *child = p_data->p_firstchild;
	while(child != NULL) {
		procdata_t *next = child->p_nextsibling;
		//Process exited, cleanup
		if(child->p_exited) {
			procdata_destroy(child);
		}
		//Process still alive, inform parent death
		else {
			child->p_parent = NULL;
		}
		child = next;
	}
	//Detach all children
	p_data->p_firstchild = NULL;

	//If parent still alive
	if(p_data->p_parent) {
		//Save exitcode for parent processes
#if OPT_A3
		p_data->p_exit_code = exitcode;
#else
		p_data->p_exit_code = _MKWAIT_EXIT(exitcode);
#endif // OPT_A3

		//Signal process exit to waiting members
		p_data->p_exited = true;
		cv_broadcast(procdata_cv, procdata_lock);
	}
	//Parent exited
	else {
		procdata_destroy(p_data);
		curproc->p_data = NULL;
	}

	lock_release(procdata_lock);
#endif // OPT_A2

	/* detach this thread from its process */
	/* note: curproc cannot be used after this call */
	proc_remthread(curthread);

	/* if this is the last user process in the system, proc_destroy()
		 will wake up the kernel menu thread */
	proc_destroy(p);
	
	thread_exit();
	/* thread_exit() does not return, so we should never get here */
	panic("return from thread_exit in sys_exit\n");
}

#if OPT_A3

void sys__exit(int exitcode) {
	_sys__exit(_MKWAIT_EXIT(exitcode));
}

void terminate_exit(int sig) {
	_sys__exit(_MKWAIT_SIG(sig));
}

#endif // OPT_A3

/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
	DEBUG(DB_PROCSYS, "Syscall: getpid\n");
	KASSERT(curproc->p_data != NULL);
	lock_acquire(procdata_lock);
	DEBUG(DB_PROCSYS, "PID: %d\n", curproc->p_data->p_pid);
	*retval = curproc->p_data->p_pid;
	lock_release(procdata_lock);
#else
	/* for now, this is just a stub that always returns a PID of 1 */
	/* you need to fix this to make it work properly */
	*retval = 1;
#endif // OPT_A2
	return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
			userptr_t status,
			int options,
			pid_t *retval)
{
	int exitstatus;
	int result;
	
#if OPT_A2
	DEBUG(DB_PROCSYS, "Syscall: waitpid(%d)\n",pid);

	if (options != 0) {
		return(EINVAL);
	}

	//Validate pid
	if(pid < 0 || pid > PID_MAX) {
		DEBUG(DB_PROCSYS, "Invalid PID\n");
		return ESRCH;
	}

	lock_acquire(procdata_lock);

	//First, check if it's a child
	procdata_t *child = curproc->p_data->p_firstchild;
	while(child != NULL) {
		if(child->p_pid == pid) {
			break;
		}
		child = child->p_nextsibling;
	}
	//Child not found
	if(child == NULL || child->p_pid != pid) {
		if(pid_use[pid]) {
			lock_release(procdata_lock);
			return ECHILD;
		}
		else {
			lock_release(procdata_lock);
			return ESRCH;
		}
	}

	//Check if the child has exited. If not wait.
	while(!child->p_exited) {
		cv_wait(procdata_cv, procdata_lock);
	}

	exitstatus = child->p_exit_code;
	DEBUG(DB_PROCSYS, "Child (%d) exited (Code %d)\n", pid, exitstatus);
	
	lock_release(procdata_lock);
#else
	/* this is just a stub implementation that always reports an
		 exit status of 0, regardless of the actual exit status of
		 the specified process.   
		 In fact, this will return 0 even if the specified process
		 is still running, and even if it never existed in the first place.

		 Fix this!
	*/

	if (options != 0) {
		return(EINVAL);
	}
	/* for now, just pretend the exitstatus is 0 */
	exitstatus = 0;
#endif // OPT_A2

	result = copyout((void *)&exitstatus,status,sizeof(int));
	if (result) {
		return(result);
	}
	*retval = pid;
	return(0);
}

#if OPT_A2

int
sys_fork(pid_t *retval, struct trapframe *tf)
{
	DEBUG(DB_PROCSYS, "Syscall: fork\n");
	//Try to assign a PID
	lock_acquire(procdata_lock);

	int pid = procdata_find_free_pid(curproc->p_data);

	//No PID available
	if(pid < 0) {
		lock_release(procdata_lock);
		DEBUG(DB_PROCSYS, "No PID Available\n");
		*retval = -1;
		return ENPROC;
	}

	pid_use[pid] = true;

	lock_release(procdata_lock);

	DEBUG(DB_PROCSYS, "New PID: %d\n", pid);

	//Create proc structure
	struct proc *proc = proc_create_runprogram2(curproc->p_name);
	if(proc == NULL) {
		lock_acquire(procdata_lock);
		pid_use[pid] = false;
		lock_release(procdata_lock);
		*retval = -1;
		return ENOMEM;
	}

	//Create procdata structure
	procdata_t *procdata = procdata_create(pid, curproc->p_data);
	if(procdata == NULL) {
		proc_destroy(proc);
		lock_acquire(procdata_lock);
		pid_use[pid] = false;
		lock_release(procdata_lock);
		*retval = -1;
		return ENOMEM;
	}
	proc->p_data = procdata;

	//Clone the address space
	struct addrspace *as = NULL;
	as_copy(curproc->p_addrspace, &as);
	if (as == NULL) {
		proc_destroy(proc);
		procdata_destroy(procdata);
		lock_acquire(procdata_lock);
		pid_use[pid] = false;
		lock_release(procdata_lock);
		*retval = -1;
		return ENOMEM;
	}

	proc->p_addrspace = as;

	//Clone a trapframe for new thread
	struct trapframe *tf_copy = kmalloc(sizeof(struct trapframe));
	memcpy((void *)tf_copy, (const void*) tf, sizeof(struct trapframe));

	//Fork the thread
	int result = thread_fork(curthread->t_name, proc, sys_fork_new_process, tf_copy, 0);
	if (result) {
		kprintf("thread_fork failed: %s\n", strerror(result));
		proc_destroy(proc);
		procdata_destroy(procdata);
		as_destroy(as);
		kfree(tf_copy);
		lock_acquire(procdata_lock);
		pid_use[pid] = false;
		lock_release(procdata_lock);
		*retval = -1;
		return result;
	}

	//Return the pid
	*retval = procdata->p_pid;
	return 0;
}

void
sys_fork_new_process(void *ptr, unsigned long nargs)
{
	(void)nargs;
	//Activate our address space
	as_activate();

	//Fetch and copy the trapframe
	struct trapframe tf;
	struct trapframe *tf_copy = (struct trapframe *) ptr;
	memcpy((void *)&tf, (const void*) tf_copy, sizeof(struct trapframe));
	kfree(tf_copy);

	enter_forked_process(&tf);
}

int
sys_execv(int *retval, userptr_t program, userptr_t args)
{
	int result;

	//If we do return, an error must have occurred
	*retval = -1;

	//Copy arguments to kernel space first
	int argc = 0;
	char **argv = execv_copyin_args(program, args, &argc);

	if(argv == NULL) {
		return E2BIG;
	}
	
	result = runprogram(argc, argv, true);
	//Should not return to here
	runprogram_argv_destroy(argc, argv);
	return result;
}

char **
execv_copyin_args(userptr_t program, userptr_t args, int *argc_return)
{
	//Count argc
	int argc = 1; //(Start with 1 for program)
	char **args_ptr = (char **)args;
	while(*args_ptr != NULL) {
		argc++;
		args_ptr++;
	}

	//Try to allocate argv with argc+1 for the extra NULL pointer at the end
	char **argv = kmalloc(sizeof(char *) * (argc + 1));
	if(argv == NULL) {
		return NULL;
	}

	//Copy in program
	argv[0] = kstrdup((const char*)program);
	if(argv[0] == NULL) {
		kfree(argv);
		return NULL;
	}

	//Zero pointers in case of error, including argv[argc]
	for(int i = 1; i < argc + 1; i++) {
		argv[i] = NULL;
	}

	//Copy in args
	bool fail = false;
	args_ptr = (char **)args;
	for(int i = 1; i < argc; i++) {
		argv[i] = kstrdup((const char*)args_ptr[i - 1]);
		//Stop if failed
		if(argv[i] == NULL) {
			fail = true;
			break;
		}
	}
	if(fail) {
		runprogram_argv_destroy(argc, argv);
		return NULL;
	}

	//By now, argc and argv should be setup properly

	//Store argc
	*argc_return = argc;
	//Return argv
	return argv;
}

#endif // OPT_A2
