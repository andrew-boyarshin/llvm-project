/* Host-triple main for CBC PIE executables (linked as libcbc-main.a).
 *
 * Adapted from cbc-engine tools/launcher/launcher.c: InitCJRuntime, then
 * __cbc_embed_load (wrap blob), then the CBC Entry.main trampoline.
 * Compiled without -fcbc. */

#include "cj-interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int InitCJRuntime(struct RuntimeParam *param);
extern enum RTErrorCode InitCJInterpreter(struct InterpreterParam *param);
extern void *RunCJTask(const void *func, void *args);
extern int GetTaskRet(const void *handle, void **ret);
extern enum RTErrorCode SetCJCommandLineArgs(int argc, char *argv[]);

extern void __cbc_embed_load(void);
extern void *engine_get_entrypoint_trampoline(void);

static void init_cangjie_runtime(int argc, char **argv) {
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  struct RuntimeParam rtParams = {
      .heapParam =
          {
              .regionSize = 64,
              .heapSize = 2 * 1024 * 1024,
              .exemptionThreshold = 0.8,
              .heapUtilization = 0.8,
              .heapGrowth = 0.15,
              .allocationRate = 0,
              .allocationWaitTime = 0,
          },
      .gcParam =
          {
              .gcThreshold = 0,
              .garbageThreshold = 0,
              .gcInterval = 0,
              .backupGCInterval = 0,
              .gcThreads = 0,
          },
      .logParam =
          {
              .logLevel = RTLOG_ERROR,
          },
      .coParam =
          {
              .thStackSize = 2 * 1024,
              .coStackSize = 64,
              .processorNum = (uint32_t)ncpu,
          },
  };

  struct InterpreterParam interpParams = {
      .interpreterLibName = "libcbcengine.so",
      .interpreterArgsCount = 0,
      .interpreterArgs = NULL,
      .appLibHandle = NULL,
  };

  int rtInitCode = InitCJRuntime(&rtParams);
  if (rtInitCode != 0) {
    fprintf(stderr, "Runtime initialization failed with code: %d\n",
            rtInitCode);
    exit(-1);
  }

  enum RTErrorCode interpInitCode = InitCJInterpreter(&interpParams);
  if (interpInitCode != E_OK) {
    fprintf(stderr, "Interpreter initialization failed with code: %d\n",
            interpInitCode);
    exit(-1);
  }

  SetCJCommandLineArgs(argc, argv);
}

static int run_cbc_main(void) {
  void *entryPoint = engine_get_entrypoint_trampoline();
  if (entryPoint == NULL) {
    fprintf(stderr, "Trampoline search failed\n");
    exit(-1);
  }

  void *fiberHandle = RunCJTask(entryPoint, NULL);
  if (fiberHandle == NULL) {
    fprintf(stderr, "Cangjie task creation failed\n");
    exit(-1);
  }

  long res;
  int runCode = GetTaskRet(fiberHandle, (void **)&res);
  if (runCode != 0) {
    fprintf(stderr, "Managed execution failed with code: %d\n", runCode);
    exit(-1);
  }

  return (int)res;
}

int main(int argc, char **argv) {
  init_cangjie_runtime(argc, argv);
  /* FindMain path is set inside __cbc_embed_load via dladdr/realpath. */
  __cbc_embed_load();
  return run_cbc_main();
}
