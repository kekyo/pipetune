/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#pragma once

/** Allocate optional test-only recording before starting PipeWire threads. */
void prepareProductPcmTrace();

/** Write the recording after all streams and their processing threads have stopped. */
void finishProductPcmTrace();

/** @return Zero when continuous, missing, and invalid PCM are distinguished without losing bounds. */
int verifyProductPcmTrace();
