// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos VDX Gate 3c engine: punit-path MSVDX bring-up and command submission.
 *
 * Ported from the exact 3.4 sources:
 *   video/decode/psb_msvdxinit.c (mtx_init, ccb alloc, post_boot_init, rendec)
 *   video/decode/psb_msvdx.c     (map_command subset, mtx send, completion)
 *   video/decode/psb_msvdx_fw.c  (pd programming reference)
 * The message walker keeps the exact MFLD message ids, flag patches,
 * mmu_ptd placement and deblock full-size advance.
 *
 * Completion is synchronous for bring-up (to-host ring poll + MTX IRQ clear)
 * instead of the 3.4 IRQ + fence path; 3d replaces it with real interrupts.
 */

#include "santos-vdx-compat.h"
#include "santos-msvdx-reg.h"
#include "santos-msvdx-msg.h"
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define SANTOS_MSVDX_OFFSET		0x90000
#define SANTOS_MSVDX_SIZE		0x10000
#define SANTOS_CCB0_VA			0x07000000ULL
#define SANTOS_CCB1_VA			0x07400000ULL
#define SANTOS_RENDEC_A_SIZE		(4 * 1024 * 1024)
#define SANTOS_RENDEC_B_SIZE		(1024 * 1024)
#define SANTOS_FIRMWAREID		0x014d42ab
#define SANTOS_WDT_CLOCK_DIVIDER	128

/*
 * Wire (msg_size) contract for the parser messages accepted from userspace.
 *
 * Derived from the exact device userspace and the exact 3.4 buffer walk:
 *  - pvr_drv_video.so psb_context_submit_cmdbuf() writes header 0x8114
 *    (msg_size 0x14 = 20, MTX_MSGID_DECODE_FE); 4343 archived real CMDBUF
 *    snapshots are byte-identical (v0 == 0x00008114, size == 20).
 *  - pvr_drv_video.so psb_context_submit_hw_deblock() writes header 0x4030
 *    (MTX_MSGID_DEBLOCK_MFLD) or 0x4130 (MTX_MSGID_INTRA_OOLD_MFLD), i.e.
 *    msg_size 0x30 = 48. struct fw_deblock_msg is 56 bytes; its last two
 *    dwords (address_c0/c1, "additional msg outside of IMG msg") are
 *    buffer-only, which is why 3.4 checks the full struct region and advances
 *    by it while the wire message stays 48 bytes.
 *  - MTX_MSGID_HOST_BE_OPP_MFLD (0x43) has no producer in the device
 *    userspace (no such header constant in pvr_drv_video.so) and its
 *    host_be_opp/error-concealment semantics are not ported, so the validator
 *    rejects it instead of guessing a size. MTX_MSGID_DECODE_BE_MFLD (0x42)
 *    is rejected by exact 3.4 map_command() as well.
 */
/* BK-3: a CMDBUF carries exactly one logical MSVDX message (validated in
 * eng_validate_walk): one DECODE_FE(20) or one deblock-family message
 * (48-byte wire / 56-byte logical backing). */
#define ENG_DECODE_FE_MSG_SIZE		20
#define ENG_DEBLOCK_MSG_SIZE		48
static_assert(sizeof(struct fw_decode_msg) == ENG_DECODE_FE_MSG_SIZE);
static_assert(sizeof(struct fw_deblock_msg) ==
	      ENG_DEBLOCK_MSG_SIZE + 2 * sizeof(u32));

static void __iomem *eng_reg;
static void *eng_ccb0;
static void *eng_ccb1;
static u32 eng_ccb0_va;
static u32 eng_ccb1_va;
static int eng_ready;
static int eng_init_error;
static DEFINE_MUTEX(eng_lock);
static atomic_t eng_bc_seq = ATOMIC_INIT(0);

/* 3.4-style command queue: submit enqueues and returns; a 1 ms tick drains
 * the to-host ring, signals per-sequence completion and sends the next
 * command (psb_msvdx_dequeue_send model). */
#define ENG_QUEUE_SLOTS		16

struct eng_cmd {
	u32 seq;
	u32 size;
	u8 data[PAGE_SIZE];
};

static struct eng_cmd *eng_queue;
static u32 eng_q_head;
static u32 eng_q_count;
static int eng_busy;
static u32 eng_seq_next = 1;
static u32 eng_seq_done;
static u32 eng_active_seq;
static int eng_timer_armed;
static struct timer_list eng_timer;
static wait_queue_head_t eng_wq;
static wait_queue_head_t eng_slot_wq;
static DEFINE_SPINLOCK(eng_qlock);

/* Engine terminal/quiesce state. All transitions happen under eng_qlock;
 * external readers use santos_vdx_engine_state() (READ_ONCE). One-way:
 * the first fatal cause owns the transition and the deferred quiesce. */
enum {
	ENG_STATE_RUNNING = 0,
	ENG_STATE_QUIESCE_PENDING,
	ENG_STATE_QUIESCE_RUNNING,
	ENG_STATE_STOPPED_SAFE,
	ENG_STATE_QUARANTINED,
};

/* Why the engine entered the terminal state (diagnostics only). */
enum {
	ENG_FAIL_NONE = 0,
	ENG_FAIL_DEADLINE,
	ENG_FAIL_FW_PANIC,
	ENG_FAIL_DEBLOCK_REQUIRED,
	ENG_FAIL_SEND,
};

static int eng_state = ENG_STATE_RUNNING;
static int eng_fail_reason = ENG_FAIL_NONE;
static unsigned int eng_quiesce_runs;

/* Deadline of the active hardware command (jiffies). Set only when a command
 * has been successfully sent and becomes the active job; a queued-but-unsent
 * command does not consume a deadline and gets a fresh one when it becomes
 * active. Never touched by the RX path, so a stale completion cannot extend
 * it. Compared with time_after_eq() (wrap-safe). */
static unsigned long eng_active_deadline;
static unsigned int eng_deadline_ms = 1000;
module_param(eng_deadline_ms, uint, 0444);
MODULE_PARM_DESC(eng_deadline_ms, "active-job completion deadline in ms (0 = default 1000)");

/* Deferred, process-context hardware stop for the terminal state. Defined
 * after eng_quiesce_terminal(); scheduled exactly once by eng_fail_locked(). */
static void eng_quiesce_work_fn(struct work_struct *work);
static DECLARE_WORK(eng_quiesce_work, eng_quiesce_work_fn);

static inline int eng_seq_reached(u32 seq)
{
	return (s32)(READ_ONCE(eng_seq_done) - seq) >= 0;
}

/* Persistent breadcrumb: KERN_EMERG so it reaches the sec_log console once
 * the runbook raises the console loglevel (dmesg -n 8). Survives a hard
 * reset in the Samsung sec_log region and is recoverable from p10 golden
 * /proc/last_kmsg after a wedge. */
static int eng_quiet;
module_param(eng_quiet, int, 0644);
MODULE_PARM_DESC(eng_quiet, "1 = suppress hot-path breadcrumbs");

static void eng_bc(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	if (eng_quiet)
		return;
	n = scnprintf(buf, sizeof(buf), "SANTOS_VDX_BC[%d] ",
		      atomic_inc_return(&eng_bc_seq));
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	printk(KERN_EMERG "%s\n", buf);
}

