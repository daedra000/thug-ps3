// THUG PS3: PPU exception handler.
//
// Without it a crash just ends the log (the game goes back to the XMB or freezes), which
// says nothing about where. lv2's debug library (liblv2dbg) calls this handler on its own
// thread when a PPU thread faults; the faulting thread is stopped, so its registers and
// stack can still be read. It logs the instruction (pc), the address touched (DAR), the
// link register and a backtrace through the stack frames, then where the game was (the
// same fields as the [alive] lines), and exits.
//
// The addresses are code addresses: tools/opd-map.py adds them to thug-ps3.map (the build's
// symbol map), so tools/lookup-addresses.py, or a search of the map, names the functions.
//
// Adapted from reVC-PS3 (src/skel/ps3/ps3_log.cpp), which uses the same liblv2dbg calls on
// hardware. If the module or the handler can't be set up (e.g. a firmware that refuses
// it), the log says so and the game runs as before.

#include <ps3_platform.h>

#include <stdio.h>
#include <string.h>
#include <sys/dbg.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>

namespace
{
	const unsigned long long kOtherStackWalk = 256 * 1024;	// threads other than the game's

	void Handler(u64 cause, sys_ppu_thread_t tid, u64 dar)
	{
		static sys_dbg_ppu_thread_context_t ctx;	// big: not on the handler's stack
		char line[256];

		snprintf(line, sizeof(line), "[crash] PPU exception: cause 0x%llx, thread 0x%llx, address touched (DAR) 0x%08llx\n",
			(unsigned long long)cause, (unsigned long long)tid, (unsigned long long)dar);
		Ps3Platform::LogNow(line);

		memset(&ctx, 0, sizeof(ctx));
		s32 ret = sysDbgReadPPUThreadContext(tid, &ctx);
		if (ret != 0)
		{
			snprintf(line, sizeof(line), "[crash] (registers unreadable: 0x%08x)\n", (unsigned)ret);
			Ps3Platform::LogNow(line);
		}
		else
		{
			snprintf(line, sizeof(line), "[crash] pc 0x%08llx (the crashing instruction)\n", (unsigned long long)ctx.pc);
			Ps3Platform::LogNow(line);
			snprintf(line, sizeof(line), "[crash] lr 0x%08llx (its caller, if it hadn't called on yet)\n", (unsigned long long)ctx.lr);
			Ps3Platform::LogNow(line);
			snprintf(line, sizeof(line), "[crash] ctr %08llx cr %08x xer %08llx\n",
				(unsigned long long)ctx.ctr, (unsigned)ctx.cr, (unsigned long long)ctx.xer);
			Ps3Platform::LogNow(line);
			for (int r = 0; r < 32; r += 4)
			{
				snprintf(line, sizeof(line), "[crash] r%-2d %016llx %016llx %016llx %016llx\n", r,
					(unsigned long long)ctx.gpr[r], (unsigned long long)ctx.gpr[r + 1],
					(unsigned long long)ctx.gpr[r + 2], (unsigned long long)ctx.gpr[r + 3]);
				Ps3Platform::LogNow(line);
			}

			// PPC64 ELFv1 frames: 0(sp) holds the caller's frame, 16(frame) the return
			// address saved there. Walk up while the chain looks like a stack.
			u64 sp = ctx.gpr[1];
			u64 top = sp + kOtherStackWalk;
			const u64 gameTop = (u64)(uintptr_t)Ps3Platform::s_stackTop;
			if (gameTop > sp && gameTop - sp < 2 * 1024 * 1024)
				top = gameTop;	// the game thread: walk to its stack top
			for (int i = 0; i < 48; ++i)
			{
				if (sp == 0 || (sp & 15) || sp >= top)
					break;
				u64 next = *(volatile u64*)(uintptr_t)sp;
				if (next <= sp || next >= top)
					break;
				u64 ra = *(volatile u64*)(uintptr_t)(next + 16);
				snprintf(line, sizeof(line), "[crash] stack %2d 0x%08llx\n", i, (unsigned long long)ra);
				Ps3Platform::LogNow(line);
				sp = next;
			}
		}

		Ps3Platform::LogWhere("[crash] the game was at:");
		Ps3Platform::LogNow("[crash] exiting\n");
		sysProcessExit(1);
	}
}

namespace Ps3Platform
{

void InstallCrashHandler()
{
	s32 ret = sysModuleLoad(SYSMODULE_LV2DBG);
	if (ret != 0)
	{
		printf("[crash handler] not available (liblv2dbg: 0x%08x): a crash only ends the log\n", (unsigned)ret);
		return;
	}
	sysDbgSetStacksizePPUExceptionHandler(64 * 1024);
	ret = sysDbgInitializePPUExceptionHandler(100);
	if (ret == 0)
		// The module calls it through a 32-bit function descriptor, like sysutil callbacks.
		// (PSL1GHT's __get_opd32 from ppu-asm.h: the 32-bit copy sits 16 bytes into the
		// 64-bit descriptor.)
		ret = sysDbgRegisterPPUExceptionHandler((dbg_exception_handler_t)((uintptr_t)(void*)Handler + 16), 0);
	if (ret != 0)
		printf("[crash handler] not available (0x%08x): a crash only ends the log\n", (unsigned)ret);
	else
		printf("[crash handler] ready: a crash logs pc, registers and a backtrace\n");
}

} // namespace Ps3Platform
