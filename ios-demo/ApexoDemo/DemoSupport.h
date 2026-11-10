#ifndef DEMO_SUPPORT_H
#define DEMO_SUPPORT_H

#include "apexo_session.h"

#ifdef __cplusplus
extern "C" {
#endif

ApexoSession* DemoCreateSession(const char* asr_model_dir, const char* model_path, void** engines_out);
ApexoSession* DemoCreateSessionWithLibraries(const char* asr_model_dir, const char* model_path,
                                             const char* sherpa_library_path, const char* litert_library_path,
                                             void** engines_out);
void DemoDestroyEngines(void* engines);

#ifdef __cplusplus
}
#endif

#endif
