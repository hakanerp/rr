/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_REPLAY_SESSION_H_
#define RR_REPLAY_SESSION_H_

#include "CPUIDBugDetector.h"
#include "EmuFs.h"
#include "Session.h"

struct syscallbuf_hdr;

/**
 * The state of a (dis)arm-desched-event ioctl that's being processed.
 */
enum ReplayDeschedType {
  DESCHED_ARM,
  DESCHED_DISARM
};
enum ReplayDeschedEnterExit {
  DESCHED_ENTER,
  DESCHED_EXIT
};
struct ReplayDeschedState {
  /* Is this an arm or disarm request? */
  ReplayDeschedType type;
  /* What's our next step to retire the ioctl? */
  ReplayDeschedEnterExit state;
};

/**
 * The state of a syscallbuf flush that's being processed.  Syscallbuf
 * flushes are an odd duck among the trace-step types (along with the
 * desched step above), because they must maintain extra state in
 * order to know which commands to issue when being resumed after an
 * interruption.  So the process of flushing the syscallbuf will
 * mutate this state in between attempts to retire the step.
 */
enum ReplayFlushBufferedSyscallStep {
  FLUSH_START,
  FLUSH_ARM,
  FLUSH_ENTER,
  FLUSH_EXIT,
  FLUSH_DISARM,
  FLUSH_DONE
};
/**
 * ReplayFlushBufferedSyscallState is saved in Session and cloned with its
 * Session, so it needs to be simple data, i.e. not holding pointers to
 * per-Session data.
 */
struct ReplayFlushBufferedSyscallState {
  /* True when we need to write the syscallbuf data back to
   * the child. */
  bool need_buffer_restore;
  /* After the data is restored, the number of record bytes that
   * still need to be flushed. */
  size_t num_rec_bytes_remaining;
  /* The offset of the next syscall record in both the rr and child
   * buffers */
  size_t syscall_record_offset;
  /* The next step to take. */
  ReplayFlushBufferedSyscallStep state;
  /* Track the state of retiring desched arm/disarm ioctls, when
   * necessary. */
  ReplayDeschedState desched;
};

/**
 * Describes the next step to be taken in order to replay a trace
 * frame.
 */
enum ReplayTraceStepType {
  TSTEP_NONE,

  /* Frame has been replayed, done. */
  TSTEP_RETIRE,

  /* Enter/exit a syscall.  |syscall| describe what should be
   * done at entry/exit. */
  TSTEP_ENTER_SYSCALL,
  TSTEP_EXIT_SYSCALL,

  /* Advance to the deterministic signal |signo|. */
  TSTEP_DETERMINISTIC_SIGNAL,

  /* Advance until |target.ticks| have been retired and then
   * |target.ip| is reached. */
  TSTEP_PROGRAM_ASYNC_SIGNAL_INTERRUPT,

  /* Deliver signal |signo|. */
  TSTEP_DELIVER_SIGNAL,

  /* Replay the upcoming buffered syscalls.  |flush| tracks the
   * replay state.*/
  TSTEP_FLUSH_SYSCALLBUF,

  /* Emulate arming or disarming the desched event.  |desched|
   * tracks the replay state. */
  TSTEP_DESCHED,
};

enum ExecOrEmulate {
  EXEC = 0,
  EMULATE = 1
};

enum ExecOrEmulateReturn {
  EXEC_RETURN = 0,
  EMULATE_RETURN = 1
};

/**
 * rep_trace_step is saved in Session and cloned with its Session, so it needs
 * to be simple data, i.e. not holding pointers to per-Session data.
 */
struct ReplayTraceStep {
  ReplayTraceStepType action;

