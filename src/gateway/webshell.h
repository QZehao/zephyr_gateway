/**
 * @file webshell.h
 * @brief 远程 Web Shell 基础版模块
 */

#ifndef WEBSHELL_H
#define WEBSHELL_H

#include "module_base.h"

#ifdef __cplusplus
extern "C" {
#endif

const module_interface_t* webshell_get_interface(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBSHELL_H */
