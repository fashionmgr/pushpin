/*
* Copyright (C) 2025 Fastly, Inc.
*
* This file is part of Pushpin.
*
* $FANOUT_BEGIN_LICENSE:APACHE2$
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
*
* $FANOUT_END_LICENSE$
*/

#ifndef DEFERCALL_H
#define DEFERCALL_H

#include <functional>
#include <memory>
#include <list>
#include <unordered_map>
#include <thread>
#include <mutex>

// queues calls to be run after returning to the event loop
class DeferCall
{
public:
	DeferCall();
	~DeferCall();

	// queue handler to be called after returning to the event loop. if
	// handler contains references, they must outlive DeferCall. the
	// recommended usage is for each object needing to perform deferred calls
	// to keep a DeferCall as a member variable, and only refer to the
	// object's own data in the handler. that way, any references are
	// guaranteed to live long enough.
	void defer(std::function<void ()> handler);

	int pendingCount() const { return deferredCalls_->size(); }

	static DeferCall *global();
	static void cleanup();

	template <typename T>
	static void deleteLater(T *p)
	{
		global()->defer([=] { delete p; });
	}

private:
	class Call;

	class CallsList
	{
	public:
		// all methods thread-safe
		std::list<std::shared_ptr<Call>>::size_type size() const;
		std::list<std::shared_ptr<Call>>::iterator append(const std::shared_ptr<Call> &c);
		void erase(std::list<std::shared_ptr<Call>>::iterator position);

	private:
		mutable std::mutex mutex;
		std::list<std::shared_ptr<Call>> l;
	};

	class Call
	{
	public:
		std::function<void ()> handler;
		std::weak_ptr<CallsList> source;
		std::list<std::shared_ptr<Call>>::iterator sourceElement;
	};

	class Manager;
	friend class Manager;

	std::thread::id thread_;
	std::shared_ptr<CallsList> deferredCalls_;

	static thread_local std::shared_ptr<Manager> localManager;
	static thread_local std::unique_ptr<DeferCall> localInstance;

	static std::unordered_map<std::thread::id, std::shared_ptr<Manager>> managerByThread;
	static std::mutex managerByThreadMutex;
};

#endif
