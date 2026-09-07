#pragma once
#include "recomp.h"
#include "test_controller.hpp"
bool rs_result_fixture_validate();
void rs_result_fixture_tick(uint8_t*,recomp_context*,TestPlayState,unsigned);
void rs_result_fixture_stats(uint8_t*,recomp_context*);
