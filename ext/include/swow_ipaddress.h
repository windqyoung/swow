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

#ifndef SWOW_IPADDRESS_H
#define SWOW_IPADDRESS_H
#ifdef __cplusplus
extern "C" {
#endif

#include "swow.h"
#include "swow_coroutine.h"

#include "ipv6.h"

extern SWOW_API zend_class_entry *swow_ipaddress_ce;
extern SWOW_API zend_object_handlers swow_ipaddress_handlers;

extern SWOW_API zend_class_entry *swow_ipaddress_exception_ce;

typedef struct swow_ipaddress_s {
    ipv6_address_full_t ipv6_address;
    zend_object std;
} swow_ipaddress_t;

/* loader */

zend_result swow_ipaddress_init(INIT_FUNC_ARGS);

/* helper*/

static zend_always_inline swow_ipaddress_t *swow_ipaddress_get_from_object(zend_object *object)
{
    return cat_container_of(object, swow_ipaddress_t, std);
}

#ifdef __cplusplus
}
#endif
#endif /* SWOW_IPADDRESS_H */
