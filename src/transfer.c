/*
 * RfUdrl - entry transfer.
 *
 * Enters the image entry point with NtContinue instead of a call, so no return
 * address into this PIC is left on the stack, and the stack bottom looks like a
 * thread started through the normal Windows path.
 *
 * The synthetic frames are built on THIS THREAD'S OWN STACK, not on a fresh
 * allocation. That is not a style choice. A stack in its own allocation puts RSP
 * outside the thread's TEB StackBase/StackLimit, and the OS walker refuses to
 * walk it at all: RtlCaptureStackBackTrace returns zero frames, which is worse
 * than leaving the return address there. Carving the frames below the live RSP
 * keeps RSP inside those bounds and the walk proceeds.
 *
 * The distances between the synthetic frames are not guessed. They are the frame
 * sizes the OS unwinder itself computes, read out of the unwind info of
 * BaseThreadInitThunk and RtlUserThreadStart with the same unwind opcodes the
 * unwinder uses, so they track whatever the running OS actually has.
 *
 * The two +0x14 / +0x21 return addresses are the offsets a real call chain
 * returns into. Those are the one part that is build specific; if a stack walk
 * ever comes out wrong, they are the first thing to re-check on that OS build.
 *
 * Does not return, and assumes the entry point does not either, the same way a
 * real thread start does not come back to RtlUserThreadStart.
 */

#include <windows.h>
#include "tcg.h"
#include "transfer.h"

WINBASEAPI DWORD WINAPI KERNEL32$BaseThreadInitThunk(DWORD dwErrCode, LPSTR lpstr1, LPSTR lpstr2);
WINBASEAPI VOID  NTAPI  NTDLL$RtlUserThreadStart(PTHREAD_START_ROUTINE lpStartAddress, PVOID lpParameter);
WINBASEAPI VOID  NTAPI  NTDLL$RtlCaptureContext(PCONTEXT ContextRecord);
WINBASEAPI PRUNTIME_FUNCTION NTAPI NTDLL$RtlLookupFunctionEntry(DWORD64 ControlPc, PDWORD64 ImageBase, PVOID HistoryTable);
WINBASEAPI NTSTATUS NTAPI NTDLL$NtContinue(PCONTEXT ContextRecord, BOOLEAN TestAlert);

/* where a real call chain returns into, in each function */
#define RDLL_BTIT_RET_OFFSET  0x14
#define RDLL_RUTS_RET_OFFSET  0x21

/*
 * How far below the live RSP to put the synthetic frames. It only has to clear
 * the frame of the NtContinue call itself, which sits below this function's RSP,
 * so the frames cannot be written on top of it before it finishes restoring.
 */
#define RDLL_STACK_MARGIN     0x1000

/* the unwind opcodes that grow a frame */
#define RDLL_UWOP_PUSH_NONVOL 0
#define RDLL_UWOP_ALLOC_LARGE 1
#define RDLL_UWOP_ALLOC_SMALL 2

typedef struct _RDLL_UNWIND_CODE {
	BYTE CodeOffset;
	BYTE UnwindOp : 4;
	BYTE OpInfo   : 4;
} RDLL_UNWIND_CODE;

typedef struct _RDLL_UNWIND_INFO {
	BYTE Version : 3;
	BYTE Flags   : 5;
	BYTE SizeOfProlog;
	BYTE CountOfCodes;
	BYTE FrameRegister : 4;
	BYTE FrameOffset   : 4;
	RDLL_UNWIND_CODE UnwindCode[1];
} RDLL_UNWIND_INFO;

/*
 * Bytes of stack the prologue of the function containing control_pc consumes.
 * That is the distance the unwinder expects between this frame's return address
 * and its caller's.
 */
static DWORD rdll_stack_size_of(DWORD64 control_pc) {
	DWORD64             image_base = 0;
	PRUNTIME_FUNCTION   rf         = NULL;
	RDLL_UNWIND_INFO  * info       = NULL;
	DWORD               size       = 0;
	BYTE                index      = 0;

	rf = NTDLL$RtlLookupFunctionEntry(control_pc, &image_base, NULL);
	if (rf == NULL)
		return 0;

	info = (RDLL_UNWIND_INFO *)(image_base + rf->UnwindData);

	for (index = 0; index < info->CountOfCodes; index++) {
		RDLL_UNWIND_CODE * code = &info->UnwindCode[index];

		if (code->UnwindOp == RDLL_UWOP_PUSH_NONVOL) {
			size += 8;
		} else if (code->UnwindOp == RDLL_UWOP_ALLOC_LARGE) {
			if (code->OpInfo == 0) {
				size += (DWORD)(*(USHORT *)(&info->UnwindCode[index + 1])) * 8;
				index += 1;
			} else {
				size += *(DWORD *)(&info->UnwindCode[index + 1]);
				index += 2;
			}
		} else if (code->UnwindOp == RDLL_UWOP_ALLOC_SMALL) {
			size += (DWORD)(code->OpInfo + 1) * 8;
		}
	}

	return size;
}

void rdll_transfer_execution(void * entry, void * image, DWORD reason) {
	CONTEXT       ctx;
	volatile BYTE anchor = 0;
	DWORD64       rsp;
	DWORD64       slot;
	DWORD         size_btit;
	DWORD         size_ruts;

	size_btit = rdll_stack_size_of((DWORD64)(ULONG_PTR)KERNEL32$BaseThreadInitThunk + RDLL_BTIT_RET_OFFSET);
	size_ruts = rdll_stack_size_of((DWORD64)(ULONG_PTR)NTDLL$RtlUserThreadStart + RDLL_RUTS_RET_OFFSET);

	/*
	 * Carve out of the live stack. rsp has to be 8 modulo 16, which is what an
	 * entry point sees after a call, or the entry point faults on its first
	 * aligned access.
	 */
	rsp  = (DWORD64)(ULONG_PTR)&anchor;
	rsp -= RDLL_STACK_MARGIN;
	rsp &= ~(DWORD64)0xF;
	rsp += 8;

	/*
	 * Lay the frames out the way the unwinder reads them back.
	 *
	 * The unwinder takes the return address of the next frame from
	 * [Rsp + frame_size] and hands that frame an Rsp of Rsp + frame_size + 8.
	 * So the slot for a frame sits 8 bytes higher than the frame size alone
	 * suggests, once per level. Getting this wrong by one 8 byte slot puts a
	 * garbage value in the chain right after the first synthetic frame, which
	 * is exactly how it looked before this was fixed.
	 */
	slot = rsp;
	*(DWORD64 *)(ULONG_PTR)slot = (DWORD64)(ULONG_PTR)KERNEL32$BaseThreadInitThunk + RDLL_BTIT_RET_OFFSET;

	slot += 8 + size_btit;
	*(DWORD64 *)(ULONG_PTR)slot = (DWORD64)(ULONG_PTR)NTDLL$RtlUserThreadStart + RDLL_RUTS_RET_OFFSET;

	slot += 8 + size_ruts;
	*(DWORD64 *)(ULONG_PTR)slot = 0;

	NTDLL$RtlCaptureContext(&ctx);

	ctx.Rip = (DWORD64)(ULONG_PTR)entry;
	ctx.Rsp = rsp;
	ctx.Rcx = (DWORD64)(ULONG_PTR)image;
	ctx.Rdx = (DWORD64)reason;
	ctx.R8  = 0;

	NTDLL$NtContinue(&ctx, FALSE);
}