u64 santos_vdx_mmu_pd_addr(void);
int santos_vdx_mmu_inval_consume(void);
int santos_vdx_mmu_map_pages(uint64_t gpu_offset, void *cpu, uint64_t size);
int santos_vdx_mmu_init(void);

static int eng_send_messages(void *cmd, unsigned long cmd_size);

static u32 eng_rm(u32 off)
{
	return ioread32(eng_reg + off);
}

static void eng_wm(u32 val, u32 off)
{
	iowrite32(val, eng_reg + off);
}

static int eng_wait_reg(u32 off, u32 val, u32 mask, u32 polls, u32 us)
{
	u32 v = 0;

	while (polls--) {
		v = eng_rm(off);
		if ((v & mask) == val)
			return 0;
		udelay(us);
	}
	pr_err("santos-vdx-engine: wait reg 0x%x expect 0x%x mask 0x%x got 0x%x\n",
	       off, val, mask, v);
	return -ETIMEDOUT;
}

static int eng_mtx_send(const void *msg, u32 msg_size)
{
	const u32 *p_msg = msg;
	u32 msg_num = (msg_size + 3) / 4;
	u32 buf_size, buf_offset, ridx, widx, words_free;
	union msg_header *header = (union msg_header *)msg;
	static struct fw_padding_msg pad_msg;

	buf_size = eng_rm(MSVDX_COMMS_TO_MTX_BUF_SIZE) & ((1 << 16) - 1);
	eng_bc("mtx_send enter type=0x%x words=%u size_reg=0x%x",
	       header->bits.msg_type, msg_num,
	       eng_rm(MSVDX_COMMS_TO_MTX_BUF_SIZE));
	if (msg_num > buf_size)
		return -EINVAL;

	ridx = eng_rm(MSVDX_COMMS_TO_MTX_RD_INDEX);
	widx = eng_rm(MSVDX_COMMS_TO_MTX_WRT_INDEX);
	buf_offset = (eng_rm(MSVDX_COMMS_TO_MTX_BUF_SIZE) >> 16) + 0x2000;
	eng_bc("mtx_send ring rd=%u wrt=%u size=%u off=0x%x",
	       ridx, widx, buf_size, buf_offset);

	if (widx + msg_num > buf_size) {
		if (header->bits.msg_type == MTX_MSGID_PADDING)
			pr_warn("santos-vdx-engine: wrapping pad msg\n");
		if (ridx == 0)
			return -EINVAL;
		pad_msg.header.bits.msg_size = (u8)((buf_size - widx) << 2);
		pad_msg.header.bits.msg_type = MTX_MSGID_PADDING;
		if (eng_mtx_send(&pad_msg, sizeof(pad_msg)))
			return -EINVAL;
		widx = eng_rm(MSVDX_COMMS_TO_MTX_WRT_INDEX);
	}

	if (widx >= ridx)
		words_free = buf_size - (widx - ridx) - 1;
	else
		words_free = ridx - widx - 1;
	if (msg_num > words_free)
		return -EBUSY;

	while (msg_num > 0) {
		eng_wm(*p_msg++, buf_offset + (widx << 2));
		widx++;
		if (widx == buf_size)
			widx = 0;
		msg_num--;
	}
	eng_wm(widx, MSVDX_COMMS_TO_MTX_WRT_INDEX);
	eng_bc("mtx_send wrote wrt=%u", widx);
	eng_bc("mtx_kick before (int=0x%x)", eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET));
	eng_wm(1, MTX_KICK_INPUT_OFFSET);
	eng_bc("mtx_kick after (int=0x%x)", eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET));
	eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET);
	eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET);
	return 0;
}

static void eng_wake_all(void)
{
	wake_up_all(&eng_wq);
	wake_up_all(&eng_slot_wq);
}

/* Terminal state predicate. The state is only ever set under eng_qlock;
 * lockless readers must use READ_ONCE() on eng_state directly. */
static inline bool eng_terminal_locked(void)
{
	return eng_state != ENG_STATE_RUNNING;
}

/* One-way terminal engine failure, called under eng_qlock. Entered from a
 * firmware panic/failure message, an unsupported request that leaves the
 * active job unrestartable (MTX_MSGID_DEBLOCK_REQUIRED), an unrecoverable
 * send-side failure, or the active-job deadline. The first cause wins: later
 * causes observe a non-RUNNING state and do nothing.
 *
 * Deliberately does not touch eng_seq_done: an aborted job is not a completed
 * one, and advancing the completion watermark would make wait_seq/wait_idle
 * report success for work that never ran. No shell-owned pin/map is released
 * here either; that is Patch 3 (BK-2) shell work and is only permitted after
 * a successful quiesce.
 *
 * The actual hardware stop is deferred: a timer/spinlock context must not run
 * eng_quiesce_terminal(). schedule_work() is atomic-safe, and the state guard above
 * makes the schedule exactly-once, so two simultaneous fatal causes cannot
 * queue two quiesce runs. eng_quiesce_work_fn() re-checks the state. */
static void eng_fail_locked(int reason)
{
	if (eng_state != ENG_STATE_RUNNING)
		return;
	eng_state = ENG_STATE_QUIESCE_PENDING;
	eng_fail_reason = reason;
	eng_busy = 0;
	eng_q_count = 0;
	eng_wm(MSVDX_INTERRUPT_STATUS_MTX_IRQ_MASK,
	       MSVDX_INTERRUPT_CLEAR_OFFSET);
	eng_wake_all();
	schedule_work(&eng_quiesce_work);
}

/* Active-job deadline check, called under eng_qlock after the to-host ring
 * has been drained for this tick (a completion that arrived at the deadline
 * boundary must win). The deadline belongs to the sent, active hardware
 * operation only; queued-but-unsent commands are unaffected. */
static void eng_check_deadline_locked(void)
{
	if (!eng_busy)
		return;
	if (!time_after_eq(jiffies, eng_active_deadline))
		return;
	pr_err("santos-vdx-engine: active seq=%u exceeded %u ms deadline; terminal failure\n",
	       eng_active_seq, eng_deadline_ms);
	eng_fail_locked(ENG_FAIL_DEADLINE);
}

