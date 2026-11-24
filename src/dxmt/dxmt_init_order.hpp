#pragma once
#include "util_env.hpp"

namespace dxmt {

/**
OBELISK_DXMT_INIT_ORDER=1 (anything else is off): two things that are done out of order without it. Nothing else of it is anywhere; unset, every
flag, command and encoder is as it was.

A resource that is immutable and is made without its data (Direct3D has none; Neptune's guest makes every one so, and its data are the update that
follows) is not taken for one the GPU only reads: what it is written by is put in order with what reads it, as any other resource's. Without it
the update and the draws that read it are independent of each other to the optimiser and to the GPU, and a draw may see what was there before
(zeros, or what the memory was).

Two passes of the same targets are put together with the counting of the first one's last draws, by an occlusion query, turned off between them. They are put
together by putting their commands together, and a pass begins with the counting off without saying so: without it, the first one's last query
counts the second one's draws, until one of them begins to be counted itself. (Turned off there alone, not where every counted pass ends: that
made counts that miss a part, which this Metal has now and then of a query over two encoders, some three times as frequent.)
*/
inline bool
InitOrder() {
  static const bool on = env::getEnvVar("OBELISK_DXMT_INIT_ORDER") == "1";
  return on;
}

} // namespace dxmt
