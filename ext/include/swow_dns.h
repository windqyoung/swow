/*
  +--------------------------------------------------------------------------+
  | Swow                                                                     |
  +--------------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License");          |
  | you may not use this file except in compliance with the License.         |
  | You may obtain a copy of the License at                                  |
  | http://www.apache.org/licenses/LICENSE-2.0                               |
  | Unless required by applicable law or agreed to in writing, software      |
  | distributed under the License is distributed on an "AS IS" BASIS,        |
  | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. |
  | See the License for the specific language governing permissions and      |
  | limitations under the License. See accompanying LICENSE file.            |
  +--------------------------------------------------------------------------+
  | Author: Twosee <twosee@php.net>                                          |
  +--------------------------------------------------------------------------+
 */

#ifndef SWOW_DNS_H
#define SWOW_DNS_H
#ifdef __cplusplus
extern "C" {
#endif

#include "swow.h"

#include "cat_dns.h"

/* loader */

zend_result swow_dns_module_init(INIT_FUNC_ARGS);
zend_result swow_dns_runtime_init(INIT_FUNC_ARGS);

#ifdef __cplusplus
}
#endif
#endif /* SWOW_DNS_H */
