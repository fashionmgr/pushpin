/*
 * Copyright (C) 2025 Fastly, Inc.
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

#include "event.h"

namespace Event {

SetReadiness::SetReadiness(ffi::SetReadiness *inner) :
	inner_(inner)
{
}

SetReadiness::~SetReadiness()
{
	ffi::set_readiness_destroy(inner_);
}

int SetReadiness::setReadiness(uint8_t readiness)
{
	return ffi::set_readiness_set_readiness(inner_, readiness);
}

}
