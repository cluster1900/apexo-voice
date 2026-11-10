#ifndef DEMO_SUPPORT_H
#define DEMO_SUPPORT_H

#include "apexo_session.h"

#ifdef __cplusplus
extern "C" {
#endif

ApexoSession* DemoCreateSession(const char* model_path);
void DemoDestroyEngines(void);

#ifdef __cplusplus
}
#endif

#endif
