#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace bgiplayer {

enum class DisplayObjectClass : uint8_t {
    CDspObj,
    CDspObjBack,
    CDspObjBackB,
    CDspObjBackD,
    CDspObjBackDST,
    CDspObjBackF,
    CDspObjBackGRD,
    CDspObjBackML,
    CDspObjBackMSC,
    CDspObjBackN,
    CDspObjBackRPL,
    CDspObjBackRTT,
    CDspObjBackS,
    CDspObjBackSTR,
    CDspObjEffector,
    CDspObjFilter,
    CDspObjGroup,
    CDspObjKnob,
    CDspObjLandscape,
    CDspObjMap,
    CDspObjPrtclScrn,
    CDspObjRainScrn,
    CDspObjSprite,
    CDspObjVirtual,
    CDspObjWindow,
};

struct DisplayObjectClassInfo {
    DisplayObjectClass id = DisplayObjectClass::CDspObj;
    std::string_view name;
    uint32_t constructor = 0;
    uint32_t vtable = 0;
    uint8_t vtable_slots = 0;
    DisplayObjectClass parent = DisplayObjectClass::CDspObj;
    uint8_t type_level = 0;
};

struct BackgroundClassSelection {
    DisplayObjectClass id = DisplayObjectClass::CDspObjBackN;
    uint32_t object_size = 0x154U;
};

[[nodiscard]] constexpr BackgroundClassSelection
background_class_for_type(uint32_t type) {
    switch (type) {
        case 2: return {DisplayObjectClass::CDspObjBackB, 0x15CU};
        case 3: return {DisplayObjectClass::CDspObjBackS, 0x1B4U};
        case 4: return {DisplayObjectClass::CDspObjBackF, 0x180U};
        case 5: return {DisplayObjectClass::CDspObjBackD, 0x2254U};
        case 6: return {DisplayObjectClass::CDspObjBackDST, 0x168U};
        case 7: return {DisplayObjectClass::CDspObjBackGRD, 0x158U};
        case 8: return {DisplayObjectClass::CDspObjBackRPL, 0x178U};
        case 9: return {DisplayObjectClass::CDspObjBackSTR, 0x180U};
        case 10: return {DisplayObjectClass::CDspObjBackRTT, 0x164U};
        case 11: return {DisplayObjectClass::CDspObjBackMSC, 0x164U};
        case 12: return {DisplayObjectClass::CDspObjBackML, 0x410U};
        default: return {DisplayObjectClass::CDspObjBackN, 0x154U};
    }
}

