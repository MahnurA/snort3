/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2002-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
// dt_var.h author Josh Rosenbaum <jorosenba@cisco.com>

#include <string>
#include <vector>
#include <iostream>

#ifndef DT_VAR_H
#define DT_VAR_H

class Variable
{
public:
    Variable(std::string name, int depth);
    Variable(std::string name);
    virtual ~Variable();

    inline std::string get_name(){ return name; };
    bool add_value(std::string);
    friend std::ostream &operator<<( std::ostream&, const Variable &);


private:
    enum class VarType { STRING, VARIABLE};

    struct VarData
    {
        std::string data;
        VarType type;
    };

    std::vector<VarData*> vars;
    std::string name;
    int depth;
    const std::size_t max_line_length = 77; // leave room for additional text

};


#endif