/* Non-blocking to-host ring drain. Called with eng_qlock held. */
static void eng_drain_host_locked(const char *tag)
{
	u32 rd = eng_rm(MSVDX_COMMS_TO_HOST_RD_INDEX);
	u32 wr = eng_rm(MSVDX_COMMS_TO_HOST_WRT_INDEX);
	u32 buf_size, buf_offset;

	if (rd == wr)
		return;

	buf_size = eng_rm(MSVDX_COMMS_TO_HOST_BUF_SIZE) & ((1 << 16) - 1);
	buf_offset = (eng_rm(MSVDX_COMMS_TO_HOST_BUF_SIZE) >> 16) + 0x2000;

	while (rd != wr) {
		u32 msg[64];
		union msg_header *h = (union msg_header *)msg;
		u32 num, ofs;

		msg[0] = eng_rm(buf_offset + (rd << 2));
		num = (h->bits.msg_size + 3) / 4;
		if (!num || num > 64) {
			eng_wm(rd, MSVDX_COMMS_TO_HOST_RD_INDEX);
			pr_err("santos-vdx-engine: bad msg size at rd=%u\n", rd);
			return;
		}
		if (++rd >= buf_size)
			rd = 0;
		for (ofs = 1; ofs < num; ofs++) {
			msg[ofs] = eng_rm(buf_offset + (rd << 2));
			if (++rd >= buf_size)
				rd = 0;
		}
		eng_wm(rd, MSVDX_COMMS_TO_HOST_RD_INDEX);

		switch (h->bits.msg_type) {
		case MTX_MSGID_COMPLETED:
			eng_bc("%s rx id=0x%x fence=0x%x size=%u fw=0x%x",
			       tag, h->bits.msg_type, h->bits.msg_fence,
			       h->bits.msg_size,
			       eng_rm(MSVDX_COMMS_FW_STATUS));
			/* Only the active job may advance completion. Keep the host
			 * generation; firmware reports only the low 16 bits. */
			if (!eng_busy || h->bits.msg_fence != (u16)eng_active_seq) {
				pr_warn_ratelimited("santos-vdx: unexpected completion fence=%u active=%u\n",
						    h->bits.msg_fence, eng_active_seq);
				break;
			}
			WRITE_ONCE(eng_seq_done, eng_active_seq);
			eng_busy = 0;
			eng_wm(MSVDX_INTERRUPT_STATUS_MTX_IRQ_MASK,
			       MSVDX_INTERRUPT_CLEAR_OFFSET);
			eng_wake_all();
			break;
		case MTX_MSGID_COMPLETED_BATCH:
			/* Exact 3.4 psb_msvdx_mtx_interrupt() has no case for
			 * MTX_MSGID_COMPLETED_BATCH. It is not a completion
			 * here: bounded log only, never advance eng_seq_done. */
			pr_warn_ratelimited("santos-vdx: unsupported COMPLETED_BATCH fence=%u active=%u\n",
					    h->bits.msg_fence, eng_active_seq);
			break;
		case MTX_MSGID_HW_PANIC:
		case MTX_MSGID_FAILED: {
			u32 w1 = (num > 1) ? msg[1] : 0;
			u32 w2 = (num > 2) ? msg[2] : 0;
			u32 w3 = (num > 3) ? msg[3] : 0;

			eng_bc("panic id=0x%x seq=%u fence=0x%x words=%u"
			       " w1=0x%x w2=0x%x w3=0x%x"
			       " int=0x%x mmu=0x%x dmac=0x%x"
			       " trig=0x%x ext=0x%x fw=0x%x",
			       h->bits.msg_type, eng_active_seq,
			       h->bits.msg_fence, num, w1, w2, w3,
			       eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET),
			       eng_rm(MSVDX_MMU_STATUS_OFFSET),
			       eng_rm(MSVDX_DMAC_STREAM_STATUS_OFFSET),
			       eng_rm(MSVDX_COMMS_ERROR_TRIG),
			       eng_rm(MSVDX_EXT_FW_ERROR_STATE),
			       eng_rm(MSVDX_COMMS_FW_STATUS));
			pr_err("santos-vdx-engine: FW panic id=0x%x fence=0x%x w1=0x%x w2=0x%x w3=0x%x\n",
			       h->bits.msg_type, h->bits.msg_fence,
			       w1, w2, w3);
			eng_fail_locked(ENG_FAIL_FW_PANIC);
			break;
		}
		case MTX_MSGID_DEBLOCK_REQUIRED:
			/* Firmware is waiting for a host deblock/error-concealment
			 * action. Exact 3.4 unblocks RENDEC and runs EC; this port
			 * does not implement EC, so fail closed instead of leaving
			 * the active job hung forever. */
			pr_err("santos-vdx-engine: FW DEBLOCK_REQUIRED fence=%u active=%u busy=%d\n",
			       h->bits.msg_fence, eng_active_seq, eng_busy);
			if (eng_busy)
				eng_fail_locked(ENG_FAIL_DEBLOCK_REQUIRED);
			break;
		default:
			break;
		}
		rd = eng_rm(MSVDX_COMMS_TO_HOST_RD_INDEX);
		wr = eng_rm(MSVDX_COMMS_TO_HOST_WRT_INDEX);
	}
}

/* Patch and send the head queue slot. Called with eng_qlock held. */
static void eng_send_one_locked(void)
{
	struct eng_cmd *slot;
	u8 *p;
	u32 remaining;
	u32 nmsg = 0;
	int ret;

	if (eng_terminal_locked() || eng_busy || !eng_q_count)
		return;

	slot = &eng_queue[eng_q_head];
	p = slot->data;
	remaining = slot->size;
	while (remaining > 0) {
		union msg_header *header = (union msg_header *)p;
		u32 cur_size = header->bits.msg_size;
		u32 cur_id = header->bits.msg_type;
		u32 advance;

		if (!cur_size || (cur_size % sizeof(u32)) ||
		    cur_size > remaining) {
			pr_err("santos-vdx-engine: bad queued msg id=0x%x size=%u\n",
			       cur_id, cur_size);
			eng_fail_locked(ENG_FAIL_SEND);
			return;
		}
		/* BK-3 defensive invariant: eng_validate_walk() accepts exactly
		 * one logical message, so a second iteration here means an
		 * internal caller bypassed validation. Fail closed before any
		 * firmware data is built. */
		if (++nmsg > 1) {
			pr_err("santos-vdx-engine: multi-message command in queue\n");
			eng_fail_locked(ENG_FAIL_SEND);
			return;
		}

		switch (cur_id) {
		case MTX_MSGID_DECODE_FE: {
			struct fw_decode_msg *decode_msg =
				(struct fw_decode_msg *)p;

			if (sizeof(*decode_msg) > remaining ||
			    cur_size != ENG_DECODE_FE_MSG_SIZE) {
				pr_err("santos-vdx-engine: short decode msg\n");
				eng_fail_locked(ENG_FAIL_SEND);
				return;
			}
			decode_msg->header.bits.msg_fence =
				(u16)(slot->seq & 0xffff);
			if (santos_vdx_mmu_inval_consume())
				decode_msg->flag_size.bits.flags |=
					FW_INVALIDATE_MMU;
			decode_msg->mmu_context.bits.mmu_ptd =
				santos_vdx_mmu_pd_addr() >> 8;
			eng_bc("fe post seq=%u fence=0x%x flags=0x%x"
			       " bsize=0x%x ctrl=0x%x ctx=0x%x ptd=0x%x op=0x%x",
			       slot->seq,
			       decode_msg->header.bits.msg_fence,
			       decode_msg->flag_size.bits.flags,
			       decode_msg->flag_size.bits.buffer_size,
			       decode_msg->crtl_alloc_addr,
			       decode_msg->mmu_context.bits.context,
			       decode_msg->mmu_context.bits.mmu_ptd,
			       decode_msg->operating_mode);
			advance = cur_size;
			break;
		}
		case MTX_MSGID_INTRA_OOLD_MFLD:
		case MTX_MSGID_DEBLOCK_MFLD: {
			struct fw_deblock_msg *deblock_msg =
				(struct fw_deblock_msg *)p;

			if (sizeof(*deblock_msg) > remaining ||
			    cur_size != ENG_DEBLOCK_MSG_SIZE) {
				pr_err("santos-vdx-engine: short deblock msg\n");
				eng_fail_locked(ENG_FAIL_SEND);
				return;
			}
			if (santos_vdx_mmu_inval_consume())
				deblock_msg->flag_type.bits.flags |=
					FW_INVALIDATE_MMU;
			/* Parser id -> firmware-visible id. The translated
			 * form is produced here only; eng_validate_walk()
			 * rejects userspace-supplied translated ids (exact
			 * 3.4 psb_msvdx_map_command() default). */
			deblock_msg->header.bits.msg_type =
				cur_id - MTX_MSGID_DEBLOCK_MFLD +
				MTX_MSGID_DEBLOCK;
			deblock_msg->header.bits.msg_fence =
				(u16)(slot->seq & 0xffff);
			deblock_msg->mmu_context.bits.mmu_ptd =
				santos_vdx_mmu_pd_addr() >> 8;
			advance = sizeof(struct fw_deblock_msg);
			break;
		}
		default:
			/* Not reachable through santos_vdx_engine_submit():
			 * eng_validate_walk() accepts only the parser ids
			 * above. Fail closed rather than forward an
			 * unpatched message to firmware. */
			pr_err("santos-vdx-engine: unvalidated queued msg id=0x%x\n",
			       cur_id);
			eng_fail_locked(ENG_FAIL_SEND);
			return;
		}
		if (advance > remaining) {
			pr_err("santos-vdx-engine: queued msg advance overflow id=0x%x\n",
			       cur_id);
			eng_fail_locked(ENG_FAIL_SEND);
			return;
		}
		p += advance;
		remaining -= advance;
	}

	eng_bc("send seq=%u size=%u q=%u", slot->seq, slot->size,
	       eng_q_count);
	ret = eng_send_messages(slot->data, slot->size);
	if (ret) {
		eng_bc("send failed seq=%u ret=%d", slot->seq, ret);
		pr_err("santos-vdx-engine: queue send failed %d\n", ret);
		eng_fail_locked(ENG_FAIL_SEND);
		return;
	}
	eng_active_seq = slot->seq;
	eng_busy = 1;
	/* The deadline belongs to the active hardware operation: it starts only
	 * now, after the command has been successfully sent, and a queued
	 * command waiting behind another does not consume it. */
	eng_active_deadline = jiffies + msecs_to_jiffies(eng_deadline_ms);
	eng_q_head = (eng_q_head + 1) % ENG_QUEUE_SLOTS;
	eng_q_count--;
	eng_wake_all();
}

