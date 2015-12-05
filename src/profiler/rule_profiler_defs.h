//--------------------------------------------------------------------------
// Copyright (C) 2015-2015 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// rule_profiler_defs.h author Joel Cornett <jocornet@cisco.com>

#ifndef RULE_PROFILER_DEFS_H
#define RULE_PROFILER_DEFS_H

#include "detection/treenodes.h"
#include "time_profiler_defs.h"

struct dot_node_state_t;

struct RuleProfilerConfig
{
    enum Sort
    {
        SORT_NONE = 0,
        SORT_CHECKS,
        SORT_AVG_CHECK,
        SORT_TOTAL_TIME,
        SORT_MATCHES,
        SORT_NO_MATCHES,
        SORT_AVG_MATCH,
        SORT_AVG_NO_MATCH
    } sort = Sort::SORT_NONE;

    unsigned count = 0;
    bool show = false;
};

class RuleContext : public TimeContextBase
{
public:
    RuleContext(dot_node_state_t& stats) :
        TimeContextBase(), stats(stats) { }

    ~RuleContext()
    { stop(); }

    void stop(bool = false);

private:
    dot_node_state_t& stats;
};

#endif