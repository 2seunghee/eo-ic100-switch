// Safety net. If this process faults while the mitm is installed, sm aborts (fatal) on the next
// client of usb:hs, because it asks a process that no longer exists. So the handler first takes the
// mitm out of sm, then writes logs/<run>_crash.txt (registers + the last log lines) and exits.
// Sessions already intercepted end with the process either way.
#include "common.h"
#include "log.h"
#include "mitm.h"

__attribute__((aligned(16))) u8 __nx_exception_stack[0x2000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    static char text[3072];
    static volatile bool entered;
    if (entered) svcExitProcess();   // a second thread faulting while the first one reports
    entered = true;

    mitm_uninstall_all();

    MemoryInfo mi;
    u32 pi;
    u64 base = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&__libnx_exception_handler))) base = mi.addr;
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    int n = snprintf(text, sizeof(text),
                     "usbhs-logmitm %s crashed: error_desc 0x%x, thread %llu\n"
                     "pc  0x%016llx (usbhslog.elf + 0x%llx)\nlr  0x%016llx (usbhslog.elf + 0x%llx)\n"
                     "sp  0x%016llx  fp 0x%016llx  far 0x%016llx\nesr 0x%08x  pstate 0x%08x\n",
                     USBHSLOG_VERSION, ctx->error_desc, (unsigned long long)tid, (unsigned long long)ctx->pc.x,
                     (unsigned long long)(ctx->pc.x - base), (unsigned long long)ctx->lr.x,
                     (unsigned long long)(ctx->lr.x - base), (unsigned long long)ctx->sp.x,
                     (unsigned long long)ctx->fp.x, (unsigned long long)ctx->far.x, ctx->esr, ctx->pstate);
    for (int i = 0; i < 29 && n > 0 && n < (int)sizeof(text) - 40; i++)
        n += snprintf(text + n, sizeof(text) - (size_t)n, "x%-2d 0x%016llx%s", i, (unsigned long long)ctx->cpu_gprs[i].x,
                      i % 3 == 2 ? "\n" : "  ");
    if (n > 0 && n < (int)sizeof(text) - 2) text[n++] = '\n';
    log_crash(text, n > 0 ? (size_t)n : 0);
    svcExitProcess();
}
