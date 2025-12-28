#pragma once

#include "i6_common.h"

typedef struct {
    unsigned int minShutterUs;
    unsigned int maxShutterUs;
    unsigned int minApertX10;
    unsigned int maxApertX10;
    unsigned int minSensorGain;
    unsigned int minIspGain;
    unsigned int maxSensorGain;
    unsigned int maxIspGain;
} i6_isp_exp;

// ABI-stable raw layout hints for SigmaStar ISP IQ saturation structs (from SSC33x headers):
// struct MI_ISP_IQ_SATURATION_TYPE_t {
//   MI_ISP_BOOL_e bEnable;        // aligned(4)
//   MI_ISP_OP_TYPE_e enOpType;    // aligned(4)
//   SATURATION_AUTO_ATTR_t stAuto;   // MI_ISP_AUTO_NUM=16, each param is 24 bytes => 384 bytes
//   SATURATION_MANUAL_ATTR_t stManual; // param is 24 bytes
// }
// We avoid depending on exact SDK headers by using a sufficiently large buffer and
// writing only the required fields at known offsets.
#define I6_IQ_SAT_BUF_SZ (4096u)
#define I6_IQ_SAT_OFF_BENABLE (0u)
#define I6_IQ_SAT_OFF_OPTYPE  (4u)
// 8 + 16*24 = 392
#define I6_IQ_SAT_OFF_MANUAL_PARAM (392u)
#define I6_IQ_SAT_MANUAL_PARAM_SZ  (24u)
#define I6_IQ_BOOL_TRUE  (1u)
#define I6_IQ_OP_MANUAL  (1u)

typedef struct {
    /*
     * Mirrors SigmaStar SDK Cus3AEnable_t (see mi_isp_datatype.h):
     * enables/disables user ("customer") 3A blocks inside ISP.
     */
    unsigned char bAE;
    unsigned char bAWB;
    unsigned char bAF;
} i6_isp_cus3a_enable;

typedef struct {
    void *handle, *handleCus3a, *handleIspAlgo;

    int (*fnDisableUserspace3A)(int channel);
    int (*fnCus3AEnable)(int channel, i6_isp_cus3a_enable *enable);
    int (*fnLoadChannelConfig)(int channel, char *path, unsigned int key);
    // Optional alternative API loader (some SDKs expose both API and ALGO_API entrypoints).
    int (*fnLoadChannelConfigAlgoApi)(int channel, char *path, unsigned int key);
    // Optional legacy loader (cmd bin). Some SigmaStar SDKs/tools generate "cmd bin" files
    // that are NOT compatible with MI_ISP_API_CmdLoadBinFile() (api bin).
    // If present in libmi_isp.so, DivinusX can fall back to it on load errors.
    int (*fnLoadChannelConfigLegacy)(int channel, char *path, unsigned int key);
    const char *fnLoadChannelConfigLegacySym;
    // SDK signature differs across SigmaStar branches:
    // - some expose MI_ISP_IQ_SetColorToGray(Channel, MI_ISP_IQ_COLORTOGRAY_TYPE_t*)
    // - others historically used (Channel, char*)
    // To be ABI-tolerant without SDK headers, we treat the second arg as an opaque pointer
    // and pass an int (0/1) by address from callers (first byte matches char, full int
    // satisfies enum/struct layouts where bEnable is 4 bytes).
    int (*fnSetColorToGray)(int channel, void *enable_or_struct);
    // Optional saturation controls (some SDKs implement grayscale via saturation=0).
    int (*fnSetSaturation)(int channel, void *sat);
    int (*fnGetSaturation)(int channel, void *sat);
    int (*fnGetExposureLimit)(int channel, i6_isp_exp *config);
    int (*fnSetExposureLimit)(int channel, i6_isp_exp *config);
} i6_isp_impl;

