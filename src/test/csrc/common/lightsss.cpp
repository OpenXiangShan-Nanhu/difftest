#include "lightsss.h"

static LightSSS *g_lightsss = nullptr;

static void fatal_signal_handler(int signo) {
  // On fatal signal, kill all fork children before dying
  if (g_lightsss) {
    g_lightsss->do_clear();
  }
  // Re-raise the signal with default handler to get proper core dump
  signal(signo, SIG_DFL);
  raise(signo);
}

ForkShareMemory::ForkShareMemory() {
  if ((key_n = ftok(".", 's') < 0)) {
    perror("Fail to ftok\n");
    FAIT_EXIT
  }

  if ((shm_id = shmget(key_n, 1024, 0666 | IPC_CREAT)) == -1) {
    perror("shmget failed...\n");
    FAIT_EXIT
  }

  void *ret = shmat(shm_id, NULL, 0);
  if (ret == (void *)-1) {
    perror("shmat failed...\n");
    FAIT_EXIT
  } else {
    info = (shinfo *)ret;
  }

  info->flag = false;
  info->notgood = false;
  info->endCycles = 0;
  info->oldest = 0;
}

ForkShareMemory::~ForkShareMemory() {
  if (shmdt(info) == -1) {
    perror("detach error\n");
  }
  shmctl(shm_id, IPC_RMID, NULL);
}

void ForkShareMemory::shwait() {
  while (true) {
    if (info->flag) {
      if (info->notgood)
        break;
      else
        exit(0);
    } else {
      sleep(WAIT_INTERVAL);
    }
  }
}

int LightSSS::do_fork() {
  // Register fatal signal handlers on first invocation so fork children
  // are cleaned up even when the parent dies from an assertion/SIGABRT.
  if (g_lightsss == nullptr) {
    g_lightsss = this;
    signal(SIGABRT, fatal_signal_handler);
    signal(SIGSEGV, fatal_signal_handler);
    signal(SIGBUS,  fatal_signal_handler);
    signal(SIGFPE,  fatal_signal_handler);
    signal(SIGILL,  fatal_signal_handler);
  }
  //kill the oldest blocked checkpoint process
  if (slotCnt == SLOT_SIZE) {
    pid_t temp = pidSlot.back();
    pidSlot.pop_back();
    kill(temp, SIGKILL);
    int status = 0;
    waitpid(temp, NULL, 0);
    slotCnt--;
  }
  // fork a new checkpoint process and block it
  if ((pid = fork()) < 0) {
    eprintf("[%d]Error: could not fork process!\n", getpid());
    return FORK_ERROR;
  }
  // the original process
  else if (pid != 0) {
    slotCnt++;
    pidSlot.push_front(pid);
    return FORK_OK;
  }
  // for the fork child
  waitProcess = 1;
  forkshm.shwait();
  //checkpoint process wakes up
  //start wave dumping
  if (forkshm.info->oldest != getpid()) {
    FORK_PRINTF("Error, non-oldest process should not live. Parent Process should kill the process manually.\n")
    return FORK_ERROR;
  }
  return FORK_CHILD;
}

int LightSSS::wakeup_child(uint64_t cycles) {
  forkshm.info->endCycles = cycles;
  forkshm.info->oldest = pidSlot.back();

  // only the oldest is wantted, so kill others by parent process.
  for (auto pid: pidSlot) {
    if (pid != forkshm.info->oldest) {
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
    }
  }
  // flush before wake up child.
  fflush(stdout);
  fflush(stderr);

  forkshm.info->notgood = true;
  forkshm.info->flag = true;
  int status = -1;
  waitpid(pidSlot.back(), &status, 0);
  return 0;
}

bool LightSSS::is_child() {
  return waitProcess;
}

int LightSSS::do_clear() {
  FORK_PRINTF("clear processes...\n")
  while (!pidSlot.empty()) {
    pid_t temp = pidSlot.back();
    pidSlot.pop_back();
    kill(temp, SIGKILL);
    waitpid(temp, NULL, 0);
    slotCnt--;
  }
  return 0;
}