static void eng_tick(struct timer_list *t)
{
	unsigned long flags;

	(void)t;
	spin_lock_irqsave(&eng_qlock, flags);
	if (!eng_terminal_locked()) {
		/* Drain first: a valid completion sitting in the to-host ring on
		 * the exact deadline tick must win over the deadline. Only after
		 * the available firmware messages have been processed may the
		 * still-active job be judged against its deadline. */
		eng_drain_host_locked("tick");
		if (!eng_terminal_locked())
			eng_check_deadline_locked();
		if (!eng_terminal_locked())
			eng_send_one_locked();
	}
	if (!eng_terminal_locked() && (eng_busy || eng_q_count)) {
		mod_timer(&eng_timer, jiffies + msecs_to_jiffies(1));
		eng_timer_armed = 1;
	} else {
		/* Terminal state never rearms; the timer is one-shot from here. */
		eng_timer_armed = 0;
	}
	spin_unlock_irqrestore(&eng_qlock, flags);
}

int santos_vdx_engine_wait_seq(u32 seq, unsigned int timeout_ms)
{
	long r;

	if (!eng_ready)
		return -EAGAIN;
	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING)
		return -EIO;
	r = wait_event_interruptible_timeout(eng_wq,
			READ_ONCE(eng_state) != ENG_STATE_RUNNING ||
			eng_seq_reached(seq),
			msecs_to_jiffies(timeout_ms));
	/* Terminal state beats a completion watermark: an aborted job must never
	 * turn into success, even after a successful quiesce. */
	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING)
		return -EIO;
	if (r > 0)
		return 0;
	return r == 0 ? -ETIMEDOUT : -ERESTARTSYS;
}
EXPORT_SYMBOL_GPL(santos_vdx_engine_wait_seq);

int santos_vdx_engine_wait_idle(unsigned int timeout_ms)
{
	long r;

	if (!eng_ready)
		return -EAGAIN;
	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING)
		return -EIO;
	r = wait_event_interruptible_timeout(eng_wq,
			READ_ONCE(eng_state) != ENG_STATE_RUNNING ||
			(!READ_ONCE(eng_busy) && !READ_ONCE(eng_q_count)),
			msecs_to_jiffies(timeout_ms));
	/* eng_busy/eng_q_count are cleared by the terminal transition, so the
	 * terminal check must come first or "idle" would report success. */
	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING)
		return -EIO;
	if (r > 0)
		return 0;
	return r == 0 ? -ETIMEDOUT : -ERESTARTSYS;
}
EXPORT_SYMBOL_GPL(santos_vdx_engine_wait_idle);

/* Read-only terminal/quiesce state for external observers (Patch 3/BK-2
 * shell abort cleanup). Values are stable:
 *   0 RUNNING, 1 QUIESCE_PENDING, 2 QUIESCE_RUNNING,
 *   3 STOPPED_SAFE (hardware verified stopped), 4 QUARANTINED (unsafe).
 * Nothing mutable is exported and no recovery control is exposed. */
int santos_vdx_engine_state(void)
{
	return READ_ONCE(eng_state);
}
EXPORT_SYMBOL_GPL(santos_vdx_engine_state);

u32 santos_vdx_engine_done_seq(void)
{
	return READ_ONCE(eng_seq_done);
}
EXPORT_SYMBOL_GPL(santos_vdx_engine_done_seq);

static int eng_mtx_init(void)
{
	u32 clk_divider = 200;
	int ret;

	eng_wm(0, MSVDX_COMMS_MSG_COUNTER);
	eng_wm(0, MSVDX_EXT_FW_ERROR_STATE);
	eng_wm(0, MSVDX_COMMS_ERROR_TRIG);
	eng_wm(0, MSVDX_COMMS_TO_HOST_RD_INDEX);
	eng_wm(0, MSVDX_COMMS_TO_HOST_WRT_INDEX);
	eng_wm(0, MSVDX_COMMS_TO_MTX_RD_INDEX);
	eng_wm(0, MSVDX_COMMS_TO_MTX_WRT_INDEX);
	eng_wm(0, MSVDX_COMMS_FIRMWARE_ID);
	eng_wm(0, MSVDX_COMMS_OFFSET_FLAGS);
	eng_wm(clk_divider - 1, MTX_SYSC_TIMERDIV_OFFSET);

	ret = eng_wait_reg(MSVDX_COMMS_SIGNATURE,
			   MSVDX_COMMS_SIGNATURE_VALUE, 0xffffffff, 1000, 1000);
	if (ret)
		pr_err("santos-vdx-engine: punit fw signature wait failed\n");
	return ret;
}

