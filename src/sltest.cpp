#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"

int probe()
{
    sl::Preferences pref{};
    pref.flags |= sl::PreferenceFlags::eUseManualHooking;
    sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex };
    pref.featuresToLoad = feats; pref.numFeaturesToLoad = 2;
    pref.renderAPI = sl::RenderAPI::eD3D12;

    sl::DLSSGOptions opt{}; opt.mode = sl::DLSSGMode::eOn;
    sl::Constants c{}; c.cameraMotionIncluded = sl::Boolean::eTrue; c.depthInverted = sl::Boolean::eTrue;
    sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatency;
    sl::ViewportHandle vp{0};
    sl::AdapterInfo ai{};

    // function pointer typedefs provided by headers
    PFun_slInit* pInit = nullptr;
    PFun_slSetD3DDevice* pDev = nullptr;
    PFun_slIsFeatureSupported* pSup = nullptr;
    PFun_slDLSSGSetOptions* pOpt = nullptr;
    PFun_slReflexSetMarker* pMark = nullptr;
    (void)pInit;(void)pDev;(void)pSup;(void)pOpt;(void)pMark;(void)vp;(void)ai;(void)c;(void)opt;(void)ro;
    return (int)sizeof(sl::Preferences) + (int)sizeof(sl::Constants) + (int)sizeof(sl::DLSSGOptions);
}
