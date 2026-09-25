/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Regression test for a use-after-free in libnice's component_io_cb when an
// ICE stream is removed during teardown.
//
// libnice keeps dispatching component_io_cb for a component's socket sources
// until its stream is removed -- clearing the io callback with
// nice_agent_attach_recv(..., NULL, NULL) does not stop that, because the
// sources stay attached to the main context. component_io_cb dereferences its
// socket source's component before taking the agent lock, so removing the
// stream from any thread other than the one dispatching those sources can free
// the component under a live dispatch.
//
// This test asserts the invariant that keeps that safe: nice_agent_remove_stream
// must run on the glib main loop thread, where GLib serializes it against the
// dispatches. It interposes two symbols to observe that, which works because
// the tests executable exports its symbols (ENABLE_EXPORTS), so the dynamic
// linker resolves libdatachannel's and libnice's calls here first:
//
//   g_main_loop_run          -- the thread that calls it is the loop thread
//   nice_agent_remove_stream -- records which thread each removal ran on
//
// Checking the invariant rather than waiting for the crash keeps this
// deterministic. Observing the actual use-after-free requires an
// ASan-instrumented libnice (a stale read of uninstrumented memory just returns
// plausible bytes) plus traffic arriving throughout teardown, which is too
// fragile to gate CI on.

#include "test.hpp"
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <dlfcn.h>

using namespace rtc;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> loop_thread_known{false};
std::atomic<std::thread::id> loop_thread{};
std::atomic<int> removals_on_loop{0};
std::atomic<int> removals_off_loop{0};

} // namespace

extern "C" {

typedef struct _GMainLoop GMainLoop;
typedef struct _NiceAgent NiceAgent;

void g_main_loop_run(GMainLoop *loop) {
	static auto real = reinterpret_cast<void (*)(GMainLoop *)>(dlsym(RTLD_NEXT, "g_main_loop_run"));
	loop_thread = std::this_thread::get_id();
	loop_thread_known = true;
	real(loop);
}

void nice_agent_remove_stream(NiceAgent *agent, unsigned int streamId) {
	static auto real = reinterpret_cast<void (*)(NiceAgent *, unsigned int)>(
	    dlsym(RTLD_NEXT, "nice_agent_remove_stream"));
	if (loop_thread_known && std::this_thread::get_id() == loop_thread.load())
		removals_on_loop++;
	else
		removals_off_loop++;
	real(agent, streamId);
}

} // extern "C"

TestResult test_ice_teardown() {
	try {
		const int before = removals_on_loop.load() + removals_off_loop.load();

		{
			Configuration config; // host candidates only, no ICE servers needed

			auto pc1 = std::make_shared<PeerConnection>(config);
			auto pc2 = std::make_shared<PeerConnection>(config);

			pc1->onLocalDescription(
			    [&pc2](Description sdp) { pc2->setRemoteDescription(std::string(sdp)); });
			pc1->onLocalCandidate(
			    [&pc2](Candidate cand) { pc2->addRemoteCandidate(std::string(cand)); });
			pc2->onLocalDescription(
			    [&pc1](Description sdp) { pc1->setRemoteDescription(std::string(sdp)); });
			pc2->onLocalCandidate(
			    [&pc1](Candidate cand) { pc1->addRemoteCandidate(std::string(cand)); });

			std::shared_ptr<DataChannel> dc2;
			std::atomic<bool> opened2{false};
			pc2->onDataChannel([&dc2, &opened2](std::shared_ptr<DataChannel> dc) {
				dc->onMessage([](message_variant) {});
				dc2 = dc;
				opened2 = true;
			});

			auto dc1 = pc1->createDataChannel("ice-teardown");
			std::atomic<bool> opened1{false};
			dc1->onOpen([&opened1]() { opened1 = true; });

			for (int i = 0; i < 200 && !(opened1 && opened2); ++i)
				std::this_thread::sleep_for(50ms);

			if (!opened1 || !opened2)
				return TestResult(false, "DataChannel did not open");

			// Send both ways so the components have live socket sources, then
			// destroy the peers, which removes their ICE streams.
			dc1->send("ping");
			if (dc2)
				dc2->send("pong");
			std::this_thread::sleep_for(100ms);
		}

		// Peer connection destruction is deferred onto the thread pool, so wait
		// for the removals rather than assuming they already happened.
		for (int i = 0; i < 200; ++i) {
			if (removals_on_loop.load() + removals_off_loop.load() > before)
				break;
			std::this_thread::sleep_for(50ms);
		}

		const int observed = removals_on_loop.load() + removals_off_loop.load() - before;
		if (observed == 0)
			return TestResult(false, "no ICE stream removal observed during teardown");

		const int off_loop = removals_off_loop.load();
		if (off_loop > 0)
			return TestResult(false, "nice_agent_remove_stream ran off the glib main loop thread (" +
			                             std::to_string(off_loop) +
			                             " times), racing component_io_cb dispatches");

		return TestResult(true);
	} catch (const std::exception &e) {
		return TestResult(false, e.what());
	}
}
