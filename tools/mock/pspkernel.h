#pragma once
#include <stdint.h>
typedef int SceUID; typedef unsigned int SceSize; typedef unsigned int SceUInt;
#define PSP_THREAD_ATTR_USER 0
int sceKernelCreateSema(const char*,int,int,int,void*); int sceKernelWaitSema(SceUID,int,SceUInt*);
int sceKernelSignalSema(SceUID,int); int sceKernelPollSema(SceUID,int);
SceUID sceKernelCreateThread(const char*, int(*)(SceSize,void*), int,int,int,void*);
int sceKernelStartThread(SceUID,SceSize,void*); int sceKernelDelayThread(SceUInt);
int sceKernelWaitThreadEnd(SceUID,SceUInt*); int sceKernelDeleteThread(SceUID);
