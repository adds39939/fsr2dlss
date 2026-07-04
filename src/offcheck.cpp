#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstddef>
#include <cstdio>
#include "sl.h"
#include "sl_dlss_g.h"
int main() {
    printf("REAL sl::DLSSGOptions size=%zu\n", sizeof(sl::DLSSGOptions));
    printf("  mode=%zu numFramesToGenerate=%zu flags=%zu\n",
        offsetof(sl::DLSSGOptions, mode), offsetof(sl::DLSSGOptions, numFramesToGenerate), offsetof(sl::DLSSGOptions, flags));
    printf("  onErrorCallback=%zu queueParallelismMode=%zu\n",
        offsetof(sl::DLSSGOptions, onErrorCallback), offsetof(sl::DLSSGOptions, queueParallelismMode));
    printf("  colorWidth=%zu uiBufferFormat=%zu\n",
        offsetof(sl::DLSSGOptions, colorWidth), offsetof(sl::DLSSGOptions, uiBufferFormat));
    return 0;
}
