/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_PROBE_NEW_SOURCE_H_
#define OB_PROBE_NEW_SOURCE_H_

#include "lib/ob_define.h"

namespace oceanbase
{
namespace observer
{

class ObProbeNewSource
{
public:
  ObProbeNewSource();
  virtual ~ObProbeNewSource();

private:
  DISALLOW_COPY_AND_ASSIGN(ObProbeNewSource);
};

} // end namespace observer
} // end namespace oceanbase

#endif // OB_PROBE_NEW_SOURCE_H_