static int eng_ccbs_alloc(void)
{
	eng_ccb0 = alloc_pages_exact(SANTOS_RENDEC_A_SIZE,
				     GFP_KERNEL | __GFP_ZERO);
	eng_ccb1 = alloc_pages_exact(SANTOS_RENDEC_B_SIZE,
				     GFP_KERNEL | __GFP_ZERO);
	if (!eng_ccb0 || !eng_ccb1)
		return -ENOMEM;

	if (santos_vdx_mmu_map_pages(SANTOS_CCB0_VA, eng_ccb0,
				     SANTOS_RENDEC_A_SIZE) ||
	    santos_vdx_mmu_map_pages(SANTOS_CCB1_VA, eng_ccb1,
				     SANTOS_RENDEC_B_SIZE))
		return -EIO;

	eng_ccb0_va = SANTOS_CCB0_VA;
	eng_ccb1_va = SANTOS_CCB1_VA;
	pr_info("santos-vdx-engine: CCB A va=0x%x B va=0x%x\n",
		eng_ccb0_va, eng_ccb1_va);
	return 0;
}

static int eng_queue_setup(void)
{
	if (!eng_queue) {
		eng_queue = kcalloc(ENG_QUEUE_SLOTS, sizeof(*eng_queue),
				    GFP_KERNEL);
		if (!eng_queue)
			return -ENOMEM;
		init_waitqueue_head(&eng_wq);
		init_waitqueue_head(&eng_slot_wq);
		timer_setup(&eng_timer, eng_tick, 0);
	}
	eng_q_head = 0;
	eng_q_count = 0;
	eng_busy = 0;
	eng_seq_next = 1;
	eng_seq_done = 0;
	eng_active_seq = 0;
	eng_timer_armed = 0;
	eng_active_deadline = 0;
	eng_state = ENG_STATE_RUNNING;
	eng_fail_reason = ENG_FAIL_NONE;
	eng_quiesce_runs = 0;
	return 0;
}

static int eng_post_boot_init(void)
{
	struct fw_init_msg init_msg;
	u32 device_node_flags = DSIABLE_IDLE_GPIO_SIG |
				DSIABLE_Auto_CLOCK_GATING |
				RETURN_VDEB_DATA_IN_COMPLETION;
	u32 fe_wdt_clks = 0x334 * SANTOS_WDT_CLOCK_DIVIDER;
	u32 be_wdt_clks = 0x2008 * SANTOS_WDT_CLOCK_DIVIDER;
	int ret;

	eng_wm(SANTOS_FIRMWAREID, MSVDX_COMMS_FIRMWARE_ID);
	eng_wm(device_node_flags, MSVDX_COMMS_OFFSET_FLAGS);
	eng_wm(fe_wdt_clks / SANTOS_WDT_CLOCK_DIVIDER,
	       FE_MSVDX_WDT_COMPAREMATCH_OFFSET);
	eng_wm(be_wdt_clks / SANTOS_WDT_CLOCK_DIVIDER,
	       BE_MSVDX_WDT_COMPAREMATCH_OFFSET);

	memset(&init_msg, 0, sizeof(init_msg));
	init_msg.header.bits.msg_size = sizeof(struct fw_init_msg);
	init_msg.header.bits.msg_type = MTX_MSGID_INIT;
	init_msg.rendec_addr0 = eng_ccb0_va;
	init_msg.rendec_addr1 = eng_ccb1_va;
	init_msg.rendec_size.bits.rendec_size0 =
		SANTOS_RENDEC_A_SIZE / (4 * 1024);
	init_msg.rendec_size.bits.rendec_size1 =
		SANTOS_RENDEC_B_SIZE / (4 * 1024);

	ret = eng_mtx_send(&init_msg, sizeof(init_msg));
	if (ret) {
		pr_err("santos-vdx-engine: rendec init send failed %d\n", ret);
		return ret;
	}
	pr_info("santos-vdx-engine: rendec send ok: t2m[rd=%u wrt=%u size=0x%x] "
		"t2h[rd=%u wrt=%u] fw=0x%x int=0x%x\n",
		eng_rm(MSVDX_COMMS_TO_MTX_RD_INDEX),
		eng_rm(MSVDX_COMMS_TO_MTX_WRT_INDEX),
		eng_rm(MSVDX_COMMS_TO_MTX_BUF_SIZE),
		eng_rm(MSVDX_COMMS_TO_HOST_RD_INDEX),
		eng_rm(MSVDX_COMMS_TO_HOST_WRT_INDEX),
		eng_rm(MSVDX_COMMS_FW_STATUS),
		eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET));
	msleep(20);
	pr_info("santos-vdx-engine: rendec +20ms: t2m[rd=%u wrt=%u] t2h[rd=%u wrt=%u] fw=0x%x int=0x%x ctx=0x%x\n",
		eng_rm(MSVDX_COMMS_TO_MTX_RD_INDEX),
		eng_rm(MSVDX_COMMS_TO_MTX_WRT_INDEX),
		eng_rm(MSVDX_COMMS_TO_HOST_RD_INDEX),
		eng_rm(MSVDX_COMMS_TO_HOST_WRT_INDEX),
		eng_rm(MSVDX_COMMS_FW_STATUS),
		eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET),
		eng_rm(MSVDX_RENDEC_CONTEXT0_OFFSET));
	return 0;
}

int santos_vdx_engine_init(void)
{
	struct pci_dev *gfx;
	int ret = 0;

	mutex_lock(&eng_lock);
	if (eng_ready) {
		mutex_unlock(&eng_lock);
		return 0;
	}

	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING) {
		/* Terminal engine: never automatically resume or reinitialize.
		 * The hardware stop is either verified (STOPPED_SAFE) or the
		 * machine is quarantined until reboot; both refuse re-init. */
		ret = -EIO;
		goto out;
	}
	if (!eng_deadline_ms)
		eng_deadline_ms = 1000;

	if (eng_init_error) {
		ret = eng_init_error;
		goto out;
	}
	eng_bc("engine_init entry");
	ret = santos_vdx_mmu_init();
	if (ret)
		goto out;

	gfx = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	if (!gfx) {
		ret = -ENODEV;
		goto out;
	}
	eng_reg = ioremap(pci_resource_start(gfx, 0) + SANTOS_MSVDX_OFFSET,
			  SANTOS_MSVDX_SIZE);
	pci_dev_put(gfx);
	if (!eng_reg) {
		ret = -EIO;
		goto out;
	}
	pr_info("santos-vdx-engine: MSVDX window 0x%x mapped (core_id=0x%x rev=0x%x)\n",
		SANTOS_MSVDX_OFFSET, eng_rm(MSVDX_CORE_ID_OFFSET),
		eng_rm(MSVDX_CORE_REV_OFFSET));
	eng_bc("msvdx mapped core_id=0x%x rev=0x%x",
	       eng_rm(MSVDX_CORE_ID_OFFSET), eng_rm(MSVDX_CORE_REV_OFFSET));

	eng_bc("mtx_init before (sig=0x%x)", eng_rm(MSVDX_COMMS_SIGNATURE));
	ret = eng_mtx_init();
	if (ret)
		goto out;
	eng_bc("mtx_init after (sig=0x%x)", eng_rm(MSVDX_COMMS_SIGNATURE));

	eng_bc("ccb alloc before");
	ret = eng_ccbs_alloc();
	if (ret)
		goto out;
	eng_bc("ccb alloc after A=0x%x B=0x%x", eng_ccb0_va, eng_ccb1_va);

	eng_bc("rendec send before");
	ret = eng_post_boot_init();
	if (ret)
		goto out;
	eng_bc("rendec done fw=0x%x", eng_rm(MSVDX_COMMS_FW_STATUS));

	ret = eng_queue_setup();
	if (ret)
		goto out;

	eng_ready = 1;
	eng_bc("engine ready");
	pr_info("santos-vdx-engine: ready\n");