static int i6_isp_load(i6_isp_impl *isp_lib) {
    isp_lib->handleIspAlgo = dlopen("libispalgo.so", RTLD_LAZY | RTLD_GLOBAL);

    isp_lib->handleCus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);

    if (!(isp_lib->handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL)))
        HAL_ERROR("i6_isp", "Failed to load library!\nError: %s\n", dlerror());

    if (!(isp_lib->fnDisableUserspace3A = (int(*)(int channel))
        hal_symbol_load("i6_isp", isp_lib->handle, "MI_ISP_DisableUserspace3A")))
        return EXIT_FAILURE;

    if (!(isp_lib->fnCus3AEnable = (int(*)(int channel, i6_isp_cus3a_enable *enable))
        hal_symbol_load("i6_isp", isp_lib->handle, "MI_ISP_CUS3A_Enable")))
        return EXIT_FAILURE;

    if (!(isp_lib->fnLoadChannelConfig = (int(*)(int channel, char *path, unsigned int key))
        hal_symbol_load("i6_isp", isp_lib->handle, "MI_ISP_API_CmdLoadBinFile")))
        return EXIT_FAILURE;

    // Optional alternative loader.
    isp_lib->fnLoadChannelConfigAlgoApi = (int(*)(int channel, char *path, unsigned int key))
        dlsym(isp_lib->handle, "MI_ISP_ALGO_API_CmdLoadBinFile");
    if (isp_lib->fnLoadChannelConfigAlgoApi)
        HAL_INFO("i6_isp", "Alternative IQ loader available: MI_ISP_ALGO_API_CmdLoadBinFile\n");

    // Optional legacy loader for "cmd bin" files.
    // Do NOT hard-fail if missing: not all SDKs export it.
    isp_lib->fnLoadChannelConfigLegacy = NULL;
    isp_lib->fnLoadChannelConfigLegacySym = NULL;
    {
        // Different SDK branches export different legacy entrypoints.
        // Try a small set of common names.
        static const char *legacy_syms[] = {
            "MI_ISP_Load_ISPCmdBinFile",
            "MI_ISP_LoadCmdBinFile",
            "MI_ISP_Load_ISPBinFile",
            "MI_ISP_LoadBinFile",
            "MI_ISP_CmdLoadBinFile",
        };
        for (unsigned int i = 0; i < (unsigned int)(sizeof(legacy_syms) / sizeof(legacy_syms[0])); i++) {
            void *fn = dlsym(isp_lib->handle, legacy_syms[i]);
            if (fn) {
                isp_lib->fnLoadChannelConfigLegacy = (int(*)(int, char*, unsigned int))fn;
                isp_lib->fnLoadChannelConfigLegacySym = legacy_syms[i];
                HAL_INFO("i6_isp", "Legacy IQ loader available: %s\n", legacy_syms[i]);
                break;
            }
        }
    }

    if (!(isp_lib->fnSetColorToGray = (int(*)(int channel, void *enable_or_struct))
        hal_symbol_load("i6_isp", isp_lib->handle, "MI_ISP_IQ_SetColorToGray")))
        return EXIT_FAILURE;

    // Optional saturation APIs (do not hard-fail if missing).
    isp_lib->fnSetSaturation = (int(*)(int, void*))dlsym(isp_lib->handle, "MI_ISP_IQ_SetSaturation");
    isp_lib->fnGetSaturation = (int(*)(int, void*))dlsym(isp_lib->handle, "MI_ISP_IQ_GetSaturation");
    if (isp_lib->fnSetSaturation && isp_lib->fnGetSaturation)
        HAL_INFO("i6_isp", "Saturation IQ APIs available (MI_ISP_IQ_[Get|Set]Saturation)\n");

    if (!(isp_lib->fnGetExposureLimit = (int(*)(int channel, i6_isp_exp *config))
        hal_symbol_load("i6_isp", isp_lib->handle, "MI_ISP_AE_GetExposureLimit")))
        return EXIT_FAILURE;

    if (!(isp_lib->fnSetExposureLimit = (int(*)(int channel, i6_isp_exp *config))
        hal_symbol_load("i6_isp", isp_lib->handle, "MI_ISP_AE_SetExposureLimit")))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

static void i6_isp_unload(i6_isp_impl *isp_lib) {
    if (isp_lib->handle) dlclose(isp_lib->handle);
    isp_lib->handle = NULL;
    if (isp_lib->handleCus3a) dlclose(isp_lib->handleCus3a);
    isp_lib->handleCus3a = NULL;
    if (isp_lib->handleIspAlgo) dlclose(isp_lib->handleIspAlgo);
    isp_lib->handleIspAlgo = NULL;
    memset(isp_lib, 0, sizeof(*isp_lib));
}