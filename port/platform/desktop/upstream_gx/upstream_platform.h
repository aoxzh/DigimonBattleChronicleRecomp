#pragma once
// No upstream runtime threads are started: adapter selects synchronous XF.
enum class ThreadRole { RenderWorker };
inline void plat_thread_role(ThreadRole, const char *) {}