out:
	if (ret)
		eng_init_error = ret;
	mutex_unlock(&eng_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_vdx_engine_init);

/*
 * Golden psb_msvdx_core_reset() ordering (psb_msvdxinit.c:116-215), the
 * hardware-stop primitive this port adopted. The step order is the point:
 *
 *  (1) clocks on (golden psb_msvdx_mtx_set_clocks(clk_enable_all))
 *  (2) MMU pause, CONTROL0 bit 1 (golden: "Always pause the MMU as the core
 *      may be still active when resetting. It is very bad to have memory
 *      activity at the same time as a reset - Very Very bad")
 *  (3) old-core MMU-fault errata check. Golden repairs it with the
 *      mmu_recover_page workaround (psb_msvdxinit.c:131-160); this port has no
 *      recovery page, so it must NOT claim a safe stop: -EBUSY -> QUARANTINED,
 *      all resources retained. This device is affected (CORE_REV=0x00040103 <
 *      0x00050502).
 *  (4) MSVDX_MMU_MEM_REQ == 0 (golden: "make sure *ALL* outstanding reads have
 *      gone away"). This is the MSVDX MMU DMAC request counter and gates every
 *      memory read the video blocks can still have in flight.
 *  (5) RENDEC_DEC_DISABLE (golden: "disconnect RENDEC decoders from memory")
 *  (6) soft-reset all but core, with the same readback ordering as golden
 *  (7) MSVDX_MMU_MEM_REQ == 0 again (golden second wait; proves nothing was
 *      re-issued between (4) and the reset)
 *  (8) assert MSVDX soft reset (golden same)
 *  (9) wait for the reset bit to self-clear (golden 2M x 5us; this port uses a
 *      shorter 100 ms bound and fails closed on timeout)
 * (10) disable host interrupts, (11) clear interrupt status (golden same)
 *
 * Meaning of success (return 0): at (4) and (7) the MMU DMAC had no outstanding
 * memory request, the MMU was paused before any reset, the RENDEC decoders were
 * disconnected, and the core blocks were soft-reset and acknowledged it. The
 * reset MSVDX blocks therefore cannot issue further accesses to the page tables
 * or the submitted BO backing after a successful return. This is the same
 * contract the golden driver relies on when it errors the command fence and
 * then reuses the same buffers after a reset (psb_fence.c:171-192 sets
 * needs_reset; the next submit resets/re-inits before reuse).
 *
 * Limits, stated explicitly so STOPPED_SAFE is never over-claimed:
 *  - This function alone does NOT stop the MTX firmware processor; it only
 *    proves that the reset MSVDX blocks have no outstanding or future memory
 *    access. The terminal stop (eng_quiesce_terminal()) adds the golden
 *    MTX reset/disable/clock-off sequence before publishing STOPPED_SAFE.
 *  - The old-core MMU-fault errata path is refused (step 3), never guessed.
 *  - Patch 2 treats the terminal stop's success as STOPPED_SAFE and any
 *    failure as QUARANTINED; neither case releases shell-owned pins/maps or
 *    advances eng_seq_done.
 */
static int eng_quiesce_core(void)
{
	int ret;

	if (!eng_reg)
		return 0;
	eng_wm(clk_enable_all, MSVDX_MAN_CLK_ENABLE_OFFSET);
	eng_wm(MSVDX_MMU_CONTROL0_MMU_PAUSE_MASK, MSVDX_MMU_CONTROL0_OFFSET);
	if (eng_rm(MSVDX_CORE_REV_OFFSET) < 0x00050502 &&
	    (eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET) &
	     MSVDX_INTERRUPT_STATUS_MMU_FAULT_IRQ_MASK) &&
	    (eng_rm(MSVDX_MMU_STATUS_OFFSET) & 1))
		return -EBUSY;
	ret = eng_wait_reg(MSVDX_MMU_MEM_REQ_OFFSET, 0, 0xff, 1000, 1);
	if (ret)
		return ret;
	eng_wm(eng_rm(MSVDX_RENDEC_CONTROL1_OFFSET) |
	       MSVDX_RENDEC_CONTROL1_RENDEC_DEC_DISABLE_MASK,
	       MSVDX_RENDEC_CONTROL1_OFFSET);
	eng_wm(~MSVDX_CONTROL_MSVDX_SOFT_RESET_MASK, MSVDX_CONTROL_OFFSET);
	eng_rm(MSVDX_CONTROL_OFFSET);
	eng_wm(0, MSVDX_CONTROL_OFFSET);
	ret = eng_wait_reg(MSVDX_MMU_MEM_REQ_OFFSET, 0, 0xff, 100, 100);
	if (ret)
		return ret;
	eng_wm(MSVDX_CONTROL_MSVDX_SOFT_RESET_MASK, MSVDX_CONTROL_OFFSET);
	ret = eng_wait_reg(MSVDX_CONTROL_OFFSET, 0,
			   MSVDX_CONTROL_MSVDX_SOFT_RESET_MASK, 20000, 5);
	if (ret)
		return ret;
	eng_wm(0, MSVDX_HOST_INTERRUPT_ENABLE_OFFSET);
	eng_wm(~0U, MSVDX_INTERRUPT_CLEAR_OFFSET);
	eng_rm(MSVDX_INTERRUPT_STATUS_OFFSET);
	return 0;
}

/*
 * Terminal hardware stop: the golden driver-side power-down stop
 * (psb_msvdx_save_context(), psb_msvdx.c:1412-1457) minus the island
 * power-down this port does not implement.
 *
 * Ordering (exact golden save_context order):
 *  (1) reset the MTX (MTX_SOFT_RESET bit 0). While the firmware processor
 *      runs it can program the MSVDX blocks and issue commands; the golden
 *      power-down path resets it *before* psb_msvdx_core_reset(). This is the
 *      agent that a core-reset-only sequence leaves alive.
 *  (2) eng_quiesce_core(): MMU pause, MEM_REQ drain twice, RENDEC disconnect,
 *      core soft reset with acknowledgement (proof above).
 *  (3) zero the VEC local RAM (the MTX COMMS rings) so no stale command or
 *      response can be replayed.
 *  (4) MTX_ENABLE = 0 with a bounded readback check: the enable bit must read
 *      back cleared.
 *  (5) golden psb_msvdx_mtx_set_clocks(0): all clocks except core, bounded
 *      wait for the core-only mask, then core clock off. Last hardware access.
 *
 * After a successful return the MTX is held in reset, disabled and unclocked;
 * the MSVDX blocks are reset; RENDEC is disconnected; the MMU DMAC has no
 * outstanding request (verified twice in (2)); and the VEC RAM holds no
 * replayable command. No MTX/MSVDX/RENDEC/MMU-DMAC agent can therefore
 * originate another access to submitted BO or MMU/RENDEC backing. That is the
 * stronger no-future-access invariant STOPPED_SAFE publishes and Patch 3 will
 * consume before releasing shell-owned pins/maps.
 *
 * Residual, stated explicitly: the video-dec power island is NOT powered down
 * (this port has no PM support; golden follows save_context with
 * ospm_power_island_down(OSPM_VIDEO_DEC_ISLAND)). The P-Unit/gunit APM
 * firmware does not itself access the MSVDX MMU or BO backing, and this
 * terminal state refuses engine re-init, so there is no driver-issued
 * island-up/FW-load path left.
 *
 * Any failure (including a missing register window) -> QUARANTINED, backing
 * retained.
 */
