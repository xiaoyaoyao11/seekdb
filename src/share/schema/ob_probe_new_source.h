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

#ifndef OB_SHARE_SCHEMA_OB_PROBE_NEW_SOURCE_H_
#define OB_SHARE_SCHEMA_OB_PROBE_NEW_SOURCE_H_

namespace oceanbase
{
namespace share
{
namespace schema
{

// A trivial translation unit which only exists to find out whether the judge
// is able to build a branch which adds a new source file to a Unity group.
int ob_probe_new_source_answer();

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OB_SHARE_SCHEMA_OB_PROBE_NEW_SOURCE_H_
