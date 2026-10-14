#ifndef __USER_MESSAGETASK_H__
#define __USER_MESSAGETASK_H__

#ifdef __cplusplus
extern "C" {
#endif

void MessageReceiveTask(void *argument);
void MessageSendTask(void *argument);

#ifdef __cplusplus
}
#endif

#endif