  union {
    struct {
      /* The syscall number we expect to
       * enter/exit. */
      int number;
      /* Is the kernel entry and exit for this
       * syscall emulated, that is, not executed? */
      ExecOrEmulate emu;
      /* The number of outparam arguments that are
       * set from what was recorded.
       * Only used when action is TSTEP_EXIT_SYSCALL. */
      ssize_t num_emu_args;
      /* Nonzero if the return from the syscall
       * should be emulated.  |emu| implies this. */
      ExecOrEmulateReturn emu_ret;
    } syscall;

    int signo;

    struct {
      Ticks ticks;
      int signo;
    } target;

    ReplayFlushBufferedSyscallState flush;

    ReplayDeschedState desched;
  };
};

/** Encapsulates additional session state related to replay. */
class ReplaySession : public Session {
public:
  typedef std::shared_ptr<ReplaySession> shr_ptr;

  ~ReplaySession();

  /**
   * Return a semantic copy of all the state managed by this,
   * that is the entire tracee tree and the state it depends on.
   * Any mutations of the returned Session can't affect the
   * state of this, and vice versa.
   *
   * This operation is also called "checkpointing" the replay
   * session.
   */
  shr_ptr clone();

  /**
   * Like |clone()|, but return a session in "diversion" mode,
   * which allows free execution.  See diverter.h.
   */
  shr_ptr clone_diversion();

  EmuFs& emufs() { return *emu_fs; }

  /** Collect garbage files from this session's emufs. */
  void gc_emufs();

  TraceReader& trace_reader() { return trace_in; }

  /**
   * True when this is an diversion session; see diversioner.h.
   */
  bool diversion() const { return is_diversion; }

  /**
   * The trace record that we are working on --- the next event
   * for replay to reach.
   */
  const TraceFrame& current_trace_frame() const { return trace_frame; }

  /**
   * The Task for the current trace record.
   */
  Task* current_task() const { return find_task(trace_frame.tid()); }

  /**
   * Set |tgid| as the one that's being debugged in this
   * session.
   *
   * Little hack: technically replayer doesn't know about the
   * fact that debugger_gdb hides all but one tgid from the gdb
   * client.  But to recognize the last_task below (another
   * little hack), we need to known when an exiting thread from
   * the target task group is the last.
   */
  void set_debugged_tgid(pid_t tgid) {
    assert(0 == tgid_debugged);
    tgid_debugged = tgid;
  }
  pid_t debugged_tgid() const { return tgid_debugged; }

  /**
   * If we've finished replaying (all tracees terminated), return the last
   * Task that ran. Sometimes debuggers need this. Returns null if replay
   * hasn't finished yet.
   */
  Task* last_task() { return last_debugged_task; }

  /**
   * Create a replay session that will use the trace specified
   * by the commad-line args |argc|/|argv|.  Return it.
   */
  static shr_ptr create(int argc, char* argv[]);

  enum ReplayStatus {
    // Some execution was replayed. replay_step() can be called again.
    REPLAY_CONTINUE,
    // All tracees are dead. replay_step() should not be called again.
    REPLAY_EXITED
  };
  enum StepBreakReason {
    BREAK_NONE,
    // A requested RUN_SINGLESTEP completed.
    BREAK_SINGLESTEP,
    // We hit a breakpoint.
    BREAK_BREAKPOINT,
    // We hit a watchpoint.
    BREAK_WATCHPOINT,
    // We hit a signal.
    BREAK_SIGNAL
  };
  struct ReplayResult {
    ReplayStatus status;
    // When status == STEP_CONTINUE
    StepBreakReason break_reason;
    // When break_reason is not BREAK_NONE, the triggering Task.
    Task* break_task;
    // When break_reason is BREAK_SIGNAL, the signal.
    int break_signal;
    // When break_reason is BREAK_WATCHPOINT, the triggering watch address.
    remote_ptr<void> break_watch_address;
    // When status == STEP_EXITED. -1 means abnormal termination.
    int exit_code;
  };
  enum RunCommand {
    RUN_CONTINUE,
    RUN_SINGLESTEP
  };
  ReplayResult replay_step(RunCommand command = RUN_CONTINUE);

