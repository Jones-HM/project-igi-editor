/******************************************************************************
 * @file    renderer_objects_internal.h
 * @brief   Private shared declarations for the renderer_objects_*.cpp modules.
 *          Carries the common include set plus cross-module file-local helpers
 *          so Renderer_Objects can be split across several .cpp files.
 *****************************************************************************/
#pragma once

#include "pch.h"
#include "renderer_objects.h"
#include "../config.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "logger.h"
#include "utils.h"
#include "../level/level_common.h"
#include "gl_helper.h"
#include "mef_native.h"
#include "model_texture_resolution.h"
#include "../level/qvm_parser.h"
#include "../level/qvm_decompiler.h"
#include "dat_writer.h"
#include "res_compiler.h"
#include "../level/mtp_writer.h"
#include <sstream>

// Used across draw/picking/atta/visual modules.
inline bool IsWeaponModel(const std::string& modelId) {
    if (modelId.empty()) return false;
    if (modelId.size() >= 4 && modelId[0] == '1' && 
        modelId[1] >= '0' && modelId[1] <= '9' && 
        modelId[2] >= '0' && modelId[2] <= '9' && 
        modelId[3] == '_') {
        return true;
    }
    if (modelId.rfind("WEAPON_ID_", 0) == 0 || modelId.rfind("AMMO_ID_", 0) == 0) {
        return true;
    }
    return false;
}

// Models that use Z -> Y -> X rotation order instead of default Z -> X -> Y.
// 506_ slide-up doors carry multi-axis Euler angles.
// 615_01_1 (missile on rack/carriage) uses multi-axis pitch/roll angles.
inline bool IsZyxEulerModel(const std::string& modelId) {
    if (modelId.empty()) return false;
    return modelId.rfind("506_", 0) == 0 || modelId == "615_01_1";
}
