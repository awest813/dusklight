/**
 * f_pc_method_iter.cpp
 * Framework - Process Method Iteration
 */

#include "SSystem/SComponent/c_list_iter.h"
#include "f_pc/f_pc_method_iter.h"

#if TARGET_PC
// cLsIt_Method calls with two arguments (the node and its data), but fpcMtdIt_MethodFunc takes one.
// Calling it through the two-argument type is undefined (WebAssembly traps on it), so the method
// travels in the unused data argument and this calls it.
static int fpcMtdIt_CallMethod(node_class* i_node, void* i_method) {
    return ((fpcMtdIt_MethodFunc)i_method)(i_node);
}
#endif

int fpcMtdIt_Method(node_list_class* i_nodeList, fpcMtdIt_MethodFunc i_methods) {
#if TARGET_PC
    return cLsIt_Method(i_nodeList, fpcMtdIt_CallMethod, (void*)i_methods);
#else
    return cLsIt_Method(i_nodeList, (cNdIt_MethodFunc)i_methods, NULL);
#endif
}