  virtual ReplaySession* as_replay() { return this; }

  virtual TraceStream& trace() { return trace_in; }

private:
  ReplaySession(const std::string& dir)
      : is_diversion(false),
        last_debugged_task(nullptr),
        tgid_debugged(0),
        trace_in(dir),
        trace_frame(),
        current_step() {
    advance_to_next_trace_frame();
    emu_fs = EmuFs::create();
  }

  ReplaySession(const ReplaySession& other)
      : is_diversion(false),
        last_debugged_task(nullptr),
        tgid_debugged(0),
        trace_in(other.trace_in),
        trace_frame(),
        current_step() {}

  /**
   * Set |t| as the last (debugged) task in this session.
   *
   * When we notify the debugger of process exit, it wants to be
   * able to poke around at that last task.  So we store it here
   * to allow processing debugger requests for it later.
   */
  void set_last_task(Task* t) {
    assert(!last_debugged_task);
    last_debugged_task = t;
  }

  const struct syscallbuf_hdr* syscallbuf_flush_buffer_hdr() {
    return (const struct syscallbuf_hdr*)syscallbuf_flush_buffer_array;
  }

  void setup_replay_one_trace_frame(Task* t);
  void advance_to_next_trace_frame();
  Completion emulate_signal_delivery(Task* oldtask, int sig);
  Completion try_one_trace_step(Task* t, RunCommand stepi);
  Completion cont_syscall_boundary(Task* t, ExecOrEmulate emu,
                                   RunCommand stepi);
  Completion enter_syscall(Task* t, RunCommand stepi);
  Completion exit_syscall(Task* t, RunCommand stepi);
  Ticks get_ticks_slack(Task* t);
  void check_ticks_consistency(Task* t, const Event& ev);
  void continue_or_step(Task* t, RunCommand stepi, int64_t tick_period = 0);
  enum ExecStateType {
    UNKNOWN,
    NOT_AT_TARGET,
    AT_TARGET
  };
  TrapType compute_trap_type(Task* t, int target_sig,
                             SignalDeterministic deterministic,
                             ExecStateType exec_state, RunCommand stepi);
  bool is_debugger_trap(Task* t, int target_sig,
                        SignalDeterministic deterministic,
                        ExecStateType exec_state, RunCommand stepi);
  Completion advance_to(Task* t, const Registers& regs, int sig,
                        RunCommand stepi, Ticks ticks);
  Completion emulate_deterministic_signal(Task* t, int sig, RunCommand stepi);
  Completion emulate_async_signal(Task* t, int sig, RunCommand stepi,
                                  Ticks ticks);
  Completion skip_desched_ioctl(Task* t, ReplayDeschedState* ds,
                                RunCommand stepi);
  void prepare_syscallbuf_records(Task* t);
  Completion flush_one_syscall(Task* t, RunCommand stepi);
  Completion flush_syscallbuf(Task* t, RunCommand stepi);
  bool is_last_interesting_task(Task* t);

  std::shared_ptr<EmuFs> emu_fs;
  // True when this is an "diversion" session; see
  // diversioner.h.  In the future, this will be a separate
  // DiversionSession class.
  bool is_diversion;
  Task* last_debugged_task;
  pid_t tgid_debugged;
  TraceReader trace_in;
  TraceFrame trace_frame;
  ReplayTraceStep current_step;
  CPUIDBugDetector cpuid_bug_detector;
  /**
   * Buffer for recorded syscallbuf bytes.  By definition buffer flushes
   * must be replayed sequentially, so we can use one buffer for all
   * tracees.  At the start of the flush, the recorded bytes are read
   * back into this buffer.  Then they're copied back to the tracee
   * record-by-record, as the tracee exits those syscalls.
   * This needs to be word-aligned.
   */
  uint8_t syscallbuf_flush_buffer_array[SYSCALLBUF_BUFFER_SIZE];
};

#endif // RR_REPLAY_SESSION_H_
