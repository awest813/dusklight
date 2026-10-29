/**
 * c_API.cpp
 *
 */

#include "SSystem/SComponent/c_API.h"

extern void mDoGph_BlankingON();
extern void mDoGph_BlankingOFF();
extern int mDoGph_BeforeOfDraw();
extern int mDoGph_AfterOfDraw();
extern int mDoGph_Painter();
extern int mDoGph_Create();

#if TARGET_PC
// Calling these int() functions through a void() pointer is undefined behaviour, and WebAssembly
// traps on it, so the table points at wrappers with the exact type.
static void cAPI_mDoGph_Create() { mDoGph_Create(); }
static void cAPI_mDoGph_BeforeOfDraw() { mDoGph_BeforeOfDraw(); }
static void cAPI_mDoGph_AfterOfDraw() { mDoGph_AfterOfDraw(); }
static void cAPI_mDoGph_Painter() { mDoGph_Painter(); }

DUSK_GAME_DATA cAPI_Interface g_cAPI_Interface = {
    cAPI_mDoGph_Create,
    cAPI_mDoGph_BeforeOfDraw,
    cAPI_mDoGph_AfterOfDraw,
    cAPI_mDoGph_Painter,
    mDoGph_BlankingON,
    mDoGph_BlankingOFF,
};
#else
DUSK_GAME_DATA cAPI_Interface g_cAPI_Interface = {
    (cAPIGph_Mthd)mDoGph_Create,
    (cAPIGph_Mthd)mDoGph_BeforeOfDraw,
    (cAPIGph_Mthd)mDoGph_AfterOfDraw,
    (cAPIGph_Mthd)mDoGph_Painter,
    mDoGph_BlankingON,
    mDoGph_BlankingOFF,
};
#endif