static int eng_quiesce_terminal(void)
{
	u32 off;
	int ret;

	if (!eng_reg)
		return -ENODEV;
	eng_wm(MTX_SOFT_RESET_MTXRESET, MTX_SOFT_RESET_OFFSET);

	ret = eng_quiesce_core();
	if (ret)
		return ret;

	for (off = 0; off < VEC_LOCAL_MEM_BYTE_SIZE; off += sizeof(u32))
		eng_wm(0, VEC_LOCAL_MEM_OFFSET + off);

	eng_wm(0, MTX_ENABLE_OFFSET);
	ret = eng_wait_reg(MTX_ENABLE_OFFSET, 0, MTX_ENABLE_MTX_ENABLE_MASK,
			   1000, 10);
	if (ret)
		return ret;

	eng_wm(MSVDX_MAN_CLK_ENABLE_CORE_MAN_CLK_ENABLE_MASK,
	       MSVDX_MAN_CLK_ENABLE_OFFSET);
	ret = eng_wait_reg(MSVDX_MAN_CLK_ENABLE_OFFSET,
			   MSVDX_MAN_CLK_ENABLE_CORE_MAN_CLK_ENABLE_MASK,
			   0xffffffff, 20000, 5);
	if (ret)
		return ret;
	eng_wm(0, MSVDX_MAN_CLK_ENABLE_OFFSET);
	return 0;
}

/* Deferred terminal hardware stop. Runs in process context, not in timer
 * context and not under eng_qlock, because eng_quiesce_terminal() polls MMIO.
 * Exactly-once: eng_fail_locked() scheduled this work only on the RUNNING ->
 * QUIESCE_PENDING transition, and the state re-check below rejects re-entry, so
 * a second fatal indication (or a duplicate schedule) cannot run quiesce
 * twice. The work never takes eng_lock, so santos_vdx_engine_fini() can
 * flush_work() it while holding eng_lock without a cancel/flush deadlock. */
static void eng_quiesce_work_fn(struct work_struct *work)
{
	unsigned long flags;
	int ret;

	(void)work;
	spin_lock_irqsave(&eng_qlock, flags);
	if (eng_state != ENG_STATE_QUIESCE_PENDING) {
		spin_unlock_irqrestore(&eng_qlock, flags);
		return;
	}
	eng_state = ENG_STATE_QUIESCE_RUNNING;
	eng_quiesce_runs++;
	spin_unlock_irqrestore(&eng_qlock, flags);

	ret = eng_quiesce_terminal();

	spin_lock_irqsave(&eng_qlock, flags);
	eng_state = ret ? ENG_STATE_QUARANTINED : ENG_STATE_STOPPED_SAFE;
	spin_unlock_irqrestore(&eng_qlock, flags);

	if (ret)
		pr_err("santos-vdx-engine: terminal quiesce failed %d; hardware state unsafe, quarantined until reboot\n",
		       ret);
	else
		pr_info("santos-vdx-engine: terminal quiesce verified (run %u); hardware stopped, shell resources retained\n",
			eng_quiesce_runs);
}

void santos_vdx_mmu_fini(void);

void santos_vdx_engine_fini(void)
{
	int ret = 0;
	int state;

	mutex_lock(&eng_lock);
	WRITE_ONCE(eng_ready, 0);
	if (eng_queue)
		timer_shutdown_sync(&eng_timer);

	/* The deferred terminal quiesce must have finished (or never have been
	 * scheduled) before the MMIO window or engine-owned memory is torn down.
	 * flush_work() runs a pending work item and waits for a running one; the
	 * work never takes eng_lock, so this ordering cannot deadlock. */
	flush_work(&eng_quiesce_work);

	state = READ_ONCE(eng_state);
	if (state == ENG_STATE_STOPPED_SAFE) {
		/* The terminal quiesce already proved the hardware stopped; only
		 * now may engine-owned MMU/RENDEC memory be freed. Shell-owned
		 * pins/maps are not touched here (Patch 3). */
		santos_vdx_mmu_fini();
		if (eng_ccb0)
			free_pages_exact(eng_ccb0, SANTOS_RENDEC_A_SIZE);
		if (eng_ccb1)
			free_pages_exact(eng_ccb1, SANTOS_RENDEC_B_SIZE);
	} else if (state == ENG_STATE_QUARANTINED) {
		/* Quarantine already decided: do NOT run a second quiesce and do
		 * not free DMA backing. */
		pr_err("santos-vdx: engine quarantined (reason=%d); MMU/RENDEC memory retained until reboot\n",
		       eng_fail_reason);
	} else {
		/* Normal unload with no terminal failure (the deferred work was
		 * never scheduled). Engine-owned MMU/page-table and RENDEC CCB
		 * memory is DMA-visible, so the same no-future-access invariant
		 * as the terminal path applies: the full golden MTX + core stop
		 * must succeed before anything is freed, even though no
		 * shell-owned pin/map can outlive the module (module refs). An
		 * intentional unload does NOT publish a runtime terminal state;
		 * this is lifecycle-local bookkeeping only. */
		if (!eng_reg) {
			/* No MMIO window was ever mapped: CCB allocation and
			 * every firmware message happen after the window
			 * exists, so no DMA-visible backing was ever handed to
			 * firmware and no hardware stop is required. */
			santos_vdx_mmu_fini();
			if (eng_ccb0)
				free_pages_exact(eng_ccb0, SANTOS_RENDEC_A_SIZE);
			if (eng_ccb1)
				free_pages_exact(eng_ccb1, SANTOS_RENDEC_B_SIZE);
		} else {
			ret = eng_quiesce_terminal();
			if (!ret) {
				santos_vdx_mmu_fini();
				if (eng_ccb0)
					free_pages_exact(eng_ccb0,
							 SANTOS_RENDEC_A_SIZE);
				if (eng_ccb1)
					free_pages_exact(eng_ccb1,
							 SANTOS_RENDEC_B_SIZE);
			} else {
				pr_err("santos-vdx: full hardware stop failed %d; engine MMU/RENDEC memory quarantined until reboot\n",
				       ret);
			}
		}
	}
	kfree(eng_queue);
	if (eng_reg)
		iounmap(eng_reg);
	mutex_unlock(&eng_lock);
}