inline constexpr std::array<DisplayObjectClassInfo, 25>
    kDisplayObjectClasses{{
        {DisplayObjectClass::CDspObj, "CDspObj",
         0x0041F390U, 0x0050F2C0U, 30,
         DisplayObjectClass::CDspObj, 0},
        {DisplayObjectClass::CDspObjBack, "CDspObjBack",
         0x00421490U, 0x0050F33CU, 34,
         DisplayObjectClass::CDspObj, 0},
        {DisplayObjectClass::CDspObjBackB, "CDspObjBackB",
         0x00421710U, 0x0050F3C8U, 34,
         DisplayObjectClass::CDspObjBack, 2},
        {DisplayObjectClass::CDspObjBackD, "CDspObjBackD",
         0x00421A00U, 0x0050F454U, 34,
         DisplayObjectClass::CDspObjBack, 5},
        {DisplayObjectClass::CDspObjBackDST, "CDspObjBackDST",
         0x004220C0U, 0x0050F4E0U, 34,
         DisplayObjectClass::CDspObjBack, 6},
        {DisplayObjectClass::CDspObjBackF, "CDspObjBackF",
         0x004224A0U, 0x0050F56CU, 34,
         DisplayObjectClass::CDspObjBack, 4},
        {DisplayObjectClass::CDspObjBackGRD, "CDspObjBackGRD",
         0x00422BC0U, 0x0050F5F8U, 34,
         DisplayObjectClass::CDspObjBack, 7},
        {DisplayObjectClass::CDspObjBackML, "CDspObjBackML",
         0x00422DB0U, 0x0050F684U, 34,
         DisplayObjectClass::CDspObjBack, 12},
        {DisplayObjectClass::CDspObjBackMSC, "CDspObjBackMSC",
         0x00423930U, 0x0050F710U, 34,
         DisplayObjectClass::CDspObjBack, 11},
        {DisplayObjectClass::CDspObjBackN, "CDspObjBackN",
         0x00423C90U, 0x0050F79CU, 34,
         DisplayObjectClass::CDspObjBack, 1},
        {DisplayObjectClass::CDspObjBackRPL, "CDspObjBackRPL",
         0x00423E30U, 0x0050F828U, 34,
         DisplayObjectClass::CDspObjBack, 8},
        {DisplayObjectClass::CDspObjBackRTT, "CDspObjBackRTT",
         0x004243F0U, 0x0050F8B4U, 34,
         DisplayObjectClass::CDspObjBack, 10},
        {DisplayObjectClass::CDspObjBackS, "CDspObjBackS",
         0x00424760U, 0x0050F940U, 34,
         DisplayObjectClass::CDspObjBack, 3},
        {DisplayObjectClass::CDspObjBackSTR, "CDspObjBackSTR",
         0x00424C00U, 0x0050F9CCU, 34,
         DisplayObjectClass::CDspObjBack, 9},
        {DisplayObjectClass::CDspObjEffector, "CDspObjEffector",
         0x00424FF0U, 0x0050FA58U, 30,
         DisplayObjectClass::CDspObj, 6},
        {DisplayObjectClass::CDspObjFilter, "CDspObjFilter",
         0x00425CD0U, 0x0050FAD4U, 30,
         DisplayObjectClass::CDspObj, 7},
        {DisplayObjectClass::CDspObjGroup, "CDspObjGroup",
         0x00426090U, 0x0050FB50U, 30,
         DisplayObjectClass::CDspObj, 9},
        {DisplayObjectClass::CDspObjKnob, "CDspObjKnob",
         0x00426170U, 0x0050FBCCU, 30,
         DisplayObjectClass::CDspObj, 10},
        {DisplayObjectClass::CDspObjLandscape, "CDspObjLandscape",
         0x004269F0U, 0x0050FC48U, 30,
         DisplayObjectClass::CDspObj, 1},
        {DisplayObjectClass::CDspObjMap, "CDspObjMap",
         0x00428BA0U, 0x0050FCC4U, 30,
         DisplayObjectClass::CDspObj, 1},
        {DisplayObjectClass::CDspObjPrtclScrn, "CDspObjPrtclScrn",
         0x004296B0U, 0x0050FD40U, 30,
         DisplayObjectClass::CDspObj, 4},
        {DisplayObjectClass::CDspObjRainScrn, "CDspObjRainScrn",
         0x0042A010U, 0x0050FDBCU, 30,
         DisplayObjectClass::CDspObj, 5},
        {DisplayObjectClass::CDspObjSprite, "CDspObjSprite",
         0x0042A9D0U, 0x0050FE38U, 30,
         DisplayObjectClass::CDspObj, 2},
        {DisplayObjectClass::CDspObjVirtual, "CDspObjVirtual",
         0x0042FB80U, 0x0050FEB4U, 30,
         DisplayObjectClass::CDspObj, 8},
        {DisplayObjectClass::CDspObjWindow, "CDspObjWindow",
         0x0042FD50U, 0x0050FF34U, 30,
         DisplayObjectClass::CDspObj, 3},
    }};

[[nodiscard]] constexpr const DisplayObjectClassInfo& display_class_info(
    DisplayObjectClass id) {
    for (const DisplayObjectClassInfo& info : kDisplayObjectClasses) {
        if (info.id == id) {
            return info;
        }
    }
    return kDisplayObjectClasses.front();
}

}  // namespace bgiplayer