static int eng_send_messages(void *cmd, unsigned long cmd_size)
{
	u8 *p = cmd;
	unsigned long advance;
	int ret;

	while (cmd_size > 0) {
		union msg_header *header = (union msg_header *)p;
		u32 cur_size = header->bits.msg_size;
		u32 cur_id = header->bits.msg_type;

		if (!cur_size || cur_size > cmd_size)
			return -EINVAL;

		/* Post-mapping stream: eng_send_one_locked() already
		 * translated the parser ids and validated each struct. The
		 * consumed region is the full fw_deblock_msg for translated
		 * deblock ids (the short parser size can leave a gap), while
		 * eng_mtx_send() still transmits only msg_size bytes -- exact
		 * 3.4 psb_msvdx_send() semantics. The type-exact wire sizes
		 * are re-checked here so a corrupted queue cannot produce an
		 * invalid firmware message even if an earlier stage missed it.
		 * MTX_MSGID_HOST_BE_OPP is intentionally absent: the validator
		 * rejects its parser id, so it can only appear through internal
		 * corruption and must fail closed. */
		switch (cur_id) {
		case MTX_MSGID_DECODE_FE:
			if (cur_size != ENG_DECODE_FE_MSG_SIZE)
				return -EINVAL;
			advance = cur_size;
			break;
		case MTX_MSGID_INTRA_OOLD:
		case MTX_MSGID_DEBLOCK:
			if (cur_size != ENG_DEBLOCK_MSG_SIZE)
				return -EINVAL;
			advance = sizeof(struct fw_deblock_msg);
			break;
		default:
			pr_err("santos-vdx-engine: refusing unvalidated msg id=0x%x\n",
			       cur_id);
			return -EINVAL;
		}
		if (advance > cmd_size)
			return -EINVAL;

		if (!eng_quiet) {
			eng_bc("send id=0x%x size=%u fence=0x%x rem=%lu",
			       cur_id, cur_size, header->bits.msg_fence,
			       cmd_size);
			pr_info("santos-vdx-engine: send id=0x%x size=%u fence=0x%x rem=%lu\n",
				cur_id, cur_size, header->bits.msg_fence,
				cmd_size);
		}
		ret = eng_mtx_send(p, cur_size);
		if (ret) {
			pr_err("santos-vdx-engine: mtx_send id=0x%x failed %d\n",
			       cur_id, ret);
			return ret;
		}
		p += advance;
		cmd_size -= advance;
	}
	return 0;
}

static int eng_armed;
module_param(eng_armed, int, 0444);
MODULE_PARM_DESC(eng_armed, "1 = allow CMDBUF submission (two-key safety)");

static int eng_validate_walk(const void *cmd, u32 size)
{
	const u8 *p = cmd;
	u32 remaining = size;
	union msg_header *header;
	u32 cur_size, cur_id, advance;

	/* The header is one 32-bit word; never read past the input. */
	if (remaining < sizeof(union msg_header))
		return -EINVAL;
	header = (union msg_header *)p;
	cur_size = header->bits.msg_size;
	cur_id = header->bits.msg_type;

	if (!cur_size || (cur_size % sizeof(u32)) || cur_size > remaining)
		return -EINVAL;

	/* Userspace (parser) ids only, each with its exact wire size.
	 * Exact 3.4 psb_msvdx_map_command() accepts DECODE_FE,
	 * DEBLOCK_MFLD, INTRA_OOLD_MFLD and HOST_BE_OPP_MFLD and
	 * returns -EINVAL for everything else, including the
	 * already-translated 0x82/0x83/0x85 forms; the mapper
	 * translates the parser ids later in eng_send_one_locked().
	 * HOST_BE_OPP_MFLD is additionally rejected here because this
	 * device's userspace never produces it and its
	 * host_be_opp/EC semantics are not ported (see the size
	 * contract comment at the top of the file). */
	switch (cur_id) {
	case MTX_MSGID_DECODE_FE:
		if (sizeof(struct fw_decode_msg) > remaining ||
		    cur_size != ENG_DECODE_FE_MSG_SIZE)
			return -EINVAL;
		advance = cur_size;
		break;
	case MTX_MSGID_DEBLOCK_MFLD:
	case MTX_MSGID_INTRA_OOLD_MFLD:
		/* Short parser message; the buffer reserves a full
		 * fw_deblock_msg (3.4 advances by the struct size). */
		if (sizeof(struct fw_deblock_msg) > remaining ||
		    cur_size != ENG_DEBLOCK_MSG_SIZE)
			return -EINVAL;
		advance = sizeof(struct fw_deblock_msg);
		break;
	default:
		return -EINVAL;
	}

	/* BK-3: exactly one logical MSVDX message per CMDBUF. The engine
	 * assigns one host sequence/fence per submission and the firmware
	 * reports only that fence, so a second logical message in the same
	 * command could not be told apart from an intermediate completion;
	 * accepting it could release the whole submission early. Rejected
	 * fail-closed here, before any sequence is assigned or anything is
	 * queued/sent. The comparison uses the logical backing advance
	 * (20 bytes for DECODE_FE, sizeof(struct fw_deblock_msg) = 56 for
	 * the deblock family), not the 48-byte wire size of the MFLD
	 * messages. First message validated exactly as before; any trailing
	 * bytes (second message or garbage) => -EINVAL. */
	if (advance != remaining)
		return -EINVAL;
	return 0;
}

/* 3.4-style submit: enqueue and return; eng_tick() sends the command and
 * drains COMPLETED (psb_msvdx_submit_cmdbuf + psb_msvdx_dequeue_send). */
int santos_vdx_engine_submit(void *cmd, unsigned int size, u32 *seq_out)
{
	unsigned long flags;
	struct eng_cmd *slot;
	u32 seq;
	int ret;

	if (!eng_ready)
		return -EAGAIN;
	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING) {
		pr_err("santos-vdx-engine: submit refused (engine terminal, reboot required)\n");
		return -EIO;
	}
	if (!eng_armed) {
		pr_err("santos-vdx-engine: submit refused (eng_armed=0)\n");
		return -EPERM;
	}
	if (!cmd || !size || size > PAGE_SIZE)
		return -EINVAL;

	ret = eng_validate_walk(cmd, size);
	if (ret)
		return ret;

retry_slot:
	/* Flow control: wait for a free queue slot. */
	ret = wait_event_interruptible_timeout(eng_slot_wq,
			READ_ONCE(eng_state) != ENG_STATE_RUNNING ||
			READ_ONCE(eng_q_count) < ENG_QUEUE_SLOTS,
			msecs_to_jiffies(500));
	if (ret < 0)
		return -ERESTARTSYS;
	if (READ_ONCE(eng_state) != ENG_STATE_RUNNING)
		return -EIO;
	if (ret == 0)
		return -EBUSY;

	spin_lock_irqsave(&eng_qlock, flags);
	if (eng_terminal_locked()) {
		spin_unlock_irqrestore(&eng_qlock, flags);
		return -EIO;
	}
	/* Another submitter may have taken the slot after the wakeup. */
	if (eng_q_count == ENG_QUEUE_SLOTS) {
		spin_unlock_irqrestore(&eng_qlock, flags);
		goto retry_slot;
	}
	seq = eng_seq_next++;
	if (!eng_seq_next)
		eng_seq_next++; /* zero means no submission in the shell ABI */
	slot = &eng_queue[(eng_q_head + eng_q_count) % ENG_QUEUE_SLOTS];
	slot->seq = seq;
	slot->size = size;
	memcpy(slot->data, cmd, size);
	eng_q_count++;
	if (!eng_busy)
		eng_send_one_locked();
	if (!eng_terminal_locked() && (eng_busy || eng_q_count) &&
	    !eng_timer_armed) {
		mod_timer(&eng_timer, jiffies + msecs_to_jiffies(1));
		eng_timer_armed = 1;
	}
	spin_unlock_irqrestore(&eng_qlock, flags);

	if (seq_out)
		*seq_out = seq;
	if (!eng_quiet)
		pr_info("santos-vdx-engine: submit queued seq=%u size=%u\n",
			seq, size);
	return 0;
}
EXPORT_SYMBOL_GPL(santos_vdx_engine_submit);
